/*
 * osynth — sequencer data model (Session 23). Contract in seq_model.h.
 */
#include "seq_model.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "synth_pack.h"

static const char* TAG = "seqmodel";

namespace {

portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

seq_step_t* s_steps = nullptr; /* [pattern][track][step], one flat block */
seq_pattern_cfg_t s_pattern[SEQ_PATTERNS];
seq_plock_t s_plock[SEQ_PLOCKS];
int s_plock_count = 0;
seq_song_entry_t s_song[SEQ_SONG_MAX];
int s_song_len = 0;
size_t s_bytes = 0;

/* Bumped by every function below that changes pattern data. Its only job is to
 * let a reader ask "is what I last read still current?" without diffing 128 KB
 * — the app has no other way to find out, because pattern data is not
 * parameter space and there is no event opcode carrying it.
 *
 * Deliberately outside the spinlock and deliberately not exact. A bulk edit
 * bumps once per step, and a reader that catches the counter mid-burst simply
 * reads again on the next one; what has to hold is only that the value
 * *differs* from what a stale reader is holding, and a monotonic counter gives
 * that for free. Relaxed ordering for the same reason: it guards nothing.
 *
 * The three tasks that mutate the store — BLE commands, the clock task's live
 * recording, the preset task's loads — all go through the functions below, so
 * there is no fourth path that could change data without moving this. */
std::atomic<uint32_t> s_revision{0};

inline void touch() { s_revision.fetch_add(1, std::memory_order_relaxed); }

uint32_t s_rng = 0x9e3779b9u;

uint32_t rng_next() {
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return s_rng = x;
}

inline size_t step_index(int pattern, int track, int step) {
    return ((size_t)pattern * SEQ_TRACKS + (size_t)track) * SEQ_MAX_STEPS +
           (size_t)step;
}

inline bool valid(int pattern, int track, int step) {
    return s_steps != nullptr && pattern >= 0 && pattern < SEQ_PATTERNS &&
           track >= 0 && track < SEQ_TRACKS && step >= 0 &&
           step < SEQ_MAX_STEPS;
}

/* Bulk edits walk the step store in bounded chunks rather than holding the
 * spinlock across a whole track. The store is 128 KB of PSRAM on the S3, and
 * a 2 KB memset — or a 256-entry scatter — with interrupts disabled is tens
 * of microseconds of jitter for everything else on core 0, the clock task
 * included. This is the header's "bulk edits take the lock per step, never
 * across a whole BLE frame" rule applied to the generators too, which is
 * where it had quietly stopped holding. A reader that catches a track
 * half-cleared or half-rotated sees steps that are on their way to exactly
 * that state; the lock table, which cannot survive a torn update, still gets
 * one section to itself. */
constexpr int kBulkChunk = 16;
static_assert(SEQ_MAX_STEPS % kBulkChunk == 0,
              "the whole-track passes below assume an exact split");

/* Ticks per step at 96 PPQN. A quarter note is 96 ticks; triplet divisions
 * divide by three, which is exactly why 96 was chosen over 24 — 1/16T needs
 * 8 ticks and would have been 2 at 24 PPQN, leaving no room for micro-timing
 * or ratchets inside a step. */
const uint16_t kDivTicks[SEQ_DIV_COUNT] = {
    384, /* 1/1  */
    192, /* 1/2  */
    96,  /* 1/4  */
    64,  /* 1/4T */
    48,  /* 1/8  */
    32,  /* 1/8T */
    24,  /* 1/16 */
    16,  /* 1/16T */
    12,  /* 1/32 */
    8,   /* 1/32T */
};

/* Scale tables as semitone masks over an octave; bit n set = degree n is in
 * the scale. A mask beats an interval list here: quantising is then a search
 * outward from the played note, which is branch-light and never allocates. */
const uint16_t kScaleMask[SEQ_SCALE_COUNT] = {
    0x0FFF, /* chromatic    C C# D D# E F F# G G# A A# B */
    0x0AB5, /* major        0 2 4 5 7 9 11 */
    0x05AD, /* minor        0 2 3 5 7 8 10 */
    0x06AD, /* dorian       0 2 3 5 7 9 10 */
    0x05AB, /* phrygian     0 1 3 5 7 8 10 */
    0x0AD5, /* lydian       0 2 4 6 7 9 11 */
    0x06B5, /* mixolydian   0 2 4 5 7 9 10 */
    0x055B, /* locrian      0 1 3 5 6 8 10 */
    0x09AD, /* harmonic min 0 2 3 5 7 8 11 */
    0x0295, /* penta major  0 2 4 7 9 */
    0x04A9, /* penta minor  0 3 5 7 10 */
    0x04E9, /* blues        0 3 5 6 7 10 */
};

const char* const kScaleNames[SEQ_SCALE_COUNT] = {
    "chromatic", "major", "minor", "dorian", "phrygian", "lydian",
    "mixolydian", "locrian", "harm minor", "penta maj", "penta min", "blues",
};

void default_track(seq_track_cfg_t* t, int index) {
    memset(t, 0, sizeof(*t));
    t->length = SEQ_DEFAULT_STEPS;
    t->div = SEQ_DIV_1_16;
    t->dir = SEQ_DIR_FWD;
    t->transpose = 0;
    t->swing = 0xFF;      /* follow the pattern */
    t->gate_scale = 100;
    t->vel_scale = 100;
    t->prob_scale = 100;
    t->humanize = 0;
    t->scale = 0xFF;
    t->root = 0xFF;
    t->flags = 0;
    /* Track 1 plays the synth engine; the rest are drum lanes on the first
     * kit slots, so a fresh pattern is immediately useful with no setup. */
    if (index == 0) {
        t->target = SEQ_TARGET_SYNTH;
        t->slot = 0;
    } else {
        t->target = SEQ_TARGET_DRUM;
        t->slot = (uint8_t)(index - 1);
    }
}

void default_pattern(seq_pattern_cfg_t* p, int index) {
    memset(p, 0, sizeof(*p));
    p->length = SEQ_DEFAULT_STEPS;
    p->scale = SEQ_SCALE_CHROMATIC;
    p->root = 0;
    p->swing = 50;
    /* The bound has to be visible to the compiler, not just true: `index` is
     * a plain int here and -Wformat-truncation assumes the full range, so a
     * two-digit modulo is what makes "pattern NN" provably fit name[12]. */
    const unsigned n = (unsigned)(index + 1) % 100u;
    snprintf(p->name, sizeof(p->name), "pattern %u", n);
    for (int t = 0; t < SEQ_TRACKS; ++t) default_track(&p->track[t], t);
}

} // namespace

