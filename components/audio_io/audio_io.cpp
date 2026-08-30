#include "audio_io.h"

#include <atomic>
#include <cmath>
#include <cstdint> /* INT16_MIN/MAX, for the differential-leg inversion */
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
#if SYNTH_ENABLE_AUDIO_IN
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
#if SYNTH_ENABLE_SPLIT_RENDER
/* The voice stage and the core it runs on (S45). Null on a build without the
 * pipeline, where `s_render` above is the whole chain rather than its tail. */
audio_render_fn s_render_a = nullptr;
TaskHandle_t s_task_a = nullptr;
#endif
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

/* Bus slots, and the one number the whole pipeline is expressed in.
 *
 * One slot without it: the same single buffer pair that has always been here,
 * reached through an index that is a compile-time constant, so the generated
 * code for a single-core build is what it was before this existed. Two with
 * it: the voice stage fills one while the bus stage drains the other, and they
 * swap every block.
 *
 * Two is also the maximum that is correct. A third slot would let the voice
 * stage run two blocks ahead, which buys no throughput -- the sink still
 * releases exactly one slot per block -- and costs a second block period of
 * latency on every note. What the depth actually buys is jitter tolerance, and
 * the sink's own DMA ring already provides that, several blocks of it, on the
 * side of the hand-off where a stall is heard. */
#if SYNTH_ENABLE_SPLIT_RENDER
constexpr int kSlots = 2;
#else
constexpr int kSlots = 1;
#endif

#if SYNTH_ENABLE_SPLIT_RENDER
/* Which core each stage runs on. Named once, and referred to by name
 * everywhere else, because the two are not interchangeable and the reason is
 * not visible from either task's own code.
 *
 * The voice stage gets the *empty* core. Every task this firmware pins is
 * pinned to core 0 — the BLE command task, the sequencer clock, the preset
 * loader, USB, app_main — because core 1 has always been kept clear for audio.
 * The voice stage is also the heavier half by a wide margin, and a stage that
 * goes over budget never blocks: its next credit is already waiting by the time
 * it finishes, so it runs back to back at 100% for as long as the patch is too
 * expensive for one core.
 *
 * Put that on core 0 and an over-budget patch does not merely glitch — it
 * starves the BLE host, the sequencer clock and the idle task with it. That was
 * not a theory: a granular patch reading the live input pushed the stage past
 * 100%, and the board went on making sound while the app dropped, the heartbeat
 * stopped and the task watchdog reported IDLE0 starved by `audio_voi`. On core
 * 1 the same patch costs the idle task of a core that has nothing else to do,
 * which is what going over budget ought to cost.
 *
 * The bus stage is the half that can afford core 0. It is the light one, and it
 * blocks on the sink's DMA every single block, so the control tasks get the
 * core back on a schedule rather than at the mercy of the patch. Its own
 * deadline is covered by the DMA ring — four blocks deep, far more than an
 * interrupt from anything else on that core costs it. */
constexpr BaseType_t kVoiceCore = 1;
constexpr BaseType_t kBusCore = 0;
#endif

float s_buf_l[kSlots][SYNTH_BLOCK_SIZE];
float s_buf_r[kSlots][SYNTH_BLOCK_SIZE];
int16_t s_out[SYNTH_BLOCK_SIZE * 2]; /* interleaved L/R for the sinks */

#if SYNTH_ENABLE_AUDIO_IN
/* Capture buffers, one per compiled device, int16 and no float copy of them:
 * the mix stages call simd_mix_i16lr_f32(), which converts, scales and
 * accumulates into the float buses in one 4-wide pass.
 *
 * Two buffers rather than one (S37b), because `in.source` gained `both` and
 * the two devices are no longer alternatives. They are still summed in the
 * *float* domain by running the mix stage once per device, never added
 * together as int16 — a line at -6 dBFS plus a mic at -6 dBFS is full scale,
 * and clipping that at the capture would throw away headroom the float bus and
 * the master soft clipper already handle properly. 256 bytes each. */
#if SYNTH_ENABLE_IN_SOURCE_SEL
constexpr int kDevCount = 2;
#else
/* One device compiled: it lives in slot 0 whichever it is, so a single-input
 * build carries exactly the one buffer it did before this existed. */
constexpr int kDevCount = 1;
#endif
/* Slot each device reads into. Absent devices are never indexed — every use is
 * inside the matching #if — and -1 makes an accidental one fail loudly rather
 * than quietly aliasing the other device's block. */
#if SYNTH_ENABLE_LINE_IN
constexpr int kSlotLine = 0;
#else
constexpr int kSlotLine = -1;
#endif
#if SYNTH_ENABLE_MIC_IN
constexpr int kSlotMic = SYNTH_ENABLE_IN_SOURCE_SEL ? 1 : 0;
#else
constexpr int kSlotMic = -1;
#endif

/* Per slot as well as per device (S45): the bus stage fills one slot while the
 * voice stage's engines read the other through audio_io_in_mono() and
 * audio_io_line_in_block(). Without the pipeline kSlots is 1 and this is the
 * same 256 bytes per device it always was. */
int16_t s_cap[kSlots][kDevCount][SYNTH_BLOCK_SIZE * 2];

/* Which capture slot each core is allowed to read this block, indexed by core
 * id. Published by the bus stage before it releases the voice stage, so the
 * notification that starts that stage is also the barrier that makes this
 * visible to it -- there is deliberately no atomic here, because there is no
 * moment when the two cores are looking at it without one of those in between.
 *
 * The whole point is that a caller deep inside an engine does not have to know
 * which core it is on. in_slot() answers that from where it is standing, and
 * on a build without the pipeline it answers 0 and disappears. */
#if SYNTH_ENABLE_SPLIT_RENDER
volatile int s_in_slot[2];

inline int SYNTH_RENDER_IRAM in_slot(void) {
    return s_in_slot[xPortGetCoreID()];
}

