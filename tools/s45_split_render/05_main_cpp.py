"""S45: main.cpp -- the chain expressed once, as three pieces, so the
single-core and two-core paths are two orderings of one definition.

Also swaps the two audio_io_start() call sites for one start_audio() helper
(they exist twice for the OSYNTH_CODEC_INIT_BEFORE_I2S A/B), and puts the
pipeline stall counter on the heartbeat.

Run from the repo root: python tools/s45_split_render/05_main_cpp.py
Idempotent: it exits early once render_stage_a is present.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _edit import Editor

ed = Editor("main/main.cpp", skip_if="render_stage_a")
sub = ed.sub

# --------------------------------------------------------------------------
# 1. the chain: three inline pieces, then the per-build entry points
# --------------------------------------------------------------------------
OLD_CHAIN = """static void SYNTH_RENDER_IRAM render_chain(float* out_l, float* out_r,
                                           size_t frames, void* ctx) {
    /* Cycle-counter reads so the heartbeat can attribute the load to a stage
     * (S21b). Chasing "clicks with underruns" from one total number is
     * guesswork — voices, drums, FX and looper have very different cost
     * curves (polyphony vs hit density vs always-on vs track count). The
     * drum bus is folded into the voices figure: both scale with how much is
     * sounding, and a fourth number would not have changed any diagnosis so
     * far. */
    const uint32_t c0 = esp_cpu_get_cycle_count();
    voice_manager_render(out_l, out_r, frames, ctx);
    drums_pre_fx(out_l, out_r, frames);
    /* Before the c1 marker, so the input's cost folds into `voi` alongside
     * the drums rather than earning a fourth number nobody would read. */
    audio_io_line_in_fx(out_l, out_r, frames);
    const uint32_t c1 = esp_cpu_get_cycle_count();
    fx_process(out_l, out_r, frames);
    const uint32_t c2 = esp_cpu_get_cycle_count();
    drums_post_fx(out_l, out_r, frames);
    audio_io_line_in_dry(out_l, out_r, frames);
    looper_process(out_l, out_r, frames);
    /* After the record tap: monitored, never printed into a take. */
    audio_io_line_in_mon(out_l, out_r, frames);
    /* The sampler's capture point (S44), and its placement *is* the definition
     * of `smp.src = bus`: after the looper, so resampling captures the loops
     * that are playing, and before the metronome, so a count-in never ends up
     * inside the sample it was counting in. Both are the same two reasons the
     * looper's own record tap sits where it does. */
    sampler_capture(out_l, out_r, frames);
    /* After the looper on purpose: the metronome is monitoring, not material,
     * and mixing it earlier printed count-in ticks into the take. */
    drums_render_click(out_l, out_r, frames);
    const uint32_t c3 = esp_cpu_get_cycle_count();
    audio_io_report_stages(c1 - c0, c2 - c1, c3 - c2);
}"""

NEW_CHAIN = """ * The chain below is written as three pieces rather than one function (S45),
 * because the P4 runs it as a two-core pipeline and the alternative was two
 * copies of the order. Everything about *what* runs and in what sequence lives
 * in the three chain_* helpers; the entry points after them add only the cycle
 * markers and, where the pipeline is compiled in, the core boundary.
 *
 * The boundary is between chain_voices() and chain_pre_fx(), and it is the
 * only place in the chain where it could be. Everywhere else something
 * downstream writes state that something upstream reads back inside the same
 * block: drums_pre_fx() renders into a scratch buffer that drums_post_fx() and
 * the FX bus compressor's key tap (drums_block_hit()) both read, and the three
 * input mix points have to agree with what audio_io_in_fx_block() hands the
 * noise-reduction units down to the last multiply. All of that stays whole on
 * core 1; what crosses to core 0 is a stage with one output and no readers. */
static inline void SYNTH_RENDER_IRAM chain_voices(float* l, float* r,
                                                  size_t frames, void* ctx) {
    voice_manager_render(l, r, frames, ctx);
}