/* ======================= lifecycle ===================================== */

esp_err_t seq_model_init(void) {
    const size_t bytes =
        (size_t)SEQ_PATTERNS * SEQ_TRACKS * SEQ_MAX_STEPS * sizeof(seq_step_t);
#if CONFIG_SPIRAM
    /* Only control tasks touch the pattern store — the audio task never sees
     * it — so PSRAM is free real estate here and leaves internal RAM for the
     * things that must not miss a deadline. */
    s_steps = (seq_step_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (s_steps == nullptr) {
        ESP_LOGW(TAG, "no PSRAM for %u KB of patterns — trying internal RAM",
                 (unsigned)(bytes / 1024));
        s_steps = (seq_step_t*)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL);
    }
#else
    s_steps = (seq_step_t*)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL);
#endif
    if (s_steps == nullptr) {
        ESP_LOGE(TAG, "cannot allocate %u KB for the pattern store",
                 (unsigned)(bytes / 1024));
        return ESP_ERR_NO_MEM;
    }
    memset(s_steps, 0, bytes);
    s_bytes = bytes;

    for (int p = 0; p < SEQ_PATTERNS; ++p) default_pattern(&s_pattern[p], p);
    s_plock_count = 0;
    s_song_len = 1;
    s_song[0].pattern = 0;
    s_song[0].repeats = 1;

    /* Hoisted out of the ESP_LOGI() argument list below. A preprocessing
     * directive inside a macro invocation is undefined behaviour in both C and
     * C++ (C++20 [cpp.replace]/11); GCC accepts it as an extension and MSVC
     * rejects it outright, which is how the host port found this one. */
#if CONFIG_SPIRAM
    const char* const pool = " PSRAM";
#else
    const char* const pool = " internal";
#endif
    ESP_LOGI(TAG,
             "up: %d patterns x %d tracks x %d steps (%u KB%s), %d p-lock "
             "slots, default length %d",
             SEQ_PATTERNS, SEQ_TRACKS, SEQ_MAX_STEPS,
             (unsigned)(bytes / 1024), pool,
             SEQ_PLOCKS, SEQ_DEFAULT_STEPS);
    return ESP_OK;
}

bool seq_model_ready(void) { return s_steps != nullptr; }
size_t seq_model_bytes(void) { return s_bytes; }

uint32_t seq_model_revision(void) {
    return s_revision.load(std::memory_order_relaxed);
}

/* ======================= step access =================================== */

