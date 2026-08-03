/*
 * osynth — modular patch graph: the render path (Session 28).
 *
 * Everything here runs on the audio task, from a published Plan, and obeys
 * the project rule that nothing blocks the audio task: no locks, no
 * allocation, no registry lookups.
 *
 * **Node-major over voices.** The obvious loop is `for each voice: for each
 * node`, and it is the wrong one. It pays the dispatch, the parameter reads
 * and the coefficient math once per voice per node — eight times over at
 * full polyphony, for values that are identical across voices. The graph
 * topology is voice-invariant, so this renders `for each node: for all
 * voices`: buffers are [voice][frame], each node is entered once per block,
 * and the per-block work (a tanf for a filter coefficient, an envelope
 * coefficient set, a smoothing step) happens once instead of eight times.
 * The inner loop over voices also keeps one node's code hot in I-cache for
 * the whole visit rather than cycling through the entire patch per voice.
 *
 * **Switch dispatch, not function pointers.** The node kinds are dispatched
 * from one switch inside a single IRAM function. On the Xtensa windowed ABI
 * a call is not free, and a table of per-kind function pointers would pay
 * one per node per block plus a register-window spill; a switch lets the
 * compiler share the prologue, keep the plan walk in registers, and predict
 * well — the node sequence is identical block after block.
 *
 * **Only the active voices.** The voice manager passes the voices that are
 * actually sounding, so a two-note chord costs two voices' worth of graph,
 * exactly as the per-voice engines do today. Buffers are allocated for the
 * maximum but only the first n rows are ever touched.
 *
 * **Plan swaps duck.** Adopting a new plan mid-note changes which buffer a
 * node reads; there is no way to make that continuous, so the render ramps
 * the output down over ~8 blocks, adopts, and ramps back up. A cable edit
 * therefore costs ~20 ms of duck — the same thing you hear patching a real
 * modular — and never a click. Parameter edits do not rebuild the plan and
 * do not duck: turning a knob is the common case and stays free.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "synth_config.h"
#include "synth_dsp.h"
#include "synth_engine.h"

#include "graph_compile.h"

namespace osynth::graph {

/* Per-node, per-voice state. A union because a slot holds one kind at a
 * time; sized by the widest member. The S33 filters widened it from 12 to
 * 24 bytes (dsp::Vowel, three SVFs), so a twelve-node voice is now under
 * 300 bytes and the whole pool ~2.3 KB at full polyphony — the cost is paid
 * by every patch whether or not it uses one, which is what a union means. */
union NodeState {
    dsp::Osc osc;
    dsp::Noise noise;
    dsp::Svf svf;
    dsp::Svf2 svf2; /* Filter24 and Dual: two SVFs either way */
    dsp::Ladder ladder;
    dsp::Vowel vowel;
    dsp::Adsr adsr;
    dsp::Lfo lfo;
    struct {
        float held;
        float phase;
        dsp::Noise rng;
    } sah;
    float f;
};

struct VoiceState {
    NodeState n[kMaxNodes];
    uint8_t note = 60;
    float vel = 0.0f;
    float gate = 0.0f;  /* 1 while held — the MidiSrc "gate" source */
    float rnd = 0.0f;   /* one random value per note-on, 0..1 */
};

/* ---- lifecycle (control task) ---- */

/* Allocates the audio buffer pool from internal RAM. Idempotent. */
esp_err_t render_init();
void render_deinit();

/* Publishes a compiled plan. Returns once the audio task has adopted it (or
 * immediately if the audio task is not running), so the caller may then
 * safely retire the parameter range the *previous* plan pointed at. This is
 * the same guarantee voice_manager_detach_engine() provides for the engine
 * pointer, and for the same reason. */
esp_err_t publish(const Plan& p);

/* Drops the current plan and waits for the audio task to be rendering
 * silence. Call before unregistering a parameter range that a live plan
 * resolved: after this returns, no plan holds those pointers. */
esp_err_t retire();

/* ---- render (audio task) ---- */

/* Renders every active voice through the published plan and mixes into
 * out_l/out_r. `states`/`frames` are parallel arrays of length n_voices.
 *
 * IRAM-resident, but the attribute lives on the *definition* only — the
 * same convention voice_manager_render() and the FX bus follow. GCC mints a
 * fresh section name per occurrence of the attribute (.iram1.0, .iram1.1,
 * …), so repeating it on the declaration is a hard error under -Werror,
 * not a redundancy. */
void render_block(void* const* states, const synth_voice_frame_t* frames,
                  size_t n_voices, float* out_l, float* out_r, size_t n);

/* Per-voice hooks, called by the engine vtable. */
void voice_reset(VoiceState& v);
void note_on(VoiceState& v, uint8_t note, float vel01, bool was_sounding);
void note_off(VoiceState& v);
bool voice_busy(const VoiceState& v);
float voice_level(const VoiceState& v);

/* Cost of the live plan in the units of graph_compile.h, for the app's
 * meter and the heartbeat. */
uint16_t live_cost();

} // namespace osynth::graph
