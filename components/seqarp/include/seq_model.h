/*
 * osynth — sequencer data model (Session 23).
 *
 * The pattern store: steps, per-track configuration, parameter locks and the
 * song chain. Deliberately separate from playback (seq_play.cpp) and from
 * the clock (seqarp.cpp), because three different kinds of task touch it —
 * the BLE command task edits it, the `seq_clk` task reads it every tick, and
 * the preset task serialises it — and keeping the ownership rules in one
 * file is the only way that stays reviewable.
 *
 * Sizing is capability-gated like the rest of the firmware. With PSRAM the
 * full spec fits comfortably (8 patterns x 8 tracks x 256 steps x 8 B =
 * 128 KB, allocated in PSRAM because only control tasks ever touch it); on
 * the classic ESP32 the same structures are built at a quarter of the size
 * out of internal RAM. Every per-step feature is present on both — what
 * shrinks is how many steps, tracks and patterns there are, not what a step
 * can do.
 *
 * Concurrency: a single spinlock guards the whole model. Critical sections
 * are a handful of bytes long (copy one step, read one track config), the
 * tick path takes it a few dozen times a second, and the alternative —
 * per-field atomics on a 8-byte struct that must be read consistently —
 * would be both slower and wrong. Bulk edits take the lock per step, never
 * across a whole BLE frame, so a 64-step write can never stall a tick.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- capacities ---- */
#define SEQ_MAX_STEPS     256
#define SEQ_DEFAULT_STEPS 64
#define SEQ_SONG_MAX      32

#if CONFIG_SPIRAM
#define SEQ_TRACKS   8
#define SEQ_PATTERNS 8
#define SEQ_PLOCKS   256
#else
#define SEQ_TRACKS   4
#define SEQ_PATTERNS 2
#define SEQ_PLOCKS   64
#endif

/* ---- one step ----
 * Eight bytes, and every byte earns its place: this struct is multiplied by
 * SEQ_MAX_STEPS x SEQ_TRACKS x SEQ_PATTERNS, so a ninth byte would cost
 * 16 KB on the S3.
 *
 *   vel == 0 means "no trig" — that is the empty/rest encoding, which is why
 *   velocity starts at 1. Everything else only matters when vel != 0.
 */
typedef struct {
    uint8_t note;    /* 0..127, MIDI note (drum lanes ignore it unless the
                      * track is in note-to-slot mode) */
    uint8_t vel;     /* 0 = empty, 1..127 */
    uint8_t gate;    /* length in 1/16 of a step: 16 = exactly one step,
                      * 255 ~= 16 steps (a tie across the bar) */
    uint8_t prob;    /* 0..100 % chance the trig fires */
    int8_t micro;    /* -50..+50 % of a step, sub-step nudge */
    uint8_t ratchet; /* 1..8 evenly-spaced retriggers inside the step */
    uint8_t cond;    /* SEQ_COND_*, trig condition */
    uint8_t flags;   /* SEQ_STEP_F_* */
} seq_step_t;

enum {
    SEQ_STEP_F_ACCENT = 1 << 0, /* velocity x accent amount */
    SEQ_STEP_F_SLIDE  = 1 << 1, /* glide into this note (synth targets) */
    SEQ_STEP_F_MUTE   = 1 << 2, /* keep the data, skip the trig */
    SEQ_STEP_F_PLOCK  = 1 << 3, /* a lock exists for this step (a hint: the
                                 * lock table is authoritative) */
};

/* Trig conditions. The x:y family fires on repetition x of every y passes of
 * the track; FILL follows the seq.fill parameter; PREV chains off whether the
 * previous conditional trig on the same track fired. */
enum {
    SEQ_COND_ALWAYS = 0,
    SEQ_COND_1_2, SEQ_COND_2_2,
    SEQ_COND_1_3, SEQ_COND_2_3, SEQ_COND_3_3,
    SEQ_COND_1_4, SEQ_COND_2_4, SEQ_COND_3_4, SEQ_COND_4_4,
    SEQ_COND_1_8, SEQ_COND_2_8, SEQ_COND_4_8, SEQ_COND_8_8,
    SEQ_COND_FILL, SEQ_COND_NOT_FILL,
    SEQ_COND_PREV, SEQ_COND_NOT_PREV,
    SEQ_COND_COUNT
};

/* ---- per-track configuration (part of the pattern) ---- */
enum {
    SEQ_TARGET_SYNTH = 0, /* notes into the MIDI router -> active engine */
    SEQ_TARGET_DRUM,      /* hits into the drum bus */
    SEQ_TARGET_COUNT
};

enum {
    SEQ_DIR_FWD = 0,
    SEQ_DIR_REV,
    SEQ_DIR_PINGPONG,
    SEQ_DIR_RANDOM,
    SEQ_DIR_BROWNIAN, /* random walk of +/-1..2 steps */
    SEQ_DIR_COUNT
};

