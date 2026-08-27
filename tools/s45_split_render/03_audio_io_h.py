"""S45: audio_io.h — the split-render entry points, the stall counter, and the
doc changes the two-core pipeline forces on things that used to be single-core
facts.

Run from the repo root: python tools/s45_split_render/03_audio_io_h.py
Idempotent: it exits early once audio_io_start_split is present.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _edit import Editor

ed = Editor("components/audio_io/include/audio_io.h", skip_if="audio_io_start_split")
sub = ed.sub


# ---- 1. file header ----
sub(
    """ * place in the render chain is "the input", and how many devices are behind it
 * is a question no consumer has to ask.
 */""",
    """ * place in the render chain is "the input", and how many devices are behind it
 * is a question no consumer has to ask.
 * Session 45: on the P4 the chain runs as a two-core pipeline
 * (SYNTH_ENABLE_SPLIT_RENDER). audio_io_start_split() takes the chain in two
 * pieces instead of one and runs them as overlapping stages -- voices on core 0
 * building block N+1, everything after them plus the sink on core 1 finishing
 * block N. Nothing below this line changes shape for it: the same render
 * callback type, the same stages, the same stats. What changes is that the
 * capture is double-buffered, because core 0's engines read the input through
 * audio_io_in_mono() and audio_io_line_in_block() while core 1 is filling the
 * next block into the other slot -- see those two for the skew that buys.
 */""",
)

# ---- 2. dsp_load_pct meaning under the split ----
sub(
    """    float dsp_load_pct;      /* render time / block budget, EMA-smoothed */""",
    """    float dsp_load_pct;      /* render time / block budget, EMA-smoothed.
                              * With the two-core pipeline this is the *worse*
                              * of the two cores rather than a sum: the deadline
                              * is per-core and per-block, so the core with the
                              * fuller block is the one that decides whether the
                              * next one arrives in time. Reading it as a total
                              * would make a pipeline about to fail look half
                              * loaded. The [voi fx loop] split beside it says
                              * which core that is -- voi is core 0's whole
                              * stage, the other two are core 1's. */""",
)

# ---- 3. underruns, which both cores can now raise ----
sub(
    """    uint32_t underruns;      /* blocks whose render ran past the block budget
                              * (deadline misses) */""",
    """    uint32_t underruns;      /* blocks whose render ran past the block budget
                              * (deadline misses). Either core can raise one
                              * under the two-core pipeline, and a block that
                              * overran on both counts twice -- a distinction
                              * not worth a second counter, since by then the
                              * answer is the same in both directions. */""",
)

# ---- 4. the stall counter, next to sink_errors ----
sub(
    """    uint32_t sink_errors;
    int32_t sink_last_err;
} audio_io_stats_t;""",
    """    uint32_t sink_errors;
    int32_t sink_last_err;
    /* Waits core 1 timed out on a voice stage that had not arrived (S45);
     * always 0 without the two-core pipeline.
     *
     * Distinct from `underruns` above, and the distinction is the whole
     * diagnosis. An underrun is one core failing to finish inside its own
     * block period, which the per-stage percentages then localise. A stall is
     * core 1 having nothing to work on at all -- core 0 starved of scheduling
     * rather than of budget -- and it presents identically at the output while
     * calling for the opposite fix: not less DSP, but finding whatever is
     * holding core 0 off the CPU.
     *
     * Counted per timed-out wait rather than per block, so it climbs at the
     * timeout's rate while the condition lasts rather than at the block rate.
     * Non-zero at all is a fault, and the sink is emitting silence for as long
     * as it is climbing. */
    uint32_t pipe_stalls;
} audio_io_stats_t;""",
)

# ---- 5. the two per-stage report entry points ----
sub(
    """/* Called once per block from the render chain (audio task only) with the
 * cycle cost of each stage. No-op before the audio task computes its
 * budget. */
void audio_io_report_stages(uint32_t voices_cycles, uint32_t fx_cycles,
                            uint32_t loop_cycles);""",
    """/* Called once per block from the render chain (audio task only) with the
 * cycle cost of each stage. No-op before the audio task computes its
 * budget. */
void audio_io_report_stages(uint32_t voices_cycles, uint32_t fx_cycles,
                            uint32_t loop_cycles);

/* The same three numbers, reported by whichever half of the chain measured
 * them, for the two-core pipeline where no single task sees all three.
 * audio_io_report_stages() above is these two called in order, so the single
 * and split paths feed one set of meters through one piece of arithmetic.
 *
 * Each is called from its own core and touches only its own fields, which is
 * what makes them safe without a lock: the EMA update is a read-modify-write,
 * and two cores doing that to one float would lose part of it. */