/* `fresh` is the slot the bus stage is about to capture into; the voice stage
 * therefore gets the other one, which is the block captured one period earlier
 * and which nothing will write while it reads. Indexed by the stage constants
 * rather than by literal 0 and 1, so swapping the assignment above cannot leave
 * the two cores pointed at one buffer. */
inline void SYNTH_RENDER_IRAM publish_in_slots(int fresh) {
    s_in_slot[kBusCore] = fresh;
    s_in_slot[kVoiceCore] = fresh ^ 1;
}
#else
/* No pipeline, one slot, and no publication to make: the only caller of
 * publish_in_slots() is the bus stage's loop, which does not exist here. */
inline int SYNTH_RENDER_IRAM in_slot(void) { return 0; }
#endif
const std::atomic<float>* s_in_route = nullptr;
const std::atomic<float>* s_in_gain = nullptr;
#if SYNTH_ENABLE_IN_SOURCE_SEL
const std::atomic<float>* s_in_source = nullptr;
const std::atomic<float>* s_in_micgain = nullptr;
/* `in.source` values. `both` is last so the two single-device values keep the
 * numbers they were persisted and preset-defaulted with. */
enum { kSelLine = 0, kSelMic = 1, kSelBoth = 2 };
#endif

/* Per-device readiness, resolved once at start.
 *
 * Separate flags rather than one, because they fail for unrelated reasons and
 * with `both` selected the difference is visible in the mix: the line input
 * dies with the I2S sink it shares a port with, while the mic has its own
 * controller and can be the only one left standing. `s_in_ok` is the question
 * the capture asks — is there any device at all, and did the params register. */
bool s_line_ok = false;
bool s_mic_ok = false;
bool s_in_ok = false;

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

/* And one smoother per *device*, tracking 1 while `in.source` selects it and 0
 * otherwise (times the mic's own trim, for the mic).
 *
 * This is what replaced the mute-swap-unmute fence the exclusive selector
 * needed, and it is better in every direction. line -> mic is now a genuine
 * crossfade rather than a gap; line -> both ramps the mic in over a line that
 * never stops; and none of it needs the capture to hold state about a switch
 * being in progress, because a device that is fading out is simply a device
 * whose gain has not reached zero yet. The old fence also had to blank the
 * shared buffer on swap, which per-device buffers make meaningless. */
osynth::dsp::Smooth s_dev_sm[kDevCount];
/* Per slot, because audio_io_in_mono() applies these and is called from both
 * stages. The smoothers beside them are not: they are advanced once per block
 * by the capture, which only ever runs on the bus stage, and what travels with
 * the block is the value they landed on rather than the state that produced it.
 *
 * s_in_g below is single for the same reason read the other way round -- every
 * one of its readers (the three mix stages, audio_io_in_fx_block()) is on the
 * bus stage with the capture, so there is no second view to keep consistent. */
float s_dev_g[kSlots][kDevCount] = {};

constexpr float kInSilent = 1e-3f;
/* int16 -> [-1, 1), folded into the gain so the mix stays one multiply. */
constexpr float kInScale = 1.0f / 32768.0f;

/* Read one device into its slot, meter it, and zero-fill a short read.
 *
 * `invert_r` un-inverts the differential pair's second leg, and is true only
 * for the line input on an ES8388 wired that way (SYNTH_LINE_IN_INVERT_R in
 * synth_config.h). It must never be applied to the microphone: that block
 * already arrives as one coherent signal duplicated across both channels, and
 * inverting one leg of it would fold to exact silence in every mono take —
 * the same trap this un-inversion exists to close, re-opened from the other
 * side. Doing it here, before anything reads the block, is what keeps every
 * consumer (the mix stages, the graph's LineIn node, the meters) seeing one
 * coherent signal instead of two opposed ones.
 *
 * The read's return code carries nothing the frame count does not: every
 * failure mode, timeout or otherwise, arrives as a short read. A device that
 * never came up takes the same path — no frames, so the block zero-fills and
 * its starve counter climbs, which is exactly what "the RX side is not
 * clocking" already means. */
void SYNTH_RENDER_IRAM capture_one(int pipe, int slot, size_t got,
                                  bool invert_r) {
    int16_t* buf = s_cap[pipe][slot];
    const bool starved = (got < SYNTH_BLOCK_SIZE);
    if (starved) {
        memset(buf + got * 2, 0,
               (SYNTH_BLOCK_SIZE - got) * 2 * sizeof(int16_t));
    }

#if SYNTH_LINE_IN_INVERT_R
    /* INT16_MIN has no positive counterpart, so it clamps to INT16_MAX; one
     * LSB at full scale, on a sample already at the converter's rail. */
    if (invert_r) {
        for (size_t i = 0; i < got * 2; i += 2) {
            const int16_t r = buf[i + 1];
            buf[i + 1] = (r == INT16_MIN) ? INT16_MAX : (int16_t)(-r);
        }
    }
#else
    (void)invert_r;
#endif

    /* Integer max-abs over what actually arrived: one compare per sample and
     * no float work in the metering path. */
    int32_t pk_l = 0, pk_r = 0, pk_m = 0;
    for (size_t i = 0; i < got * 2; i += 2) {
        const int32_t al = (buf[i] < 0) ? -(int32_t)buf[i] : (int32_t)buf[i];
        const int32_t ar =
            (buf[i + 1] < 0) ? -(int32_t)buf[i + 1] : (int32_t)buf[i + 1];
        if (al > pk_l) pk_l = al;
        if (ar > pk_r) pk_r = ar;
        /* The same fold the looper's mono take applies, so the meter answers
         * the question a per-channel peak cannot: is there anything left of
         * this input once L and R are summed? */
        const int32_t m = ((int32_t)buf[i] + (int32_t)buf[i + 1]) / 2;
        const int32_t am = (m < 0) ? -m : m;
        if (am > pk_m) pk_m = am;
    }
    const float peak_l = (float)pk_l * kInScale;
    const float peak_r = (float)pk_r * kInScale;
    const float peak_m = (float)pk_m * kInScale;

    portENTER_CRITICAL(&s_stats_mux);
    if (peak_l > s_stats.in_peak_l[slot]) s_stats.in_peak_l[slot] = peak_l;
    if (peak_r > s_stats.in_peak_r[slot]) s_stats.in_peak_r[slot] = peak_r;
    if (peak_m > s_stats.in_peak_mono[slot]) {
        s_stats.in_peak_mono[slot] = peak_m;
    }
    if (starved) s_stats.in_starves[slot]++;
    portEXIT_CRITICAL(&s_stats_mux);
}

