/*
 * osynth — MIDI parser and router (Session 4; seqarp taps Session 12).
 * Inputs: USB MIDI (S3), serial/DIN MIDI (optional, SYNTH_ENABLE_SERIAL_MIDI),
 * the sequencer/arpeggiator (S12), BLE NOTE_ON/OFF commands (S14).
 * Outputs: voice manager (notes, pitch bend), the seqarp note tap and
 * real-time callback (S12); future work: USB MIDI OUT.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the USB MIDI RX callback and starts serial MIDI if enabled. */
esp_err_t midi_init(void);

/* Routes one complete channel-voice message (status 0x8n..0xEn plus data
 * bytes; d2 = 0 for two-byte messages) to the voice manager. Omni mode: the
 * channel nibble is ignored. Safe from any control task — this is also the
 * entry point for the sequencer/arpeggiator/BLE later. */
void midi_route_channel_message(uint8_t status, uint8_t d1, uint8_t d2);

/* The same, for a caller that has already decided whether chord mode (S41)
 * may expand this note. `allow_chord` false plays it exactly as written.
 *
 * Two callers pass false, for the same underlying reason — the note is
 * already final:
 *
 *   the sequencer     chord expansion is a per-track opt-in
 *                     (SEQ_TRACK_F_CHORD), so a lane carrying a written-out
 *                     melody keeps playing what the pattern says while the
 *                     bass lane beside it is chorded.
 *   the arpeggiator   for the steps it plays out of a held list that already
 *                     *is* a chord's tones, which is what its list holds in
 *                     the pre-arp routing. Expanding those again would stack
 *                     a chord on every note of the arpeggio. In the post-arp
 *                     routing the same list holds played keys and the arp
 *                     passes true, which is what makes each step a block
 *                     chord. It tells the two apart by what the note tap
 *                     reported as `src` when the note arrived.
 *
 * A flag on the call rather than a mode the caller sets and clears: the
 * router is entered from four tasks at once, and any "current track" state
 * parked between them would be read by whichever note arrived in between. */
void midi_route_note(uint8_t status, uint8_t d1, uint8_t d2, bool allow_chord);

/* Routes one System Real-Time byte (0xF8 clock / 0xFA start / 0xFB continue /
 * 0xFC stop) to the seqarp callback below; other real-time bytes (active
 * sensing, reset) are ignored. Called by the USB and serial inputs (S12). */
void midi_route_realtime(uint8_t status);

/* seqarp hooks (S12), registered once by seqarp_init(). The note tap sees
 * every incoming note on/off before the voice manager and returns true to
 * consume the event (the arpeggiator's key input; the sequencer's recorder
 * observes without consuming). Notes emitted by seqarp itself re-enter
 * midi_route_channel_message() and must pass the tap untouched — seqarp
 * guards on its own task handle.
 *
 * `src` (S41) says where the note came from, and two consumers need it for
 * two different reasons:
 *
 *   the recorder    stores PLAYED and CHORD_ROOT and drops CHORD_TONE. A
 *                   chord is one key press and belongs in one step; without
 *                   this a press wrote three notes into the same step and the
 *                   last one won.
 *   the arpeggiator remembers whether a held note arrived already expanded,
 *                   so its own steps can be routed back as final. In the
 *                   pre-arp routing its held list *is* the chord's tones, and
 *                   without this each step would be expanded a second time —
 *                   a chord stacked on every note of the arpeggio. */
enum {
    MIDI_NOTE_PLAYED = 0,  /* a key, a pad, an external note: not expanded */
    MIDI_NOTE_CHORD_ROOT,  /* chord mode's tone standing for the played key */
    MIDI_NOTE_CHORD_TONE,  /* one of the other tones of that chord */
};
typedef bool (*midi_note_tap_fn)(uint8_t note, uint8_t velocity, bool on,
                                 int src, void* ctx);
void midi_set_note_tap(midi_note_tap_fn fn, void* ctx);

/* Drum tap: a note the drum bus claimed on `drums.midich`, reported *after* it
 * has been played. Those notes never reach the note tap above — the router
 * gives them to the drum bus first, on purpose, so a drum note cannot become
 * arpeggiator input — which also meant the sequencer's recorder never saw one.
 * This is that path back, and only that: it observes, it cannot consume, and
 * the hit sounds either way. Registered by seqarp_init(); the drum component
 * does not know the sequencer exists, and this keeps it that way. */
typedef void (*midi_drum_tap_fn)(uint8_t note, uint8_t velocity, void* ctx);
void midi_set_drum_tap(midi_drum_tap_fn fn, void* ctx);

/* Real-time callback: receives 0xF8/0xFA/0xFB/0xFC on the input's task. */
typedef void (*midi_realtime_fn)(uint8_t status, void* ctx);
void midi_set_realtime_callback(midi_realtime_fn fn, void* ctx);

/* ---- chord mode hook (S41, components/chord) ----
 *
 * Registered by chord_init(), consulted at two points in the note path, and
 * unset on a build without chord mode — in which case the router behaves
 * exactly as it did before it existed.
 *
 * `when` says which point:
 *   MIDI_CHORD_PRE      before the note tap. Chord tones land in the
 *                       arpeggiator's held list, so one key gives a running
 *                       arpeggio of that chord.
 *   MIDI_CHORD_POST     after the tap declined the note. The arpeggiator
 *                       steps through the keys and each step is a block chord.
 *   MIDI_CHORD_ALL_OFF  panic: drop every key the hook is holding. `note`,
 *                       `velocity`, `on` and `allow` are all meaningless.
 *
 * Returns true when the hook took the event, in which case the router does
 * nothing further with it. Chord mode answers for exactly one of PRE/POST
 * depending on `chord.route`, so asking at both can never expand twice. */
enum {
    MIDI_CHORD_PRE = 0,
    MIDI_CHORD_POST = 1,
    MIDI_CHORD_ALL_OFF = 2,
};
typedef bool (*midi_chord_fn)(uint8_t note, uint8_t velocity, bool on,
                              bool allow, int when, void* ctx);
void midi_set_chord_hook(midi_chord_fn fn, void* ctx);

/* Plays one note the way the router would have, with the chord hook already
 * consulted — chord mode's own tones come back through here. `pre` selects
 * the note-tap path (the arpeggiator and the sequencer's recorder see the
 * tone) over going straight to the voice manager.
 *
 * `src` is MIDI_NOTE_CHORD_ROOT for the tone standing for the played key and
 * MIDI_NOTE_CHORD_TONE for the rest — see the note tap above for what the two
 * consumers do with that. */
void midi_play_note(uint8_t note, uint8_t velocity, bool on, bool pre,
                    int src);

#ifdef __cplusplus
}
#endif
