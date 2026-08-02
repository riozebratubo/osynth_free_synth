#include "audio_io.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_sink.h"
#include "synth_config.h"
#include "synth_dsp.h"
#include "synth_params.h"
#if SYNTH_ENABLE_LINE_IN
#include "synth_simd.h"
#include "synth_smooth.h"
#endif

static const char* TAG = "audio_io";

namespace {

audio_render_fn s_render = nullptr;
void* s_render_ctx = nullptr;
const std::atomic<float>* s_master_volume = nullptr;
TaskHandle_t s_task = nullptr;
/* Owns the audio clock: its blocking write is what makes the loop below run
 * in real time. */
const audio_sink_t* s_sink = nullptr;
/* Optional second destination fed the same blocks (S29). Never paces, never
 * blocks — see audio_sink.h. NULL on builds without one. */
const audio_sink_t* s_tap = nullptr;
char s_sink_name[24] = "none";
audio_io_stats_t s_stats = {};

/* Guards the peak meters against a read-and-reset landing between the audio
 * task's compare and its store. The counters either side are 32-bit aligned
 * and cannot tear on Xtensa, but `out_peak` and `dsp_load_peak_pct` are
 * read-modify-write on *both* sides: without this, a reset could drop a block
 * that maxed after the copy-out, or resurrect a peak the reader had just
 * cleared. Uncontended on every block, so it costs a couple of instructions
 * in the audio task and nothing anywhere else. */
portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;

/* Consecutive blocks whose output peak stayed below kQuietPeak. Written by the
 * audio task, read by control tasks — a relaxed atomic, because it is a hint
 * and being one block stale never matters. */
std::atomic<uint32_t> s_quiet_blocks{0};
/* ~-78 dBFS: below the 16-bit noise floor, so a decaying reverb tail still
 * counts as sound right down to the point where it stops mattering. */
constexpr float kQuietPeak = 1.0f / 8192.0f;
/* Stop counting at a minute — nothing needs a longer answer, and it keeps the
 * conversion to milliseconds well away from overflow. */
constexpr uint32_t kQuietBlockCap =
    (uint32_t)(60.0f * SYNTH_SAMPLE_RATE / SYNTH_BLOCK_SIZE);

float s_buf_l[SYNTH_BLOCK_SIZE];
float s_buf_r[SYNTH_BLOCK_SIZE];
int16_t s_out[SYNTH_BLOCK_SIZE * 2]; /* interleaved L/R for the sinks */

#if SYNTH_ENABLE_LINE_IN
/* One int16 capture buffer (256 B) and no float copy of it: the mix stages
 * call simd_mix_i16lr_f32(), which converts, scales and accumulates into the
 * float buses in one 4-wide pass. */
int16_t s_in[SYNTH_BLOCK_SIZE * 2];
const std::atomic<float>* s_in_route = nullptr;
const std::atomic<float>* s_in_gain = nullptr;
/* False when the I2S sink is not the primary, when the RX half of the port
 * failed to come up, or when the params never registered. The stages then
 * never see a non-zero gain and the capture is skipped entirely. */
bool s_line_in_ok = false;

/* `in.route` positions. Order matches the param's enum values offset by one,
 * since 0 there is "off": 1 = mon, 2 = fx, 3 = dry. */
enum { kInMon = 0, kInFx = 1, kInDry = 2, kInPositions = 3 };

/* One smoother per position, each tracking `in.gain` while its position is
 * selected and 0 otherwise. Switching route then crossfades between two mix
 * points rather than clicking, dragging the gain does not zipper, and each
 * inactive position costs one compare per block — the same kSilent gate the
 * FX bus uses. */
osynth::dsp::Smooth s_in_sm[kInPositions];
float s_in_g[kInPositions] = {0.0f, 0.0f, 0.0f};
constexpr float kInSilent = 1e-3f;
/* int16 -> [-1, 1), folded into the gain so the mix stays one multiply. */
constexpr float kInScale = 1.0f / 32768.0f;

/* Capture one block from the ADC and advance the three route gains. Called
 * from the audio task after the cycle counter starts (so the cost lands
 * honestly in dsp_load_pct) and before the render chain (so the block just
 * captured is the one the chain mixes).
 *
 * Never blocks, and never needs to drain. audio_source_i2s_read() uses a zero
 * timeout, and a short read is zero-filled and counted. Latency cannot
 * accumulate either: the driver's RX ISR drops the oldest DMA descriptor once
 * the queue fills, and the read skips forward when the queue is nearly full,
 * so the backlog self-clamps at one or two buffers. Sharing BCLK with the DAC
 * does the rest — capture and playback run off one clock, so there is nothing
 * to resample and nothing to drift. */
void SYNTH_RENDER_IRAM line_in_capture(void) {
    if (!s_line_in_ok) return; /* gains stay at 0; the stages early-out */

    /* The return code carries nothing the frame count does not: every failure
     * mode, timeout or otherwise, arrives here as a short read. */
    size_t got = 0;
    (void)audio_source_i2s_read(s_in, SYNTH_BLOCK_SIZE, &got);
    const bool starved = (got < SYNTH_BLOCK_SIZE);
    if (starved) {
        memset(s_in + got * 2, 0,
               (SYNTH_BLOCK_SIZE - got) * 2 * sizeof(int16_t));
    }

    /* Integer max-abs over what actually arrived: one compare per sample and
     * no float work in the metering path. */
    int32_t pk = 0;
    for (size_t i = 0; i < got * 2; ++i) {
        const int32_t a = (s_in[i] < 0) ? -(int32_t)s_in[i] : (int32_t)s_in[i];
        if (a > pk) pk = a;
    }
    const float peak = (float)pk * kInScale;

    portENTER_CRITICAL(&s_stats_mux);
    if (peak > s_stats.in_peak) s_stats.in_peak = peak;
    if (starved) s_stats.in_starves++;
    portEXIT_CRITICAL(&s_stats_mux);

    const int route =
        (int)(s_in_route->load(std::memory_order_relaxed) + 0.5f);
    const float g = s_in_gain->load(std::memory_order_relaxed);
    for (int i = 0; i < kInPositions; ++i) {
        s_in_g[i] = osynth::dsp::smooth_lin(s_in_sm[i],
                                            (route == i + 1) ? g : 0.0f);
    }
}
#endif /* SYNTH_ENABLE_LINE_IN */

/* Master volume is slewed like the voice manager's mute (one bounded step
 * per block, ramped linearly inside the block) so a jump — preset load, a
 * coarse CC — never zippers or clicks. */
float s_gain = -1.0f; /* < 0: snap to the param on the first block */
constexpr float kVolStep = 0.125f;

/* TPDF dither at the 16-bit boundary: ±1 LSB triangular noise decorrelates
 * the quantization error, so long release tails fade into noise instead of
 * crackling. One xorshift per channel-sample; its two 16-bit halves form
 * the triangle. */
uint32_t s_dith = 0x6d5f3781u;

inline int16_t to_i16_dith(float v) {
    uint32_t x = s_dith;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_dith = x;
    const float d =
        ((float)(x & 0xFFFFu) + (float)(x >> 16)) * (1.0f / 65536.0f) - 1.0f;
    int32_t s = (int32_t)(v * 32767.0f + d);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return (int16_t)s;
}

/* Cycles in one block period; also the divisor for the per-stage meters.
 * Set by the audio task before its first block. */
uint32_t s_cycles_per_block = 0;

void SYNTH_RENDER_IRAM audio_task(void*) {
    ESP_LOGI(TAG, "audio task up: core %d, %d Hz, block %d (%.2f ms), sink %s",
             xPortGetCoreID(), SYNTH_SAMPLE_RATE, SYNTH_BLOCK_SIZE,
             1000.0f * SYNTH_BLOCK_SIZE / SYNTH_SAMPLE_RATE, s_sink->name);

    /* DSP load in CPU cycles (esp_cpu_get_cycle_count: one register read,
     * finer and cheaper than esp_timer). Budget = cycles per block period. */
    const uint32_t cycles_per_block =
        (uint32_t)((uint64_t)esp_rom_get_cpu_ticks_per_us() * 1000000u *
                   SYNTH_BLOCK_SIZE / SYNTH_SAMPLE_RATE);
    s_cycles_per_block = cycles_per_block;

    for (;;) {
        const uint32_t c0 = esp_cpu_get_cycle_count();

#if SYNTH_ENABLE_LINE_IN
        /* Inside the c0 window on purpose: the input is not free and the DSP
         * meter should say so. As early in the block as possible, too — it
         * gives the RX DMA the longest run at refilling before the next
         * read. */
        line_in_capture();
#endif

        memset(s_buf_l, 0, sizeof(s_buf_l));
        memset(s_buf_r, 0, sizeof(s_buf_r));

        if (s_render != nullptr) {
            s_render(s_buf_l, s_buf_r, SYNTH_BLOCK_SIZE, s_render_ctx);
        }

        const float target = s_master_volume
                                 ? s_master_volume->load(std::memory_order_relaxed)
                                 : 1.0f;
        if (s_gain < 0.0f) s_gain = target; /* first block: no boot fade */
        float g1 = s_gain;
        const float d = target - g1;
        g1 += (d > kVolStep) ? kVolStep : (d < -kVolStep) ? -kVolStep : d;
        const float dg = (g1 - s_gain) / (float)SYNTH_BLOCK_SIZE;
        float gain = s_gain;
        float peak = 0.0f;
        uint32_t clips = 0;
        for (size_t i = 0; i < SYNTH_BLOCK_SIZE; ++i) {
            gain += dg;
            const float l = s_buf_l[i] * gain;
            const float r = s_buf_r[i] * gain;
            /* Metering happens post-volume, pre-saturation: what the meter
             * reports is what would have hit the int16 hard clamp. */
            const float al = fabsf(l), ar = fabsf(r);
            if (al > peak) peak = al;
            if (ar > peak) peak = ar;
            if (al > osynth::dsp::kSoftKnee) ++clips;
            if (ar > osynth::dsp::kSoftKnee) ++clips;
            s_out[2 * i]     = to_i16_dith(osynth::dsp::soft_clip(l));
            s_out[2 * i + 1] = to_i16_dith(osynth::dsp::soft_clip(r));
        }
        s_gain = g1;
        portENTER_CRITICAL(&s_stats_mux);
        if (peak > s_stats.out_peak) s_stats.out_peak = peak;
        s_stats.soft_clips += clips;
        portEXIT_CRITICAL(&s_stats_mux);

        /* Silence run, for anything that needs a moment where a stall cannot
         * be heard — persist.c writes NVS in one. One compare per block, on a
         * peak that was computed anyway. */
        if (peak > kQuietPeak) {
            s_quiet_blocks.store(0, std::memory_order_relaxed);
        } else {
            const uint32_t n = s_quiet_blocks.load(std::memory_order_relaxed);
            if (n < kQuietBlockCap) {
                s_quiet_blocks.store(n + 1, std::memory_order_relaxed);
            }
        }

        /* DSP load = cycles to produce the block vs its real-time budget,
         * smoothed with a ~130 ms EMA. A block over budget missed its
         * deadline and counts as an underrun. */
        const uint32_t busy = esp_cpu_get_cycle_count() - c0;
        const float inst_pct = 100.0f * (float)busy / (float)cycles_per_block;
        portENTER_CRITICAL(&s_stats_mux);
        s_stats.dsp_load_pct += 0.01f * (inst_pct - s_stats.dsp_load_pct);
        if (inst_pct > s_stats.dsp_load_peak_pct) {
            s_stats.dsp_load_peak_pct = inst_pct;
        }
        if (busy > cycles_per_block) s_stats.underruns++;
        s_stats.blocks_rendered++;
        portEXIT_CRITICAL(&s_stats_mux);

        /* Tap first, primary second. The primary's write is what consumes the
         * block period, so feeding the tap ahead of it keeps the tap's
         * deposits at a fixed phase against the audio clock instead of
         * trailing a DMA wait of varying length. Its result is deliberately
         * ignored: a tap is best-effort, and letting it influence anything
         * here would hand a USB host partial control of the DAC's timing. */
        if (s_tap != nullptr) {
            (void)s_tap->write(s_out, SYNTH_BLOCK_SIZE);
        }

        /* Blocking write: the sink's DMA (or timer) is the real clock. */
        esp_err_t err = s_sink->write(s_out, SYNTH_BLOCK_SIZE);
        if (err != ESP_OK && (s_stats.blocks_rendered % 750u) == 1u) {
            ESP_LOGW(TAG, "sink '%s' write failed: %s", s_sink->name,
                     esp_err_to_name(err));
        }
    }
}

} // namespace

