"""S45: audio_io.cpp -- the two-core render pipeline.

Four groups of change:
  1. the bus and the capture gain a slot dimension (one slot without the
     pipeline, so the single-core build indexes a constant and the compiler
     folds it away),
  2. the per-block tail of the audio task is factored into account_load /
     convert_block / emit_block so both task loops share one definition,
  3. audio_io_start()'s body becomes start_common(), with two thin public
     entry points on top of it,
  4. the stage meters split in two, one per core.

Run from the repo root: python tools/s45_split_render/04_audio_io_cpp.py
Idempotent: it exits early once audio_io_start_split is present.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _edit import Editor

ed = Editor("components/audio_io/audio_io.cpp", skip_if="audio_io_start_split")
sub = ed.sub

# --------------------------------------------------------------------------
# 1. globals: the slot dimension, the second render callback, the second task
# --------------------------------------------------------------------------
sub(
    """const audio_sink_t* s_tap = nullptr;
char s_sink_name[24] = "none";""",
    """const audio_sink_t* s_tap = nullptr;
char s_sink_name[24] = "none";
#if SYNTH_ENABLE_SPLIT_RENDER
/* The voice stage and the core it runs on (S45). Null on a build without the
 * pipeline, where `s_render` above is the whole chain rather than its tail. */
audio_render_fn s_render_a = nullptr;
TaskHandle_t s_task_a = nullptr;
#endif""",
)

sub(
    """float s_buf_l[SYNTH_BLOCK_SIZE];
float s_buf_r[SYNTH_BLOCK_SIZE];
int16_t s_out[SYNTH_BLOCK_SIZE * 2]; /* interleaved L/R for the sinks */""",
    """/* Bus slots, and the one number the whole pipeline is expressed in.
 *
 * One slot without it: the same single buffer pair that has always been here,
 * reached through an index that is a compile-time constant, so the generated
 * code for a single-core build is what it was before this existed. Two with
 * it: core 0 fills one while core 1 drains the other, and they swap every
 * block.
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

float s_buf_l[kSlots][SYNTH_BLOCK_SIZE];
float s_buf_r[kSlots][SYNTH_BLOCK_SIZE];
int16_t s_out[SYNTH_BLOCK_SIZE * 2]; /* interleaved L/R for the sinks */""",
)

# --------------------------------------------------------------------------
# 2. the capture: per-slot buffers and per-slot device gains
# --------------------------------------------------------------------------
sub(
    """int16_t s_cap[kDevCount][SYNTH_BLOCK_SIZE * 2];""",
    """/* Per slot as well as per device (S45): core 1 fills one slot while core 0's
 * engines read the other through audio_io_in_mono() and
 * audio_io_line_in_block(). Without the pipeline kSlots is 1 and this is the
 * same 256 bytes per device it always was. */
int16_t s_cap[kSlots][kDevCount][SYNTH_BLOCK_SIZE * 2];

/* Which capture slot each core is allowed to read this block, indexed by core
 * id. Published by core 1 before it releases the voice stage, so the
 * notification that starts core 0 is also the barrier that makes this visible
 * to it -- there is deliberately no atomic here, because there is no moment
 * when the two cores are looking at it without one of those in between.
 *
 * The whole point is that a caller deep inside an engine does not have to know
 * which core it is on. in_slot() answers that from where it is standing, and
 * on a build without the pipeline it answers 0 and disappears. */
#if SYNTH_ENABLE_SPLIT_RENDER
volatile int s_in_slot[2];

inline int SYNTH_RENDER_IRAM in_slot(void) {
    return s_in_slot[xPortGetCoreID()];
}

/* `fresh` is the slot core 1 is about to capture into; core 0 therefore gets
 * the other one, which is the block captured one period earlier and which
 * nothing will write while it reads. */
inline void SYNTH_RENDER_IRAM publish_in_slots(int fresh) {
    s_in_slot[1] = fresh;
    s_in_slot[0] = fresh ^ 1;
}
#else
inline int SYNTH_RENDER_IRAM in_slot(void) { return 0; }
inline void SYNTH_RENDER_IRAM publish_in_slots(int) {}
#endif""",
)

