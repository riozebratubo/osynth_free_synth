/*
 * osynth — sequencer playback engine (Session 23). Contract in seq_play.h.
 *
 * Timing model. Every track carries `base_tick`, the *nominal* tick of its
 * next step, and `next_tick`, that value shifted by swing, micro-timing and
 * humanisation. Firing advances base_tick by exactly one division and
 * recomputes the shift for the new step. Keeping the two apart is what stops
 * timing offsets from accumulating: a step nudged 30 % late does not drag
 * the following step with it, which is what a naive "delay then continue"
 * implementation gets wrong and why such sequencers drift out of sync with
 * an external clock over a few bars.
 */
#include "seq_play.h"

#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "drums.h"
#include "midi.h"
#include "seqarp.h"
#include "synth_params.h"

static const char* TAG = "seqplay";

using osynth::ParamOrigin;
using osynth::ParamStore;

namespace {

constexpr int kMaxPendingOffs = 24; /* one per track plus slide overlaps */

struct ActiveLock {
    uint16_t pid;
    uint32_t gen; /* ParamStore::generation() when `base` was captured */
    float base;   /* value to restore when the lock lapses */
};

/* True when the id `pid` refers to now is still the parameter it referred to
 * when a lock captured its pre-lock value. The 0x02xx range is re-registered
 * from scratch on every engine switch, so a lock captured under the FM engine
 * and released under the wavetable engine would write an FM operator level
 * into whatever wavetable parameter inherited the id. */
inline bool lock_still_valid(const ActiveLock& l) {
    return l.gen == ParamStore::instance().generation();
}

struct TrackState {
    int32_t base_tick;  /* nominal tick of the next step */
    int32_t next_tick;  /* base_tick after swing / micro / humanise */
    int16_t step;       /* index of the next step to fire */
    int8_t dir;         /* +1 / -1, ping-pong only */
    uint16_t loop_count; /* completed passes, drives the x:y conditions */
    bool prev_cond;     /* result of the last conditional trig */
    uint8_t last_note;  /* for slide/legato hand-off */
    bool note_held;

    /* Ratchet in flight */
    uint8_t rat_left;
    uint16_t rat_interval;
    int32_t rat_next;
    uint8_t rat_note;
    uint8_t rat_vel;
    uint16_t rat_gate;

    uint8_t lock_count;
    ActiveLock lock[SEQ_TRACK_LOCKS];