esp_err_t audio_io_start(audio_render_fn render, void* ctx) {
    if (s_task != nullptr) return ESP_ERR_INVALID_STATE;

    s_render = render;
    s_render_ctx = ctx;
    s_master_volume =
        osynth::ParamStore::instance().valuePtr(osynth::PID_MASTER_VOLUME);
    if (s_master_volume == nullptr) {
        ESP_LOGW(TAG, "master.volume not registered; running at unity gain");
    }

#if SYNTH_ENABLE_I2S_DAC
    s_sink = audio_sink_i2s(); /* explicit user choice beats the USB default */
#elif SYNTH_ENABLE_USB
    s_sink = audio_sink_usb();
#elif SYNTH_HAS_INTERNAL_DAC
    s_sink = audio_sink_dac();
#else
    s_sink = audio_sink_null();
#endif

    esp_err_t err = s_sink->start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sink '%s' failed to start (%s); falling back to null sink",
                 s_sink->name, esp_err_to_name(err));
        s_sink = audio_sink_null();
        ESP_ERROR_CHECK(s_sink->start());
    }

#if SYNTH_ENABLE_LINE_IN
    /* The input belongs to the I2S port, so it only exists if that port is
     * what actually came up. If the sink fell back to the null sink, or the
     * RX half was refused, the params stay registered and settable — they
     * simply do nothing, which is a better answer for the app than a control
     * that vanishes depending on how boot went. */
    s_in_route = osynth::ParamStore::instance().valuePtr(osynth::PID_LINE_IN_ROUTE);
    s_in_gain = osynth::ParamStore::instance().valuePtr(osynth::PID_LINE_IN_GAIN);
    s_line_in_ok = (s_sink == audio_sink_i2s()) && audio_source_i2s_ready() &&
                   s_in_route != nullptr && s_in_gain != nullptr;
    if (s_line_in_ok) {
        ESP_LOGI(TAG, "line input ready (stereo, sample-locked to the DAC)");
    } else {
        ESP_LOGW(TAG, "line input inactive: sink %s, rx %d, params %d",
                 s_sink->name, (int)audio_source_i2s_ready(),
                 (int)(s_in_route != nullptr && s_in_gain != nullptr));
    }