/* Clock divisions, in 96-PPQN ticks per step. Index into seq_div_ticks(). */
enum {
    SEQ_DIV_1_1 = 0, SEQ_DIV_1_2, SEQ_DIV_1_4, SEQ_DIV_1_4T,
    SEQ_DIV_1_8, SEQ_DIV_1_8T, SEQ_DIV_1_16, SEQ_DIV_1_16T,
    SEQ_DIV_1_32, SEQ_DIV_1_32T, SEQ_DIV_COUNT
};

/* Drum lanes normally play one fixed slot; this in `slot` means "the step's
 * note picks the slot through the kit's note map" instead. */
#define SEQ_SLOT_FROM_NOTE 0xFF

typedef struct {
    uint8_t target;     /* SEQ_TARGET_* */
    uint8_t slot;       /* drum slot, or SEQ_SLOT_FROM_NOTE */
    uint16_t length;    /* 1..SEQ_MAX_STEPS — per track, so polymeter is free */
    uint8_t div;        /* SEQ_DIV_* */
    uint8_t dir;        /* SEQ_DIR_* */
    int8_t transpose;   /* -24..+24 semitones */
    uint8_t swing;      /* 0..75 %, 50 = straight; 0xFF = follow the pattern */
    uint8_t gate_scale; /* 0..200 % applied to every step's gate */
    uint8_t vel_scale;  /* 0..200 % applied to every step's velocity */
    uint8_t prob_scale; /* 0..100 % applied to every step's probability */
    uint8_t humanize;   /* 0..100 %, random velocity/timing jitter */
    uint8_t flags;      /* SEQ_TRACK_F_* */
    uint8_t scale;      /* 0xFF = follow the pattern */
    uint8_t root;       /* 0xFF = follow the pattern */
    uint8_t reserved;
} seq_track_cfg_t; /* 16 bytes */

enum {
    SEQ_TRACK_F_MUTE = 1 << 0,
    SEQ_TRACK_F_SOLO = 1 << 1,
    /* Chord mode (S41) expands this track's notes. Per track and not global,
     * because the useful case is one lane: a one-note-per-step bassline
     * becomes a chord progression while the lead lane beside it keeps playing
     * the melody as written. Off on every existing pattern, which is what a
     * new flag bit in a saved blob means, and that is the right default. */
    SEQ_TRACK_F_CHORD = 1 << 2,
};

/* ---- scales for the quantiser ---- */
enum {
    SEQ_SCALE_CHROMATIC = 0, SEQ_SCALE_MAJOR, SEQ_SCALE_MINOR,
    SEQ_SCALE_DORIAN, SEQ_SCALE_PHRYGIAN, SEQ_SCALE_LYDIAN,
    SEQ_SCALE_MIXOLYDIAN, SEQ_SCALE_LOCRIAN, SEQ_SCALE_HARM_MINOR,
    SEQ_SCALE_PENTA_MAJ, SEQ_SCALE_PENTA_MIN, SEQ_SCALE_BLUES,
    SEQ_SCALE_COUNT
};

typedef struct {
    uint16_t length;  /* master length; a track's own length wins */
    uint8_t scale;
    uint8_t root;     /* 0..11 */
    uint8_t swing;    /* 0..75 %, 50 = straight */
    uint8_t reserved[3];
    char name[12];
    seq_track_cfg_t track[SEQ_TRACKS];
} seq_pattern_cfg_t;

/* ---- parameter locks ----
 * Sparse: most steps lock nothing, and a dense {step x param} table would be
 * larger than the whole pattern store. A lock is applied when its step fires
 * and released when the same track next fires a step that does not lock that
 * parameter (or when the transport stops) — the pre-lock value is stashed by
 * the playback engine, so locks compose with live knob edits.
 */
typedef struct {
    uint8_t pattern;
    uint8_t track;
    uint16_t step;
    uint16_t pid;
    uint16_t reserved;
    float value;
} seq_plock_t; /* 12 bytes */

typedef struct {
    uint8_t pattern;
    uint8_t repeats; /* 1..64 */
} seq_song_entry_t;

/* ---- lifecycle ---- */

/* Allocates the pattern store (PSRAM when available) and fills it with an
 * empty, musically sane default: every track SEQ_DEFAULT_STEPS long, track 1
 * targeting the synth, the rest targeting drum slots. Returns ESP_ERR_NO_MEM
 * if the store cannot be allocated, in which case the sequencer stays
 * disabled and the rest of the firmware runs untouched. */
esp_err_t seq_model_init(void);
bool seq_model_ready(void);
size_t seq_model_bytes(void);