sub(
    """osynth::dsp::Smooth s_dev_sm[kDevCount];
float s_dev_g[kDevCount] = {};""",
    """osynth::dsp::Smooth s_dev_sm[kDevCount];
/* Per slot, because audio_io_in_mono() applies these and is called from both
 * cores. The smoothers beside them are not: they are advanced once per block
 * by the capture, which only ever runs on core 1, and what travels with the
 * block is the value they landed on rather than the state that produced it.
 *
 * s_in_g below is single for the same reason read the other way round -- every
 * one of its readers (the three mix stages, audio_io_in_fx_block()) is on core
 * 1 with the capture, so there is no second view of it to keep consistent. */
float s_dev_g[kSlots][kDevCount] = {};""",
)

# --------------------------------------------------------------------------
# 3. capture_one / audio_in_capture take the pipeline slot
# --------------------------------------------------------------------------
sub(
    """void SYNTH_RENDER_IRAM capture_one(int slot, size_t got, bool invert_r) {
    int16_t* buf = s_cap[slot];""",
    """void SYNTH_RENDER_IRAM capture_one(int pipe, int slot, size_t got,
                                  bool invert_r) {
    int16_t* buf = s_cap[pipe][slot];""",
)

sub(
    """void SYNTH_RENDER_IRAM audio_in_capture(void) {
    if (!s_in_ok) return; /* gains stay at 0; the stages early-out */

    size_t got = 0;
#if SYNTH_ENABLE_LINE_IN
    got = 0;
    if (s_line_ok) {
        (void)audio_source_i2s_read(s_cap[kSlotLine], SYNTH_BLOCK_SIZE, &got);
    }
    capture_one(kSlotLine, got, /*invert_r=*/true);
#endif
#if SYNTH_ENABLE_MIC_IN
    got = 0;
    if (s_mic_ok) {
        (void)audio_source_mic_read(s_cap[kSlotMic], SYNTH_BLOCK_SIZE, &got);
    }
    capture_one(kSlotMic, got, /*invert_r=*/false);
#endif
    (void)got;""",
    """void SYNTH_RENDER_IRAM audio_in_capture(int pipe) {
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
    (void)got;""",
)

sub(
    """    s_dev_g[kSlotLine] =
        osynth::dsp::smooth_lin(s_dev_sm[kSlotLine], line_on ? 1.0f : 0.0f);
    s_dev_g[kSlotMic] =
        osynth::dsp::smooth_lin(s_dev_sm[kSlotMic], mic_on ? micg : 0.0f);
#else""",
    """    s_dev_g[pipe][kSlotLine] =
        osynth::dsp::smooth_lin(s_dev_sm[kSlotLine], line_on ? 1.0f : 0.0f);
    s_dev_g[pipe][kSlotMic] =
        osynth::dsp::smooth_lin(s_dev_sm[kSlotMic], mic_on ? micg : 0.0f);
#else""",
)

sub(
    """    s_dev_g[0] = 1.0f;
#endif""",
    """    s_dev_g[pipe][0] = 1.0f;
#endif""",
)

sub(
    """    for (int d = 0; d < kDevCount; ++d) s_stats.in_dev_g[d] = s_dev_g[d];""",
    """    for (int d = 0; d < kDevCount; ++d) s_stats.in_dev_g[d] = s_dev_g[pipe][d];""",
)

# --------------------------------------------------------------------------
# 4. the readers pick up the slot
# --------------------------------------------------------------------------
sub(
    """    const float g = s_in_g[pos];
    if (g <= kInSilent) return;
    for (int d = 0; d < kDevCount; ++d) {
        const float dg = s_dev_g[d];
        if (dg <= kInSilent) continue;
        osynth::dsp::simd_mix_i16lr_f32(s_cap[d], g * dg * kInScale, l, r,
                                        frames);
    }""",
    """    const float g = s_in_g[pos];
    if (g <= kInSilent) return;
    const int p = in_slot();
    for (int d = 0; d < kDevCount; ++d) {
        const float dg = s_dev_g[p][d];
        if (dg <= kInSilent) continue;
        osynth::dsp::simd_mix_i16lr_f32(s_cap[p][d], g * dg * kInScale, l, r,
                                        frames);
    }""",
)