/* Capture every compiled input device and advance the route and device gains.
 * Called from the audio task after the cycle counter starts (so the cost lands
 * honestly in dsp_load_pct) and before the render chain (so the block just
 * captured is the one the chain mixes).
 *
 * Every device is read every block, including one `in.source` is not currently
 * selecting, and that is deliberate rather than lazy. It costs about 0.7% of
 * the block budget per device, and it buys two things: a device that has just
 * been selected hands over *current* audio instead of whatever its DMA ring
 * accumulated while nobody was reading it, and its meters stay live so a
 * microphone can be seen working before anything is switched to it. The
 * alternative saves 0.7% and makes every switch open with a burst of stale
 * sound.
 *
 * Never blocks, and never needs to drain. Both sources read with a zero
 * timeout, and a short read is zero-filled and counted. Latency cannot
 * accumulate either: the driver's RX ISR drops the oldest DMA descriptor once
 * the queue fills, and the read skips forward when the queue is nearly full,
 * so the backlog self-clamps at one or two buffers. Sharing BCLK with the DAC
 * does the rest — capture and playback run off one clock, so there is nothing
 * to resample and nothing to drift. The mic shares it too under
 * OSYNTH_MIC_SHARE_CLOCKS, which is why that is the default; a mic mastering
 * its own pins sits at a fixed phase offset instead, which the DMA ring
 * absorbs the same way it absorbs everything else at the same nominal rate. */
void SYNTH_RENDER_IRAM audio_in_capture(int pipe) {
    if (!s_in_ok) return; /* gains stay at 0; the stages early-out */

    size_t got = 0;
#if SYNTH_ENABLE_LINE_IN
    got = 0;
    if (s_line_ok) {
        (void)audio_source_i2s_read(s_cap[pipe][kSlotLine], SYNTH_BLOCK_SIZE,
                                    &got);
    }
    capture_one(pipe, kSlotLine, got, /*invert_r=*/true);
#endif
#if SYNTH_ENABLE_MIC_IN
    got = 0;
    if (s_mic_ok) {
        (void)audio_source_mic_read(s_cap[pipe][kSlotMic], SYNTH_BLOCK_SIZE,
                                    &got);
    }
    capture_one(pipe, kSlotMic, got, /*invert_r=*/false);
#endif
    (void)got;

    const int route =
        (int)(s_in_route->load(std::memory_order_relaxed) + 0.5f);
    const float g = s_in_gain->load(std::memory_order_relaxed);
    for (int i = 0; i < kInPositions; ++i) {
        s_in_g[i] = osynth::dsp::smooth_lin(s_in_sm[i],
                                            (route == i + 1) ? g : 0.0f);
    }

#if SYNTH_ENABLE_IN_SOURCE_SEL
    /* Device gains. `both` is the only value that leaves two of them non-zero,
     * and the mic carries its own trim because the two devices do not arrive
     * anywhere near each other in level: a line source sits where the ADC was
     * set up to take it, a MEMS mic at conversational distance sits far below
     * full scale. One shared `in.gain` across both would be a control that is
     * wrong for one of them whichever way it is set — which is the whole
     * reason `in.micgain` exists, and why it is registered only where there
     * are two devices for it to sit between. */
    const int sel = (int)(s_in_source->load(std::memory_order_relaxed) + 0.5f);
    const float micg = s_in_micgain->load(std::memory_order_relaxed);
    const bool line_on = (sel == kSelLine || sel == kSelBoth);
    const bool mic_on = (sel == kSelMic || sel == kSelBoth);
    s_dev_g[pipe][kSlotLine] =
        osynth::dsp::smooth_lin(s_dev_sm[kSlotLine], line_on ? 1.0f : 0.0f);
    s_dev_g[pipe][kSlotMic] =
        osynth::dsp::smooth_lin(s_dev_sm[kSlotMic], mic_on ? micg : 0.0f);
#else
    /* One device compiled in: it is the source, always, and its gain is a
     * constant the compiler folds into the mix below. */
    s_dev_g[pipe][0] = 1.0f;
#endif

    /* Published for the heartbeat, from here rather than by reading the
     * parameter again on the other side: what matters is the route the audio
     * task actually acted on and the gains it actually mixed with, which is
     * the pair a re-read cannot vouch for. Same mux as the peaks above — a
     * second short critical section rather than one widened over the capture,
     * so the metering loop stays outside it. */
    portENTER_CRITICAL(&s_stats_mux);
    s_stats.in_route = (uint8_t)((route < 0) ? 0 : (route > 3) ? 0 : route);
    /* The device gains the audio task actually mixed with, which during a
     * crossfade are both non-zero and are the only thing that says so. */
    for (int d = 0; d < kDevCount; ++d) s_stats.in_dev_g[d] = s_dev_g[pipe][d];
    s_stats.in_g[kInMon] = s_in_g[kInMon];
    s_stats.in_g[kInFx] = s_in_g[kInFx];
    s_stats.in_g[kInDry] = s_in_g[kInDry];
    portEXIT_CRITICAL(&s_stats_mux);
}
#endif /* SYNTH_ENABLE_AUDIO_IN */

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
    /* NaN, before anything else, and for the reason f2i16() in synth_line.h
     * spells out: the conversion below saturates a NaN to INT32_MAX, so
     * without this one compare a single NaN sample leaves the box as +full
     * scale on both channels — and because soft_clip() upstream deliberately
     * does not fence non-finite input (a size decision, see the note above
     * it), a NaN that has settled into a recursive filter's state arrives
     * here every sample. That is a DC rail, not a click.
     *
     * This is the fence that note points at. It is a separate copy rather
     * than a call to f2i16() only because the dither has to be added before
     * the conversion; the rule the two implement is the same one, and it
     * should stay the same one. */
    if (v != v) return 0;
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
 * Set by start_common() before either task exists (S45) -- the pipeline has two
 * of them, and neither is a sensible owner of a number the other divides by. */
