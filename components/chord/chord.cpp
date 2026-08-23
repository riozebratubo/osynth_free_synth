/*
 * osynth — chord mode (Session 41). Contract and rationale in chord.h.
 *
 * Nothing here runs on the audio task. Expansion happens on whichever control
 * task delivered the key — USB MIDI, serial MIDI, BLE, or the sequencer's
 * clock task — and the strum task exists only so a chord can be spread in
 * time without any of those blocking.
 *
 * The lock discipline is the one rule worth stating up front: the held table
 * and the reference counts are touched inside `s_lock`, and every note is
 * emitted *outside* it. midi_play_note() runs the router's note tap, which
 * takes seqarp's own spinlock and walks its held-note list; holding two
 * spinlocks across that would be a deadlock waiting for the day someone adds
 * a third. So each entry point builds a small emission list under the lock
 * and plays it afterwards.
 */
#include "chord.h"

#include <atomic>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "midi.h"
#include "seq_model.h"
#include "seqarp.h"
#include "synth_params.h"

static const char* TAG = "chord";

/* chord.h names the two router points itself so a caller need not include
 * midi.h; the router is the authority on their values. Compared as ints
 * because they are two distinct unnamed enums, and comparing those directly
 * is what -Werror=enum-compare exists to catch. */
static_assert((int)CHORD_WHEN_PRE == (int)MIDI_CHORD_PRE,
              "router point mismatch");
static_assert((int)CHORD_WHEN_POST == (int)MIDI_CHORD_POST,
              "router point mismatch");

using osynth::ParamCurve;
using osynth::ParamDesc;
using osynth::ParamOrigin;
using osynth::ParamStore;
using osynth::ParamType;

