/*
 * osynth — chord mode (Session 41).
 *
 * One key in, several notes out. The expansion happens in the MIDI router
 * (components/midi/midi.c), which is the one place every note source meets:
 * the app's on-screen keyboard (BLE NOTE_ON), a USB MIDI controller, DIN
 * MIDI, the arpeggiator and the sequencer. Doing it in the app would have
 * chorded the app's own keys and left a plugged-in controller playing single
 * notes.
 *
 * Drums are not a concern here: midi_route_channel_message() hands notes on
 * `drums.midich` to the drum bus *before* anything below is reached, so a kit
 * slot can never be chorded.
 *
 * ---------------------------------------------------------------------------
 * Three modes (`chord.mode`)
 *
 *   free   one quality (`chord.type`) transposed under every key. C plays
 *          C-E-G, D plays D-F#-A. What a chord button on a home keyboard does.
 *   scale  the key picks a *degree* of `chord.scale`/`chord.root` and the
 *          chord is stacked in scale thirds, so the quality follows the
 *          degree: I maj, ii min, V dom7, vii dim. There is no wrong chord to
 *          play, which is the whole point of the mode.
 *   user   twelve slots, one per pitch class relative to the root, each an
 *          arbitrary interval list. The progression you actually wanted, laid
 *          out across the keyboard.
 *
 * `chord.size` is how many notes are stacked, and 1 is a legal answer: a
 * single note, still snapped into the scale. So "chord mode" covers plain
 * scale-locking too, and that is deliberate — it is the same mechanism with
 * one fewer tone.
 *
 * ---------------------------------------------------------------------------
 * Where it sits relative to the arpeggiator (`chord.route`)
 *
 *   pre    expand before the note tap. The arpeggiator's held-note list fills
 *          with the chord's *tones*, so one key gives a running arpeggio of
 *          that chord.
 *   post   expand after the tap. The arpeggiator steps through the keys you
 *          are holding and each step comes out as a block chord.
 *
 * The two are mutually exclusive by construction: chord_key_on() answers for
 * exactly one of CHORD_WHEN_PRE / CHORD_WHEN_POST and returns false for the
 * other, so the router can ask at both points without ever expanding twice.
 *
 * ---------------------------------------------------------------------------
 * Polyphony
 *
 * CONFIG_OSYNTH_VOICES is 8. A triad is three voices, so two keys reach the
 * ceiling and a third steals. `chord.keys` = mono makes a new key release the
 * previous key's chord, which is how most hardware chord modes behave and
 * what makes 8 voices generous rather than tight. Poly is the default and
 * leaves the voice manager's oldest-note stealing exactly as it was.
 *
 * ---------------------------------------------------------------------------
 * Live changes
 *
 * Every setting here applies to the chord already being held, not just to the
 * next one — set the root mid-chord and the chord moves. `chord.restrike`
 * decides how:
 *
 *   changed  play only the tones the change added, and leave the ones that
 *            survived completely alone. Dragging a control through five values
 *            morphs the chord instead of retriggering every voice five times.
 *   all      release the chord and play it again whole. How you hear a change
 *            *land*, and on a percussive patch the whole point.
 *
 * Either way a change that does not move a single tone emits nothing at all,
 * which is what keeps an unrelated write — strum, the route, a parameter lock
 * on something else in the block — from retriggering a sustained chord.
 *
 * ---------------------------------------------------------------------------
 * Note-off correctness
 *
 * The tones a key produced are remembered per key, so a note-off releases
 * exactly what that key started even if every setting moved while it was
 * held. Two keys can legitimately want the same tone (C and E in one scale
 * both reach G), so tones are reference counted across keys: releasing C
 * cannot cut a G that E is still holding.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- parameters (0x044x-0x045x, docs/PARAM_MAP.md) ---- */
#define CHORD_PID_ENABLE   0x0440 /* bool, default off */
#define CHORD_PID_MODE     0x0441 /* enum free / scale / user */
#define CHORD_PID_TYPE     0x0442 /* enum, quality used by free mode */
#define CHORD_PID_SCALE    0x0443 /* enum, shares the SEQ_SCALE_* table */
#define CHORD_PID_ROOT     0x0444 /* int 0..11 */
#define CHORD_PID_FOLLOW   0x0445 /* bool, mirror seq.scale / seq.root */
#define CHORD_PID_KEYMAP   0x0446 /* enum degrees / chromatic */
#define CHORD_PID_SIZE     0x0447 /* int 1..7 tones */
#define CHORD_PID_INV      0x0448 /* int 0..3 inversions */
#define CHORD_PID_VOICING  0x0449 /* enum close / drop2 / drop3 / open */
#define CHORD_PID_BASS     0x044A /* enum off / -1 oct / -2 oct */
#define CHORD_PID_STRUM    0x044B /* float 0..200 ms between tones */
#define CHORD_PID_STRUMDIR 0x044C /* enum up / down / alt / random */
#define CHORD_PID_VEL      0x044D /* float 0..1, velocity falloff upward */
#define CHORD_PID_LEAD     0x044E /* bool, auto voice-leading */
#define CHORD_PID_RANGE    0x044F /* int 0..48 semitones, 0 = no limit */
#define CHORD_PID_ROUTE    0x0450 /* enum pre / post (arpeggiator) */
#define CHORD_PID_KEYS     0x0451 /* enum poly / mono */
#define CHORD_PID_REV      0x0452 /* int, read-only user-set revision */
#define CHORD_PID_RESTRIKE 0x0453 /* enum changed / all, on a live change */