static inline void SYNTH_RENDER_IRAM chain_pre_fx(float* l, float* r,
                                                  size_t frames) {
    drums_pre_fx(l, r, frames);
    audio_io_line_in_fx(l, r, frames);
}

static inline void SYNTH_RENDER_IRAM chain_post_fx(float* l, float* r,
                                                   size_t frames) {
    drums_post_fx(l, r, frames);
    audio_io_line_in_dry(l, r, frames);
    looper_process(l, r, frames);
    /* After the record tap: monitored, never printed into a take. */
    audio_io_line_in_mon(l, r, frames);
    /* The sampler's capture point (S44), and its placement *is* the definition
     * of `smp.src = bus`: after the looper, so resampling captures the loops
     * that are playing, and before the metronome, so a count-in never ends up
     * inside the sample it was counting in. Both are the same two reasons the
     * looper's own record tap sits where it does. */
    sampler_capture(l, r, frames);
    /* After the looper on purpose: the metronome is monitoring, not material,
     * and mixing it earlier printed count-in ticks into the take. */
    drums_render_click(l, r, frames);
}

#if SYNTH_ENABLE_SPLIT_RENDER

/* Core 0, one block ahead of the sink. */
static void SYNTH_RENDER_IRAM render_stage_a(float* out_l, float* out_r,
                                             size_t frames, void* ctx) {
    const uint32_t c0 = esp_cpu_get_cycle_count();
    chain_voices(out_l, out_r, frames, ctx);
    audio_io_report_stage_voices(esp_cpu_get_cycle_count() - c0);
}

/* Core 1, on the block core 0 finished one period ago, and then the sink.
 *
 * `voi` on the heartbeat therefore means voices *only* here, where on a
 * single-core build it also carries the drum bus and the input mix. That is
 * the meter following the hardware rather than drifting from it: the three
 * numbers now read as core 0 | core 1, which is the split that says which core
 * to take load off. */
static void SYNTH_RENDER_IRAM render_stage_b(float* out_l, float* out_r,
                                             size_t frames, void* ctx) {
    (void)ctx; /* the voice stage got it; nothing downstream needs one */
    const uint32_t c1 = esp_cpu_get_cycle_count();
    chain_pre_fx(out_l, out_r, frames);
    fx_process(out_l, out_r, frames);
    const uint32_t c2 = esp_cpu_get_cycle_count();
    chain_post_fx(out_l, out_r, frames);
    const uint32_t c3 = esp_cpu_get_cycle_count();
    audio_io_report_stage_fx_loop(c2 - c1, c3 - c2);
}

#else

static void SYNTH_RENDER_IRAM render_chain(float* out_l, float* out_r,
                                           size_t frames, void* ctx) {
    /* Cycle-counter reads so the heartbeat can attribute the load to a stage
     * (S21b). Chasing "clicks with underruns" from one total number is
     * guesswork — voices, drums, FX and looper have very different cost
     * curves (polyphony vs hit density vs always-on vs track count). The
     * drum bus is folded into the voices figure: both scale with how much is
     * sounding, and a fourth number would not have changed any diagnosis so
     * far. The input mix is folded in with it, for the same reason — it would
     * never have earned a number of its own. */
    const uint32_t c0 = esp_cpu_get_cycle_count();
    chain_voices(out_l, out_r, frames, ctx);
    chain_pre_fx(out_l, out_r, frames);
    const uint32_t c1 = esp_cpu_get_cycle_count();
    fx_process(out_l, out_r, frames);
    const uint32_t c2 = esp_cpu_get_cycle_count();
    chain_post_fx(out_l, out_r, frames);
    const uint32_t c3 = esp_cpu_get_cycle_count();
    audio_io_report_stages(c1 - c0, c2 - c1, c3 - c2);
}

#endif /* SYNTH_ENABLE_SPLIT_RENDER */