namespace {

/* ---- parameters (order matches PIdx) ---- */

enum PIdx {
    ENABLE, MODE, TYPE, SCALE, ROOT, FOLLOW, KEYMAP, SIZE, INV, VOICING,
    BASS, STRUM, STRUMDIR, VEL, LEAD, RANGE, ROUTE, KEYS, REV, RESTRIKE,
    P_COUNT
};

const char* const kModeNames[] = {"free", "scale", "user"};
const char* const kKeymapNames[] = {"degrees", "chromatic"};
const char* const kVoicingNames[] = {"close", "drop2", "drop3", "open"};
const char* const kBassNames[] = {"off", "-1 oct", "-2 oct"};
const char* const kStrumDirNames[] = {"up", "down", "alt", "random"};
const char* const kRouteNames[] = {"pre-arp", "post-arp"};
const char* const kKeysNames[] = {"poly", "mono"};
const char* const kRestrikeNames[] = {"changed", "all"};

/* Scale names are seqarp's, not a second copy: `chord.follow` mirrors
 * seq.scale into chord.scale by value, which is only meaningful while the two
 * enums number the same scales in the same order. Sharing the table is what
 * guarantees that rather than hoping for it. */
const char* const kScaleNames[SEQ_SCALE_COUNT] = {
    "chromatic", "major", "minor", "dorian", "phrygian", "lydian",
    "mixolydian", "locrian", "harm minor", "penta maj", "penta min", "blues",
};

/* ---- chord qualities (free mode) ----
 *
 * Semitones from the root. The order runs roughly from thin to rich, which is
 * the order a player scrolls through them in, and `single` is first so that
 * turning chord mode on before choosing anything changes nothing audible.
 *
 * The order *is* the chord.type enum, which PARAM_INFO serves to the app —
 * and the app keeps a display copy of this table to name chords with (see
 * chordNotesFor() in the app's synthcontroller.cpp). Appending is safe;
 * reordering would relabel every saved value and mislabel every chord the
 * app prints.
 */
struct Quality {
    const char* name;  /* enum label, and the suffix in a printed chord name */
    uint8_t count;
    int8_t iv[7];
};

const Quality kQuality[] = {
    {"single",  1, {0}},
    {"5th",     2, {0, 7}},
    {"oct",     2, {0, 12}},
    {"sus2",    3, {0, 2, 7}},
    {"sus4",    3, {0, 5, 7}},
    {"maj",     3, {0, 4, 7}},
    {"min",     3, {0, 3, 7}},
    {"dim",     3, {0, 3, 6}},
    {"aug",     3, {0, 4, 8}},
    {"maj6",    4, {0, 4, 7, 9}},
    {"min6",    4, {0, 3, 7, 9}},
    {"7",       4, {0, 4, 7, 10}},
    {"maj7",    4, {0, 4, 7, 11}},
    {"min7",    4, {0, 3, 7, 10}},
    {"m7b5",    4, {0, 3, 6, 10}},
    {"dim7",    4, {0, 3, 6, 9}},
    {"mmaj7",   4, {0, 3, 7, 11}},
    {"add9",    4, {0, 4, 7, 14}},
    {"6/9",     5, {0, 4, 7, 9, 14}},
    {"9",       5, {0, 4, 7, 10, 14}},
    {"maj9",    5, {0, 4, 7, 11, 14}},
    {"min9",    5, {0, 3, 7, 10, 14}},
    {"11",      6, {0, 4, 7, 10, 14, 17}},
    {"13",      6, {0, 4, 7, 10, 14, 21}},
    {"quartal", 4, {0, 5, 10, 15}},
};
constexpr int kQualityCount = (int)(sizeof(kQuality) / sizeof(kQuality[0]));

/* Enum labels for chord.type, built once from the table above so the two can
 * never drift. PARAM_INFO serves these to the app. */
const char* s_type_names[kQualityCount];

/* ---- limits ---- */

/* Keys that can be held at once. Well past the eight voices a chord can
 * actually reach, because a key stays in the table until its note-off
 * arrives — a sustained-pedal-style pile-up of released-but-latched keys is
 * the case this has to survive, not simultaneous fingers. */
constexpr int kMaxKeys = 16;
/* Tones waiting on the strum clock. A 13th plus a bass note is eight, so this
 * is three full chords in flight. */
constexpr int kMaxPending = 24;
constexpr int kStrumTaskPrio = 9; /* control plane, just under seq_clk */
constexpr int kStrumTaskStack = 3072;

/* Middle C. In `degrees` keymap this key plays the tonic, and each semitone
 * above it is one scale degree — so the two octaves the app draws cover about
 * three and a half octaves of a seven-note scale and no key is dead. */
constexpr int kDegreeOrigin = 60;

ParamDesc s_params[P_COUNT];
const std::atomic<float>* s_p[P_COUNT];
/* seq.scale / seq.root, for `chord.follow`. Resolved at init because
 * chord_init() runs after seqarp_init(); null if that ever stops being true,
 * in which case follow silently reads chord's own values. */
const std::atomic<float>* s_seq_scale = nullptr;
const std::atomic<float>* s_seq_root = nullptr;

inline float pv(PIdx i) {
    const std::atomic<float>* p = s_p[i];
    return p != nullptr ? p->load(std::memory_order_relaxed) : 0.0f;
}
inline int pi(PIdx i) { return (int)(pv(i) + 0.5f); }

/* ---- shared state ---- */

portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

struct Held {
    uint8_t key;
    /* The velocity the key was struck at. Kept because a re-voice has to
     * strike the tones a setting change *added* at the level the player
     * actually played, and by then the note-on is long gone. */
    uint8_t vel;
    uint8_t count;
    uint8_t note[CHORD_MAX_NOTES];
    /* Which tones have actually sounded. A strummed chord is entered into the
     * table whole and emitted over the next fraction of a second, so a key
     * released mid-strum must release the tones that made it out and cancel
     * the rest rather than send note-offs for notes that never started. */
    uint16_t emitted;
    bool pre;
    bool used;
};
Held s_held[kMaxKeys];

/* One count per MIDI note. Two keys can legitimately want the same tone — C
 * and E of one scale both reach G — and without this, releasing C would cut a
 * G that E is still holding. */
uint8_t s_ref[128];

struct Pending {
    int64_t due_us;
    uint8_t key;
    uint8_t note;
    uint8_t vel;
    int8_t slot; /* index into s_held, so the emitted bit lands in the right entry */
    uint8_t bit;
    bool pre;
    int src;
    bool used;
};
Pending s_pend[kMaxPending];

TaskHandle_t s_strum_task = nullptr;

/* Alternating strum direction, and the random one's generator. Deliberately
 * outside s_lock: two keys landing at the same instant on two tasks can only
 * make one of them strum the direction the other was owed, which is a
 * difference nobody can hear — and holding the lock across the shuffle would
 * put a loop inside a critical section to buy that. */
bool s_alt_down = false;
uint32_t s_rng = 0x9e3779b9u;
inline uint32_t rng_next() {
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return s_rng = x;
}

/* The previous chord's tones, for `chord.lead`. Not under the lock: it is
 * advisory — a stale read picks a slightly worse inversion, never a wrong
 * note — and taking the lock across the scoring loop would put arithmetic
 * inside a critical section for no correctness gain. */
uint8_t s_prev[CHORD_MAX_NOTES];
int s_prev_count = 0;

/* ---- the user set ---- */

chord_user_slot_t s_user[CHORD_USER_SLOTS];
std::atomic<uint32_t> s_user_rev{0};

/* A set that does something out of the box: the diatonic sevenths of a major
 * key, laid out on the twelve pitch classes above the root. The five
 * chromatic degrees are silent rather than guessed at — a slot that plays
 * nothing is a legitimate answer, and it is a far better default than five
 * chords nobody asked for. */
void user_defaults() {
    memset(s_user, 0, sizeof(s_user));
    struct { uint8_t pc; uint8_t quality; } kSeed[] = {
        {0,  12}, /* I    maj7  */
        {2,  13}, /* ii   min7  */
        {4,  13}, /* iii  min7  */
        {5,  12}, /* IV   maj7  */
        {7,  11}, /* V    7     */
        {9,  13}, /* vi   min7  */
        {11, 14}, /* vii  m7b5  */
    };
    for (const auto& s : kSeed) {
        const Quality& q = kQuality[s.quality];
        chord_user_slot_t& u = s_user[s.pc];
        u.transpose = 0;
        u.count = q.count > CHORD_USER_IVS ? CHORD_USER_IVS : q.count;
        for (int i = 0; i < u.count; ++i) u.iv[i] = q.iv[i];
    }
}

/* ---- scale helpers ---- */

/* Expands a scale mask into its degrees. Always at least one degree: an empty
 * mask cannot come out of seq_scale_mask(), but a chord built on zero degrees
 * would divide by zero, so this is the guard rather than a comment. */
int scale_degrees(int scale, int8_t* semi) {
    const uint16_t mask = seq_scale_mask(scale);
    int n = 0;
    for (int i = 0; i < 12; ++i) {
        if (mask & (1u << i)) semi[n++] = (int8_t)i;
    }
    if (n == 0) {
        semi[0] = 0;
        n = 1;
    }
    return n;
}

/* Where degree 0 sits. `60 + root` would move the whole keyboard up to eleven
 * semitones as the key changes; folding roots above F# down an octave keeps
 * the shift within half an octave either way, so changing key does not also
 * change register. */
inline int scale_base(int root) {
    return kDegreeOrigin + (root > 6 ? root - 12 : root);
}

int degree_to_midi(int deg, int base, const int8_t* semi, int n) {
    int oct = deg / n;
    int step = deg % n;
    if (step < 0) {
        step += n;
        --oct;
    }
    return base + 12 * oct + semi[step];
}

/* The live scale and root, honouring `chord.follow`. */
void effective_scale(int* scale, int* root) {
    if (pv(FOLLOW) >= 0.5f && s_seq_scale != nullptr && s_seq_root != nullptr) {
        *scale = (int)(s_seq_scale->load(std::memory_order_relaxed) + 0.5f);
        *root = (int)(s_seq_root->load(std::memory_order_relaxed) + 0.5f);
    } else {
        *scale = pi(SCALE);
        *root = pi(ROOT);
    }
    if (*scale < 0 || *scale >= SEQ_SCALE_COUNT) *scale = SEQ_SCALE_CHROMATIC;
    *root = ((*root % 12) + 12) % 12;
}

/* ---- voicing ---- */

void sort_ascending(int* n, int count) {
    for (int i = 1; i < count; ++i) { /* insertion: count is at most 8 */
        const int v = n[i];
        int j = i - 1;
        while (j >= 0 && n[j] > v) {
            n[j + 1] = n[j];
            --j;
        }
        n[j + 1] = v;
    }
}

/* Total distance from a candidate voicing to the chord that came before it —
 * each tone measured against the nearest tone of the previous chord. Lower is
 * smoother. With no previous chord every candidate scores the same, so the
 * caller keeps the one `chord.inv` asked for. */
int lead_cost(const int* n, int count) {
    if (s_prev_count == 0) return 0;
    int total = 0;
    for (int i = 0; i < count; ++i) {
        int best = 127;
        for (int j = 0; j < s_prev_count; ++j) {
            const int d = n[i] > (int)s_prev[j] ? n[i] - (int)s_prev[j]
                                                : (int)s_prev[j] - n[i];
            if (d < best) best = d;
        }
        total += best;
    }
    return total;
}

/* Rotates the lowest tone up an octave, `times` times. The chord stays
 * ascending, which everything downstream assumes. */
void invert(int* n, int count, int times) {
    for (int t = 0; t < times && count > 1; ++t) {
        const int low = n[0] + 12;
        for (int i = 1; i < count; ++i) n[i - 1] = n[i];
        n[count - 1] = low;
    }
}

void apply_voicing(int* n, int count, int voicing) {
    if (count < 3) return; /* nothing to drop out of a dyad */
    switch (voicing) {
        case CHORD_VOICING_DROP2:
            n[count - 2] -= 12;
            break;
        case CHORD_VOICING_DROP3:
            if (count >= 4) n[count - 3] -= 12;
            break;
        case CHORD_VOICING_OPEN:
            /* Every other tone from the second one down an octave: the widest
             * of the four and the one that stops a five- or six-note chord
             * from sounding like a cluster. */
            for (int i = 1; i < count; i += 2) n[i] -= 12;
            break;
        default:
            return;
    }
    sort_ascending(n, count);
}

/* Folds anything more than `range` semitones above the lowest tone back down
 * by octaves. Off at 0. Runs after voicing, so it is the last word on how far
 * a chord can spread — which is what makes the top of the keyboard playable
 * in scale mode, where a stacked 13th would otherwise run off the end. */
void apply_range(int* n, int count, int range) {
    if (range <= 0 || count < 2) return;
    sort_ascending(n, count);
    const int low = n[0];
    for (int i = 1; i < count; ++i) {
        while (n[i] - low > range) n[i] -= 12;
        if (n[i] < low) n[i] += 12; /* never fold below the bass */
    }
    sort_ascending(n, count);
}

/* ---- chord construction ----
 *
 * Returns the tone count and writes ascending MIDI notes into `out`, with
 * `root_out` naming which of them the recorder should treat as the played
 * note. Zero is a legal answer and means "this key is silent" (an empty user
 * slot), which is not the same as "chord mode is off".
 */
int build_chord(uint8_t key, int* out, int* root_out) {
    const int mode = pi(MODE);
    int tone[CHORD_MAX_NOTES];
    int count = 0;
    int root = key;

    if (mode == CHORD_MODE_USER) {
        int scale = 0, r = 0;
        effective_scale(&scale, &r);
        const int slot = (((int)key - r) % 12 + 12) % 12;
        chord_user_slot_t u;
        taskENTER_CRITICAL(&s_lock);
        u = s_user[slot];
        taskEXIT_CRITICAL(&s_lock);
        if (u.count == 0) return 0; /* deliberately silent */
        root = (int)key + u.transpose;
        for (int i = 0; i < u.count && count < CHORD_MAX_NOTES; ++i) {
            tone[count++] = root + u.iv[i];
        }
    } else if (mode == CHORD_MODE_SCALE) {
        int scale = 0, r = 0;
        effective_scale(&scale, &r);
        int8_t semi[12];
        const int n = scale_degrees(scale, semi);
        const int base = scale_base(r);

        int deg = 0;
        if (pi(KEYMAP) == CHORD_KEYMAP_DEGREES) {
            deg = (int)key - kDegreeOrigin;
        } else {
            /* Chromatic keymap: the key keeps its pitch, snapped into the
             * scale, and we recover which degree that landed on. */
            const int q = (int)seq_quantize(key, scale, r);
            const int pc = ((q - r) % 12 + 12) % 12;
            int step = 0;
            for (int i = 0; i < n; ++i) {
                if (semi[i] == (int8_t)pc) {
                    step = i;
                    break;
                }
            }
            int oct = (q - base - semi[step]);
            oct = oct >= 0 ? oct / 12 : -((-oct + 11) / 12);
            deg = oct * n + step;
        }

        int size = pi(SIZE);
        if (size < 1) size = 1;
        if (size > CHORD_MAX_NOTES - 1) size = CHORD_MAX_NOTES - 1;
        /* Every other degree — thirds *of the scale*, which is what makes the
         * quality follow the degree with nothing to choose. On a chromatic
         * "scale" the same rule stacks whole tones; exotic, but consistent,
         * and a rule that holds everywhere beats a special case. */
        for (int i = 0; i < size && count < CHORD_MAX_NOTES; ++i) {
            tone[count++] = degree_to_midi(deg + 2 * i, base, semi, n);
        }
        root = tone[0];
    } else {
        int type = pi(TYPE);
        if (type < 0 || type >= kQualityCount) type = 0;
        const Quality& q = kQuality[type];
        root = key;
        for (int i = 0; i < q.count && count < CHORD_MAX_NOTES; ++i) {
            tone[count++] = (int)key + q.iv[i];
        }
    }

    if (count == 0) return 0;

    /* Inversion — or voice-leading, which picks the inversion for you and
     * therefore overrides it. Scored against the chord that came before, so
     * a progression moves by the shortest path instead of jumping register
     * every time the root does. */
    if (pv(LEAD) >= 0.5f && count > 1) {
        int best[CHORD_MAX_NOTES];
        int best_cost = -1;
        for (int inv = 0; inv < count; ++inv) {
            int cand[CHORD_MAX_NOTES];
            memcpy(cand, tone, sizeof(int) * (size_t)count);
            invert(cand, count, inv);
            const int cost = lead_cost(cand, count);
            if (best_cost < 0 || cost < best_cost) {
                best_cost = cost;
                memcpy(best, cand, sizeof(int) * (size_t)count);
            }
        }
        memcpy(tone, best, sizeof(int) * (size_t)count);
    } else {
        int inv = pi(INV);
        if (inv < 0) inv = 0;
        if (inv > 3) inv = 3;
        invert(tone, count, inv);
    }

    apply_voicing(tone, count, pi(VOICING));
    apply_range(tone, count, pi(RANGE));
    sort_ascending(tone, count);

    /* Which tone is the root, after everything above has moved them around.
     * The recorder stores this one, so it has to survive inversion: matched
     * by pitch class, and the lowest instance of it wins. */
    int root_idx = 0;
    const int root_pc = ((root % 12) + 12) % 12;
    for (int i = 0; i < count; ++i) {
        if ((((tone[i] % 12) + 12) % 12) == root_pc) {
            root_idx = i;
            break;
        }
    }

    /* The bass note goes on last so it cannot be inverted, dropped or folded
     * away — the whole point of it is to be the lowest thing sounding. */
    const int bass = pi(BASS);
    if (bass != CHORD_BASS_OFF && count < CHORD_MAX_NOTES) {
        const int b = tone[0] - (bass == CHORD_BASS_OCT2 ? 24 : 12);
        if (b >= 0) {
            for (int i = count; i > 0; --i) tone[i] = tone[i - 1];
            tone[0] = b;
            ++count;
            ++root_idx;
        }
    }

    /* Two passes in one loop over an array that is still ascending.
     *
     * Out of MIDI range: dropped rather than clamped, because two tones
     * clamped to 127 are one note at the wrong pitch — worse than a chord
     * with one fewer voice.
     *
     * Duplicates: dropped because a tone is reference counted once per held
     * key, and an entry listing the same note twice would take one reference
     * and give back two — releasing a note another key was still holding. The
     * range fold is what makes this reachable: it moves tones down by octaves
     * until they fit, and two of them can land on the same pitch. */
    int kept = 0;
    int last = -1;
    for (int i = 0; i < count; ++i) {
        const bool out_of_range = tone[i] < 0 || tone[i] > 127;
        if (out_of_range || tone[i] == last) {
            if (i < root_idx) {
                --root_idx;
            } else if (i == root_idx) {
                /* A dropped duplicate still sounds — as the copy already
                 * kept — so the root follows it there rather than resetting
                 * to the bass note. */
                root_idx = (!out_of_range && kept > 0) ? kept - 1 : 0;
            }
            continue;
        }
        last = tone[i];
        out[kept++] = tone[i];
    }
    if (kept == 0) return 0;
    if (root_idx < 0 || root_idx >= kept) root_idx = 0;
    *root_out = root_idx;
    return kept;
}

/* ---- emission ----
 *
 * Every path below builds one of these under the lock and plays it after
 * releasing — see the file header on why.
 */
struct EmitItem {
    uint8_t note;
    uint8_t vel;
    bool on;
    bool pre;
    int src; /* MIDI_NOTE_CHORD_ROOT / _TONE, for the tap's two consumers */
};

void flush(const EmitItem* items, int count) {
    for (int i = 0; i < count; ++i) {
        midi_play_note(items[i].note, items[i].vel, items[i].on, items[i].pre,
                       items[i].src);
    }
}

/* Velocity for tone `i` of `count`, tapering toward the top of the chord:
 * quietest on top, which is how a hand actually plays one and what keeps a
 * stacked 13th from sounding like an organ. Never below 1 — 0 is a note-off
 * in MIDI. Shared by the initial strike and by a re-voice, so a chord that
 * changes under a held key keeps the same shape it was struck with. */
uint8_t tone_velocity(int i, int count, uint8_t key_vel) {
    float v = (float)key_vel;
    const float fall = pv(VEL);
    if (count > 1 && fall > 0.0f) {
        v *= 1.0f - fall * ((float)i / (float)(count - 1));
    }
    int iv = (int)(v + 0.5f);
    if (iv < 1) iv = 1;
    if (iv > 127) iv = 127;
    return (uint8_t)iv;
}

/* s_lock held. Adds a reference and reports whether the tone has to be
 * sounded — a tone already held by another key is not retriggered, which is
 * what stops a second key from restarting a note the first one is still
 * playing. */
bool ref_add(uint8_t note) { return s_ref[note]++ == 0; }

/* s_lock held. Drops a reference and reports whether the tone should stop. */
bool ref_release(uint8_t note) {
    if (s_ref[note] == 0) return false;
    return --s_ref[note] == 0;
}

/* s_lock held. Releases one held entry into `items`, returning how many it
 * added. Cancels anything of that key still waiting on the strum clock. */
int release_entry(int slot, EmitItem* items, int max) {
    Held& h = s_held[slot];
    int n = 0;
    if (!h.used) return 0;
    for (int i = 0; i < kMaxPending; ++i) {
        if (s_pend[i].used && s_pend[i].slot == (int8_t)slot) {
            s_pend[i].used = false; /* never sounded: nothing to release */
        }
    }
    for (int i = 0; i < h.count && n < max; ++i) {
        if ((h.emitted & (1u << i)) == 0) continue;
        if (ref_release(h.note[i])) {
            items[n].note = h.note[i];
            items[n].vel = 0;
            items[n].on = false;
            items[n].pre = h.pre;
            /* A release is not a chord voicing, but the tap still reads `src`:
             * CHORD_TONE keeps the note-off out of the recorder, which only
             * ever wanted the note-on anyway. */
            items[n].src = MIDI_NOTE_CHORD_TONE;
            ++n;
        }
    }
    h.used = false;
    h.count = 0;
    h.emitted = 0;
    return n;
}

int find_key(uint8_t key, bool pre) {
    for (int i = 0; i < kMaxKeys; ++i) {
        if (s_held[i].used && s_held[i].key == key && s_held[i].pre == pre) {
            return i;
        }
    }
    return -1;
}

/* Releases held keys one entry at a time, flushing between them, so the
 * emission buffer stays one chord wide however many keys are down. Doing it
 * in a single pass would need a kMaxKeys x CHORD_MAX_NOTES array on the stack
 * of whichever task delivered the key — and that is the USB MIDI task, whose
 * stack has an isochronous refill deadline sitting on it.
 *
 * `keep` is a key to leave alone (-1 for none), which is what mono mode wants:
 * everything but the key being pressed right now. */
void release_others(int keep) {
    for (;;) {
        EmitItem items[CHORD_MAX_NOTES];
        int n = 0;
        int slot = -1;
        taskENTER_CRITICAL(&s_lock);
        for (int i = 0; i < kMaxKeys; ++i) {
            if (s_held[i].used && (int)s_held[i].key != keep) {
                slot = i;
                break;
            }
        }
        if (slot >= 0) n = release_entry(slot, items, CHORD_MAX_NOTES);
        taskEXIT_CRITICAL(&s_lock);
        if (slot < 0) return;
        flush(items, n);
    }
}

/* ---- the strum task ----
 *
 * Sleeps until the next tone is due, so a chord with no strum costs nothing
 * at all: chord_key_on() emits inline in that case and never notifies.
 */
void strum_task(void* arg) {
    (void)arg;
    for (;;) {
        int64_t next = 0;
        taskENTER_CRITICAL(&s_lock);
        for (int i = 0; i < kMaxPending; ++i) {
            if (!s_pend[i].used) continue;
            if (next == 0 || s_pend[i].due_us < next) next = s_pend[i].due_us;
        }
        taskEXIT_CRITICAL(&s_lock);

        TickType_t wait = portMAX_DELAY;
        if (next != 0) {
            const int64_t dt = next - esp_timer_get_time();
            wait = dt <= 0 ? 0 : pdMS_TO_TICKS((uint32_t)(dt / 1000) + 1);
        }
        ulTaskNotifyTake(pdTRUE, wait);

        EmitItem items[kMaxPending];
        int n = 0;
        const int64_t now = esp_timer_get_time();
        taskENTER_CRITICAL(&s_lock);
        for (int i = 0; i < kMaxPending && n < kMaxPending; ++i) {
            Pending& p = s_pend[i];
            if (!p.used || p.due_us > now) continue;
            p.used = false;
            const int slot = (int)p.slot;
            if (slot < 0 || slot >= kMaxKeys || !s_held[slot].used ||
                s_held[slot].key != p.key) {
                continue; /* the key was released while this waited */
            }
            s_held[slot].emitted |= (uint16_t)(1u << p.bit);
            if (!ref_add(p.note)) continue;
            items[n].note = p.note;
            items[n].vel = p.vel;
            items[n].on = true;
            items[n].pre = p.pre;
            items[n].src = p.src;
            ++n;
        }
        taskEXIT_CRITICAL(&s_lock);
        flush(items, n);
    }
}

/* ---- parameter registration ---- */

void build_params() {
    for (int i = 0; i < kQualityCount; ++i) s_type_names[i] = kQuality[i].name;

    int n = 0;
    s_params[n++] = {CHORD_PID_ENABLE, "chord.enable", ParamType::Bool,
                     ParamCurve::Linear, 0.0f, 1.0f, 0.0f, nullptr, 0};
    s_params[n++] = {CHORD_PID_MODE, "chord.mode", ParamType::Enum,
                     ParamCurve::Linear, 0.0f, (float)(CHORD_MODE_COUNT - 1),
                     0.0f, kModeNames, CHORD_MODE_COUNT};
    s_params[n++] = {CHORD_PID_TYPE, "chord.type", ParamType::Enum,
                     ParamCurve::Linear, 0.0f, (float)(kQualityCount - 1),
                     5.0f /* maj */, s_type_names, (uint8_t)kQualityCount};
    s_params[n++] = {CHORD_PID_SCALE, "chord.scale", ParamType::Enum,
                     ParamCurve::Linear, 0.0f, (float)(SEQ_SCALE_COUNT - 1),
                     (float)SEQ_SCALE_MAJOR, kScaleNames, SEQ_SCALE_COUNT};
    s_params[n++] = {CHORD_PID_ROOT, "chord.root", ParamType::Int,
                     ParamCurve::Linear, 0.0f, 11.0f, 0.0f, nullptr, 0};
    s_params[n++] = {CHORD_PID_FOLLOW, "chord.follow", ParamType::Bool,
                     ParamCurve::Linear, 0.0f, 1.0f, 0.0f, nullptr, 0};
    s_params[n++] = {CHORD_PID_KEYMAP, "chord.keymap", ParamType::Enum,
                     ParamCurve::Linear, 0.0f, 1.0f, 0.0f, kKeymapNames, 2};
    s_params[n++] = {CHORD_PID_SIZE, "chord.size", ParamType::Int,
                     ParamCurve::Linear, 1.0f, 7.0f, 3.0f, nullptr, 0};
    s_params[n++] = {CHORD_PID_INV, "chord.inv", ParamType::Int,
                     ParamCurve::Linear, 0.0f, 3.0f, 0.0f, nullptr, 0};
    s_params[n++] = {CHORD_PID_VOICING, "chord.voicing", ParamType::Enum,
                     ParamCurve::Linear, 0.0f, 3.0f, 0.0f, kVoicingNames, 4};
    s_params[n++] = {CHORD_PID_BASS, "chord.bass", ParamType::Enum,
                     ParamCurve::Linear, 0.0f, 2.0f, 0.0f, kBassNames, 3};
    s_params[n++] = {CHORD_PID_STRUM, "chord.strum", ParamType::Float,
                     ParamCurve::Linear, 0.0f, 200.0f, 0.0f, nullptr, 0};
    s_params[n++] = {CHORD_PID_STRUMDIR, "chord.strumdir", ParamType::Enum,
                     ParamCurve::Linear, 0.0f, 3.0f, 0.0f, kStrumDirNames, 4};
    s_params[n++] = {CHORD_PID_VEL, "chord.vel", ParamType::Float,
                     ParamCurve::Linear, 0.0f, 1.0f, 0.0f, nullptr, 0};
    s_params[n++] = {CHORD_PID_LEAD, "chord.lead", ParamType::Bool,
                     ParamCurve::Linear, 0.0f, 1.0f, 0.0f, nullptr, 0};
    s_params[n++] = {CHORD_PID_RANGE, "chord.range", ParamType::Int,
                     ParamCurve::Linear, 0.0f, 48.0f, 0.0f, nullptr, 0};
    s_params[n++] = {CHORD_PID_ROUTE, "chord.route", ParamType::Enum,
                     ParamCurve::Linear, 0.0f, 1.0f, 0.0f, kRouteNames, 2};
    s_params[n++] = {CHORD_PID_KEYS, "chord.keys", ParamType::Enum,
                     ParamCurve::Linear, 0.0f, 1.0f, 0.0f, kKeysNames, 2};
    /* Read-only, bumped by every edit to the user set. Pattern data's trick
     * (seq.rev, graph.rev): the set is not parameter space, so a counter is
     * the only way the app can learn that a working-state restore or a
     * chord-set load changed it under them. Only inequality means anything. */
    s_params[n++] = {CHORD_PID_REV, "chord.rev", ParamType::Int,
                     ParamCurve::Linear, 0.0f, 16777215.0f, 0.0f, nullptr, 0};
    s_params[n++] = {CHORD_PID_RESTRIKE, "chord.restrike", ParamType::Enum,
                     ParamCurve::Linear, 0.0f, 1.0f, 0.0f, kRestrikeNames, 2};
}

/* Chord mode is only *on* when the toggle says so. Kept as a function rather
 * than inlined at each site because every entry point has to agree, and
 * "enabled" has to stay one idea if a second condition is ever added. */
inline bool enabled() { return pv(ENABLE) >= 0.5f; }

/* The router's single entry point into chord mode. */
bool router_hook(uint8_t note, uint8_t vel, bool on, bool allow, int when,
                 void* ctx) {
    (void)ctx;
    if (when == MIDI_CHORD_ALL_OFF) {
        chord_all_off();
        return true;
    }
    return on ? chord_key_on(note, vel, allow, when)
              : chord_key_off(note, allow, when);
}

/* ---- live re-voicing ----------------------------------------------------
 *
 * A chord is built when the key goes down. Without this, changing the key,
 * the scale, the chord size or the voicing was inaudible until the *next*
 * note: you would set the root and go on hearing the old chord until you let
 * go — which on a page whose whole point is choosing a key made the controls
 * feel broken.
 *
 * So a change rebuilds what is held and plays only the difference. Tones that
 * survive are left completely alone — not released and re-struck — because
 * dragging a knob through five values would otherwise retrigger every voice
 * five times, and a sustained chord would machine-gun instead of morphing.
 *
 * One held key at a time, so the emission buffer stays one chord wide on the
 * caller's stack rather than kMaxKeys chords wide. That caller is whichever
 * task wrote the parameter — the BLE control task for an app edit, the clock
 * task for a parameter lock — which is the same set of tasks that already
 * play notes through here.
 */
void revoice_entry(int slot) {
    uint8_t key, key_vel;
    bool pre;
    uint8_t old_note[CHORD_MAX_NOTES];
    int old_count;
    uint16_t old_emitted;

    taskENTER_CRITICAL(&s_lock);
    if (!s_held[slot].used) {
        taskEXIT_CRITICAL(&s_lock);
        return;
    }
    key = s_held[slot].key;
    key_vel = s_held[slot].vel;
    pre = s_held[slot].pre;
    old_count = s_held[slot].count;
    old_emitted = s_held[slot].emitted;
    memcpy(old_note, s_held[slot].note, sizeof(old_note));
    taskEXIT_CRITICAL(&s_lock);

    /* Outside the lock, because build_chord() takes it to read the user set. */
    int tone[CHORD_MAX_NOTES];
    int root_idx = 0;
    int count;
    if (enabled()) {
        count = build_chord(key, tone, &root_idx);
    } else {
        /* Chord mode switched off under a held key. It goes back to being the
         * note it is rather than falling silent until the player lets go —
         * "off" should mean the keyboard plays notes, including the one
         * already down. */
        tone[0] = (int)key;
        count = 1;
        root_idx = 0;
    }

    EmitItem items[CHORD_MAX_NOTES * 2];
    int n = 0;

    taskENTER_CRITICAL(&s_lock);
    Held& h = s_held[slot];
    if (!h.used || h.key != key) { /* released while we were building */
        taskEXIT_CRITICAL(&s_lock);
        return;
    }

    /* Did the chord actually move? Both tone lists are built ascending, so
     * comparing them element by element compares the sets.
     *
     * This is what makes the listener safe to point at the whole block. A
     * write that changes no pitch — strum, the strum direction, the route, a
     * parameter lock on something else in the range — leaves here having done
     * nothing, which matters far more in `all` mode: without it a sustained
     * chord would retrigger for reasons the player could not see. */
    bool changed = count != (int)old_count;
    for (int i = 0; !changed && i < count; ++i) {
        if (tone[i] != (int)old_note[i]) changed = true;
    }
    if (!changed) {
        taskEXIT_CRITICAL(&s_lock);
        return;
    }

    /* Play it again whole, rather than only what the change added. Reference
     * counting still has the last word: a tone another key is also holding
     * does not fall to zero, so it is not restruck and not cut — which is the
     * correct answer, since that key never asked for anything. */
    const bool restrike = pi(RESTRIKE) == CHORD_RESTRIKE_ALL;

    /* Anything of this key still waiting on the strum clock describes the old
     * chord and would arrive as a stray note. */
    for (int i = 0; i < kMaxPending; ++i) {
        if (s_pend[i].used && s_pend[i].slot == (int8_t)slot) {
            s_pend[i].used = false;
        }
    }

    /* Release what left the chord — or all of it, when the whole chord is
     * about to be played again. A tone kept in `changed` mode keeps the
     * reference it already holds, so nothing has to be added back for it in
     * the strike loop below. */
    for (int i = 0; i < old_count && n < CHORD_MAX_NOTES; ++i) {
        if ((old_emitted & (uint16_t)(1u << i)) == 0) continue;
        bool kept = false;
        if (!restrike) {
            for (int j = 0; j < count; ++j) {
                if (tone[j] == (int)old_note[i]) kept = true;
            }
        }
        if (kept) continue;
        if (!ref_release(old_note[i])) continue;
        items[n].note = old_note[i];
        items[n].vel = 0;
        items[n].on = false;
        items[n].pre = pre;
        items[n].src = MIDI_NOTE_CHORD_TONE;
        ++n;
    }

    h.count = (uint8_t)count;
    h.emitted = 0;
    for (int i = 0; i < count; ++i) {
        h.note[i] = (uint8_t)tone[i];
        h.emitted |= (uint16_t)(1u << i);
    }

    /* Strike what arrived — every tone, in `all` mode, since the releases
     * above dropped the whole chord. `src` is recomputed rather than carried:
     * an inversion can move which tone stands for the played key, and the
     * note tap reads that to decide what the recorder stores. */
    for (int i = 0; i < count; ++i) {
        bool already = false;
        if (!restrike) {
            for (int j = 0; j < old_count; ++j) {
                if ((old_emitted & (uint16_t)(1u << j)) &&
                    (int)old_note[j] == tone[i]) {
                    already = true;
                }
            }
        }
        if (already) continue;
        if (!ref_add((uint8_t)tone[i])) continue;
        items[n].note = (uint8_t)tone[i];
        items[n].vel = tone_velocity(i, count, key_vel);
        items[n].on = true;
        items[n].pre = pre;
        items[n].src = i == root_idx ? MIDI_NOTE_CHORD_ROOT
                                     : MIDI_NOTE_CHORD_TONE;
        ++n;
    }
    taskEXIT_CRITICAL(&s_lock);

    flush(items, n);
}

void revoice_all() {
    for (int i = 0; i < kMaxKeys; ++i) revoice_entry(i);
}

/* Every 0x044x parameter, plus the two seqarp ones chord.follow mirrors.
 *
 * Deliberately not filtered down to the settings that move a pitch. A
 * parameter that does not — strum, the strum direction, the route — produces
 * an identical chord, the diff above comes out empty and nothing is emitted,
 * so the cost of being generous is one comparison per held tone. Filtering
 * would only add a list to forget to update.
 *
 * Runs on whichever task wrote the parameter, synchronously. Nothing here
 * writes a parameter, so it cannot re-enter. */
void param_listener(uint16_t id, float value, ParamOrigin origin, void* ctx) {
    (void)value;
    (void)origin;
    (void)ctx;
    const bool ours = (id >= CHORD_PID_FIRST && id <= CHORD_PID_LAST) ||
                      id == SEQ_PID_SCALE || id == SEQ_PID_ROOT;
    if (!ours) return;
    revoice_all();
}

} // namespace

