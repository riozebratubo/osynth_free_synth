"""S45 fix: swap which core each render stage runs on, and stop naming cores in
the prose so the assignment lives in exactly one place.

The first cut put the voice stage on core 0 and the bus stage on core 1. That
is backwards for this firmware. Every task it pins is pinned to core 0 -- the
BLE command task, the sequencer clock, the preset loader, USB, app_main -- and
core 1 has always been kept empty for audio. The voice stage is also the far
heavier half (65% of the block budget against 16% on the patches this was first
measured on), and a stage that goes over budget never blocks: its next credit is
already waiting when it finishes, so it runs back-to-back at 100% for as long as
the patch is too expensive.

Observed: loading granular/14 'in: live cloud' pushed the voice stage past one
core's worth of budget, and with it on core 0 that starved the idle task, the
heartbeat and the BLE host -- `task_wdt: CPU 0: audio_voi`, and no `alive` line
after it.

So: voices onto the empty core, bus + sink onto the shared one. The bus stage is
the half that can afford core 0, because it blocks on the sink's DMA every block
and hands the core back on a schedule instead of at the mercy of the patch.

Run from the repo root: python tools/s45_split_render/08_swap_stage_cores.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _edit import Editor

# ==========================================================================
# audio_io.cpp -- the assignment itself, plus the prose that named cores
# ==========================================================================
c = Editor("components/audio_io/audio_io.cpp", skip_if="kVoiceCore")

c.sub(
    """#if SYNTH_ENABLE_SPLIT_RENDER
constexpr int kSlots = 2;
#else
constexpr int kSlots = 1;
#endif""",
    """#if SYNTH_ENABLE_SPLIT_RENDER
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
#endif""",
)

c.sub(
    """ * One slot without it: the same single buffer pair that has always been here,
 * reached through an index that is a compile-time constant, so the generated
 * code for a single-core build is what it was before this existed. Two with
 * it: core 0 fills one while core 1 drains the other, and they swap every
 * block.""",
    """ * One slot without it: the same single buffer pair that has always been here,
 * reached through an index that is a compile-time constant, so the generated
 * code for a single-core build is what it was before this existed. Two with
 * it: the voice stage fills one while the bus stage drains the other, and they
 * swap every block.""",
)

c.sub(
    """/* Per slot as well as per device (S45): core 1 fills one slot while core 0's
 * engines read the other through audio_io_in_mono() and
 * audio_io_line_in_block(). Without the pipeline kSlots is 1 and this is the
 * same 256 bytes per device it always was. */""",
    """/* Per slot as well as per device (S45): the bus stage fills one slot while the
 * voice stage's engines read the other through audio_io_in_mono() and
 * audio_io_line_in_block(). Without the pipeline kSlots is 1 and this is the
 * same 256 bytes per device it always was. */""",
)

c.sub(
    """/* Which capture slot each core is allowed to read this block, indexed by core
 * id. Published by core 1 before it releases the voice stage, so the
 * notification that starts core 0 is also the barrier that makes this visible
 * to it -- there is deliberately no atomic here, because there is no moment
 * when the two cores are looking at it without one of those in between.""",
    """/* Which capture slot each core is allowed to read this block, indexed by core
 * id. Published by the bus stage before it releases the voice stage, so the
 * notification that starts that stage is also the barrier that makes this
 * visible to it -- there is deliberately no atomic here, because there is no
 * moment when the two cores are looking at it without one of those in between.""",
)

c.sub(
    """/* `fresh` is the slot core 1 is about to capture into; core 0 therefore gets
 * the other one, which is the block captured one period earlier and which
 * nothing will write while it reads. */
inline void SYNTH_RENDER_IRAM publish_in_slots(int fresh) {
    s_in_slot[1] = fresh;
    s_in_slot[0] = fresh ^ 1;
}""",
    """/* `fresh` is the slot the bus stage is about to capture into; the voice stage
 * therefore gets the other one, which is the block captured one period earlier
 * and which nothing will write while it reads. Indexed by the stage constants
 * rather than by literal 0 and 1, so swapping the assignment above cannot leave
 * the two cores pointed at one buffer. */
inline void SYNTH_RENDER_IRAM publish_in_slots(int fresh) {
    s_in_slot[kBusCore] = fresh;
    s_in_slot[kVoiceCore] = fresh ^ 1;
}""",
)