#endif

#if SYNTH_ENABLE_USB_TAP
    /* Attached even if the primary fell back to the null sink: the tap is
     * independent of whether the DAC came up, and a silent board that still
     * streams to the host is strictly more useful than one that does
     * neither. */
    s_tap = audio_sink_usb_tap();
    err = s_tap->start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tap '%s' failed to start (%s); continuing without it",
                 s_tap->name, esp_err_to_name(err));
        s_tap = nullptr;
    }
#endif

    if (s_tap != nullptr) {
        snprintf(s_sink_name, sizeof(s_sink_name), "%s+%s", s_sink->name,
                 s_tap->name);
    } else {
        snprintf(s_sink_name, sizeof(s_sink_name), "%s", s_sink->name);
    }

    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "audio", 6144, nullptr,
                                            configMAX_PRIORITIES - 2, &s_task, 1);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

#if SYNTH_ENABLE_LINE_IN

void SYNTH_RENDER_IRAM audio_io_line_in_fx(float* l, float* r, size_t frames) {
    const float g = s_in_g[kInFx];
    if (g <= kInSilent) return;
    osynth::dsp::simd_mix_i16lr_f32(s_in, g * kInScale, l, r, frames);
}

void SYNTH_RENDER_IRAM audio_io_line_in_dry(float* l, float* r, size_t frames) {
    const float g = s_in_g[kInDry];
    if (g <= kInSilent) return;
    osynth::dsp::simd_mix_i16lr_f32(s_in, g * kInScale, l, r, frames);
}