/* ======================= public API ==================================== */

bool chord_key_on(uint8_t note, uint8_t vel, bool allow, int when) {
    if (!allow || note > 127 || vel == 0) return false;
    if (!enabled()) return false;
    const bool want_pre = pi(ROUTE) == CHORD_ROUTE_PRE;
    if ((when == CHORD_WHEN_PRE) != want_pre) return false;

    int tone[CHORD_MAX_NOTES];
    int root_idx = 0;
    const int count = build_chord(note, tone, &root_idx);

    /* What the next chord leads away from. Written here rather than inside
     * build_chord(): only a key that is actually sounding should become the
     * chord the next one leads away from. */
    if (count > 0) {
        for (int i = 0; i < count; ++i) s_prev[i] = (uint8_t)tone[i];
        s_prev_count = count;
    }

    uint8_t tvel[CHORD_MAX_NOTES];
    for (int i = 0; i < count; ++i) tvel[i] = tone_velocity(i, count, vel);

    /* Emission order, which is only ever different from pitch order when
     * strumming. The root keeps its CHORD_ROOT marking wherever it lands in
     * that order: it is the note the sequencer's recorder stores, so one key
     * press writes one step instead of three notes fighting over it. */
    int order[CHORD_MAX_NOTES];
    const int dir = pi(STRUMDIR);
    const float strum_ms = pv(STRUM);
    bool down = dir == CHORD_STRUM_DOWN;
    if (dir == CHORD_STRUM_ALT) {
        down = s_alt_down;
        s_alt_down = !s_alt_down;
    }
    for (int i = 0; i < count; ++i) order[i] = down ? count - 1 - i : i;
    if (dir == CHORD_STRUM_RAND && strum_ms > 0.0f) {
        for (int i = count - 1; i > 0; --i) {
            const int j = (int)(rng_next() % (uint32_t)(i + 1));
            const int t = order[i];
            order[i] = order[j];
            order[j] = t;
        }
    }

    /* Mono: the previous key's chord goes before this one lands, which is
     * what keeps a three-note chord inside eight voices however fast the
     * player moves. Outside the lock, and before it — release_others() flushes
     * as it goes. */
    if (pi(KEYS) == CHORD_KEYS_MONO) release_others((int)note);

    EmitItem items[CHORD_MAX_NOTES * 2];
    int n = 0;

    taskENTER_CRITICAL(&s_lock);

    /* Re-pressing a held key releases the old chord first: the settings may
     * have moved, and two entries for one key would leak the first one's
     * references when the single note-off arrives. At most one entry, so this
     * one fits in the buffer above. */
    const int existing = find_key(note, want_pre);
    if (existing >= 0) {
        n += release_entry(existing, items + n, CHORD_MAX_NOTES);
    }

    int slot = -1;
    for (int i = 0; i < kMaxKeys; ++i) {
        if (!s_held[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* Table full. Taking the key anyway would strand its note-off with
         * nothing to release; handing it back means it plays as a plain note,
         * which is audible and recoverable. */
        taskEXIT_CRITICAL(&s_lock);
        flush(items, n);
        return false;
    }

    Held& h = s_held[slot];
    h.used = true;
    h.key = note;
    h.vel = vel;
    h.pre = want_pre;
    h.count = (uint8_t)count;
    h.emitted = 0;
    for (int i = 0; i < count; ++i) h.note[i] = (uint8_t)tone[i];

    const int64_t now = esp_timer_get_time();
    bool scheduled = false;
    for (int k = 0; k < count; ++k) {
        const int i = order[k];
        const int src = i == root_idx ? MIDI_NOTE_CHORD_ROOT
                                      : MIDI_NOTE_CHORD_TONE;
        const int64_t delay_us = (int64_t)(strum_ms * 1000.0f) * (int64_t)k;
        if (delay_us <= 0) {
            h.emitted |= (uint16_t)(1u << i);
            if (!ref_add((uint8_t)tone[i])) continue;
            items[n].note = (uint8_t)tone[i];
            items[n].vel = tvel[i];
            items[n].on = true;
            items[n].pre = want_pre;
            items[n].src = src;
            ++n;
            continue;
        }
        int pi_slot = -1;
        for (int j = 0; j < kMaxPending; ++j) {
            if (!s_pend[j].used) {
                pi_slot = j;
                break;
            }
        }
        if (pi_slot < 0) {
            /* No room on the strum clock: play it now rather than drop it. A
             * chord missing its top note is worse than one that strummed
             * less evenly than asked. */
            h.emitted |= (uint16_t)(1u << i);
            if (!ref_add((uint8_t)tone[i])) continue;
            items[n].note = (uint8_t)tone[i];
            items[n].vel = tvel[i];
            items[n].on = true;
            items[n].pre = want_pre;
            items[n].src = src;
            ++n;
            continue;
        }
        Pending& p = s_pend[pi_slot];
        p.used = true;
        p.due_us = now + delay_us;
        p.key = note;
        p.note = (uint8_t)tone[i];
        p.vel = tvel[i];
        p.slot = (int8_t)slot;
        p.bit = (uint8_t)i;
        p.pre = want_pre;
        p.src = src;
        scheduled = true;
    }
    taskEXIT_CRITICAL(&s_lock);

    flush(items, n);
    if (scheduled && s_strum_task != nullptr) xTaskNotifyGive(s_strum_task);
    return true;
}

bool chord_key_off(uint8_t note, bool allow, int when) {
    if (note > 127 || !allow) return false;
    /* Answered from the table, not from the parameters: a key held while
     * chord mode was switched off, or its route changed, still has to release
     * exactly what it started. */
    const bool pre = when == CHORD_WHEN_PRE;
    EmitItem items[CHORD_MAX_NOTES];
    int n = 0;
    taskENTER_CRITICAL(&s_lock);
    const int slot = find_key(note, pre);
    if (slot >= 0) {
        n = release_entry(slot, items,
                          (int)(sizeof(items) / sizeof(items[0])));
    }
    taskEXIT_CRITICAL(&s_lock);
    if (slot < 0) return false;
    flush(items, n);
    return true;
}

void chord_all_off(void) {
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < kMaxPending; ++i) s_pend[i].used = false;
    taskEXIT_CRITICAL(&s_lock);
    /* -1 keeps nothing: every entry goes. The note-offs are sent even though
     * the caller has usually just told the voice manager to drop everything —
     * this path also serves chord.enable going false with keys down, where
     * nothing else has released them. */
    release_others(-1);
    taskENTER_CRITICAL(&s_lock);
    memset(s_ref, 0, sizeof(s_ref));
    taskEXIT_CRITICAL(&s_lock);
    s_prev_count = 0;
}

/* ---- the user set ---- */

void chord_user_get(int slot, chord_user_slot_t* out) {
    if (out == nullptr) return;
    if (slot < 0 || slot >= CHORD_USER_SLOTS) {
        memset(out, 0, sizeof(*out));
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    *out = s_user[slot];
    taskEXIT_CRITICAL(&s_lock);
}

void chord_user_set(int slot, const chord_user_slot_t* in) {
    if (in == nullptr || slot < 0 || slot >= CHORD_USER_SLOTS) return;
    chord_user_slot_t u = *in;
    if (u.count > CHORD_USER_IVS) u.count = CHORD_USER_IVS;
    if (u.transpose < -24) u.transpose = -24;
    if (u.transpose > 24) u.transpose = 24;
    taskENTER_CRITICAL(&s_lock);
    s_user[slot] = u;
    taskEXIT_CRITICAL(&s_lock);
    const uint32_t rev = s_user_rev.fetch_add(1, std::memory_order_relaxed) + 1;
    ParamStore::instance().set(CHORD_PID_REV, (float)(rev & 0xFFFFFFu),
                               ParamOrigin::Internal);
}

void chord_user_reset(void) {
    taskENTER_CRITICAL(&s_lock);
    user_defaults();
    taskEXIT_CRITICAL(&s_lock);
    const uint32_t rev = s_user_rev.fetch_add(1, std::memory_order_relaxed) + 1;
    ParamStore::instance().set(CHORD_PID_REV, (float)(rev & 0xFFFFFFu),
                               ParamOrigin::Internal);
}

size_t chord_user_export(void* buf, size_t cap) {
    if (buf == nullptr || cap < sizeof(s_user)) return 0;
    taskENTER_CRITICAL(&s_lock);
    memcpy(buf, s_user, sizeof(s_user));
    taskEXIT_CRITICAL(&s_lock);
    return sizeof(s_user);
}

bool chord_user_import(const void* buf, size_t len) {
    if (buf == nullptr || len != sizeof(s_user)) return false;
    taskENTER_CRITICAL(&s_lock);
    memcpy(s_user, buf, sizeof(s_user));
    for (int i = 0; i < CHORD_USER_SLOTS; ++i) {
        if (s_user[i].count > CHORD_USER_IVS) s_user[i].count = CHORD_USER_IVS;
    }
    taskEXIT_CRITICAL(&s_lock);
    const uint32_t rev = s_user_rev.fetch_add(1, std::memory_order_relaxed) + 1;
    ParamStore::instance().set(CHORD_PID_REV, (float)(rev & 0xFFFFFFu),
                               ParamOrigin::Internal);
    return true;
}

/* ---- lifecycle ---- */

esp_err_t chord_init(void) {
    memset(s_held, 0, sizeof(s_held));
    memset(s_pend, 0, sizeof(s_pend));
    memset(s_ref, 0, sizeof(s_ref));
    user_defaults();

    build_params();
    ParamStore& ps = ParamStore::instance();
    const size_t added = ps.add(s_params, P_COUNT);
    if (added != P_COUNT) {
        ESP_LOGE(TAG, "registered %u/%d params", (unsigned)added, P_COUNT);
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < P_COUNT; ++i) s_p[i] = ps.valuePtr(s_params[i].id);

    /* seqarp_init() runs first (main.cpp), so these resolve. A null pointer
     * is not fatal — effective_scale() falls back to chord's own scale and
     * root, which is a working instrument with one feature quietly off. */
    s_seq_scale = ps.valuePtr(SEQ_PID_SCALE);
    s_seq_root = ps.valuePtr(SEQ_PID_ROOT);
    if (s_seq_scale == nullptr || s_seq_root == nullptr) {
        ESP_LOGW(TAG, "seq.scale/root unavailable — chord.follow will not");
    }

    if (xTaskCreatePinnedToCore(strum_task, "chord_str", kStrumTaskStack,
                                nullptr, kStrumTaskPrio, &s_strum_task,
                                0) != pdPASS) {
        /* A strum is a garnish; the chord is not. Without the task, strummed
         * tones fall back to the immediate path (no pending slot is ever
         * consumed because nothing drains it — so the scheduler below would
         * stall), which is why this refuses instead: a silent top note on
         * every chord would be far harder to explain than a boot error. */
        ESP_LOGE(TAG, "cannot create the strum task");
        return ESP_ERR_NO_MEM;
    }

    /* Re-voice held chords when a setting moves (see revoice_entry). Failing
     * to get a listener slot is not fatal: chord mode works, it just goes
     * back to applying a change on the next note. */
    if (ps.addListener(param_listener, nullptr) < 0) {
        ESP_LOGW(TAG, "no listener slot — settings apply on the next note");
    }

    /* Last, so the router can never see a half-built component: the hook is
     * a plain pointer assignment and the first note may arrive on another
     * task the instant it lands. */
    midi_set_chord_hook(router_hook, nullptr);

    ESP_LOGI(TAG, "chord mode ready (%d qualities, %d scales, %d user slots)",
             kQualityCount, SEQ_SCALE_COUNT, CHORD_USER_SLOTS);
    return ESP_OK;
}