    uint32_t rng;
};

struct PendingOff {
    uint8_t note;
    uint8_t track;
    int32_t at_tick;
    bool used;
};

TrackState s_trk[SEQ_TRACKS];
PendingOff s_off[kMaxPendingOffs];
int32_t s_tick = 0;
bool s_running = false;
bool s_fill = false;
int s_pattern = 0;
int s_queued = -1;
int32_t s_pattern_ticks = 0; /* length of one pass of the master track */
int32_t s_pattern_tick = 0;
int s_song_index = 0;
int s_song_repeat = 0;

/* Parameters the tick path reads every step. */
const std::atomic<float>* s_p_swing = nullptr;
const std::atomic<float>* s_p_accent = nullptr;
const std::atomic<float>* s_p_song = nullptr;
const std::atomic<float>* s_p_quant = nullptr;
const std::atomic<float>* s_p_edit_trk = nullptr;
const std::atomic<float>* s_p_edit_stp = nullptr;
const std::atomic<float>* s_p_mute[SEQ_TRACKS];
const std::atomic<float>* s_p_solo[SEQ_TRACKS];

inline float pv(const std::atomic<float>* p) {
    return p != nullptr ? p->load(std::memory_order_relaxed) : 0.0f;
}
inline int pi(const std::atomic<float>* p) { return (int)(pv(p) + 0.5f); }

uint32_t rng_next(uint32_t& s) {
    uint32_t x = s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return s = x;
}

/* ---- note plumbing ---------------------------------------------------- */

void emit_on(uint8_t note, uint8_t vel) {
    midi_route_channel_message(0x90, note, vel);
}
void emit_off(uint8_t note) { midi_route_channel_message(0x80, note, 0); }

void schedule_off(uint8_t note, int track, int32_t at_tick) {
    for (int i = 0; i < kMaxPendingOffs; ++i) {
        if (!s_off[i].used) {
            s_off[i].used = true;
            s_off[i].note = note;
            s_off[i].track = (uint8_t)track;
            s_off[i].at_tick = at_tick;
            return;
        }
    }
    /* Table full: release the note now rather than leaking it. A stuck note
     * is the one failure mode a sequencer must never have. */
    emit_off(note);
}

void flush_offs(int track) {
    for (int i = 0; i < kMaxPendingOffs; ++i) {
        if (s_off[i].used && (track < 0 || s_off[i].track == track)) {
            emit_off(s_off[i].note);
            s_off[i].used = false;
        }
    }
    if (track >= 0 && track < SEQ_TRACKS) s_trk[track].note_held = false;
}

void tick_offs() {
    for (int i = 0; i < kMaxPendingOffs; ++i) {
        if (s_off[i].used && s_tick >= s_off[i].at_tick) {
            emit_off(s_off[i].note);
            s_off[i].used = false;
            if (s_off[i].track < SEQ_TRACKS) {
                s_trk[s_off[i].track].note_held = false;
            }
        }
    }
}

/* ---- parameter locks -------------------------------------------------- */

/* Applies this step's locks and releases any that no longer apply. The
 * pre-lock value is stashed on first apply, so a lock composes with a live
 * knob edit instead of fighting it, and the parameter returns to what the
 * player left it at once the locked steps stop coming. */
void apply_locks(int track, int pattern, int step) {
    seq_plock_t locks[SEQ_TRACK_LOCKS];
    const int n = seq_plocks_for_step(pattern, track, step, locks,
                                      SEQ_TRACK_LOCKS);
    TrackState& t = s_trk[track];
    ParamStore& ps = ParamStore::instance();

    /* Release the ones this step does not carry. */
    for (int i = t.lock_count - 1; i >= 0; --i) {
        bool still = false;
        for (int j = 0; j < n; ++j) {
            if (locks[j].pid == t.lock[i].pid) {
                still = true;
                break;
            }
        }
        if (!still) {
            if (lock_still_valid(t.lock[i])) {
                ps.set(t.lock[i].pid, t.lock[i].base, ParamOrigin::Internal);
            }
            t.lock[i] = t.lock[--t.lock_count];
        }
    }
    /* Apply (or update) the ones it does. */
    for (int j = 0; j < n; ++j) {
        int found = -1;
        for (int i = 0; i < t.lock_count; ++i) {
            if (t.lock[i].pid == locks[j].pid) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            if (t.lock_count >= SEQ_TRACK_LOCKS) continue;
            found = t.lock_count++;
            t.lock[found].pid = locks[j].pid;
            t.lock[found].gen = ps.generation();
            t.lock[found].base = ps.get(locks[j].pid);
        } else if (!lock_still_valid(t.lock[found])) {
            /* The registry moved under a held lock: re-anchor on the new
             * parameter rather than carrying a base that means nothing. */
            t.lock[found].gen = ps.generation();
            t.lock[found].base = ps.get(locks[j].pid);
        }
        ps.set(locks[j].pid, locks[j].value, ParamOrigin::Internal);
    }
}

void release_locks(int track) {
    TrackState& t = s_trk[track];
    ParamStore& ps = ParamStore::instance();
    for (int i = 0; i < t.lock_count; ++i) {
        if (lock_still_valid(t.lock[i])) {
            ps.set(t.lock[i].pid, t.lock[i].base, ParamOrigin::Internal);
        }
    }
    t.lock_count = 0;
}

/* ---- trig conditions -------------------------------------------------- */

bool cond_passes(uint8_t cond, TrackState& t) {
    const int loop = t.loop_count;
    switch (cond) {
        case SEQ_COND_ALWAYS:   return true;
        case SEQ_COND_1_2:      return (loop % 2) == 0;
        case SEQ_COND_2_2:      return (loop % 2) == 1;
        case SEQ_COND_1_3:      return (loop % 3) == 0;
        case SEQ_COND_2_3:      return (loop % 3) == 1;
        case SEQ_COND_3_3:      return (loop % 3) == 2;
        case SEQ_COND_1_4:      return (loop % 4) == 0;
        case SEQ_COND_2_4:      return (loop % 4) == 1;
        case SEQ_COND_3_4:      return (loop % 4) == 2;
        case SEQ_COND_4_4:      return (loop % 4) == 3;
        case SEQ_COND_1_8:      return (loop % 8) == 0;
        case SEQ_COND_2_8:      return (loop % 8) == 1;
        case SEQ_COND_4_8:      return (loop % 8) == 3;
        case SEQ_COND_8_8:      return (loop % 8) == 7;
        case SEQ_COND_FILL:     return s_fill;
        case SEQ_COND_NOT_FILL: return !s_fill;
        case SEQ_COND_PREV:     return t.prev_cond;
        case SEQ_COND_NOT_PREV: return !t.prev_cond;
        default:                return true;
    }
}

/* ---- step scheduling -------------------------------------------------- */

int effective_swing(const seq_track_cfg_t& cfg, const seq_pattern_cfg_t& pat) {
    int s = cfg.swing == 0xFF ? pat.swing : cfg.swing;
    /* The live parameter overrides a stored 50 (straight), so the app's
     * swing knob works without editing every track. */
    if (s == 50) s = (int)(pv(s_p_swing) + 0.5f);
    if (s < 0) s = 0;
    if (s > 75) s = 75;
    return s;
}

/* Where step `index` should actually fire, relative to its nominal tick. */
int32_t step_offset(const seq_track_cfg_t& cfg, const seq_pattern_cfg_t& pat,
                    const seq_step_t& st, int index, int step_ticks,
                    TrackState& t) {
    int32_t off = 0;
    const int swing = effective_swing(cfg, pat);
    if ((index & 1) != 0 && swing != 50) {
        /* Second of each pair lands at swing% of the pair's span. */
        off += (int32_t)step_ticks * (2 * swing - 100) / 100;
    }
    off += (int32_t)st.micro * step_ticks / 100;
    if (cfg.humanize > 0) {
        const int spread = (int)cfg.humanize * step_ticks / 400; /* <= 25 % */
        if (spread > 0) {
            off += (int32_t)(rng_next(t.rng) % (uint32_t)(2 * spread + 1)) -
                   spread;
        }
    }
    /* Never let a shift push a step before the previous one's nominal slot;
     * that would reorder trigs and break the note-off bookkeeping. */
    if (off <= -step_ticks) off = -step_ticks + 1;
    return off;
}

/* Advances a track's step index and returns true if it completed a pass. */
bool advance_step(TrackState& t, const seq_track_cfg_t& cfg, int len) {
    if (len <= 1) {
        t.step = 0;
        return true;
    }
    switch (cfg.dir) {
        case SEQ_DIR_REV:
            if (--t.step < 0) {
                t.step = (int16_t)(len - 1);
                return true;
            }
            return false;
        case SEQ_DIR_PINGPONG:
            t.step = (int16_t)(t.step + t.dir);
            if (t.step >= len - 1) {
                t.step = (int16_t)(len - 1);
                t.dir = -1;
                return true;
            }
            if (t.step <= 0) {
                t.step = 0;
                t.dir = 1;
                return true;
            }
            return false;
        case SEQ_DIR_RANDOM:
            t.step = (int16_t)(rng_next(t.rng) % (uint32_t)len);
            return t.step == 0;
        case SEQ_DIR_BROWNIAN: {
            const int delta = (int)(rng_next(t.rng) % 5) - 2;
            int s = t.step + (delta == 0 ? 1 : delta);
            s %= len;
            if (s < 0) s += len;
            t.step = (int16_t)s;
            return s == 0;
        }
        case SEQ_DIR_FWD:
        default:
            if (++t.step >= len) {
                t.step = 0;
                return true;
            }
            return false;
    }
}

void reschedule(int track, const seq_track_cfg_t& cfg,
                const seq_pattern_cfg_t& pat) {
    TrackState& t = s_trk[track];
    const int step_ticks = seq_div_ticks(cfg.div);
    seq_step_t st;
    seq_step_get(s_pattern, track, t.step, &st);
    t.next_tick = t.base_tick + step_offset(cfg, pat, st, t.step, step_ticks, t);
}

/* ---- firing ----------------------------------------------------------- */

bool track_audible(int track) {
    /* Solo is global: any soloed track silences every track that is not. */
    bool any_solo = false;
    for (int i = 0; i < SEQ_TRACKS; ++i) {
        if (pv(s_p_solo[i]) >= 0.5f) {
            any_solo = true;
            break;
        }
    }
    if (any_solo && pv(s_p_solo[track]) < 0.5f) return false;
    return pv(s_p_mute[track]) < 0.5f;
}

void trigger_note(int track, const seq_track_cfg_t& cfg, uint8_t note,
                  uint8_t vel, int gate_ticks, bool slide) {
    TrackState& t = s_trk[track];
    if (cfg.target == SEQ_TARGET_DRUM) {
        int slot = cfg.slot;
        if (slot == SEQ_SLOT_FROM_NOTE) {
            /* Let the kit's own note map choose. Falling back to note-36
             * keeps a lane usable with a kit that has no GM mapping (an SD
             * folder), where slot n simply answers to note 36+n. */
            slot = drums_slot_for_note(note);
            if (slot < 0) slot = (int)note - 36;
        }
        drums_trigger(slot, vel, 0);
        return;
    }

    if (slide && t.note_held && t.last_note != note) {
        /* Overlap the previous note by a tick so the engine's glide has
         * something to glide *from* — that is what makes a slide a slide. */
        for (int i = 0; i < kMaxPendingOffs; ++i) {
            if (s_off[i].used && s_off[i].track == track) {
                s_off[i].at_tick = s_tick + 1;
            }
        }
    } else {
        flush_offs(track);
    }
    emit_on(note, vel);
    schedule_off(note, track, s_tick + (gate_ticks > 0 ? gate_ticks : 1));
    t.last_note = note;
    t.note_held = true;
}

void fire_step(int track, const seq_track_cfg_t& cfg,
               const seq_pattern_cfg_t& pat) {
    TrackState& t = s_trk[track];
    const int index = t.step;
    seq_step_t st;
    seq_step_get(s_pattern, track, index, &st);

    apply_locks(track, s_pattern, index);

    if (st.vel != 0 && (st.flags & SEQ_STEP_F_MUTE) == 0 && track_audible(track)) {
        int prob = (int)st.prob * (int)cfg.prob_scale / 100;
        const bool cond_ok = cond_passes(st.cond, t);
        const bool prob_ok =
            prob >= 100 || (int)(rng_next(t.rng) % 100u) < prob;
        if (cond_ok && prob_ok) {
            int vel = (int)st.vel * (int)cfg.vel_scale / 100;
            if (st.flags & SEQ_STEP_F_ACCENT) {
                vel = (int)((float)vel * pv(s_p_accent));
            }
            if (cfg.humanize > 0) {
                const int spread = (int)cfg.humanize * 30 / 100;
                if (spread > 0) {
                    vel += (int)(rng_next(t.rng) %
                                 (uint32_t)(2 * spread + 1)) - spread;
                }
            }
            if (vel < 1) vel = 1;
            if (vel > 127) vel = 127;

            int note = (int)st.note + cfg.transpose;
            while (note < 0) note += 12;
            while (note > 127) note -= 12;
            if (cfg.target == SEQ_TARGET_SYNTH) {
                const int scale = cfg.scale == 0xFF ? pat.scale : cfg.scale;
                const int root = cfg.root == 0xFF ? pat.root : cfg.root;
                note = seq_quantize((uint8_t)note, scale, root);
            }

            const int step_ticks = seq_div_ticks(cfg.div);
            int gate = (int)st.gate * step_ticks / 16;
            gate = gate * (int)cfg.gate_scale / 100;
            if (gate < 1) gate = 1;

            trigger_note(track, cfg, (uint8_t)note, (uint8_t)vel, gate,
                         (st.flags & SEQ_STEP_F_SLIDE) != 0);

            if (st.ratchet > 1) {
                t.rat_left = (uint8_t)(st.ratchet - 1);
                t.rat_interval = (uint16_t)(step_ticks / st.ratchet);
                if (t.rat_interval < 1) t.rat_interval = 1;
                t.rat_next = s_tick + t.rat_interval;
                t.rat_note = (uint8_t)note;
                t.rat_vel = (uint8_t)vel;
                t.rat_gate = (uint16_t)(t.rat_interval > 1 ? t.rat_interval - 1
                                                           : 1);
            }
        }
        if (st.cond != SEQ_COND_ALWAYS) t.prev_cond = cond_ok && prob_ok;
    }
}

void tick_ratchets(int track, const seq_track_cfg_t& cfg) {
    TrackState& t = s_trk[track];
    if (t.rat_left == 0 || s_tick < t.rat_next) return;
    trigger_note(track, cfg, t.rat_note, t.rat_vel, t.rat_gate, false);
    t.rat_next += t.rat_interval;
    --t.rat_left;
}

void reset_track(int track, bool rewind) {
    TrackState& t = s_trk[track];
    if (rewind) {
        seq_track_cfg_t cfg;
        seq_track_cfg_get(s_pattern, track, &cfg);
        t.step = (cfg.dir == SEQ_DIR_REV)
                     ? (int16_t)(seq_track_length(s_pattern, track) - 1)
                     : 0;
        t.base_tick = s_tick;
        t.next_tick = s_tick;
        t.loop_count = 0;
    }
    t.dir = 1;
    t.rat_left = 0;
    t.note_held = false;
    t.prev_cond = false;
    if (t.rng == 0) t.rng = 0x2545f491u ^ (uint32_t)(track * 2654435761u);
}

void recompute_pattern_span() {
    seq_track_cfg_t cfg;
    seq_track_cfg_get(s_pattern, 0, &cfg);
    s_pattern_ticks = (int32_t)seq_track_length(s_pattern, 0) *
                      seq_div_ticks(cfg.div);
    if (s_pattern_ticks < 1) s_pattern_ticks = 1;
}

void switch_pattern(int pattern) {
    if (pattern == s_pattern) return;
    for (int i = 0; i < SEQ_TRACKS; ++i) release_locks(i);
    s_pattern = pattern;
    for (int i = 0; i < SEQ_TRACKS; ++i) reset_track(i, true);
    recompute_pattern_span();
    ParamStore::instance().set(SEQ_PID_CURPAT, (float)s_pattern,
                               ParamOrigin::Internal);
}

/* At a pattern boundary: advance the song chain, or honour a queued manual
 * pattern change. Both only ever happen here — a pattern that switched
 * mid-bar would restart every track's step counter out of phase. */
void pattern_boundary() {
    if (pv(s_p_song) >= 0.5f && seq_song_length() > 0) {
        seq_song_entry_t e;
        seq_song_get(s_song_index, &e);
        /* seq_song_set()/seq_song_set_length() both floor `repeats` at 1, so
         * a chain entry always advances after at least one pass. */
        if (++s_song_repeat >= e.repeats) {
            s_song_repeat = 0;
            s_song_index = (s_song_index + 1) % seq_song_length();
            seq_song_get(s_song_index, &e);
            switch_pattern(e.pattern);
            ParamStore::instance().set(SEQ_PID_PATTERN, (float)e.pattern,
                                       ParamOrigin::Internal);
            return;
        }
    }
    if (s_queued >= 0) {
        switch_pattern(s_queued);
        s_queued = -1;
    }
}

} // namespace