void SYNTH_RENDER_IRAM audio_io_line_in_mon(float* l, float* r, size_t frames) {
    const float g = s_in_g[kInMon];
    if (g <= kInSilent) return;
    osynth::dsp::simd_mix_i16lr_f32(s_in, g * kInScale, l, r, frames);
}

#else /* empty bodies so render_chain() needs no #if of its own */

void audio_io_line_in_fx(float*, float*, size_t) {}
void audio_io_line_in_dry(float*, float*, size_t) {}
void audio_io_line_in_mon(float*, float*, size_t) {}

#endif /* SYNTH_ENABLE_LINE_IN */

void SYNTH_RENDER_IRAM audio_io_report_stages(uint32_t voices_cycles,
                                              uint32_t fx_cycles,
                                              uint32_t loop_cycles) {
    if (s_cycles_per_block == 0) return;
    const float inv = 100.0f / (float)s_cycles_per_block;
    /* Same ~130 ms EMA as dsp_load_pct so the three read against it. */
    s_stats.stage_voices_pct +=
        0.01f * ((float)voices_cycles * inv - s_stats.stage_voices_pct);
    s_stats.stage_fx_pct +=
        0.01f * ((float)fx_cycles * inv - s_stats.stage_fx_pct);
    s_stats.stage_loop_pct +=
        0.01f * ((float)loop_cycles * inv - s_stats.stage_loop_pct);
}

const char* audio_io_sink_name(void) { return s_sink_name; }

uint32_t audio_io_quiet_ms(void) {
    const uint32_t blocks = s_quiet_blocks.load(std::memory_order_relaxed);
    return (uint32_t)((uint64_t)blocks * SYNTH_BLOCK_SIZE * 1000u /
                      SYNTH_SAMPLE_RATE);
}

void audio_io_get_stats(audio_io_stats_t* out) {
    if (out == nullptr) return;
    /* The peaks are read-and-reset windows, so the copy and the clear have to
     * be one operation as far as the audio task is concerned — otherwise a
     * block that maxes in between is reported by neither window. */
    portENTER_CRITICAL(&s_stats_mux);
    *out = s_stats;
    s_stats.dsp_load_peak_pct = 0.0f;
    s_stats.out_peak = 0.0f;
    s_stats.in_peak = 0.0f;
    portEXIT_CRITICAL(&s_stats_mux);
}
