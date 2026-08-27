"""S45: carry the note-start tap across the pipeline cut.

voice_manager_block_note() (S43) is written by drain_events() inside
voice_manager_render() and read by the FX bus's vocoder. On a single-core
build both happen in one render callback and a plain byte is the whole story.
Split across two cores they are one block period apart and concurrent: core 0
clears the byte at the top of block N+1 while core 1's fx_process() is still
reading it for block N, so the retrigger would fire late, early, or not at all
depending on how the two cores interleaved.

Fixed by latching it into the same two slots, with the same phasing, the audio
bus already uses. This is the only piece of state that crosses the cut -- every
other downstream dependency (drums_block_hit, seqarp, the input mix points)
either lives entirely on core 1 or is already an atomic written by a control
task.

Run from the repo root: python tools/s45_split_render/07_block_note.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _edit import Editor

# --------------------------------------------------------------------------
# synth_voice.h -- the two pipeline hooks
# --------------------------------------------------------------------------
hdr = Editor("components/synth_core/include/synth_voice.h",
             skip_if="voice_manager_stage_block_note")
hdr.sub(
    """ * The vocoder's sample replay is the caller -- it restarts the recorded
 * phrase on each note. */
uint8_t voice_manager_block_note(void);""",
    """ * The vocoder's sample replay is the caller -- it restarts the recorded
 * phrase on each note.
 *
 * Under the two-core pipeline (S45) the two halves of that contract land on
 * different cores a block apart, so the value has to travel with the block
 * rather than sit in a byte. The two calls below are how, and the render
 * stages in main.cpp are their only callers. */
uint8_t voice_manager_block_note(void);

#if SYNTH_ENABLE_SPLIT_RENDER
/* Core 0, immediately after voice_manager_render(): file this block's tap into
 * the slot that will reach the FX bus one block period from now. */
void voice_manager_stage_block_note(void);

/* Core 1, before the FX bus runs: make the slot belonging to the block now
 * being finished the one voice_manager_block_note() answers with.
 *
 * The pair is safe for the same reason the audio bus's two slots are, and it
 * is deliberately the same argument rather than a new one: each is called
 * exactly once per block by a stage that runs exactly once per block, so the
 * producer is always one slot ahead of the consumer and the two never name the
 * same one. Calling either from anywhere else breaks that, and would present
 * as a vocoder that retriggers on the wrong note. */
void voice_manager_take_block_note(void);
#endif""",
)
hdr.save("block-note pipeline hooks")

# --------------------------------------------------------------------------
# synth_voice.cpp -- the latch itself
# --------------------------------------------------------------------------
src = Editor("components/synth_core/synth_voice.cpp",
             skip_if="voice_manager_stage_block_note")
src.sub(
    """/* Note-start tap (S43) -- see voice_manager_block_note(). Written and cleared
 * inside drain_events(), read later in the same render callback by the FX
 * bus, both on the audio task, so a plain byte is the whole synchronisation
 * story. */
uint8_t s_block_note = 0;""",
    """/* Note-start tap (S43) -- see voice_manager_block_note(). Written and cleared
 * inside drain_events(), read later in the same render callback by the FX
 * bus, both on the audio task, so a plain byte is the whole synchronisation
 * story. */
uint8_t s_block_note = 0;

#if SYNTH_ENABLE_SPLIT_RENDER
/* ...except when those two are not the same callback (S45). The voice stage
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
uint8_t s_note_fx = 0;   /* core 1 only: what the FX bus is answered with */
#endif""",
)

src.sub(
    """extern "C" uint8_t SYNTH_RENDER_IRAM voice_manager_block_note(void) {
    return s_block_note;
}""",
    """extern "C" uint8_t SYNTH_RENDER_IRAM voice_manager_block_note(void) {
#if SYNTH_ENABLE_SPLIT_RENDER
    /* The FX bus is the only caller, and it is on core 1, so this answers with
     * what the pipeline latched for the block core 1 is finishing -- never
     * with s_block_note, which by now belongs to the block core 0 is building. */
    return s_note_fx;
#else
    return s_block_note;
#endif
}

#if SYNTH_ENABLE_SPLIT_RENDER
extern "C" void SYNTH_RENDER_IRAM voice_manager_stage_block_note(void) {
    s_note_slot[s_note_prod] = s_block_note;
    s_note_prod ^= 1;
}

extern "C" void SYNTH_RENDER_IRAM voice_manager_take_block_note(void) {
    s_note_fx = s_note_slot[s_note_cons];
    s_note_cons ^= 1;
}
#endif""",
)
src.save("block-note slot latch")

# --------------------------------------------------------------------------
# main.cpp -- call them from the two stages
# --------------------------------------------------------------------------
m = Editor("main/main.cpp", skip_if="voice_manager_stage_block_note")
m.sub(
    """static void SYNTH_RENDER_IRAM render_stage_a(float* out_l, float* out_r,
                                             size_t frames, void* ctx) {
    const uint32_t c0 = esp_cpu_get_cycle_count();
    chain_voices(out_l, out_r, frames, ctx);
    audio_io_report_stage_voices(esp_cpu_get_cycle_count() - c0);
}""",
    """static void SYNTH_RENDER_IRAM render_stage_a(float* out_l, float* out_r,
                                             size_t frames, void* ctx) {
    const uint32_t c0 = esp_cpu_get_cycle_count();
    chain_voices(out_l, out_r, frames, ctx);
    /* The one piece of state that has to cross the cut: the note-start tap the
     * vocoder retriggers on, produced here and read by the FX bus a block
     * later. Inside the measured window because it is part of the stage's
     * work, and after chain_voices() because that is what fills it. */
    voice_manager_stage_block_note();
    audio_io_report_stage_voices(esp_cpu_get_cycle_count() - c0);
}""",
)

m.sub(
    """    (void)ctx; /* the voice stage got it; nothing downstream needs one */
    const uint32_t c1 = esp_cpu_get_cycle_count();
    chain_pre_fx(out_l, out_r, frames);""",
    """    (void)ctx; /* the voice stage got it; nothing downstream needs one */
    const uint32_t c1 = esp_cpu_get_cycle_count();
    /* Before anything in this stage can ask for it, and in particular before
     * fx_process(): this is what makes voice_manager_block_note() answer for
     * the block being finished rather than the one core 0 is building. */
    voice_manager_take_block_note();
    chain_pre_fx(out_l, out_r, frames);""",
)
m.save("stage hooks for the note tap")