uint32_t s_cycles_per_block = 0;

#if SYNTH_ENABLE_SPLIT_RENDER
/* The voice stage's load EMA, deliberately not s_stats.dsp_load_pct.
 *
 * The EMA update is a read-modify-write, so two cores sharing one float would
 * lose part of every other sample of it, and taking a lock per block to
 * protect a diagnostic would cost more than the diagnostic is worth. Each core
 * owns its own; audio_io_get_stats() folds them together by taking the worse,
 * which is the one of the two that predicts a dropout. */
float s_load_a_pct = 0.0f;

/* How long the bus stage waits for a block before calling the voice stage
 * stalled.
 *
 * Generous on purpose. A legitimate wait is bounded by how far the voice stage
 * is behind, and the sink's DMA ring absorbs several blocks of that before
 * anything is heard, so a wait long enough to expire here is not a late
 * pipeline but a broken one. Short enough, still, that the counter moves while
 * someone is watching the heartbeat rather than long after. */
constexpr TickType_t kStallTicks = pdMS_TO_TICKS(20);
#endif

/* One block's cost, charged to `ema`.
 *
 * DSP load = cycles to produce the block vs its real-time budget, smoothed
 * with a ~130 ms EMA. A block over budget missed its deadline and counts as an
 * underrun. Everything except the EMA is shared between the cores, which is
 * why the mux is taken here rather than at either call site: which core is
 * calling changes only which average moves.
 *
 * `count_block` is false for the voice stage. blocks_rendered counts what
 * reached the sink, and that stage is one hand-off short of it -- counting
 * both would double every figure derived from it. */
void SYNTH_RENDER_IRAM account_load(uint32_t busy, float* ema,
                                    bool count_block) {
    const float inst_pct = 100.0f * (float)busy / (float)s_cycles_per_block;
    portENTER_CRITICAL(&s_stats_mux);
    *ema += 0.01f * (inst_pct - *ema);
    if (inst_pct > s_stats.dsp_load_peak_pct) {
        s_stats.dsp_load_peak_pct = inst_pct;
    }
    if (busy > s_cycles_per_block) s_stats.underruns++;
    if (count_block) s_stats.blocks_rendered++;
    portEXIT_CRITICAL(&s_stats_mux);
}

/* Master volume, the int16 conversion and the output meters: the tail every
 * render path shares. Factored out when the pipeline gave it a second caller
 * (S45), so the single-core and two-core paths cannot drift apart on the one
 * piece of arithmetic that decides what actually leaves the box. */
void SYNTH_RENDER_IRAM convert_block(const float* bl, const float* br) {
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
        const float l = bl[i] * gain;
        const float r = br[i] * gain;
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
     * be heard -- persist.c writes NVS in one. One compare per block, on a
     * peak that was computed anyway. */
    if (peak > kQuietPeak) {
        s_quiet_blocks.store(0, std::memory_order_relaxed);
    } else {
        const uint32_t n = s_quiet_blocks.load(std::memory_order_relaxed);
        if (n < kQuietBlockCap) {
            s_quiet_blocks.store(n + 1, std::memory_order_relaxed);
        }
    }
}

/* Hand the converted block to the tap and then to the primary sink.
 *
 * Separate from convert_block() above because the DSP-load window closes
 * between the two: blocking on the sink's DMA is how the audio task waits for
 * real time to catch up, and charging that to the render would report a
 * perfectly healthy synth at 100%.
 *
 * Tap first, primary second. The primary's write is what consumes the block
 * period, so feeding the tap ahead of it keeps the tap's deposits at a fixed
 * phase against the audio clock instead of trailing a DMA wait of varying
 * length. Its result is deliberately ignored: a tap is best-effort, and
 * letting it influence anything here would hand a USB host partial control of
 * the DAC's timing. */
void SYNTH_RENDER_IRAM emit_block(void) {
    if (s_tap != nullptr) {
        (void)s_tap->write(s_out, SYNTH_BLOCK_SIZE);
    }

    /* Blocking write: the sink's DMA (or timer) is the real clock. */
    const esp_err_t err = s_sink->write(s_out, SYNTH_BLOCK_SIZE);
    if (SYNTH_UNLIKELY(err != ESP_OK)) {
        /* Counted, not logged -- see sink_errors in audio_io.h. The
         * heartbeat prints it, with the name this err resolves to. */
        portENTER_CRITICAL(&s_stats_mux);
        s_stats.sink_errors++;
        s_stats.sink_last_err = (int32_t)err;
        portEXIT_CRITICAL(&s_stats_mux);
    }
}

/* The single-core chain: capture, render the whole thing, convert, emit. */
void SYNTH_RENDER_IRAM audio_task(void*) {
    ESP_LOGI(TAG, "audio task up: core %d, %d Hz, block %d (%.2f ms), sink %s",
             xPortGetCoreID(), SYNTH_SAMPLE_RATE, SYNTH_BLOCK_SIZE,
             1000.0f * SYNTH_BLOCK_SIZE / SYNTH_SAMPLE_RATE, s_sink->name);

    for (;;) {
        const uint32_t c0 = esp_cpu_get_cycle_count();

#if SYNTH_ENABLE_AUDIO_IN
        /* Inside the c0 window on purpose: the input is not free and the DSP
         * meter should say so. As early in the block as possible, too -- it
         * gives the RX DMA the longest run at refilling before the next
         * read. */
        audio_in_capture(0);
#endif

        memset(s_buf_l[0], 0, sizeof(s_buf_l[0]));
        memset(s_buf_r[0], 0, sizeof(s_buf_r[0]));

        if (s_render != nullptr) {
            s_render(s_buf_l[0], s_buf_r[0], SYNTH_BLOCK_SIZE, s_render_ctx);
        }

        convert_block(s_buf_l[0], s_buf_r[0]);
        account_load(esp_cpu_get_cycle_count() - c0, &s_stats.dsp_load_pct,
                     /*count_block=*/true);
        emit_block();
    }
}