/* ======================= public API ==================================== */

void seq_play_init(void) {
    memset(s_trk, 0, sizeof(s_trk));
    memset(s_off, 0, sizeof(s_off));
    for (int i = 0; i < SEQ_TRACKS; ++i) {
        s_trk[i].rng = 0x2545f491u ^ (uint32_t)((i + 1) * 2654435761u);
        s_trk[i].dir = 1;
    }
    s_pattern = 0;
    s_queued = -1;
    recompute_pattern_span();
}

void seq_play_bind_params(void) {
    ParamStore& ps = ParamStore::instance();
    s_p_swing = ps.valuePtr(SEQ_PID_SWING);
    s_p_accent = ps.valuePtr(SEQ_PID_ACCENT);
    s_p_song = ps.valuePtr(SEQ_PID_SONG);
    s_p_quant = ps.valuePtr(SEQ_PID_QUANT);
    s_p_edit_trk = ps.valuePtr(SEQ_PID_EDIT_TRACK);
    s_p_edit_stp = ps.valuePtr(SEQ_PID_EDIT_STEP);
    for (int i = 0; i < SEQ_TRACKS; ++i) {
        s_p_mute[i] = ps.valuePtr((uint16_t)SEQ_PID_TRACK_MUTE(i));
        s_p_solo[i] = ps.valuePtr((uint16_t)SEQ_PID_TRACK_SOLO(i));
    }
}