sub(
    """    bool any = false;
    for (int d = 0; d < kDevCount; ++d) {
        const float dg = s_dev_g[d];
        if (dg <= kInSilent) continue;
        const int16_t* __restrict__ c = s_cap[d];""",
    """    bool any = false;
    const int p = in_slot();
    for (int d = 0; d < kDevCount; ++d) {
        const float dg = s_dev_g[p][d];
        if (dg <= kInSilent) continue;
        const int16_t* __restrict__ c = s_cap[p][d];""",
)

sub(
    """#if SYNTH_ENABLE_LINE_IN
    return s_in_ok ? s_cap[kSlotLine] : nullptr;
#else
    return s_in_ok ? s_cap[kSlotMic] : nullptr;
#endif""",
    """#if SYNTH_ENABLE_LINE_IN
    return s_in_ok ? s_cap[in_slot()][kSlotLine] : nullptr;
#else
    return s_in_ok ? s_cap[in_slot()][kSlotMic] : nullptr;
#endif""",
)

# the no-input build needs the capture stub to take the same argument
sub(
    """void audio_io_line_in_fx(float*, float*, size_t) {}""",
    """void audio_io_line_in_fx(float*, float*, size_t) {}""",
)

# --------------------------------------------------------------------------
# 5. the per-block tail, factored, and the task loops
# --------------------------------------------------------------------------
TASK_START = "/* Cycles in one block period; also the divisor for the per-stage meters."
TASK_END = "\n} // namespace"

i = ed.text.index(TASK_START)
j = ed.text.index(TASK_END, i)

NEW_TASKS = r'''/* Cycles in one block period; also the divisor for the per-stage meters.
 * Set by start_common() before either task exists (S45) -- the pipeline has two
 * of them, and neither is a sensible owner of a number the other divides by. */
uint32_t s_cycles_per_block = 0;

#if SYNTH_ENABLE_SPLIT_RENDER
/* Core 0's load EMA, deliberately not s_stats.dsp_load_pct.
 *
 * The EMA update is a read-modify-write, so two cores sharing one float would
 * lose part of every other sample of it, and taking a lock per block to
 * protect a diagnostic would cost more than the diagnostic is worth. Each core
 * owns its own; audio_io_get_stats() folds them together by taking the worse,
 * which is the one of the two that predicts a dropout. */
float s_load_a_pct = 0.0f;

/* How long core 1 waits for a block before calling the voice stage stalled.
 *
 * Generous on purpose. A legitimate wait is bounded by how far core 0 is
 * behind, and the sink's DMA ring absorbs several blocks of that before
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

/* Stage A, core 0: the voice manager, working one block ahead.
 *
 * Owns no clock and no sink, and that is the load-bearing part. It runs only
 * when core 1 hands it a free bus slot and blocks the rest of the time, so the
 * sink still paces both cores through it. A second free-running audio task
 * would be a second clock, which is the one thing a pipeline must not have.
 *
 * The notification pair is the whole of the synchronisation, and it is the
 * memory barrier as well: core 1 writes the slot it is releasing and publishes
 * the capture slot *before* the give, and a FreeRTOS notify is a full barrier
 * on both sides. Nothing here needs an atomic of its own, because there is no
 * moment when the two cores touch the same buffer at all. */
void SYNTH_RENDER_IRAM audio_task_voices(void*) {
    ESP_LOGI(TAG, "voice stage up: core %d, one block ahead of the sink",
             xPortGetCoreID());

    int prod = 0;
    for (;;) {
        /* Unbounded, unlike core 1's wait below. With no slot to fill there is
         * nothing useful this task could do with the CPU, and core 1 is the
         * only thing that can ever hand one over -- a timeout here would just
         * spin a high-priority task on core 0 against whatever is already
         * keeping it busy, which is the situation it would be waking up to
         * diagnose. */
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

/* Stage B, core 1: the capture, everything downstream of the voices, and the
 * sink that paces the pair.
 *
 * The block indices are worth following once, because their agreement is what
 * keeps the two cores off each other's buffers. Core 0's iteration i fills bus
 * slot i&1; this task's iteration i drains the same one, having been woken by
 * the give at the end of core 0's. The two therefore overlap by exactly one
 * iteration -- core 0 is building block N+1 into the slot this task finished
 * with last time, while this task finishes block N -- and the capture slot
 * follows the same index, which is why core 0 reading `cons ^ 1` is reading
 * the one this task is not writing. */
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
         * from a patch that is simply not making a sound. */
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
         * visible to core 0, and what it publishes is the slot core 0 may read
         * -- the opposite one to the capture two lines further down. Getting
         * this order wrong is the single way the two cores could end up on one
         * capture buffer. */
        publish_in_slots(cons);
#endif
        /* Released here rather than at the end of the block, so core 0 gets
         * this stage's own work *and* the sink's DMA wait to render the next
         * block in. That is where the pipeline's headroom actually comes
         * from. The slot it takes is the one this task finished with last time
         * round, which nothing has touched since. */
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

#endif /* SYNTH_ENABLE_SPLIT_RENDER */'''