c.sub(
    """/* Per slot, because audio_io_in_mono() applies these and is called from both
 * cores. The smoothers beside them are not: they are advanced once per block
 * by the capture, which only ever runs on core 1, and what travels with the
 * block is the value they landed on rather than the state that produced it.
 *
 * s_in_g below is single for the same reason read the other way round -- every
 * one of its readers (the three mix stages, audio_io_in_fx_block()) is on core
 * 1 with the capture, so there is no second view of it to keep consistent. */""",
    """/* Per slot, because audio_io_in_mono() applies these and is called from both
 * stages. The smoothers beside them are not: they are advanced once per block
 * by the capture, which only ever runs on the bus stage, and what travels with
 * the block is the value they landed on rather than the state that produced it.
 *
 * s_in_g below is single for the same reason read the other way round -- every
 * one of its readers (the three mix stages, audio_io_in_fx_block()) is on the
 * bus stage with the capture, so there is no second view to keep consistent. */""",
)

c.sub(
    """#if SYNTH_ENABLE_SPLIT_RENDER
/* Core 0's load EMA, deliberately not s_stats.dsp_load_pct.""",
    """#if SYNTH_ENABLE_SPLIT_RENDER
/* The voice stage's load EMA, deliberately not s_stats.dsp_load_pct.""",
)

c.sub(
    """/* How long core 1 waits for a block before calling the voice stage stalled.
 *
 * Generous on purpose. A legitimate wait is bounded by how far core 0 is
 * behind, and the sink's DMA ring absorbs several blocks of that before
 * anything is heard, so a wait long enough to expire here is not a late
 * pipeline but a broken one. Short enough, still, that the counter moves while
 * someone is watching the heartbeat rather than long after. */""",
    """/* How long the bus stage waits for a block before calling the voice stage
 * stalled.
 *
 * Generous on purpose. A legitimate wait is bounded by how far the voice stage
 * is behind, and the sink's DMA ring absorbs several blocks of that before
 * anything is heard, so a wait long enough to expire here is not a late
 * pipeline but a broken one. Short enough, still, that the counter moves while
 * someone is watching the heartbeat rather than long after. */""",
)

c.sub(
    """/* Stage A, core 0: the voice manager, working one block ahead.
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
 * moment when the two cores touch the same buffer at all. */""",
    """/* Stage A, on kVoiceCore: the voice manager, working one block ahead.
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
 * is no moment when the two cores touch the same buffer at all. */""",
)

c.sub(
    """        /* Unbounded, unlike core 1's wait below. With no slot to fill there is
         * nothing useful this task could do with the CPU, and core 1 is the
         * only thing that can ever hand one over -- a timeout here would just
         * spin a high-priority task on core 0 against whatever is already
         * keeping it busy, which is the situation it would be waking up to
         * diagnose. */""",
    """        /* Unbounded, unlike the bus stage's wait below. With no slot to fill
         * there is nothing useful this task could do with the CPU, and the bus
         * stage is the only thing that can ever hand one over -- a timeout here
         * would just spin a high-priority task against whatever is already
         * keeping its core busy, which is the situation it would be waking up
         * to diagnose. */""",
)

c.sub(
    """/* Stage B, core 1: the capture, everything downstream of the voices, and the
 * sink that paces the pair.
 *
 * The block indices are worth following once, because their agreement is what
 * keeps the two cores off each other's buffers. Core 0's iteration i fills bus
 * slot i&1; this task's iteration i drains the same one, having been woken by
 * the give at the end of core 0's. The two therefore overlap by exactly one
 * iteration -- core 0 is building block N+1 into the slot this task finished
 * with last time, while this task finishes block N -- and the capture slot
 * follows the same index, which is why core 0 reading `cons ^ 1` is reading
 * the one this task is not writing. */""",
    """/* Stage B, on kBusCore: the capture, everything downstream of the voices, and
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
 * `cons ^ 1` is reading the one this task is not writing. */""",
)

c.sub(
    """        /* Bounded, so a voice stage that has stopped delivering is *counted*
         * rather than merely inaudible. Nothing is written to the sink during
         * this wait, so its DMA drains and the output goes quiet -- and
         * pipe_stalls is then the only thing that separates a starved pipeline
         * from a patch that is simply not making a sound. */""",
    """        /* Bounded, so a voice stage that has stopped delivering is *counted*
         * rather than merely inaudible. Nothing is written to the sink during
         * this wait, so its DMA drains and the output goes quiet -- and
         * pipe_stalls is then the only thing that separates a starved pipeline
         * from a patch that is simply not making a sound.
         *
         * Note that an over-budget voice stage does not land here: it delivers,
         * only late, and the DMA absorbs that until it cannot. That case shows
         * up as underruns and a dsp_load_pct near 100, not as a stall. */""",
)