/* The whole block, for the preset system's range tests. */
#define CHORD_PID_FIRST 0x0440
#define CHORD_PID_LAST  0x0453

enum { CHORD_MODE_FREE = 0, CHORD_MODE_SCALE, CHORD_MODE_USER,
       CHORD_MODE_COUNT };
enum { CHORD_KEYMAP_DEGREES = 0, CHORD_KEYMAP_CHROMATIC };
enum { CHORD_VOICING_CLOSE = 0, CHORD_VOICING_DROP2, CHORD_VOICING_DROP3,
       CHORD_VOICING_OPEN };
enum { CHORD_BASS_OFF = 0, CHORD_BASS_OCT1, CHORD_BASS_OCT2 };
enum { CHORD_STRUM_UP = 0, CHORD_STRUM_DOWN, CHORD_STRUM_ALT,
       CHORD_STRUM_RAND };
enum { CHORD_ROUTE_PRE = 0, CHORD_ROUTE_POST };
enum { CHORD_KEYS_POLY = 0, CHORD_KEYS_MONO };
enum { CHORD_RESTRIKE_CHANGED = 0, CHORD_RESTRIKE_ALL };

/* Ceiling on the tones one key can produce: a 13th chord is seven, plus a
 * bass note. Also the width of the per-key release list. */
#define CHORD_MAX_NOTES 8

/* ---- the user chord set ----
 *
 * Twelve slots addressed by (played note - chord.root) mod 12, so the layout
 * transposes with the root instead of being nailed to absolute pitch classes:
 * one set is the same progression in every key.
 *
 * `count` 0 is a legal, useful entry — that key is silent, which is how you
 * keep a five-chord set from answering on the seven keys it does not use.
 */
#define CHORD_USER_SLOTS 12
#define CHORD_USER_IVS   6

typedef struct {
    int8_t transpose;          /* semitones added to the played key, -24..24 */
    uint8_t count;             /* 0 = silent, else intervals in use, 1..6 */
    int8_t iv[CHORD_USER_IVS]; /* semitones from the transposed key, -24..36 */
} chord_user_slot_t; /* 8 bytes; the whole set is 96 */

/* Registers the 0x044x parameters and creates the strum task. Call before
 * audio_io_start(), like every other registration (the S9 rule). */
esp_err_t chord_init(void);

/* ---- router integration (components/midi/midi.c) ----
 *
 * The dependency runs one way: chord_init() registers itself with the router
 * (midi_set_chord_hook) and plays its tones through midi_play_note(), so the
 * router knows nothing about chord mode — exactly the arrangement seqarp
 * already has with the note tap. The other direction would have closed a
 * cycle, because chord mode needs seqarp's scale tables and seqarp needs the
 * router.
 *
 * These mirror MIDI_CHORD_PRE / MIDI_CHORD_POST in midi.h; chord.cpp static
 * asserts that they still agree.
 */
enum { CHORD_WHEN_PRE = 0, CHORD_WHEN_POST };

/* Expands a key press. Returns true when chord mode took the key at this
 * point in the router — the caller must then do nothing more with it. False
 * means "not mine": chord mode is off, `allow` is false, or the configured
 * route is the other one.
 *
 * `allow` is the sequencer's per-track opt-in (SEQ_TRACK_F_CHORD) reaching
 * this far down: a track without the flag passes false and plays its notes
 * exactly as written.
 *
 * Safe from any control task. Not from the audio task. */
bool chord_key_on(uint8_t note, uint8_t vel, bool allow, int when);

/* Releases whatever that key started at that point in the router. Returns
 * true when the key was one of ours, in which case the caller must not send a
 * note-off of its own.
 *
 * Answers from the held table rather than from the live parameters — a key
 * held across a change of mode, route or `chord.enable` must still release the
 * notes it actually started. `allow` is the one live input it does honour, and
 * it has to: a note routed as final is by definition not a key this ever
 * expanded, and the arpeggiator's steps are exactly that. In the pre-arp
 * routing an arp step frequently *is* the played key's own pitch, so without
 * this the arpeggiator's first note-off would tear down the whole chord the
 * player is still holding. */
bool chord_key_off(uint8_t note, bool allow, int when);

/* Drops every held key and releases every tone still sounding. The router's
 * all-notes-off path calls this so a panic cannot leave the table holding
 * keys the voice manager has already forgotten. */
void chord_all_off(void);

/* ---- the user set (BLE opcode 0x3E, the app's editor) ---- */
void chord_user_get(int slot, chord_user_slot_t* out);
void chord_user_set(int slot, const chord_user_slot_t* in);
/* Back to the built-in set — the diatonic sevenths of a major key on the
 * seven scale degrees, the other five slots silent. The instrument-wide reset
 * (`state.reset`) calls this: the set is a blob, so a loop over parameter
 * defaults cannot reach it. */
void chord_user_reset(void);
/* Serialises the whole set for the working state, and reads one back. The
 * blob is exactly CHORD_USER_SLOTS * sizeof(chord_user_slot_t) bytes, which
 * both callers size their buffer from — an import of any other length is
 * refused rather than partially applied. */
size_t chord_user_export(void* buf, size_t cap);
bool chord_user_import(const void* buf, size_t len);

#ifdef __cplusplus
}
#endif