void audio_io_report_stage_voices(uint32_t voices_cycles);
void audio_io_report_stage_fx_loop(uint32_t fx_cycles, uint32_t loop_cycles);""",
)

# ---- 6. the split start entry point ----
sub(
    """/* Picks and starts the output sink, then starts the audio task.
 * `render` may be NULL (silence). Falls back to the null sink (no output,
 * timer pacing) if the hardware sink fails to start. */
esp_err_t audio_io_start(audio_render_fn render, void* ctx);""",
    """/* Picks and starts the output sink, then starts the audio task.
 * `render` may be NULL (silence). Falls back to the null sink (no output,
 * timer pacing) if the hardware sink fails to start. */
esp_err_t audio_io_start(audio_render_fn render, void* ctx);

#if SYNTH_ENABLE_SPLIT_RENDER
/* The same, with the chain handed over in two pieces to run as a two-core
 * pipeline (S45). `stage_a` runs on core 0 and must be the part of the chain
 * with no reader on the other side of the cut -- the voice manager, and
 * nothing else. `stage_b` runs on core 1 with the sink, and gets the bus
 * `stage_a` produced one block period earlier.
 *
 * Both are called exactly once per block, in the same order and with the same
 * frame count as the single-core chain, so every ordering invariant the chain
 * documents still holds. What the caller pays for it is one block period of
 * added output latency and the requirement that the two halves really are
 * separable: a `stage_a` that reads anything `stage_b` wrote during the same
 * block gets the previous block's version of it.
 *
 * Either may be NULL (that half renders silence). Falls back to the null sink
 * exactly as audio_io_start() does -- a sink that fails does not collapse the
 * pipeline, since the null sink paces it just as well. */
esp_err_t audio_io_start_split(audio_render_fn stage_a, audio_render_fn stage_b,
                               void* ctx);
#endif""",
)

# ---- 7. the skew the split gives the two engine-side accessors ----
sub(
    """ * A MEMS mic at conversational distance sits far below full scale even after
 * that trim, so a caller that wants a usable signal from one should offer a
 * gain of its own rather than assume this arrives near unity. */
bool audio_io_in_mono(float* dst, size_t frames);""",
    """ * A MEMS mic at conversational distance sits far below full scale even after
 * that trim, so a caller that wants a usable signal from one should offer a
 * gain of its own rather than assume this arrives near unity.
 *
 * Under the two-core pipeline (S45) what "the current block" means depends on
 * which core is asking, and both do: the granular engine and the sampler's
 * pre-roll sit on opposite sides of the cut. A caller on core 1 -- the FX bus,
 * the sampler tap -- gets the block captured for the block being finished, the
 * same one the three mix stages used. A caller on core 0 -- an engine -- gets
 * the previous slot instead, two block periods (2.7 ms) older, because core 1
 * is filling the fresh one while that engine runs. Nothing is ever read while
 * it is being written, and no caller has to know which core it is on.
 *
 * The skew is real but it is not a phase error anyone can hear: it applies to
 * granulation and to sample pre-roll, neither of which is summed against the
 * monitored input. A caller that *did* need phase agreement with the bus would
 * have to be on core 1, and audio_io_in_fx_block() below is the entry point
 * for exactly that. */
bool audio_io_in_mono(float* dst, size_t frames);""",
)

sub(
    """ * off or an input heard twice when it is not. So a graph patch works with
 * `in.route` at off, which is also how you would set it up. */
const int16_t* audio_io_line_in_block(void);""",
    """ * off or an input heard twice when it is not. So a graph patch works with
 * `in.route` at off, which is also how you would set it up.
 *
 * Its one caller is the graph's LineIn node, which is inside an engine and so
 * on core 0 under the two-core pipeline. It therefore gets the same
 * previous-slot block audio_io_in_mono() describes above, for the same reason
 * and with the same 2.7 ms of extra age. The pointer stays valid for the whole
 * of that core's block. */
const int16_t* audio_io_line_in_block(void);""",
)

# SYNTH_ENABLE_SPLIT_RENDER has to be visible to the declaration above.
sub(
    """#include "esp_err.h"
""",
    """#include "esp_err.h"

#include "synth_config.h"
""",
)

ed.save("split-render API, pipe_stalls, pipeline docs")