/* One place for "start the audio engine": which entry point that is depends on
 * the build, and the call itself appears twice below — either side of
 * codec_init(), for the OSYNTH_CODEC_INIT_BEFORE_I2S A/B. */
static esp_err_t start_audio(void) {
#if SYNTH_ENABLE_SPLIT_RENDER
    return audio_io_start_split(render_stage_a, render_stage_b, nullptr);
#else
    return audio_io_start(render_chain, nullptr);
#endif
}"""

# The block comment above render_chain ends with the `in.route` paragraph; the
# new text opens with a paragraph of its own, so splice it onto that comment.
sub(
    """ * `in.route` selects carries gain; the other two return on a compare. */
""" + OLD_CHAIN,
    """ * `in.route` selects carries gain; the other two return on a compare.
""" + NEW_CHAIN,
)

# --------------------------------------------------------------------------
# 2. the two start sites
# --------------------------------------------------------------------------
sub(
    """#if !OSYNTH_CODEC_INIT_BEFORE_I2S
    ESP_ERROR_CHECK(audio_io_start(render_chain, nullptr));
#endif""",
    """#if !OSYNTH_CODEC_INIT_BEFORE_I2S
    ESP_ERROR_CHECK(start_audio());
#endif""",
)

sub(
    """#if OSYNTH_CODEC_INIT_BEFORE_I2S
    ESP_ERROR_CHECK(audio_io_start(render_chain, nullptr));
#endif""",
    """#if OSYNTH_CODEC_INIT_BEFORE_I2S
    ESP_ERROR_CHECK(start_audio());
#endif""",
)

# the stall segment needs a buffer of its own beside the others
sub(
    """    char sink_seg[64];""",
    """    char sink_seg[64];
    char pipe_seg[32]; /* the voice stage falling behind (S45) */""",
)

# --------------------------------------------------------------------------
# 3. the heartbeat: the pipeline stall counter, appended only when it is a
#    fault, exactly as the sink-error segment is
# --------------------------------------------------------------------------
sub(
    """        if (st.sink_errors != 0) {
            snprintf(sink_seg, sizeof(sink_seg), " | SINK ERR %u (%s)",
                     (unsigned)st.sink_errors,
                     esp_err_to_name((esp_err_t)st.sink_last_err));
        } else {
            sink_seg[0] = '\\0';
        }""",
    """        if (st.sink_errors != 0) {
            snprintf(sink_seg, sizeof(sink_seg), " | SINK ERR %u (%s)",
                     (unsigned)st.sink_errors,
                     esp_err_to_name((esp_err_t)st.sink_last_err));
        } else {
            sink_seg[0] = '\\0';
        }

        /* Appended on the same terms and for the same reason (S45): a healthy
         * board never prints it, and a board that does has a fault the
         * underrun count beside it cannot express. An underrun is a core that
         * ran out of *budget*; a stall is core 1 with nothing to work on at
         * all, because the voice stage never delivered — core 0 starved of
         * scheduling rather than of DSP. The output is silence either way,
         * which is exactly why the two need separating from here. */
        if (st.pipe_stalls != 0) {
            snprintf(pipe_seg, sizeof(pipe_seg), " | PIPE STALL %u",
                     (unsigned)st.pipe_stalls);
        } else {
            pipe_seg[0] = '\\0';
        }""",
)

sub(
    """                 eng != nullptr ? eng->name : "none",
                 ble_ctrl_state_name(), usb_seg, in_seg, sink_seg);""",
    """                 eng != nullptr ? eng->name : "none",
                 ble_ctrl_state_name(), usb_seg, in_seg, sink_seg, pipe_seg);""",
)

sub(
    """                 "out pk %.2f, sat %u | voices %u/%d (+%d drum) | engine %s | "
                 "ble %s%s%s%s",""",
    """                 "out pk %.2f, sat %u | voices %u/%d (+%d drum) | engine %s | "
                 "ble %s%s%s%s%s",""",
)

ed.save("chain split into stages, start_audio(), pipe-stall segment")