void seq_play_tick(void) {
    if (!s_running || !seq_model_ready()) return;
    ++s_tick;
    tick_offs();

    seq_pattern_cfg_t pat;
    seq_pattern_cfg_get(s_pattern, &pat);

    for (int i = 0; i < SEQ_TRACKS; ++i) {
        seq_track_cfg_t cfg;
        seq_track_cfg_get(s_pattern, i, &cfg);
        tick_ratchets(i, cfg);

        TrackState& t = s_trk[i];
        /* `while` rather than `if`: a very fast division combined with a
         * catch-up burst of ticks can owe more than one step. */
        int guard = 0;
        while (s_tick >= t.next_tick && ++guard <= 16) {
            const int len = seq_track_length(s_pattern, i);
            if (t.step >= len) t.step = 0;
            fire_step(i, cfg, pat);
            const int step_ticks = seq_div_ticks(cfg.div);
            t.base_tick += step_ticks;
            if (advance_step(t, cfg, len)) ++t.loop_count;
            reschedule(i, cfg, pat);
            if (i == 0) {
                ParamStore::instance().set(SEQ_PID_POS, (float)t.step,
                                           ParamOrigin::Internal);
            }
        }
    }

    if (++s_pattern_tick >= s_pattern_ticks) {
        s_pattern_tick = 0;
        pattern_boundary();
    }
}