#if SYNTH_ENABLE_SPLIT_RENDER

/* Stage A, on kVoiceCore: the voice manager, working one block ahead.
 *
 * Owns no clock and no sink, and that is the load-bearing part. It runs only
 * when the bus stage hands it a free slot and blocks the rest of the time, so
 * the sink still paces both cores through it. A second free-running audio task
 * would be a second clock, which is the one thing a pipeline must not have.
 *
 * What it does *not* do is yield when it is over budget: a stage that runs long
 * finds its next credit already waiting and starts again immediately, at 100%
 * of its core for as long as the patch is too expensive. That is why it is on
 * the empty core — see kVoiceCore for what it cost to learn.
 *
 * The notification pair is the whole of the synchronisation, and it is the
 * memory barrier as well: the bus stage writes the slot it is releasing and
 * publishes the capture slot *before* the give, and a FreeRTOS notify is a full
 * barrier on both sides. Nothing here needs an atomic of its own, because there
 * is no moment when the two cores touch the same buffer at all. */
void SYNTH_RENDER_IRAM audio_task_voices(void*) {
    ESP_LOGI(TAG, "voice stage up: core %d, one block ahead of the sink",
             xPortGetCoreID());

    int prod = 0;
    for (;;) {
        /* Unbounded, unlike the bus stage's wait below. With no slot to fill
         * there is nothing useful this task could do with the CPU, and the bus
         * stage is the only thing that can ever hand one over -- a timeout here
         * would just spin a high-priority task against whatever is already
         * keeping its core busy, which is the situation it would be waking up
         * to diagnose. */
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        const uint32_t c0 = esp_cpu_get_cycle_count();
        memset(s_buf_l[prod], 0, sizeof(s_buf_l[prod]));
        memset(s_buf_r[prod], 0, sizeof(s_buf_r[prod]));
        if (s_render_a != nullptr) {
            s_render_a(s_buf_l[prod], s_buf_r[prod], SYNTH_BLOCK_SIZE,
                       s_render_ctx);
        }
        account_load(esp_cpu_get_cycle_count() - c0, &s_load_a_pct,
                     /*count_block=*/false);

        xTaskNotifyGive(s_task);
        prod ^= 1;
    }
}

/* Stage B, on kBusCore: the capture, everything downstream of the voices, and
 * the sink that paces the pair.
 *
 * The light half, and on the core everything else in this firmware is pinned
 * to. It earns its place there by blocking on the sink's DMA every block, which
 * is what hands that core back to the BLE host and the sequencer clock on a
 * schedule rather than whenever the patch happens to allow it.
 *
 * The block indices are worth following once, because their agreement is what
 * keeps the two cores off each other's buffers. The voice stage's iteration i
 * fills bus slot i&1; this task's iteration i drains the same one, having been
 * woken by the give at the end of that one. The two therefore overlap by
 * exactly one iteration -- the voice stage is building block N+1 into the slot
 * this task finished with last time, while this task finishes block N -- and
 * the capture slot follows the same index, which is why the voice stage reading
 * `cons ^ 1` is reading the one this task is not writing. */
void SYNTH_RENDER_IRAM audio_task_pipe(void*) {
    ESP_LOGI(TAG, "audio task up: core %d, %d Hz, block %d (%.2f ms), sink %s",
             xPortGetCoreID(), SYNTH_SAMPLE_RATE, SYNTH_BLOCK_SIZE,
             1000.0f * SYNTH_BLOCK_SIZE / SYNTH_SAMPLE_RATE, s_sink->name);

    int cons = 0;
    for (;;) {
        /* Bounded, so a voice stage that has stopped delivering is *counted*
         * rather than merely inaudible. Nothing is written to the sink during
         * this wait, so its DMA drains and the output goes quiet -- and
         * pipe_stalls is then the only thing that separates a starved pipeline
         * from a patch that is simply not making a sound.
         *
         * Note that an over-budget voice stage does not land here: it delivers,
         * only late, and the DMA absorbs that until it cannot. That case shows
         * up as underruns and a dsp_load_pct near 100, not as a stall. */
        while (ulTaskNotifyTake(pdTRUE, kStallTicks) == 0) {
            portENTER_CRITICAL(&s_stats_mux);
            /* Not before the first block has ever landed. The voice stage is
             * created with one credit already given and starts within
             * microseconds, but the scheduler is free to run this task first,
             * and a stall counted there would be a fault report for a pipeline
             * that is merely still starting up. */
            if (s_stats.blocks_rendered != 0) s_stats.pipe_stalls++;
            portEXIT_CRITICAL(&s_stats_mux);
        }

        const uint32_t c0 = esp_cpu_get_cycle_count();

#if SYNTH_ENABLE_AUDIO_IN
        /* Before the release below, never after: the give is what makes this
         * visible to the voice stage, and what it publishes is the slot that
         * stage may read -- the opposite one to the capture two lines further
         * down. Getting this order wrong is the single way the two cores could
         * end up on one capture buffer. */
        publish_in_slots(cons);
#endif
        /* Released here rather than at the end of the block, so the voice
         * stage gets this stage's own work *and* the sink's DMA wait to render
         * the next block in. That is where the pipeline's headroom actually
         * comes from. The slot it takes is the one this task finished with last
         * time round, which nothing has touched since. */
        xTaskNotifyGive(s_task_a);

#if SYNTH_ENABLE_AUDIO_IN
        /* Inside the c0 window on purpose: the input is not free and the DSP
         * meter should say so. As early in the block as possible, too -- it
         * gives the RX DMA the longest run at refilling before the next
         * read. */
        audio_in_capture(cons);
#endif

        /* No memset: this slot arrives carrying the voice stage's block, which
         * is exactly what the rest of the chain is supposed to add to. */
        if (s_render != nullptr) {
            s_render(s_buf_l[cons], s_buf_r[cons], SYNTH_BLOCK_SIZE,
                     s_render_ctx);
        }

        convert_block(s_buf_l[cons], s_buf_r[cons]);
        account_load(esp_cpu_get_cycle_count() - c0, &s_stats.dsp_load_pct,
                     /*count_block=*/true);
        emit_block();
        cons ^= 1;
    }
}