ed.text = ed.text[:i] + NEW_TASKS + ed.text[j:]

# --------------------------------------------------------------------------
# 6. audio_io_start()'s body becomes start_common(); two entry points on top
# --------------------------------------------------------------------------
sub(
    """esp_err_t audio_io_start(audio_render_fn render, void* ctx) {
    if (s_task != nullptr) return ESP_ERR_INVALID_STATE;

    s_render = render;
    s_render_ctx = ctx;
    s_master_volume =""",
    """/* Everything both start paths do: pick and start the sink, bring the input
 * devices up, attach the tap, name the result, and work out the block budget
 * the meters divide by. Split out of audio_io_start() when the pipeline gave
 * it a sibling (S45) -- all either caller adds on top is which tasks to
 * create. */
static esp_err_t start_common(void* ctx) {
    s_render_ctx = ctx;
    s_master_volume =""",
)

sub(
    """    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "audio", 6144, nullptr,
                                            configMAX_PRIORITIES - 2, &s_task, 1);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}""",
    """    /* Before any task exists, because both of them divide by it and neither
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

    /* Core 0 first, and primed with one credit before core 1 exists: the
     * pipeline starts empty, and the voice stage is the only thing that can
     * put a block into it. Creating them the other way round would leave core
     * 1 waiting on a task that is waiting for a credit nobody has given, for
     * as long as it took to create it.
     *
     * Same priority as core 1 and for the same reason: on its core it must
     * outrank the control tasks, so a busy UI or a BLE host is starved of CPU
     * ahead of the audio deadline rather than alongside it. */
    BaseType_t ok =
        xTaskCreatePinnedToCore(audio_task_voices, "audio_voi", 6144, nullptr,
                                configMAX_PRIORITIES - 2, &s_task_a, 0);
    if (ok != pdPASS) return ESP_FAIL;
    xTaskNotifyGive(s_task_a);

    ok = xTaskCreatePinnedToCore(audio_task_pipe, "audio", 6144, nullptr,
                                 configMAX_PRIORITIES - 2, &s_task, 1);
    if (ok != pdPASS) return ESP_FAIL;

    ESP_LOGI(TAG,
             "render pipeline: voices on core 0, fx/looper + sink on core 1, "
             "+1 block (%.2f ms) of output latency",
             1000.0f * SYNTH_BLOCK_SIZE / SYNTH_SAMPLE_RATE);
    return ESP_OK;
}
#endif""",
)

# --------------------------------------------------------------------------
# 7. the stage meters, one reporter per core
# --------------------------------------------------------------------------
sub(
    """void SYNTH_RENDER_IRAM audio_io_report_stages(uint32_t voices_cycles,
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
}""",
    """void SYNTH_RENDER_IRAM audio_io_report_stage_voices(uint32_t voices_cycles) {
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
}""",
)

# --------------------------------------------------------------------------
# 8. get_stats folds the two cores' load figures together
# --------------------------------------------------------------------------
sub(
    """    portENTER_CRITICAL(&s_stats_mux);
    *out = s_stats;
    s_stats.dsp_load_peak_pct = 0.0f;""",
    """    portENTER_CRITICAL(&s_stats_mux);
    *out = s_stats;
#if SYNTH_ENABLE_SPLIT_RENDER
    /* The worse of the two cores, not their sum -- see dsp_load_pct in
     * audio_io.h. Folded in on the way out rather than kept folded, so each
     * core still owns exactly one float and neither has to read the other's. */
    if (s_load_a_pct > out->dsp_load_pct) out->dsp_load_pct = s_load_a_pct;
#endif
    s_stats.dsp_load_peak_pct = 0.0f;""",
)

ed.save("two-core render pipeline")