void seq_play_start(bool rewind) {
    if (!seq_model_ready()) return;
    if (rewind) {
        s_tick = 0;
        s_pattern_tick = 0;
        s_song_index = 0;
        s_song_repeat = 0;
        if (pv(s_p_song) >= 0.5f && seq_song_length() > 0) {
            seq_song_entry_t e;
            seq_song_get(0, &e);
            if (e.pattern != s_pattern) switch_pattern(e.pattern);
        }
    }
    for (int i = 0; i < SEQ_TRACKS; ++i) reset_track(i, rewind);
    recompute_pattern_span();
    s_running = true;
}

void seq_play_stop(void) {
    s_running = false;
    flush_offs(-1);
    for (int i = 0; i < SEQ_TRACKS; ++i) {
        s_trk[i].rat_left = 0;
        release_locks(i);
    }
    ParamStore::instance().set(SEQ_PID_POS, -1.0f, ParamOrigin::Internal);
}

bool seq_play_running(void) { return s_running; }

void seq_play_select_pattern(int pattern, bool immediate) {
    if (pattern < 0 || pattern >= SEQ_PATTERNS) return;
    if (immediate || !s_running) {
        switch_pattern(pattern);
        s_queued = -1;
    } else {
        s_queued = pattern;
    }
}

int seq_play_current_pattern(void) { return s_pattern; }