#endif /* SYNTH_ENABLE_SPLIT_RENDER */

} // namespace

/* Everything both start paths do: pick and start the sink, bring the input
 * devices up, attach the tap, name the result, and work out the block budget
 * the meters divide by. Split out of audio_io_start() when the pipeline gave
 * it a sibling (S45) -- all either caller adds on top is which tasks to
 * create. */
static esp_err_t start_common(void* ctx) {
    s_render_ctx = ctx;
    s_master_volume =
        osynth::ParamStore::instance().valuePtr(osynth::PID_MASTER_VOLUME);
    if (s_master_volume == nullptr) {
        ESP_LOGW(TAG, "master.volume not registered; running at unity gain");
    }

#if defined(SYNTH_TARGET_HOST)
    /* First, and unconditionally: on a host there is exactly one output and
     * none of the peripheral capabilities below exist to choose between. */
    s_sink = audio_sink_host();
#elif SYNTH_ENABLE_I2S_DAC
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

#if SYNTH_ENABLE_AUDIO_IN
    /* The input params exist whatever the hardware did. If the sink fell back
     * to the null sink, or a device was refused, they stay registered and
     * settable — they simply do nothing, which is a better answer for the app
     * than a control that vanishes depending on how boot went. */
    s_in_route = osynth::ParamStore::instance().valuePtr(osynth::PID_LINE_IN_ROUTE);
    s_in_gain = osynth::ParamStore::instance().valuePtr(osynth::PID_LINE_IN_GAIN);
    bool params_ok = (s_in_route != nullptr && s_in_gain != nullptr);

#if SYNTH_ENABLE_LINE_IN
#if defined(SYNTH_TARGET_HOST)
    /* No port to belong to. The capture device is opened by the sink itself --
     * one duplex device, so playback and capture share a clock the way the I2S
     * port's two halves do -- and ready() is the whole answer. */
    s_line_ok = audio_source_i2s_ready();
    if (s_line_ok) {
        ESP_LOGI(TAG, "audio input ready (stereo, duplex with the output)");
    } else {
        ESP_LOGW(TAG, "audio input inactive: no capture device");
    }
#else
    /* The line input belongs to the I2S port, so it only exists if that port
     * is what actually came up. */
    s_line_ok = (s_sink == audio_sink_i2s()) && audio_source_i2s_ready();
    if (s_line_ok) {
        ESP_LOGI(TAG, "line input ready (stereo, sample-locked to the DAC)");
    } else {
        ESP_LOGW(TAG, "line input inactive: sink %s, rx %d", s_sink->name,
                 (int)audio_source_i2s_ready());
    }
#endif
#endif

#if SYNTH_ENABLE_MIC_IN
    /* After the sink, and that ordering is load-bearing: with shared clocks
     * the mic is a slave on the output port's BCLK and WS, so it needs the
     * master already driving those pins. Its own port, though, so it does not
     * care whether the *sink* is the I2S one — a mic still works on a build
     * whose DAC fell back to silence, which is the case a bench session most
     * wants to keep. */
    (void)audio_source_mic_start();
    s_mic_ok = audio_source_mic_ready();
#endif

#if SYNTH_ENABLE_IN_SOURCE_SEL
    s_in_source =
        osynth::ParamStore::instance().valuePtr(osynth::PID_LINE_IN_SOURCE);
    s_in_micgain =
        osynth::ParamStore::instance().valuePtr(osynth::PID_LINE_IN_MICGAIN);
    /* Both are required together: the capture reads them unconditionally on
     * every block, and half a selector is worse than none. Missing either
     * leaves the input off rather than dereferencing a null in the audio
     * task — a fault that would present as a crash on the first block rather
     * than as the missing control it actually is. */
    params_ok = params_ok && s_in_source != nullptr && s_in_micgain != nullptr;
#endif

    s_in_ok = params_ok && (s_line_ok || s_mic_ok);
    ESP_LOGI(TAG, "audio input: line %d, mic %d", (int)s_line_ok,
             (int)s_mic_ok);
#if SYNTH_ENABLE_IN_SOURCE_SEL
    /* Whether the *selector* exists, separately from whether the devices do.
     *
     * These are different failures with one symptom — "I cannot find the mic
     * in the app" — and nothing else distinguishes them. Two live devices and
     * no `in.source` means the parameter did not register and the app has
     * nothing to draw; a registered `in.source` means the firmware published
     * it and the question moved to the app, which discovers parameters once
     * per connection and will not notice a new one until it reconnects. This
     * line is which of those two it is, without a BLE trace. */
    ESP_LOGI(TAG, "audio input: in.source %s",
             s_in_source != nullptr
                 ? "registered (reconnect the app if it is not listed)"
                 : "NOT REGISTERED — the app cannot offer a source selector");
#else
    ESP_LOGI(TAG,
             "audio input: no in.source selector in this build (needs both "
             "line and mic compiled in)");
#endif
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

    /* Before any task exists, because both of them divide by it and neither
     * owns it. The clamp is not defensiveness about the ROM call so much as
     * about the division in account_load(), which runs on every block on both
     * cores and has no other guard. */
    s_cycles_per_block =
        (uint32_t)((uint64_t)esp_rom_get_cpu_ticks_per_us() * 1000000u *
                   SYNTH_BLOCK_SIZE / SYNTH_SAMPLE_RATE);
    if (s_cycles_per_block == 0) s_cycles_per_block = 1;

    return ESP_OK;
}

esp_err_t audio_io_start(audio_render_fn render, void* ctx) {
    if (s_task != nullptr) return ESP_ERR_INVALID_STATE;

    s_render = render;
    const esp_err_t err = start_common(ctx);
    if (err != ESP_OK) return err;

    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "audio", 6144, nullptr,
                                            configMAX_PRIORITIES - 2, &s_task, 1);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