c.sub(
    """        /* Before the release below, never after: the give is what makes this
         * visible to core 0, and what it publishes is the slot core 0 may read
         * -- the opposite one to the capture two lines further down. Getting
         * this order wrong is the single way the two cores could end up on one
         * capture buffer. */""",
    """        /* Before the release below, never after: the give is what makes this
         * visible to the voice stage, and what it publishes is the slot that
         * stage may read -- the opposite one to the capture two lines further
         * down. Getting this order wrong is the single way the two cores could
         * end up on one capture buffer. */""",
)

c.sub(
    """        /* Released here rather than at the end of the block, so core 0 gets
         * this stage's own work *and* the sink's DMA wait to render the next
         * block in. That is where the pipeline's headroom actually comes
         * from. The slot it takes is the one this task finished with last time
         * round, which nothing has touched since. */""",
    """        /* Released here rather than at the end of the block, so the voice
         * stage gets this stage's own work *and* the sink's DMA wait to render
         * the next block in. That is where the pipeline's headroom actually
         * comes from. The slot it takes is the one this task finished with last
         * time round, which nothing has touched since. */""",
)

c.sub(
    """    /* Both tasks exist before either is allowed to run a block, and the credit
     * that starts the pipeline is given last. The order matters twice over.
     *
     * The voice stage is created at a priority that preempts app_main on its
     * own core, so it runs the instant it exists — and the first thing it
     * would do after a block is notify `s_task`, which the second create below
     * has not filled in yet. Priming it here rather than there is what keeps
     * that handle from being read before it is written; the task blocks
     * immediately on a notification nobody has sent, which costs nothing and
     * has no deadline to miss.
     *
     * Core 1 waking first is harmless in the other direction: it waits, times
     * out, and declines to count a stall because no block has ever landed yet
     * (see audio_task_pipe).
     *
     * Same priority on both, for the same reason: on each core the stage has
     * to outrank the control tasks, so a busy UI or a BLE host is starved of
     * CPU ahead of the audio deadline rather than alongside it. */
    BaseType_t ok =
        xTaskCreatePinnedToCore(audio_task_voices, "audio_voi", 6144, nullptr,
                                configMAX_PRIORITIES - 2, &s_task_a, 0);
    if (ok != pdPASS) return ESP_FAIL;

    ok = xTaskCreatePinnedToCore(audio_task_pipe, "audio", 6144, nullptr,
                                 configMAX_PRIORITIES - 2, &s_task, 1);
    if (ok != pdPASS) return ESP_FAIL;

    xTaskNotifyGive(s_task_a); /* the pipeline's first free slot */

    ESP_LOGI(TAG,
             "render pipeline: voices on core 0, fx/looper + sink on core 1, "
             "+1 block (%.2f ms) of output latency",
             1000.0f * SYNTH_BLOCK_SIZE / SYNTH_SAMPLE_RATE);
    return ESP_OK;""",
    """    /* Both tasks exist before either is allowed to run a block, and the credit
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
    return ESP_OK;""",
)

c.sub(
    """    ESP_LOGI(TAG, "voice stage up: core %d, one block ahead of the sink",
             xPortGetCoreID());""",
    """    ESP_LOGI(TAG, "voice stage up: core %d, one block ahead of the sink",
             xPortGetCoreID());""",
)

c.sub(
    """#if SYNTH_ENABLE_SPLIT_RENDER
    /* The worse of the two cores, not their sum -- see dsp_load_pct in
     * audio_io.h. Folded in on the way out rather than kept folded, so each
     * core still owns exactly one float and neither has to read the other's. */""",
    """#if SYNTH_ENABLE_SPLIT_RENDER
    /* The worse of the two stages, not their sum -- see dsp_load_pct in
     * audio_io.h. Folded in on the way out rather than kept folded, so each
     * stage still owns exactly one float and neither has to read the other's. */""",
)

c.save("voice stage to the empty core, bus stage to the shared one")

# ==========================================================================
# audio_io.h
# ==========================================================================
h = Editor("components/audio_io/include/audio_io.h", skip_if="the voice stage's whole core")