void seq_play_set_fill(bool on) { s_fill = on; }

int seq_play_position(int track) {
    if (!s_running || track < 0 || track >= SEQ_TRACKS) return -1;
    return s_trk[track].step;
}

int seq_play_record_note(uint8_t note, uint8_t velocity) {
    if (!seq_model_ready() || velocity == 0) return -1;
    int track = pi(s_p_edit_trk) - 1;
    if (track < 0) track = 0;
    if (track >= SEQ_TRACKS) track = SEQ_TRACKS - 1;

    seq_track_cfg_t cfg;
    seq_track_cfg_get(s_pattern, track, &cfg);
    const int len = seq_track_length(s_pattern, track);

    int at;
    if (s_running) {
        /* Quantise to the grid the player can hear: the note lands on
         * whichever step boundary is nearest, so playing slightly early
         * records as "on the beat" rather than one step back. */
        const TrackState& t = s_trk[track];
        const int step_ticks = seq_div_ticks(cfg.div);
        const int32_t into = s_tick - (t.base_tick - step_ticks);
        at = t.step - 1;
        if (into * 2 >= step_ticks) at = t.step; /* closer to the next one */
        const int q = pi(s_p_quant);
        if (q > 0) {
            /* Coarser quantise: 1 = 1/4, 2 = 1/8, 3 = 1/16. */
            static const int kQuantTicks[] = {0, 96, 48, 24};
            const int qt = kQuantTicks[q < 4 ? q : 3];
            if (qt > 0 && step_ticks > 0) {
                const int per = qt / step_ticks;
                if (per > 1) at = (at / per) * per;
            }
        }
        while (at < 0) at += len;
        at %= len;
    } else {
        at = pi(s_p_edit_stp);
        if (at < 0) at = 0;
        if (at >= len) at = 0;
    }

    seq_step_t st = {};
    st.note = note & 0x7F;
    st.vel = velocity & 0x7F;
    st.gate = 16;
    st.prob = 100;
    st.ratchet = 1;
    st.cond = SEQ_COND_ALWAYS;
    seq_step_set(s_pattern, track, at, &st);

    if (!s_running) { /* step input: walk the cursor forward */
        ParamStore::instance().set(SEQ_PID_EDIT_STEP, (float)((at + 1) % len),
                                   ParamOrigin::Internal);
    }
    ESP_LOGD(TAG, "rec: track %d step %d note %d vel %d", track + 1, at, note,
             velocity);
    return at;
}