#if SYNTH_ENABLE_SPLIT_RENDER
esp_err_t audio_io_start_split(audio_render_fn stage_a, audio_render_fn stage_b,
                               void* ctx) {
    if (s_task != nullptr || s_task_a != nullptr) return ESP_ERR_INVALID_STATE;

    s_render_a = stage_a;
    s_render = stage_b;
    const esp_err_t err = start_common(ctx);
    if (err != ESP_OK) return err;

    /* Both tasks exist before either is allowed to run a block, and the credit
     * that starts the pipeline is given last. The order matters twice over.
     *
     * The voice stage is created at a priority that preempts whatever is on its
     * core, so it runs the instant it exists — and the first thing it would do
     * after a block is notify `s_task`, which the second create below has not
     * filled in yet. Priming it here rather than there is what keeps that
     * handle from being read before it is written; the task blocks immediately
     * on a notification nobody has sent, which costs nothing and has no
     * deadline to miss.
     *
     * The bus stage waking first is harmless in the other direction: it waits,
     * times out, and declines to count a stall because no block has ever landed
     * yet (see audio_task_pipe).
     *
     * Same priority on both, for the same reason: on each core the stage has
     * to outrank the control tasks, so a busy UI or a BLE host is starved of
     * CPU ahead of the audio deadline rather than alongside it. */
    BaseType_t ok = xTaskCreatePinnedToCore(audio_task_voices, "audio_voi",
                                            6144, nullptr,
                                            configMAX_PRIORITIES - 2, &s_task_a,
                                            kVoiceCore);
    if (ok != pdPASS) return ESP_FAIL;

    ok = xTaskCreatePinnedToCore(audio_task_pipe, "audio", 6144, nullptr,
                                 configMAX_PRIORITIES - 2, &s_task, kBusCore);
    if (ok != pdPASS) return ESP_FAIL;

    xTaskNotifyGive(s_task_a); /* the pipeline's first free slot */

    ESP_LOGI(TAG,
             "render pipeline: voices on core %d, fx/looper + sink on core %d, "
             "+1 block (%.2f ms) of output latency",
             (int)kVoiceCore, (int)kBusCore,
             1000.0f * SYNTH_BLOCK_SIZE / SYNTH_SAMPLE_RATE);
    return ESP_OK;
}
#endif

#if SYNTH_ENABLE_AUDIO_IN

/* Mix the input into the bus at one of the three positions.
 *
 * One pass per *device*, summed in float. `both` therefore costs a second
 * SIMD pass over the block and nothing else — no extra buffer at mix time, no
 * intermediate sum, and no int16 headroom lost, which a capture-side sum would
 * have cost whether or not the second device was in use.
 *
 * The per-device compare is what keeps a single-source build at exactly the
 * cost it had before `both` existed: an unselected device's gain sits at 0 and
 * its pass is skipped, the same kSilent gate the FX bus and the route
 * positions already use. */
inline void SYNTH_RENDER_IRAM mix_in(int pos, float* l, float* r,
                                     size_t frames) {
    const float g = s_in_g[pos];
    if (g <= kInSilent) return;
    const int p = in_slot();
    for (int d = 0; d < kDevCount; ++d) {
        const float dg = s_dev_g[p][d];
        if (dg <= kInSilent) continue;
        osynth::dsp::simd_mix_i16lr_f32(s_cap[p][d], g * dg * kInScale, l, r,
                                        frames);
    }
}

void SYNTH_RENDER_IRAM audio_io_line_in_fx(float* l, float* r, size_t frames) {
    mix_in(kInFx, l, r, frames);
}

void SYNTH_RENDER_IRAM audio_io_line_in_dry(float* l, float* r, size_t frames) {
    mix_in(kInDry, l, r, frames);
}

void SYNTH_RENDER_IRAM audio_io_line_in_mon(float* l, float* r, size_t frames) {
    mix_in(kInMon, l, r, frames);
}

bool SYNTH_RENDER_IRAM audio_io_in_mono(float* dst, size_t frames) {
    if (!s_in_ok || dst == nullptr) return false;
    if (frames > SYNTH_BLOCK_SIZE) frames = SYNTH_BLOCK_SIZE;

    /* One pass per selected device, same shape and same gate as mix_in(): an
     * unselected device's trim sits at 0 and its pass is skipped, so a
     * single-source build costs exactly one pass. The first writer assigns and
     * the rest accumulate, which saves clearing the buffer in the common case
     * of exactly one device.
     *
     * The 0.5 folds L+R to mono at unity: a mono source arriving on both
     * channels comes back at its own level rather than 6 dB up, and the
     * ES8311 path is exactly that case (one mic, duplicated across the pair). */
    bool any = false;
    const int p = in_slot();
    for (int d = 0; d < kDevCount; ++d) {
        const float dg = s_dev_g[p][d];
        if (dg <= kInSilent) continue;
        const int16_t* __restrict__ c = s_cap[p][d];
        const float k = dg * kInScale * 0.5f;
        if (!any) {
            for (size_t i = 0; i < frames; ++i) {
                dst[i] = ((float)c[2 * i] + (float)c[2 * i + 1]) * k;
            }
            any = true;
        } else {
            for (size_t i = 0; i < frames; ++i) {
                dst[i] += ((float)c[2 * i] + (float)c[2 * i + 1]) * k;
            }
        }
    }
    /* `in.source` selecting nothing is not the same as having no input: the
     * caller still wants a block, and silence is the honest one. */
    if (!any) memset(dst, 0, frames * sizeof(float));
    return true;
}

/* mix_in() into a cleared pair of buffers rather than a second copy of its
 * arithmetic: the value of this function is that it agrees with what was
 * actually mixed, and two expressions that have to agree are one expression
 * with a copy of it somewhere else. */
