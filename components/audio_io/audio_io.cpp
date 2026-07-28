#include "audio_io.h"

#include <atomic>
#include <cmath>
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

static const char* TAG = "audio_io";

namespace {

audio_render_fn s_render = nullptr;
void* s_render_ctx = nullptr;
const std::atomic<float>* s_master_volume = nullptr;
TaskHandle_t s_task = nullptr;
const audio_sink_t* s_sink = nullptr;
audio_io_stats_t s_stats = {};

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
        if (peak > s_stats.out_peak) s_stats.out_peak = peak;
        s_stats.soft_clips += clips;

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
        s_stats.dsp_load_pct += 0.01f * (inst_pct - s_stats.dsp_load_pct);
        if (inst_pct > s_stats.dsp_load_peak_pct) {
            s_stats.dsp_load_peak_pct = inst_pct;
        }
        if (busy > cycles_per_block) s_stats.underruns++;
        s_stats.blocks_rendered++;

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

    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "audio", 6144, nullptr,
                                            configMAX_PRIORITIES - 2, &s_task, 1);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

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

const char* audio_io_sink_name(void) {
    return (s_sink != nullptr) ? s_sink->name : "none";
}

uint32_t audio_io_quiet_ms(void) {
    const uint32_t blocks = s_quiet_blocks.load(std::memory_order_relaxed);
    return (uint32_t)((uint64_t)blocks * SYNTH_BLOCK_SIZE * 1000u /
                      SYNTH_SAMPLE_RATE);
}

void audio_io_get_stats(audio_io_stats_t* out) {
    if (out != nullptr) {
        *out = s_stats;
        /* The peaks are read-and-reset windows. Clearing from this task can
         * lose a concurrently-maxing block — fine for a diagnostic meter. */
        s_stats.dsp_load_peak_pct = 0.0f;
        s_stats.out_peak = 0.0f;
    }
}