/* Monotonic counter, bumped by every function below that changes pattern data
 * — steps, track and pattern configuration, the generators, parameter locks
 * and the song chain. Only its *inequality* means anything: a reader that
 * remembers the value it last read alongside its copy of the data can tell
 * that copy is stale without comparing 128 KB.
 *
 * That reader is the app. Pattern data is not parameter space and has no event
 * opcode, so nothing told it when the firmware changed a pattern under it —
 * steps recorded live, a Euclidean fill, a preset load. seqarp mirrors this
 * into the read-only `seq.rev` parameter, which the S14 listener already
 * batches out at ~20 Hz, and the app re-reads when it moves. Exactly the trick
 * `graph.rev` plays for the modular patch (graph_model.h).
 *
 * Not a change *count*: a bulk edit bumps it once per step, and callers must
 * not read anything into the size of a jump. */
uint32_t seq_model_revision(void);

/* ---- step access ---- */
void seq_step_get(int pattern, int track, int step, seq_step_t* out);
void seq_step_set(int pattern, int track, int step, const seq_step_t* in);
void seq_step_clear(int pattern, int track, int step);
/* Toggle convenience for the app's grid: fills a cleared step with sensible
 * defaults (velocity 100, one-step gate, always, no ratchet) or clears it.
 * Returns true if the step is now filled. */
bool seq_step_toggle(int pattern, int track, int step, uint8_t note);

/* ---- track / pattern configuration ---- */
void seq_track_cfg_get(int pattern, int track, seq_track_cfg_t* out);
void seq_track_cfg_set(int pattern, int track, const seq_track_cfg_t* in);
void seq_pattern_cfg_get(int pattern, seq_pattern_cfg_t* out);
void seq_pattern_cfg_set(int pattern, const seq_pattern_cfg_t* in);
/* Effective length/swing/scale for a track, resolving the 0xFF "follow the
 * pattern" sentinels. Cheap enough for the tick path. */
int seq_track_length(int pattern, int track);
int seq_div_ticks(int div);

/* ---- editing helpers (used by BLE and the local UI) ---- */
void seq_pattern_clear(int pattern);
void seq_track_clear(int pattern, int track);
void seq_pattern_copy(int src, int dst);
/* Shifts a track's steps by `delta` positions, wrapping inside its length. */
void seq_track_rotate(int pattern, int track, int delta);
/* Fills a track with a Euclidean rhythm — `pulses` hits spread as evenly as
 * possible over `steps`, rotated by `rotate`. The single most useful
 * generator to have on a device with no keyboard. */
void seq_track_euclid(int pattern, int track, int pulses, int steps,
                      int rotate, uint8_t note, uint8_t vel);
/* Randomises a track's velocities/probabilities without touching its notes. */
void seq_track_humanize(int pattern, int track, int amount);

/* ---- parameter locks ---- */
int seq_plock_count(void);
bool seq_plock_get_at(int index, seq_plock_t* out);
/* Returns the number of locks written to `out` (up to `max`). */
int seq_plocks_for_step(int pattern, int track, int step, seq_plock_t* out,
                        int max);
/* Sets (or replaces) a lock. Passing NAN as `value` removes it. Returns
 * false when the pool is full. */
bool seq_plock_set(int pattern, int track, int step, uint16_t pid, float value);
void seq_plock_clear_step(int pattern, int track, int step);
void seq_plock_clear_pattern(int pattern);

/* ---- song chain ---- */
int seq_song_length(void);
void seq_song_get(int index, seq_song_entry_t* out);
void seq_song_set_length(int len);
void seq_song_set(int index, const seq_song_entry_t* in);

/* ---- scale quantiser ---- */
const char* seq_scale_name(int scale);
/* Snaps `note` into `scale` rooted at `root` (0..11). Chromatic is a no-op. */
uint8_t seq_quantize(uint8_t note, int scale, int root);
/* The scale as a 12-bit pitch-class set relative to its root, bit 0 = the
 * root. Chord mode (components/chord) needs the degrees themselves, not just
 * the nearest legal note, and one owner of these tables is better than two
 * copies that can disagree about what "dorian" means. Out-of-range asks
 * answer chromatic. */
uint16_t seq_scale_mask(int scale);

/* ---- serialisation (preset task) ----
 * Writes one pattern into `buf` in the on-disk format, returning the byte
 * count (0 if it does not fit). Only the live part is written: each track
 * stores its steps up to the last filled one, so an empty track costs a
 * couple of bytes. seq_pattern_deserialize() accepts this format, the S23
 * one that stored every step of every track, and the S12 32-step legacy
 * file. */
size_t seq_pattern_serialize(int pattern, void* buf, size_t cap);
bool seq_pattern_deserialize(int pattern, const void* buf, size_t len);
/* Upper bound for a serialised pattern, for sizing the preset task buffer. */
size_t seq_pattern_max_bytes(void);

#ifdef __cplusplus
}
#endif