void seq_step_get(int pattern, int track, int step, seq_step_t* out) {
    if (out == nullptr) return;
    if (!valid(pattern, track, step)) {
        memset(out, 0, sizeof(*out));
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    *out = s_steps[step_index(pattern, track, step)];
    taskEXIT_CRITICAL(&s_lock);
}

void seq_step_set(int pattern, int track, int step, const seq_step_t* in) {
    if (in == nullptr || !valid(pattern, track, step)) return;
    seq_step_t s = *in;
    /* Clamp on the way in so nothing downstream has to re-check: the tick
     * path treats these as trusted. */
    s.note &= 0x7F;
    s.vel &= 0x7F;
    if (s.gate == 0) s.gate = 16;
    if (s.prob > 100) s.prob = 100;
    if (s.micro > 50) s.micro = 50;
    if (s.micro < -50) s.micro = -50;
    if (s.ratchet == 0) s.ratchet = 1;
    if (s.ratchet > 8) s.ratchet = 8;
    if (s.cond >= SEQ_COND_COUNT) s.cond = SEQ_COND_ALWAYS;
    taskENTER_CRITICAL(&s_lock);
    s_steps[step_index(pattern, track, step)] = s;
    taskEXIT_CRITICAL(&s_lock);
    touch();
}

void seq_step_clear(int pattern, int track, int step) {
    if (!valid(pattern, track, step)) return;
    taskENTER_CRITICAL(&s_lock);
    memset(&s_steps[step_index(pattern, track, step)], 0, sizeof(seq_step_t));
    taskEXIT_CRITICAL(&s_lock);
    touch();
    seq_plock_clear_step(pattern, track, step);
}

bool seq_step_toggle(int pattern, int track, int step, uint8_t note) {
    if (!valid(pattern, track, step)) return false;
    bool filled;
    taskENTER_CRITICAL(&s_lock);
    seq_step_t& s = s_steps[step_index(pattern, track, step)];
    if (s.vel != 0) {
        memset(&s, 0, sizeof(s));
        filled = false;
    } else {
        s.note = note & 0x7F;
        s.vel = 100;
        s.gate = 16;
        s.prob = 100;
        s.micro = 0;
        s.ratchet = 1;
        s.cond = SEQ_COND_ALWAYS;
        s.flags = 0;
        filled = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    touch();
    if (!filled) seq_plock_clear_step(pattern, track, step);
    return filled;
}

/* ======================= configuration ================================= */

void seq_track_cfg_get(int pattern, int track, seq_track_cfg_t* out) {
    if (out == nullptr) return;
    if (pattern < 0 || pattern >= SEQ_PATTERNS || track < 0 ||
        track >= SEQ_TRACKS) {
        memset(out, 0, sizeof(*out));
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    *out = s_pattern[pattern].track[track];
    taskEXIT_CRITICAL(&s_lock);
}

void seq_track_cfg_set(int pattern, int track, const seq_track_cfg_t* in) {
    if (in == nullptr || pattern < 0 || pattern >= SEQ_PATTERNS || track < 0 ||
        track >= SEQ_TRACKS) {
        return;
    }
    seq_track_cfg_t c = *in;
    if (c.length < 1) c.length = 1;
    if (c.length > SEQ_MAX_STEPS) c.length = SEQ_MAX_STEPS;
    if (c.target >= SEQ_TARGET_COUNT) c.target = SEQ_TARGET_SYNTH;
    if (c.div >= SEQ_DIV_COUNT) c.div = SEQ_DIV_1_16;
    if (c.dir >= SEQ_DIR_COUNT) c.dir = SEQ_DIR_FWD;
    if (c.transpose > 24) c.transpose = 24;
    if (c.transpose < -24) c.transpose = -24;
    if (c.swing != 0xFF && c.swing > 75) c.swing = 75;
    if (c.gate_scale > 200) c.gate_scale = 200;
    if (c.vel_scale > 200) c.vel_scale = 200;
    if (c.prob_scale > 100) c.prob_scale = 100;
    if (c.humanize > 100) c.humanize = 100;
    if (c.scale != 0xFF && c.scale >= SEQ_SCALE_COUNT) c.scale = 0xFF;
    if (c.root != 0xFF && c.root > 11) c.root = 0xFF;
    taskENTER_CRITICAL(&s_lock);
    s_pattern[pattern].track[track] = c;
    taskEXIT_CRITICAL(&s_lock);
    touch();
}

void seq_pattern_cfg_get(int pattern, seq_pattern_cfg_t* out) {
    if (out == nullptr) return;
    if (pattern < 0 || pattern >= SEQ_PATTERNS) {
        memset(out, 0, sizeof(*out));
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    *out = s_pattern[pattern];
    taskEXIT_CRITICAL(&s_lock);
}

void seq_pattern_cfg_set(int pattern, const seq_pattern_cfg_t* in) {
    if (in == nullptr || pattern < 0 || pattern >= SEQ_PATTERNS) return;
    seq_pattern_cfg_t p = *in;
    if (p.length < 1) p.length = 1;
    if (p.length > SEQ_MAX_STEPS) p.length = SEQ_MAX_STEPS;
    if (p.scale >= SEQ_SCALE_COUNT) p.scale = SEQ_SCALE_CHROMATIC;
    if (p.root > 11) p.root = 0;
    if (p.swing > 75) p.swing = 75;
    p.name[sizeof(p.name) - 1] = '\0';
    taskENTER_CRITICAL(&s_lock);
    /* Track configs travel through seq_track_cfg_set(), which validates
     * them; preserve whatever is live rather than trusting this copy. */
    for (int t = 0; t < SEQ_TRACKS; ++t) p.track[t] = s_pattern[pattern].track[t];
    s_pattern[pattern] = p;
    taskEXIT_CRITICAL(&s_lock);
    touch();
}

int seq_track_length(int pattern, int track) {
    if (pattern < 0 || pattern >= SEQ_PATTERNS || track < 0 ||
        track >= SEQ_TRACKS) {
        return SEQ_DEFAULT_STEPS;
    }
    const int len = s_pattern[pattern].track[track].length;
    if (len < 1) return 1;
    if (len > SEQ_MAX_STEPS) return SEQ_MAX_STEPS;
    return len;
}

int seq_div_ticks(int div) {
    if (div < 0 || div >= SEQ_DIV_COUNT) div = SEQ_DIV_1_16;
    return kDivTicks[div];
}

/* ======================= editing helpers =============================== */

void seq_track_clear(int pattern, int track) {
    if (!valid(pattern, track, 0)) return;
    for (int s = 0; s < SEQ_MAX_STEPS; s += kBulkChunk) {
        taskENTER_CRITICAL(&s_lock);
        memset(&s_steps[step_index(pattern, track, s)], 0,
               (size_t)kBulkChunk * sizeof(seq_step_t));
        taskEXIT_CRITICAL(&s_lock);
    }
    /* The lock compaction keeps one section of its own: the swap-with-last
     * removal reshuffles indices, so re-reading the count outside the lock
     * between iterations would walk off the live entries. It is a bounded
     * scan of an internal-RAM table, not a PSRAM copy. */
    taskENTER_CRITICAL(&s_lock);
    for (int i = s_plock_count - 1; i >= 0; --i) {
        if (s_plock[i].pattern == pattern && s_plock[i].track == track) {
            s_plock[i] = s_plock[--s_plock_count];
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    touch();
}

void seq_pattern_clear(int pattern) {
    if (pattern < 0 || pattern >= SEQ_PATTERNS) return;
    for (int t = 0; t < SEQ_TRACKS; ++t) seq_track_clear(pattern, t);
    /* Built on the stack (default_pattern snprintf()s a name, which has no
     * business inside a critical section) and copied in under the lock. This
     * was the one writer in the file not honouring the file's own rule: the
     * tick path reads track configs through the lock, and memsetting ~150 live
     * bytes from outside it let a tick catch a half-reset track — a zero div
     * and a zero length, both of which the readers clamp, but neither of which
     * describes anything. The copy in is one struct, the same cost as
     * seq_pattern_cfg_set(). */
    seq_pattern_cfg_t fresh;
    default_pattern(&fresh, pattern);
    taskENTER_CRITICAL(&s_lock);
    s_pattern[pattern] = fresh;
    taskEXIT_CRITICAL(&s_lock);
    touch();
}

void seq_pattern_copy(int src, int dst) {
    if (src < 0 || src >= SEQ_PATTERNS || dst < 0 || dst >= SEQ_PATTERNS ||
        src == dst || s_steps == nullptr) {
        return;
    }
    for (int t = 0; t < SEQ_TRACKS; ++t) {
        for (int s = 0; s < SEQ_MAX_STEPS; ++s) {
            taskENTER_CRITICAL(&s_lock);
            s_steps[step_index(dst, t, s)] = s_steps[step_index(src, t, s)];
            taskEXIT_CRITICAL(&s_lock);
        }
    }
    taskENTER_CRITICAL(&s_lock);
    seq_pattern_cfg_t copy = s_pattern[src];
    memcpy(copy.name, s_pattern[dst].name, sizeof(copy.name));
    s_pattern[dst] = copy;

    /* Carry the source's locks over, dropping any the destination had. Both
     * halves run under one lock — the removal pass reshuffles indices, and
     * `base` must be the count *after* it for the copy pass to see only
     * pre-existing entries rather than the ones it is appending. */
    for (int i = s_plock_count - 1; i >= 0; --i) {
        if (s_plock[i].pattern == dst) s_plock[i] = s_plock[--s_plock_count];
    }
    const int base = s_plock_count;
    for (int i = 0; i < base && s_plock_count < SEQ_PLOCKS; ++i) {
        if (s_plock[i].pattern != src) continue;
        seq_plock_t l = s_plock[i];
        l.pattern = (uint8_t)dst;
        s_plock[s_plock_count++] = l;
    }
    taskEXIT_CRITICAL(&s_lock);
    touch();
}

void seq_track_rotate(int pattern, int track, int delta) {
    if (!valid(pattern, track, 0) || delta == 0) return;
    const int len = seq_track_length(pattern, track);
    if (len < 2) return;
    int d = delta % len;
    if (d < 0) d += len;
    if (d == 0) return;

    /* Static rather than a 2 KB stack array: this runs on the BLE command
     * task, and callers are serialised by being the only editor. */
    static seq_step_t tmp[SEQ_MAX_STEPS];
    for (int s = 0; s < len; s += kBulkChunk) {
        const int n = (len - s < kBulkChunk) ? len - s : kBulkChunk;
        taskENTER_CRITICAL(&s_lock);
        memcpy(&tmp[s], &s_steps[step_index(pattern, track, s)],
               (size_t)n * sizeof(seq_step_t));
        taskEXIT_CRITICAL(&s_lock);
    }
    for (int s = 0; s < len; s += kBulkChunk) {
        const int n = (len - s < kBulkChunk) ? len - s : kBulkChunk;
        taskENTER_CRITICAL(&s_lock);
        for (int i = s; i < s + n; ++i) {
            s_steps[step_index(pattern, track, (i + d) % len)] = tmp[i];
        }
        taskEXIT_CRITICAL(&s_lock);
    }
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < s_plock_count; ++i) {
        if (s_plock[i].pattern == pattern && s_plock[i].track == track &&
            s_plock[i].step < len) {
            s_plock[i].step = (uint16_t)((s_plock[i].step + d) % len);
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    touch();
}

void seq_track_euclid(int pattern, int track, int pulses, int steps,
                      int rotate, uint8_t note, uint8_t vel) {
    if (!valid(pattern, track, 0)) return;
    if (steps < 1) steps = seq_track_length(pattern, track);
    if (steps > SEQ_MAX_STEPS) steps = SEQ_MAX_STEPS;
    if (pulses < 0) pulses = 0;
    if (pulses > steps) pulses = steps;

    /* Bjorklund via the bucket method: walk the steps accumulating the pulse
     * ratio and place a hit whenever the accumulator rolls over. Same output
     * as the recursive formulation, a fraction of the code. */
    seq_step_t on = {};
    on.note = note & 0x7F;
    on.vel = vel ? (vel & 0x7F) : 100;
    on.gate = 16;
    on.prob = 100;
    on.ratchet = 1;
    on.cond = SEQ_COND_ALWAYS;

    for (int s = 0; s < SEQ_MAX_STEPS; s += kBulkChunk) {
        taskENTER_CRITICAL(&s_lock);
        memset(&s_steps[step_index(pattern, track, s)], 0,
               (size_t)kBulkChunk * sizeof(seq_step_t));
        taskEXIT_CRITICAL(&s_lock);
    }
    if (pulses > 0) {
        int acc = 0;
        for (int i = 0; i < steps; ++i) {
            acc += pulses;
            if (acc >= steps) {
                acc -= steps;
                int at = i + rotate;
                at %= steps;
                if (at < 0) at += steps;
                taskENTER_CRITICAL(&s_lock);
                s_steps[step_index(pattern, track, at)] = on;
                taskEXIT_CRITICAL(&s_lock);
            }
        }
    }
    /* Length last: until the hits are placed, the track is better described
     * by its old length than by a new one over empty steps. */
    taskENTER_CRITICAL(&s_lock);
    s_pattern[pattern].track[track].length = (uint16_t)steps;
    taskEXIT_CRITICAL(&s_lock);
    touch();
}

void seq_track_humanize(int pattern, int track, int amount) {
    if (!valid(pattern, track, 0) || amount <= 0) return;
    if (amount > 100) amount = 100;
    const int len = seq_track_length(pattern, track);
    for (int i = 0; i < len; ++i) {
        taskENTER_CRITICAL(&s_lock);
        seq_step_t& s = s_steps[step_index(pattern, track, i)];
        if (s.vel != 0) {
            const int spread = amount * 40 / 100; /* up to +/-40 velocity */
            int v = (int)s.vel + (int)(rng_next() % (uint32_t)(2 * spread + 1)) -
                    spread;
            if (v < 1) v = 1;
            if (v > 127) v = 127;
            s.vel = (uint8_t)v;
            const int mspread = amount * 12 / 100; /* up to +/-12 % of a step */
            if (mspread > 0) {
                int m = (int)(rng_next() % (uint32_t)(2 * mspread + 1)) - mspread;
                s.micro = (int8_t)m;
            }
        }
        taskEXIT_CRITICAL(&s_lock);
    }
    touch();
}

/* ======================= parameter locks =============================== */

namespace {

/* Parameters a lock must never target.
 *
 * A lock is applied every time its step fires, so anything that is a
 * *command* rather than a value fires with it: a lock on preset.load reloads
 * a patch sixteen times a bar, one on drums.trig machine-guns a slot, one on
 * engine.type runs the whole S6 switch protocol under the playhead. The
 * looper range and the transport/navigation parameters are excluded for the
 * same reason the preset system skips them — they are state, not sound.
 *
 * Spelled as literal ids rather than pulled from the owning components'
 * headers: this file sits below drums, looper and presets in the component
 * graph (presets already depends on seqarp), and the id map is a stable
 * contract — see docs/PARAM_MAP.md. */
bool plock_forbidden(uint16_t pid) {
    if (pid < 0x0010) return true; /* master.volume, engine.type, preset.* */
    if (pid >= 0x0600 && pid < 0x0700) return true; /* looper: transport + mix */
    switch (pid) {
        case 0x0401: /* seq.clock      — rig wiring, not music */
        case 0x0405: /* seq.pattern    — navigation */
        case 0x0406: /* seq.song */
        case 0x040B: /* seq.pos        — firmware-written telemetry */
        case 0x040C: /* seq.curpat */
        case 0x040E: /* seq.edit.track — editor cursors */
        case 0x040F: /* seq.edit.step */
        case 0x0420: /* seq.mode       — transport */
        case 0x0421: /* seq.steps */
        case 0x0423: /* seq.rev        — this counter; locking it would have a
                      * step overwrite the "did anything change?" signal with a
                      * fixed number, which is the one value that must never be
                      * fixed */
        case 0x0703: /* drums.kit */
        case 0x0704: /* drums.trig */
            return true;
        default:
            return false;
    }
}

} // namespace

int seq_plock_count(void) { return s_plock_count; }

bool seq_plock_get_at(int index, seq_plock_t* out) {
    if (out == nullptr || index < 0 || index >= s_plock_count) return false;
    taskENTER_CRITICAL(&s_lock);
    *out = s_plock[index];
    taskEXIT_CRITICAL(&s_lock);
    return true;
}

int seq_plocks_for_step(int pattern, int track, int step, seq_plock_t* out,
                        int max) {
    if (out == nullptr || max <= 0) return 0;
    int n = 0;
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < s_plock_count && n < max; ++i) {
        const seq_plock_t& l = s_plock[i];
        if (l.pattern == pattern && l.track == track && l.step == step) {
            out[n++] = l;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    return n;
}

bool seq_plock_set(int pattern, int track, int step, uint16_t pid,
                   float value) {
    if (!valid(pattern, track, step)) return false;
    const bool remove = std::isnan(value); /* the documented "remove" value */
    /* Removal is always allowed, so a file written before this check can
     * still be cleaned up; only creating one is refused. */
    if (!remove && plock_forbidden(pid)) {
        ESP_LOGW(TAG, "lock on 0x%04x refused: that parameter is a command or "
                      "transport state, not part of the sound", pid);
        return false;
    }
    bool ok = true;
    taskENTER_CRITICAL(&s_lock);
    int found = -1;
    for (int i = 0; i < s_plock_count; ++i) {
        if (s_plock[i].pattern == pattern && s_plock[i].track == track &&
            s_plock[i].step == step && s_plock[i].pid == pid) {
            found = i;
            break;
        }
    }
    if (remove) {
        if (found >= 0) s_plock[found] = s_plock[--s_plock_count];
    } else if (found >= 0) {
        s_plock[found].value = value;
    } else if (s_plock_count < SEQ_PLOCKS) {
        seq_plock_t& l = s_plock[s_plock_count++];
        l.pattern = (uint8_t)pattern;
        l.track = (uint8_t)track;
        l.step = (uint16_t)step;
        l.pid = pid;
        l.reserved = 0;
        l.value = value;
    } else {
        ok = false;
    }
    /* Keep the step's hint flag honest so the app can shade locked steps
     * without querying the lock table for every cell in the grid. */
    if (s_steps != nullptr) {
        seq_step_t& s = s_steps[step_index(pattern, track, step)];
        bool any = false;
        for (int i = 0; i < s_plock_count; ++i) {
            if (s_plock[i].pattern == pattern && s_plock[i].track == track &&
                s_plock[i].step == step) {
                any = true;
                break;
            }
        }
        if (any) {
            s.flags |= SEQ_STEP_F_PLOCK;
        } else {
            s.flags &= (uint8_t)~SEQ_STEP_F_PLOCK;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    touch();
    return ok;
}

void seq_plock_clear_step(int pattern, int track, int step) {
    taskENTER_CRITICAL(&s_lock);
    for (int i = s_plock_count - 1; i >= 0; --i) {
        if (s_plock[i].pattern == pattern && s_plock[i].track == track &&
            s_plock[i].step == step) {
            s_plock[i] = s_plock[--s_plock_count];
        }
    }
    if (valid(pattern, track, step)) {
        s_steps[step_index(pattern, track, step)].flags &=
            (uint8_t)~SEQ_STEP_F_PLOCK;
    }
    taskEXIT_CRITICAL(&s_lock);
    touch();
}

void seq_plock_clear_pattern(int pattern) {
    taskENTER_CRITICAL(&s_lock);
    for (int i = s_plock_count - 1; i >= 0; --i) {
        if (s_plock[i].pattern == pattern) s_plock[i] = s_plock[--s_plock_count];
    }
    taskEXIT_CRITICAL(&s_lock);
    touch();
}

/* ======================= song chain ==================================== */

int seq_song_length(void) { return s_song_len; }

void seq_song_get(int index, seq_song_entry_t* out) {
    if (out == nullptr) return;
    if (index < 0 || index >= SEQ_SONG_MAX) {
        out->pattern = 0;
        out->repeats = 1;
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    *out = s_song[index];
    taskEXIT_CRITICAL(&s_lock);
}

void seq_song_set_length(int len) {
    if (len < 1) len = 1;
    if (len > SEQ_SONG_MAX) len = SEQ_SONG_MAX;
    taskENTER_CRITICAL(&s_lock);
    for (int i = s_song_len; i < len; ++i) {
        s_song[i].pattern = 0;
        s_song[i].repeats = 1;
    }
    s_song_len = len;
    taskEXIT_CRITICAL(&s_lock);
    touch();
}

void seq_song_set(int index, const seq_song_entry_t* in) {
    if (in == nullptr || index < 0 || index >= SEQ_SONG_MAX) return;
    seq_song_entry_t e = *in;
    if (e.pattern >= SEQ_PATTERNS) e.pattern = 0;
    if (e.repeats < 1) e.repeats = 1;
    if (e.repeats > 64) e.repeats = 64;
    taskENTER_CRITICAL(&s_lock);
    /* Writing past the end extends the chain, so the entries skipped over have
     * to be filled the way seq_song_set_length() fills them. They were left as
     * they lay — zero on a fresh boot, which is `repeats` 0 — and
     * seq_play.cpp's pattern_boundary() states in a comment that both setters
     * floor repeats at 1 and advances on `++repeat >= e.repeats`, so a gap
     * entry burned a pass on pattern 1 that nobody put in the chain. */
    for (int i = s_song_len; i < index; ++i) {
        s_song[i].pattern = 0;
        s_song[i].repeats = 1;
    }
    s_song[index] = e;
    if (index >= s_song_len) s_song_len = index + 1;
    taskEXIT_CRITICAL(&s_lock);
    touch();
}

/* ======================= scale quantiser =============================== */

const char* seq_scale_name(int scale) {
    if (scale < 0 || scale >= SEQ_SCALE_COUNT) return "?";
    return kScaleNames[scale];
}

uint16_t seq_scale_mask(int scale) {
    if (scale < 0 || scale >= SEQ_SCALE_COUNT) return kScaleMask[0];
    return kScaleMask[scale];
}

uint8_t seq_quantize(uint8_t note, int scale, int root) {
    if (scale <= SEQ_SCALE_CHROMATIC || scale >= SEQ_SCALE_COUNT) return note;
    const uint16_t mask = kScaleMask[scale];
    const int r = ((root % 12) + 12) % 12;
    const int degree = (((int)note - r) % 12 + 12) % 12;
    if (mask & (1u << degree)) return note;
    /* Search outward: down first (a flattened note reads as intentional far
     * more often than a sharpened one), then up, never leaving 0..127. */
    for (int d = 1; d <= 6; ++d) {
        const int lo = (degree - d + 12) % 12;
        if (mask & (1u << lo)) {
            const int n = (int)note - d;
            if (n >= 0) return (uint8_t)n;
        }
        const int hi = (degree + d) % 12;
        if (mask & (1u << hi)) {
            const int n = (int)note + d;
            if (n <= 127) return (uint8_t)n;
        }
    }
    return note;
}

/* ======================= serialisation ================================= */

namespace {

constexpr uint32_t kMagic = 0x51455351; /* "QSEQ" little-endian */
/* v3 (S27) writes a per-track stored-step count after the track config and
 * stores only that many steps; v2 stored `tc.length` steps unconditionally.
 * Both are read (kVersionMin..kVersion) — a v2 slot saved by S23-S26 firmware
 * still loads. */
constexpr uint16_t kVersion = 3;
constexpr uint16_t kVersionMin = 2;

struct FileHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint16_t length;
    uint8_t scale;
    uint8_t root;
    uint8_t swing;
    uint8_t track_count;
    uint16_t plock_count;
    char name[12];
}; /* 26 bytes */

/* The S12 file: 32 fixed steps of note+velocity, no per-track anything.
 * Layout copied from the S12-S22 presets.cpp `SeqFile` (which no longer
 * exists there — the preset system writes whole patterns now); kept
 * byte-identical so sequence slots saved by that firmware still load. */
OSYNTH_PACK_PUSH
struct OSYNTH_PACKED LegacyFile {
    uint32_t magic;
    uint8_t version; /* 1 */
    uint8_t count;   /* 0..32 */
    uint8_t pad[2];
    uint8_t notes[32];
    uint8_t vels[32];
};
static_assert(sizeof(LegacyFile) == 72, "S12 on-disk layout");
OSYNTH_PACK_POP
constexpr uint32_t kLegacyMagic = 0x3151534Fu; /* "OSQ1" */

} // namespace

size_t seq_pattern_max_bytes(void) {
    return sizeof(FileHeader) +
           (size_t)SEQ_TRACKS * (sizeof(seq_track_cfg_t) + sizeof(uint16_t) +
                                 SEQ_MAX_STEPS * sizeof(seq_step_t)) +
           (size_t)SEQ_PLOCKS * sizeof(seq_plock_t);
}

size_t seq_pattern_serialize(int pattern, void* buf, size_t cap) {
    if (buf == nullptr || pattern < 0 || pattern >= SEQ_PATTERNS ||
        s_steps == nullptr) {
        return 0;
    }
    uint8_t* p = (uint8_t*)buf;
    size_t off = 0;

    seq_pattern_cfg_t cfg;
    seq_pattern_cfg_get(pattern, &cfg);

    FileHeader h = {};
    h.magic = kMagic;
    h.version = kVersion;
    h.length = cfg.length;
    h.scale = cfg.scale;
    h.root = cfg.root;
    h.swing = cfg.swing;
    h.track_count = SEQ_TRACKS;
    memcpy(h.name, cfg.name, sizeof(h.name));

    /* plock_count is filled in after the lock pass below and the header
     * rewritten in place, because it has to be whatever that pass actually
     * wrote. */
    h.plock_count = 0;

    if (off + sizeof(h) > cap) return 0;
    memcpy(p + off, &h, sizeof(h));
    off += sizeof(h);

    for (int t = 0; t < SEQ_TRACKS; ++t) {
        seq_track_cfg_t tc;
        seq_track_cfg_get(pattern, t, &tc);
        /* Trailing empty steps are not written either (S27): an untouched
         * 64-step track costs two bytes instead of 512, which is what keeps a
         * whole-set file — every pattern at once — small on the filesystem.
         * vel == 0 *is* the empty encoding, so nothing else in the step can
         * still be meaningful, and the load path clears the pattern first. */
        uint16_t stored = 0;
        for (int s = 0; s < tc.length; ++s) {
            seq_step_t st;
            seq_step_get(pattern, t, s, &st);
            if (st.vel != 0) stored = (uint16_t)(s + 1);
        }
        const size_t steps_bytes = (size_t)stored * sizeof(seq_step_t);
        if (off + sizeof(tc) + sizeof(stored) + steps_bytes > cap) return 0;
        memcpy(p + off, &tc, sizeof(tc));
        off += sizeof(tc);
        memcpy(p + off, &stored, sizeof(stored));
        off += sizeof(stored);
        for (int s = 0; s < stored; ++s) {
            seq_step_t st;
            seq_step_get(pattern, t, s, &st);
            memcpy(p + off, &st, sizeof(st));
            off += sizeof(st);
        }
    }

    /* The count in the header is whatever this pass writes, not what a
     * separate counting pass saw a moment earlier. Both used to walk
     * s_plock/s_plock_count unlocked, so a BLE edit landing between them left
     * the header declaring a number the body did not contain — the loader
     * stops on its own bounds check, but the file is silently wrong.
     *
     * The table is still read one entry at a time rather than under a single
     * section: an edit that reshuffles it mid-walk (removal is swap-with-last)
     * can cost or repeat one lock, which re-saving fixes, while a header that
     * disagrees with its body cannot be fixed by anything. */
    uint16_t locks = 0;
    for (int i = 0;; ++i) {
        seq_plock_t l = {}; /* the copy is conditional; keep it defined */
        taskENTER_CRITICAL(&s_lock);
        const bool more = i < s_plock_count;
        if (more) l = s_plock[i];
        taskEXIT_CRITICAL(&s_lock);
        if (!more) break;
        if (l.pattern != pattern) continue;
        if (off + sizeof(l) > cap) return 0;
        memcpy(p + off, &l, sizeof(l));
        off += sizeof(l);
        ++locks;
    }
    h.plock_count = locks;
    memcpy(p, &h, sizeof(h)); /* rewrite the header with the real count */
    return off;
}

bool seq_pattern_deserialize(int pattern, const void* buf, size_t len) {
    if (buf == nullptr || pattern < 0 || pattern >= SEQ_PATTERNS ||
        s_steps == nullptr) {
        return false;
    }
    const uint8_t* p = (const uint8_t*)buf;

    /* Legacy S12 sequences: 32 mono steps into track 1, everything else
     * cleared. Old saves keep working, which is the whole point. */
    if (len >= sizeof(LegacyFile)) {
        LegacyFile lf;
        memcpy(&lf, p, sizeof(lf));
        if (lf.magic == kLegacyMagic && lf.version == 1 && lf.count <= 32) {
            seq_pattern_clear(pattern);
            seq_track_cfg_t tc;
            seq_track_cfg_get(pattern, 0, &tc);
            tc.target = SEQ_TARGET_SYNTH;
            tc.length = lf.count ? lf.count : SEQ_DEFAULT_STEPS;
            seq_track_cfg_set(pattern, 0, &tc);
            for (int i = 0; i < lf.count; ++i) {
                seq_step_t s = {};
                s.note = lf.notes[i] & 0x7F;
                s.vel = lf.vels[i] & 0x7F;
                s.gate = 16;
                s.prob = 100;
                s.ratchet = 1;
                seq_step_set(pattern, 0, i, &s);
            }
            ESP_LOGI(TAG, "loaded a legacy 32-step sequence into pattern %d",
                     pattern + 1);
            return true;
        }
    }

    if (len < sizeof(FileHeader)) return false;
    FileHeader h;
    memcpy(&h, p, sizeof(h));
    if (h.magic != kMagic || h.version < kVersionMin || h.version > kVersion) {
        return false;
    }
    if (h.track_count > SEQ_TRACKS) {
        /* A file from a PSRAM build being read on a classic ESP32: take the
         * tracks that fit rather than refusing the whole pattern. */
        ESP_LOGW(TAG, "pattern has %u tracks, this build has %d — truncating",
                 h.track_count, SEQ_TRACKS);
    }
    size_t off = sizeof(h);

    seq_pattern_clear(pattern);
    seq_pattern_cfg_t cfg;
    seq_pattern_cfg_get(pattern, &cfg);
    cfg.length = h.length;
    cfg.scale = h.scale;
    cfg.root = h.root;
    cfg.swing = h.swing;
    memcpy(cfg.name, h.name, sizeof(cfg.name));
    cfg.name[sizeof(cfg.name) - 1] = '\0';
    seq_pattern_cfg_set(pattern, &cfg);

    for (int t = 0; t < h.track_count; ++t) {
        if (off + sizeof(seq_track_cfg_t) > len) return false;
        seq_track_cfg_t tc;
        memcpy(&tc, p + off, sizeof(tc));
        off += sizeof(tc);
        /* v3 stores how many steps follow (trailing empties are dropped);
         * v2 always stored the track's full length. */
        uint16_t stored = tc.length;
        if (h.version >= 3) {
            if (off + sizeof(stored) > len) return false;
            memcpy(&stored, p + off, sizeof(stored));
            off += sizeof(stored);
            if (stored > tc.length) stored = tc.length;
        }
        uint16_t count = stored;
        if (count > SEQ_MAX_STEPS) count = SEQ_MAX_STEPS;
        if (off + (size_t)count * sizeof(seq_step_t) > len) return false;
        if (t < SEQ_TRACKS) seq_track_cfg_set(pattern, t, &tc);
        for (int s = 0; s < count; ++s) {
            seq_step_t st;
            memcpy(&st, p + off, sizeof(st));
            off += sizeof(st);
            if (t < SEQ_TRACKS) seq_step_set(pattern, t, s, &st);
        }
        /* Skip the remainder of a track this build cannot hold. */
        if (stored > count) {
            off += (size_t)(stored - count) * sizeof(seq_step_t);
        }
    }

    for (int i = 0; i < h.plock_count; ++i) {
        if (off + sizeof(seq_plock_t) > len) break;
        seq_plock_t l;
        memcpy(&l, p + off, sizeof(l));
        off += sizeof(l);
        if (l.track < SEQ_TRACKS) {
            seq_plock_set(pattern, l.track, l.step, l.pid, l.value);
        }
    }
    return true;
}