bool SYNTH_RENDER_IRAM audio_io_in_fx_block(float* l, float* r, size_t frames) {
    if (!s_in_ok || l == nullptr || r == nullptr) return false;
    if (frames > SYNTH_BLOCK_SIZE) frames = SYNTH_BLOCK_SIZE;
    /* Nothing was added at this position, so there is nothing to correct.
     * Reported rather than answered with a block of zeros: a caller that
     * corrected a silent block would still be running its filters on it, and
     * would have no way to tell "the input is quiet" from "the input is not
     * here". */
    if (s_in_g[kInFx] <= kInSilent) return false;
    memset(l, 0, frames * sizeof(float));
    memset(r, 0, frames * sizeof(float));
    mix_in(kInFx, l, r, frames);
    return true;
}

const int16_t* SYNTH_RENDER_IRAM audio_io_line_in_block(void) {
    /* NULL rather than a buffer of stale samples when the capture is not
     * running: s_cap is only refilled by audio_in_capture(), which returns
     * immediately in that case, so what is in it is whatever was there at
     * boot. A caller that gets NULL renders silence and knows why.
     *
     * Returns the *line* device's block wherever there is one, and the mic's
     * only on a build with no line input — deliberately not following
     * `in.source`, and deliberately not summing.
     *
     * A graph patch asks for "the input" and stores a node index, not a
     * device: making this follow the selector would mean a saved patch that
     * sounds different depending on what someone later plugged into a jack it
     * never mentions. Summing would be worse still, since the sum's level
     * moves with `in.micgain` — a parameter that patch has no idea exists.
     * The bus mix points are where two devices are heard at once; a node is
     * one wire, and this is the end of it. */
#if SYNTH_ENABLE_LINE_IN
    return s_in_ok ? s_cap[in_slot()][kSlotLine] : nullptr;
#else
    return s_in_ok ? s_cap[in_slot()][kSlotMic] : nullptr;
#endif
}

#else /* empty bodies so render_chain() needs no #if of its own */

void audio_io_line_in_fx(float*, float*, size_t) {}
void audio_io_line_in_dry(float*, float*, size_t) {}
void audio_io_line_in_mon(float*, float*, size_t) {}
const int16_t* audio_io_line_in_block(void) { return nullptr; }
bool audio_io_in_mono(float*, size_t) { return false; }
bool audio_io_in_fx_block(float*, float*, size_t) { return false; }

#endif /* SYNTH_ENABLE_AUDIO_IN */

void SYNTH_RENDER_IRAM audio_io_report_stage_voices(uint32_t voices_cycles) {
    if (s_cycles_per_block == 0) return;
    const float inv = 100.0f / (float)s_cycles_per_block;
    /* Same ~130 ms EMA as dsp_load_pct so the three read against it. */
    s_stats.stage_voices_pct +=
        0.01f * ((float)voices_cycles * inv - s_stats.stage_voices_pct);
}

void SYNTH_RENDER_IRAM audio_io_report_stage_fx_loop(uint32_t fx_cycles,
                                                     uint32_t loop_cycles) {
    if (s_cycles_per_block == 0) return;
    const float inv = 100.0f / (float)s_cycles_per_block;
    s_stats.stage_fx_pct +=
        0.01f * ((float)fx_cycles * inv - s_stats.stage_fx_pct);
    s_stats.stage_loop_pct +=
        0.01f * ((float)loop_cycles * inv - s_stats.stage_loop_pct);
}

/* The single-core chain measures all three in one place, so it reports them
 * that way. Expressed as the two halves rather than beside them: three EMAs
 * updated in two places would be three chances for the split and single paths
 * to disagree about what a percentage means. */
void SYNTH_RENDER_IRAM audio_io_report_stages(uint32_t voices_cycles,
                                              uint32_t fx_cycles,
                                              uint32_t loop_cycles) {
    audio_io_report_stage_voices(voices_cycles);
    audio_io_report_stage_fx_loop(fx_cycles, loop_cycles);
}

const char* audio_io_sink_name(void) { return s_sink_name; }

uint32_t audio_io_quiet_ms(void) {
    const uint32_t blocks = s_quiet_blocks.load(std::memory_order_relaxed);
    return (uint32_t)((uint64_t)blocks * SYNTH_BLOCK_SIZE * 1000u /
                      SYNTH_SAMPLE_RATE);
}

void audio_io_mic_probe_pads(void) {
#if SYNTH_ENABLE_MIC_IN
    if (s_mic_ok) audio_source_mic_probe_pads("codec up");
#endif
}

void audio_io_get_stats(audio_io_stats_t* out) {
    if (out == nullptr) return;
    /* The peaks are read-and-reset windows, so the copy and the clear have to
     * be one operation as far as the audio task is concerned — otherwise a
     * block that maxes in between is reported by neither window. */
    portENTER_CRITICAL(&s_stats_mux);
    *out = s_stats;
#if SYNTH_ENABLE_SPLIT_RENDER
    /* The worse of the two stages, not their sum -- see dsp_load_pct in
     * audio_io.h. Folded in on the way out rather than kept folded, so each
     * stage still owns exactly one float and neither has to read the other's. */
    if (s_load_a_pct > out->dsp_load_pct) out->dsp_load_pct = s_load_a_pct;
#endif
    s_stats.dsp_load_peak_pct = 0.0f;
    s_stats.out_peak = 0.0f;
    for (int d = 0; d < 2; ++d) {
        s_stats.in_peak_l[d] = 0.0f;
        s_stats.in_peak_r[d] = 0.0f;
        s_stats.in_peak_mono[d] = 0.0f;
    }
    portEXIT_CRITICAL(&s_stats_mux);

    /* Outside the critical section on purpose: it is its own read-and-reset
     * pair of atomics in source_mic.cpp, and it does not have to be consistent
     * with the peaks above — it answers a question about the pin, not about
     * the block. */
    out->mic_raw_or[0] = 0;
    out->mic_raw_or[1] = 0;
#if SYNTH_ENABLE_MIC_IN
    audio_source_mic_raw_take(&out->mic_raw_or[0], &out->mic_raw_or[1]);
#endif
}
