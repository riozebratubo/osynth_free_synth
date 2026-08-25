/*
 * osynth — voice manager: polyphonic note allocation (Session 4; engine
 * render path, glide, unison and sustain pedal since Session 5).
 *
 * Control tasks push note events through the thread-safe entry points below;
 * the audio task consumes them inside voice_manager_render(), which drains a
 * lock-free ring at the start of every block, then renders each active voice
 * through the bound engine (synth_engine.h). The manager owns allocation,
 * retrigger/stealing, the sustain pedal, glide and unison fan-out, and turns
 * note + bend + detune into a per-voice frequency; the engine owns the DSP.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "synth_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the engine-common (0x01xx) parameters and clears the voice
 * pool. Call before voice_manager_set_engine() / audio_io_start(). */
esp_err_t voice_manager_init(void);

/* Binds the engine the voices render through: allocates and resets the
 * per-voice state pool (engine->voice_size x SYNTH_VOICES, internal RAM).
 * Control task. Only call while no engine is bound (boot, or after
 * voice_manager_detach_engine()) — the S6 switch protocol is
 * mute -> detach -> swap params -> set_engine -> unmute (engines.cpp). */
esp_err_t voice_manager_set_engine(const synth_engine_t* engine);

/* Unbinds the engine mid-stream: publishes the detach, then waits for the
 * audio task to pass two render boundaries so no in-flight block still
 * touches the engine or the voice pool, then frees the pool. Control task.
 * ESP_ERR_TIMEOUT (audio task stalled): the old engine stays bound. */
esp_err_t voice_manager_detach_engine(void);

/* Output mute with a ~10 ms ramp, applied after the voice sum — the "mute"
 * step of the engine switch. voice_manager_muted() reports when the ramp
 * has fully settled at silence. */
void voice_manager_set_muted(bool muted);
bool voice_manager_muted(void);

/* Event entry points — any control task (USB/serial MIDI, later seq/arp/
 * BLE), never the audio task. velocity is 1..127; velocity 0 is NOT treated
 * as note off — the MIDI router does that conversion. */
void voice_manager_note_on(uint8_t note, uint8_t velocity);

/* Note-start tap (S43): the velocity (1..127) of a note that *started a
 * voice* during the block currently being rendered, or 0 for "none did".
 * Several note-ons in one block report the loudest, so a chord reads as one
 * event, which is what a retrigger wants.
 *
 * The sibling of drums_block_hit(), with the same contract and the same
 * caveat about when it may be read: voice_manager_render() drains its event
 * queue at block start, and main.cpp calls it before fx_process(), so the FX
 * bus sees this block's note-ons. Reading it from anywhere else, or before
 * voice_manager_render() has run in this callback, gets the previous block.
 *
 * The vocoder's sample replay is the caller -- it restarts the recorded
 * phrase on each note. */
uint8_t voice_manager_block_note(void);
void voice_manager_note_off(uint8_t note);
void voice_manager_all_notes_off(void); /* release everything (CC 123) */
void voice_manager_all_sound_off(void); /* immediate silence (CC 120) */

/* Sustain pedal (CC 64): while down, note-offs are deferred until the pedal
 * lifts. */
void voice_manager_set_sustain(bool down);

/* Normalized pitch bend in [-1, 1]; scaled by the common.bend.range
 * parameter (semitones) at block rate. */
void voice_manager_set_pitch_bend(float bend_norm);

/* The raw normalized bend, for consumers that want it as a modulation
 * source rather than as pitch (the S9 matrix's `bend`, the S28 graph's
 * MidiSrc node). Any task. */
float voice_manager_pitch_bend(void);

/* Currently sounding voices — for the heartbeat log. */
size_t voice_manager_active_voices(void);

/* audio_render_fn-compatible; sums all active voices into out_l/out_r
 * (buffers arrive zeroed). Audio task only. */
void voice_manager_render(float* out_l, float* out_r, size_t frames, void* ctx);

#ifdef __cplusplus
}
#endif