h.sub(
    """ * pieces instead of one and runs them as overlapping stages -- voices on core 0
 * building block N+1, everything after them plus the sink on core 1 finishing
 * block N. Nothing below this line changes shape for it: the same render
 * callback type, the same stages, the same stats. What changes is that the
 * capture is double-buffered, because core 0's engines read the input through
 * audio_io_in_mono() and audio_io_line_in_block() while core 1 is filling the
 * next block into the other slot -- see those two for the skew that buys.""",
    """ * pieces instead of one and runs them as overlapping stages -- a voice stage
 * building block N+1, and a bus stage finishing block N with everything after
 * the voices plus the sink. Which core each lands on is decided in audio_io.cpp
 * and matters: see kVoiceCore there. Nothing below this line changes shape for
 * it -- the same render callback type, the same stages, the same stats. What
 * changes is that the capture is double-buffered, because the voice stage's
 * engines read the input through audio_io_in_mono() and
 * audio_io_line_in_block() while the bus stage is filling the next block into
 * the other slot -- see those two for the skew that buys.""",
)

h.sub(
    """                              * loaded. The [voi fx loop] split beside it says
                              * which core that is -- voi is core 0's whole
                              * stage, the other two are core 1's. */""",
    """                              * loaded. The [voi fx loop] split beside it says
                              * which stage that is -- voi is the whole of the
                              * voice stage, the other two are the bus stage. */""",
)

h.sub(
    """    /* Waits core 1 timed out on a voice stage that had not arrived (S45);
     * always 0 without the two-core pipeline.
     *
     * Distinct from `underruns` above, and the distinction is the whole
     * diagnosis. An underrun is one core failing to finish inside its own
     * block period, which the per-stage percentages then localise. A stall is
     * core 1 having nothing to work on at all -- core 0 starved of scheduling
     * rather than of budget -- and it presents identically at the output while
     * calling for the opposite fix: not less DSP, but finding whatever is
     * holding core 0 off the CPU.""",
    """    /* Waits the bus stage timed out on a voice stage that had not arrived
     * (S45); always 0 without the two-core pipeline.
     *
     * Distinct from `underruns` above, and the distinction is the whole
     * diagnosis. An underrun is one stage failing to finish inside its own
     * block period, which the per-stage percentages then localise -- an
     * over-budget voice stage lands there, not here, because it does deliver,
     * only late. A stall is the bus stage having nothing to work on at all: the
     * voice stage starved of scheduling rather than of budget. It presents
     * identically at the output while calling for the opposite fix -- not less
     * DSP, but finding whatever is holding that core off the CPU.""",
)

h.sub(
    """ * which core is asking, and both do: the granular engine and the sampler's
 * pre-roll sit on opposite sides of the cut. A caller on core 1 -- the FX bus,
 * the sampler tap -- gets the block captured for the block being finished, the
 * same one the three mix stages used. A caller on core 0 -- an engine -- gets
 * the previous slot instead, two block periods (2.7 ms) older, because core 1
 * is filling the fresh one while that engine runs. Nothing is ever read while
 * it is being written, and no caller has to know which core it is on.""",
    """ * which stage is asking, and both do: the granular engine and the sampler's
 * pre-roll sit on opposite sides of the cut. A caller on the bus stage -- the
 * FX bus, the sampler tap -- gets the block captured for the block being
 * finished, the same one the three mix stages used. A caller on the voice stage
 * -- an engine -- gets the previous slot instead, two block periods (2.7 ms)
 * older, because the bus stage is filling the fresh one while that engine runs.
 * Nothing is ever read while it is being written, and no caller has to know
 * which core it is on.""",
)

h.sub(
    """ * have to be on core 1, and audio_io_in_fx_block() below is the entry point
 * for exactly that. */""",
    """ * have to be on the bus stage, and audio_io_in_fx_block() below is the entry
 * point for exactly that. */""",
)

h.sub(
    """ * Its one caller is the graph's LineIn node, which is inside an engine and so
 * on core 0 under the two-core pipeline. It therefore gets the same""",
    """ * Its one caller is the graph's LineIn node, which is inside an engine and so
 * on the voice stage under the two-core pipeline. It therefore gets the same""",
)

h.sub(
    """/* The same, with the chain handed over in two pieces to run as a two-core
 * pipeline (S45). `stage_a` runs on core 0 and must be the part of the chain
 * with no reader on the other side of the cut -- the voice manager, and
 * nothing else. `stage_b` runs on core 1 with the sink, and gets the bus
 * `stage_a` produced one block period earlier.""",
    """/* The same, with the chain handed over in two pieces to run as a two-core
 * pipeline (S45). `stage_a` must be the part of the chain with no reader on the
 * other side of the cut -- the voice manager, and nothing else. `stage_b` runs
 * with the sink, and gets the bus `stage_a` produced one block period earlier.
 * Which core each is pinned to, and why it is not arbitrary, is kVoiceCore in
 * audio_io.cpp.""",
)

