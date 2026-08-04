/*
 * osynth — master clock, arpeggiator and step sequencer.
 *
 * S12 built this as a 24-PPQN clock driving an arpeggiator and a
 * monophonic 32-step recorder. S23 kept the clock and the arpeggiator and
 * replaced the recorder with a multitrack sequencer (seq_model.h /
 * seq_play.h): up to 8 tracks x 256 steps x 8 patterns, per-step gate,
 * probability, micro-timing, ratchets and trig conditions, parameter locks,
 * per-track polymeter and direction, a scale quantiser and a song chain.
 * Tracks target either the synth engine (through the MIDI router, so the
 * voice manager cannot tell a sequenced note from a played one) or the drum
 * bus (components/drums).
 *
 * The clock now runs at 96 PPQN internally. External MIDI clock still
 * arrives at the standard 24 PPQN and is multiplied up by a small recovery
 * loop, so micro-timing and ratchets survive being slaved to a DAW.
 *
 * Parameters live in 0x04xx (docs/PARAM_MAP.md). Pattern *data* deliberately
 * does not: 16384 steps cannot be parameters. It travels over the BLE
 * sequencer opcodes (docs/BLE_PROTOCOL.md) and is persisted by the preset
 * system.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- transport and clock (S12) ---- */
#define SEQ_PID_TEMPO     0x0400
#define SEQ_PID_CLOCK_SRC 0x0401
#define SEQ_PID_DIV       0x0402 /* arpeggiator division */
#define SEQ_PID_GATE      0x0403 /* arpeggiator gate */

/* ---- sequencer feel and navigation (S23) ---- */
#define SEQ_PID_SWING      0x0404 /* float 0..75 %, 50 = straight */
#define SEQ_PID_PATTERN    0x0405 /* int, selected pattern (queued while running) */
#define SEQ_PID_SONG       0x0406 /* bool, follow the song chain */
#define SEQ_PID_SCALE      0x0407 /* enum, pattern scale */
#define SEQ_PID_ROOT       0x0408 /* int 0..11 */
#define SEQ_PID_FILL       0x0409 /* bool, live fill for trig conditions */
#define SEQ_PID_ACCENT     0x040A /* float 1..2, accent multiplier */
#define SEQ_PID_POS        0x040B /* int, read-only playhead (-1 = stopped) */
#define SEQ_PID_CURPAT     0x040C /* int, read-only playing pattern */
#define SEQ_PID_QUANT      0x040D /* enum off/1-4/1-8/1-16 record quantise */
#define SEQ_PID_EDIT_TRACK 0x040E /* int 1..tracks, the track being edited */
#define SEQ_PID_EDIT_STEP  0x040F /* int, edit/step-input cursor */

/* ---- arpeggiator (S12) ---- */
#define SEQ_PID_ARP_MODE  0x0410
#define SEQ_PID_ARP_OCT   0x0411
#define SEQ_PID_ARP_HOLD  0x0412

/* ---- transport mode + legacy length mirror ---- */
#define SEQ_PID_SEQ_MODE  0x0420 /* enum stop | play | rec */
#define SEQ_PID_SEQ_STEPS 0x0421 /* int, length of the edited track */
#define SEQ_PID_COUNTIN   0x0422 /* bool, 4-beat count-in before play/rec */
/* Read-only mirror of seq_model_revision(): "the pattern data changed".
 *
 * Pattern data is not parameter space and has no event opcode of its own, so
 * a control surface had no way to learn about a change it did not make — a
 * step recorded live, a Euclidean fill, a preset load. It could only re-read
 * on a gesture, which is why a note recorded into a drum lane appeared on the
 * app's grid when you left the track and came back, and not before.
 *
 * A read-only parameter needs neither: the S14 listener batches non-BLE writes
 * out at ~20 Hz already. Same mechanism `graph.rev` uses for the modular
 * patch. Only inequality is meaningful — see seq_model_revision(). */
#define SEQ_PID_REV       0x0423 /* int, read-only pattern-data revision */

/* ---- per-track live performance controls ----
 * Only mute and solo: everything else about a track is pattern data and is
 * edited over the sequencer opcodes. These two are here because they are
 * performance gestures that want to be reachable from a MIDI CC, the mod
 * matrix and a hardware button, all of which speak parameters. */
#define SEQ_PID_TRACK_BASE     0x0430
#define SEQ_PID_TRACK_STRIDE   2
#define SEQ_PID_TRACK_MUTE(t)  (SEQ_PID_TRACK_BASE + (t) * SEQ_PID_TRACK_STRIDE + 0)
#define SEQ_PID_TRACK_SOLO(t)  (SEQ_PID_TRACK_BASE + (t) * SEQ_PID_TRACK_STRIDE + 1)

/* Registers the 0x04xx params, allocates the pattern store, creates the
 * clock task and hooks the MIDI router's note tap + real-time callback.
 * Call before audio_io_start() (the S9 registration-race rule). */
esp_err_t seqarp_init(void);

/* Sequence snapshot access used by the preset system. The pattern-blob form
 * carries a whole pattern (all tracks, configuration and parameter locks);
 * seq_pattern_deserialize() also accepts the S12 32-step file, so sequences
 * saved before S23 still load. */
size_t seqarp_pattern_export(int pattern, void* buf, size_t cap);
bool seqarp_pattern_import(int pattern, const void* buf, size_t len);
size_t seqarp_pattern_max_bytes(void);

/* Republishes a pattern's scale/root/swing and its tracks' mute/solo state
 * into the live parameters. An import does this for the pattern it loaded; a
 * whole-set load (S27) needs it once more at the end, for whichever pattern
 * the set left selected. */
void seqarp_pattern_reflect(int pattern);

/* Beat clock, for anything that wants to start on the grid rather than the
 * instant a button was pressed — the looper's sync-to-sequencer and count-in
 * both hang off this. Fired on every quarter note from the `seq_clk` task
 * whether or not the sequencer is playing: the clock free-runs, so a looper
 * can be bar-locked with the sequencer stopped. `beat_in_bar` is 0..3 and 0
 * is the downbeat. Keep the callback short — it runs on the clock task.
 * One subscriber; registering again replaces it. */
typedef void (*seqarp_beat_fn)(int beat_in_bar, void* ctx);
void seqarp_set_beat_callback(seqarp_beat_fn fn, void* ctx);

/* Ticks per beat and the current position within the bar, so a subscriber can
 * work out how long it has until the next downbeat. */
int seqarp_ticks_per_beat(void);
int seqarp_beat_in_bar(void);

/* Which pattern the app/preset system is currently pointed at. */
int seqarp_edit_pattern(void);

/* Records a drum hit as a step, when — and only when — seq.mode is rec.
 * `slot` is a kit slot index; velocity 0 is ignored. Returns the step it wrote
 * to, or -1 (not armed, no lane that can play this slot, sequencer disabled).
 *
 * This exists because a drum hit deliberately never reaches the MIDI note tap:
 * midi_route_channel_message() hands drum-channel notes to the drum bus before
 * the tap so they cannot become arpeggiator input, and the app's pads use their
 * own opcode for velocity. Both therefore bypassed the recorder entirely —
 * arming rec and hitting pads recorded nothing at all. Callers that trigger a
 * drum outside the sequencer call this alongside; it is a no-op when not armed,
 * so there is nothing to guard at the call site.
 *
 * Safe from any control task. Not from the audio task — it writes the pattern
 * store and may touch the ParamStore. */
int seqarp_record_drum(int slot, uint8_t velocity);

#ifdef __cplusplus
}
#endif