h.save("stage-named prose")

# ==========================================================================
# main.cpp
# ==========================================================================
m = Editor("main/main.cpp", skip_if="/* The voice stage, one block ahead")
m.sub(
    """/* Core 0, one block ahead of the sink. */""",
    """/* The voice stage, one block ahead of the sink. */""",
)
m.sub(
    """/* Core 1, on the block core 0 finished one period ago, and then the sink.
 *
 * `voi` on the heartbeat therefore means voices *only* here, where on a
 * single-core build it also carries the drum bus and the input mix. That is
 * the meter following the hardware rather than drifting from it: the three
 * numbers now read as core 0 | core 1, which is the split that says which core
 * to take load off. */""",
    """/* The bus stage, on the block the voice stage finished one period ago, and
 * then the sink.
 *
 * `voi` on the heartbeat therefore means voices *only* here, where on a
 * single-core build it also carries the drum bus and the input mix. That is
 * the meter following the hardware rather than drifting from it: the three
 * numbers now read as voice stage | bus stage, which is the split that says
 * which of the two to take load off. */""",
)
m.sub(
    """    /* The one piece of state that has to cross the cut: the note-start tap the
     * vocoder retriggers on, produced here and read by the FX bus a block
     * later. Inside the measured window because it is part of the stage's
     * work, and after chain_voices() because that is what fills it. */""",
    """    /* The one piece of state that has to cross the cut: the note-start tap the
     * vocoder retriggers on, produced here and read by the FX bus a block
     * later. Inside the measured window because it is part of the stage's
     * work, and after chain_voices() because that is what fills it. */""",
)
m.sub(
    """    /* Before anything in this stage can ask for it, and in particular before
     * fx_process(): this is what makes voice_manager_block_note() answer for
     * the block being finished rather than the one core 0 is building. */""",
    """    /* Before anything in this stage can ask for it, and in particular before
     * fx_process(): this is what makes voice_manager_block_note() answer for
     * the block being finished rather than the one the voice stage is
     * building. */""",
)
m.sub(
    """ * in the three chain_* helpers; the entry points after them add only the cycle
 * markers and, where the pipeline is compiled in, the core boundary.""",
    """ * in the three chain_* helpers; the entry points after them add only the cycle
 * markers and, where the pipeline is compiled in, the stage boundary.""",
)
m.sub(
    """ * noise-reduction units down to the last multiply. All of that stays whole on
 * core 1; what crosses to core 0 is a stage with one output and no readers. */""",
    """ * noise-reduction units down to the last multiply. All of that stays whole on
 * the bus stage; what crosses to the voice stage is a piece with one output and
 * no readers. */""",
)
m.save("stage-named prose")

# ==========================================================================
# synth_voice.{h,cpp}
# ==========================================================================
vh = Editor("components/synth_core/include/synth_voice.h", skip_if="on the voice stage, immediately")
vh.sub(
    """#if SYNTH_ENABLE_SPLIT_RENDER
/* Core 0, immediately after voice_manager_render(): file this block's tap into
 * the slot that will reach the FX bus one block period from now. */
void voice_manager_stage_block_note(void);

/* Core 1, before the FX bus runs: make the slot belonging to the block now
 * being finished the one voice_manager_block_note() answers with.""",
    """#if SYNTH_ENABLE_SPLIT_RENDER
/* On the voice stage, immediately after voice_manager_render(): file this
 * block's tap into the slot that will reach the FX bus one block period from
 * now. */
void voice_manager_stage_block_note(void);

/* On the bus stage, before the FX bus runs: make the slot belonging to the
 * block now being finished the one voice_manager_block_note() answers with.""",
)
vh.save("stage-named prose")

vc = Editor("components/synth_core/synth_voice.cpp", skip_if="produces this on the voice stage")
vc.sub(
    """/* ...except when those two are not the same callback (S45). The voice stage
 * produces this on core 0 for block N+1 while the FX bus consumes block N's on
 * core 1, so the byte above is being cleared and rewritten under the reader.
 * These carry it across instead, in the two slots and with the phasing the
 * audio bus already uses: the producer index is touched only by core 0 and the
 * consumer index only by core 1, and because each stage runs exactly once per
 * block the two are permanently one apart. Neither core ever names the slot
 * the other is working on, which is what makes plain bytes enough here too. */
uint8_t s_note_slot[2] = {0, 0};
uint8_t s_note_prod = 0; /* core 0 only */
uint8_t s_note_cons = 0; /* core 1 only */
uint8_t s_note_fx = 0;   /* core 1 only: what the FX bus is answered with */""",
    """/* ...except when those two are not the same callback (S45). The synth
 * produces this on the voice stage for block N+1 while the FX bus consumes
 * block N's on the bus stage, so the byte above is being cleared and rewritten
 * under the reader. These carry it across instead, in the two slots and with
 * the phasing the audio bus already uses: the producer index is touched only by
 * the voice stage and the consumer index only by the bus stage, and because
 * each runs exactly once per block the two are permanently one apart. Neither
 * ever names the slot the other is working on, which is what makes plain bytes
 * enough here too. */
uint8_t s_note_slot[2] = {0, 0};
uint8_t s_note_prod = 0; /* voice stage only */
uint8_t s_note_cons = 0; /* bus stage only */
uint8_t s_note_fx = 0;   /* bus stage only: what the FX bus is answered with */""",
)
vc.sub(
    """#if SYNTH_ENABLE_SPLIT_RENDER
    /* The FX bus is the only caller, and it is on core 1, so this answers with
     * what the pipeline latched for the block core 1 is finishing -- never
     * with s_block_note, which by now belongs to the block core 0 is building. */""",
    """#if SYNTH_ENABLE_SPLIT_RENDER
    /* The FX bus is the only caller, and it is on the bus stage, so this
     * answers with what the pipeline latched for the block that stage is
     * finishing -- never with s_block_note, which by now belongs to the block
     * the voice stage is building. */""",
)
vc.save("stage-named prose")

# ==========================================================================
# synth_config.h -- the S3 exclusion argument, which now runs the other way
# ==========================================================================
sc = Editor("components/synth_core/include/synth_config.h",
            skip_if="The bus stage shares its core")
sc.sub(
    """ * The chain is cut in one place — after the voice manager — and the two halves
 * run as pipeline stages on separate cores: voices on core 0 producing block
 * N+1, everything after them (drum bus, FX bus, looper, sampler tap,
 * metronome) plus the sink on core 1 finishing block N. The stages overlap, so
 * the budget for one block period is roughly doubled.""",
    """ * The chain is cut in one place — after the voice manager — and the two halves
 * run as pipeline stages on separate cores: a voice stage producing block N+1,
 * and a bus stage finishing block N with everything after the voices (drum bus,
 * FX bus, looper, sampler tap, metronome) plus the sink. The stages overlap, so
 * the budget for one block period is roughly doubled. Which core each is pinned
 * to is kVoiceCore in audio_io.cpp, and it is not arbitrary.""",
)
sc.sub(
    """ * The capability gate is repeated here rather than left to Kconfig, the same
 * way SYNTH_ENABLE_USB_TAP's is, and for a reason that is real hardware rather
 * than defensiveness: a stage has one block period of slack, so anything that
 * stalls its core for longer is heard. On the P4 the BLE controller is on the
 * companion radio chip (SYNTH_BLE_VIA_HOSTED below), so core 0 sees only the
 * transport driver's interrupts. On the S3 the on-die controller runs
 * high-priority interrupts pinned to core 0 that no task priority can preempt,
 * which is exactly the stall this cannot absorb. */""",
    """ * The capability gate is repeated here rather than left to Kconfig, the same
 * way SYNTH_ENABLE_USB_TAP's is, and for a reason that is real hardware rather
 * than defensiveness. The bus stage shares its core with everything this
 * firmware pins — the BLE host, the sequencer clock, the preset loader — and it
 * is the half holding the sink, and therefore the hard deadline. On the P4 that
 * is affordable: the BLE controller lives on the companion radio chip
 * (SYNTH_BLE_VIA_HOSTED below), so that core sees only the transport driver's
 * interrupts. On the S3 the on-die controller runs high-priority interrupts
 * pinned to core 0 that no task priority can preempt, landing them squarely on
 * the stage that must not miss a block. There the single-core chain keeps the
 * whole render on core 1, where it never meets them. */""",
)
sc.save("S3 exclusion argument follows the new assignment")

print("\nall stage-core prose updated")
