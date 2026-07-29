/*
 * osynth — master clock, arpeggiator and transport (S12, reworked in S23).
 *
 * One 96-PPQN tick pipeline feeds the arpeggiator and the sequencer
 * (seq_play.cpp). Ticks come from an internal esp_timer at seq.tempo
 * (seq.clock = internal) or are recovered from MIDI real-time clock bytes
 * (seq.clock = midi); either source only bumps an atomic counter and wakes
 * the `seq_clk` task (core 0), which does all sequencing work — nothing
 * heavier than a counter increment ever runs on the USB or serial-MIDI
 * tasks.
 *
 * 24 -> 96 PPQN. External MIDI clock is defined at 24 PPQN, and S12 ran the
 * whole pipeline at that rate. The S23 sequencer needs finer resolution
 * *inside* a step (micro-timing, up to 8 ratchets, exact triplets), so the
 * internal grid is 4x finer and external clock is multiplied up: the free-
 * running sub-tick timer is paced at the measured clock interval / 4, while
 * the count of received clock bytes bounds how far it may run ahead. That
 * gives sub-tick resolution under external sync without ever drifting past
 * the master — the timer can only fill in between clock bytes, never
 * invent them.
 *
 * Generated notes leave through midi_route_channel_message() — the same
 * entry point as played notes, so the voice manager, engines and FX cannot
 * tell sequenced notes from played ones. Input arrives through the router's
 * note tap: the arpeggiator consumes key on/offs while active, and the
 * sequencer records note-ons as steps while in rec. A 128-bit `owned` map
 * remembers which note-ons were consumed, so a note-off is only swallowed
 * when its note-on was — toggling the arp mid-hold can never hang a voice.
 * The tap ignores events raised by the seq_clk task itself (task-handle
 * guard), which is what breaks the feedback loop.
 */
#include "seqarp.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "drums.h"
#include "midi.h"
#include "seq_model.h"
#include "seq_play.h"
#include "synth_params.h"

static const char* TAG = "seqarp";

using osynth::ParamCurve;
using osynth::ParamDesc;
using osynth::ParamOrigin;
using osynth::ParamStore;
using osynth::ParamType;

namespace {

constexpr int kPpqn = 96;      /* internal resolution: ticks per quarter */
constexpr int kMidiPpqn = 24;  /* what a MIDI clock byte means */
constexpr int kSubTicks = kPpqn / kMidiPpqn;
constexpr int kMaxHeld = 16;   /* arp input chord ceiling */
constexpr int kMaxPending = 8; /* arp note-offs in flight */
constexpr int kMaxBurst = 384; /* tick catch-up ceiling per wake (one bar) */
constexpr int kFireNow = 0x4000; /* tick_in_step: force a step on next tick */
/* How far the recovered clock may lag before it stops interpolating and
 * jumps: a quarter of a 1/16 step, i.e. inaudible, but bounded. */
constexpr int32_t kSlaveCatchUp = 8;
/* Silence on the external clock that counts as "the master is gone". Long
 * enough that a ritardando to the 30 BPM floor (500 ms between 0xF8 bytes at
 * 24 PPQN is already 5 BPM) never trips it, short enough that a yanked cable
 * does not leave a chord droning. */
constexpr int64_t kExtStallUs = 750000;

constexpr int kClkTaskPrio = 10; /* control plane, core 0 (ARCHITECTURE.md) */
constexpr int kClkTaskStack = 5120;

/* ---- parameters (order matches PIdx) ---- */

enum PIdx {
    TEMPO, CLOCK_SRC, DIV, GATE,
    SWING, PATTERN, SONG, SCALE, ROOT, FILL, ACCENT, POS, CURPAT, QUANT,
    EDIT_TRACK, EDIT_STEP,
    ARP_MODE, ARP_OCT, ARP_HOLD,
    SEQ_MODE, SEQ_STEPS, COUNTIN,
    P_FIXED
};
constexpr int P_COUNT = P_FIXED + SEQ_TRACKS * 2;

const char* const kClockNames[] = {"internal", "midi"};
const char* const kDivNames[] = {"1/4", "1/8", "1/8t", "1/16", "1/16t", "1/32"};
/* Arpeggiator divisions in 96-PPQN ticks. */
const uint8_t kArpDivTicks[] = {96, 48, 32, 24, 16, 12};
const char* const kArpModeNames[] = {"off",    "up",     "down",
                                     "updown", "random", "played"};
const char* const kSeqModeNames[] = {"stop", "play", "rec"};
const char* const kQuantNames[] = {"off", "1/4", "1/8", "1/16"};
const char* const kScaleNames[SEQ_SCALE_COUNT] = {
    "chromatic", "major", "minor", "dorian", "phrygian", "lydian",
    "mixolydian", "locrian", "harm minor", "penta maj", "penta min", "blues",
};

enum { CLK_INTERNAL = 0, CLK_MIDI };
enum { ARP_OFF = 0, ARP_UP, ARP_DOWN, ARP_UPDOWN, ARP_RANDOM, ARP_PLAYED };
enum { SEQ_STOP = 0, SEQ_PLAY, SEQ_REC };

ParamDesc s_params[P_COUNT];
char s_track_names[SEQ_TRACKS * 2][16];
const std::atomic<float>* s_p[P_COUNT];

inline float pv(PIdx i) { return s_p[i]->load(std::memory_order_relaxed); }
inline int pi(PIdx i) { return (int)(pv(i) + 0.5f); }

/* ---- state shared between the tap (USB/serial tasks) and seq_clk ---- */

portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

struct HeldNote {
    uint8_t note;
    uint8_t vel;
    bool down; /* key physically held (false = kept latched by arp.hold) */
};
HeldNote s_held[kMaxHeld];
int s_held_count = 0;

/* Notes whose note-on the arp consumed: only their note-offs are swallowed. */
uint32_t s_owned[4];
inline bool own_test(uint8_t n) { return (s_owned[n >> 5] >> (n & 31)) & 1u; }
inline void own_set(uint8_t n) { s_owned[n >> 5] |= 1u << (n & 31); }
inline void own_clear(uint8_t n) { s_owned[n >> 5] &= ~(1u << (n & 31)); }

int s_seq_mode_prev = SEQ_STOP; /* edge detection: tap + seq_clk, locked */

/* ---- clock plumbing ---- */

TaskHandle_t s_clk_task = nullptr;
esp_timer_handle_t s_timer = nullptr;
std::atomic<uint32_t> s_ticks{0};      /* sub-ticks produced by the timer */
std::atomic<uint32_t> s_ext_clocks{0}; /* 0xF8 bytes received */
std::atomic<uint64_t> s_ext_last_us{0};
std::atomic<uint32_t> s_ext_period_us{20833 / kSubTicks};
std::atomic<uint32_t> s_flags{0};
constexpr uint32_t kFlagStart = 1u << 0;    /* MIDI 0xFA */
constexpr uint32_t kFlagContinue = 1u << 1; /* MIDI 0xFB */
constexpr uint32_t kFlagStop = 1u << 2;     /* MIDI 0xFC */
constexpr uint32_t kFlagKick = 1u << 3;     /* first arp key: fire now */

/* ---- seq_clk-task-only state ---- */

int s_arp_tick_in_step = 0;
int s_arp_idx = 0;
int s_arp_mode_prev = ARP_OFF;
bool s_arp_hold_prev = false;
bool s_suppress_reset = false; /* MIDI continue: keep the position */
uint32_t s_slave_emitted = 0;
uint32_t s_slave_clocks = 0;
/* Free-running beat position, so a subscriber can bar-lock even with the
 * sequencer stopped. Counts every tick the pipeline processes. */
int s_beat_tick = 0;
int s_beat_in_bar = 0;
seqarp_beat_fn s_beat_cb = nullptr;
void* s_beat_ctx = nullptr;

/* Count-in: beats still to click before the transport is really allowed to
 * play. Armed when seq.mode leaves stop with seq.countin on. */
int s_countin_left = 0;

int s_pattern_prev = 0;
int s_scale_prev = -1;
int s_root_prev = -1;
int s_edit_track_prev = 1;
int s_steps_prev = -1;
bool s_fill_prev = false;

struct Pending {
    uint8_t note;
    int16_t ticks; /* emits the note-off when it reaches 0 */
};
Pending s_pend[kMaxPending];
int s_pend_count = 0;

uint32_t s_rng = 0x2545f491u;
inline uint32_t rng_next() {
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return s_rng = x;
}

/* ---- arpeggiator note emission (seq_clk task only) ---- */

inline void emit_on(uint8_t note, uint8_t vel) {
    midi_route_channel_message(0x90, note, vel);
}
inline void emit_off(uint8_t note) { midi_route_channel_message(0x80, note, 0); }

void pend_push(uint8_t note, int gate_ticks) {
    if (s_pend_count == kMaxPending) { /* never leak a note-off */
        emit_off(s_pend[0].note);
        for (int i = 1; i < kMaxPending; ++i) s_pend[i - 1] = s_pend[i];
        --s_pend_count;
    }
    s_pend[s_pend_count].note = note;
    s_pend[s_pend_count].ticks = (int16_t)gate_ticks;
    ++s_pend_count;
}

void pend_tick() {
    int w = 0;
    for (int i = 0; i < s_pend_count; ++i) {
        if (--s_pend[i].ticks <= 0) {
            emit_off(s_pend[i].note);
        } else {
            s_pend[w++] = s_pend[i];
        }
    }
    s_pend_count = w;
}

void pend_flush() {
    for (int i = 0; i < s_pend_count; ++i) emit_off(s_pend[i].note);
    s_pend_count = 0;
}

int cur_arp_ticks() {
    int d = pi(DIV);
    if (d < 0) d = 0;
    if (d > 5) d = 5;
    return kArpDivTicks[d];
}

int cur_gate_ticks(int step_ticks) {
    int g = (int)(pv(GATE) * (float)step_ticks + 0.5f);
    if (g < 1) g = 1;
    if (g > step_ticks) g = step_ticks;
    return g;
}

void sort_pairs(uint8_t* notes, uint8_t* vels, int n) {
    for (int i = 1; i < n; ++i) {
        const uint8_t nt = notes[i], vl = vels[i];
        int j = i - 1;
        while (j >= 0 && notes[j] > nt) {
            notes[j + 1] = notes[j];
            vels[j + 1] = vels[j];
            --j;
        }
        notes[j + 1] = nt;
        vels[j + 1] = vl;
    }
}

void arp_step(int mode, int octaves, int gate_ticks) {
    uint8_t notes[kMaxHeld], vels[kMaxHeld];
    int n;
    taskENTER_CRITICAL(&s_lock);
    n = s_held_count;
    for (int i = 0; i < n; ++i) {
        notes[i] = s_held[i].note;
        vels[i] = s_held[i].vel;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (n == 0) {
        s_arp_idx = 0;
        return;
    }

    if (mode != ARP_PLAYED) sort_pairs(notes, vels, n); /* arrival order else */
    if (octaves < 1) octaves = 1;

    /* Expanded pattern = n notes x octaves; up-down ping-pongs the expansion
     * without repeating the endpoints. Indexing is arithmetic — nothing is
     * materialized. */
    const int base_len = n * octaves;
    const int total = (mode == ARP_UPDOWN && base_len > 1) ? 2 * base_len - 2
                                                           : base_len;
    if (s_arp_idx >= total) s_arp_idx = 0;

    int e = s_arp_idx;
    if (mode == ARP_RANDOM) {
        e = (int)(rng_next() % (uint32_t)base_len);
    } else if (mode == ARP_UPDOWN && e >= base_len) {
        e = 2 * base_len - 2 - e; /* descending phase */
    }

    int i, o;
    if (mode == ARP_DOWN) { /* top octave first, highest note first */
        o = octaves - 1 - e / n;
        i = n - 1 - e % n;
    } else {
        o = e / n;
        i = e % n;
    }
    int note = (int)notes[i] + 12 * o;
    while (note > 127) note -= 12;

    emit_on((uint8_t)note, vels[i]);
    pend_push((uint8_t)note, gate_ticks);
    s_arp_idx = (s_arp_idx + 1) % total;
}

/* One quarter note. Clicks the count-in, releases the transport on the
 * downbeat after it, and notifies the beat subscriber. */
void on_beat() {
    const bool downbeat = s_beat_in_bar == 0;

    if (s_countin_left > 0) {
        if (--s_countin_left == 0) {
            /* Four counts have been given; this beat is the "1" the pattern
             * starts on, and it gets no click of its own — the last tick
             * would otherwise sound over the first step (and, with the
             * looper recording, be printed into the take). */
            seq_play_start(true);
            ESP_LOGI(TAG, "count-in done: sequencer running");
        } else {
            drums_click(downbeat);
        }
    }
    if (s_beat_cb != nullptr) s_beat_cb(s_beat_in_bar, s_beat_ctx);
}

void process_tick() {
    pend_tick(); /* offs first: a full gate releases exactly at the next on */
    const int arp_ticks = cur_arp_ticks();
    if (++s_arp_tick_in_step >= arp_ticks) {
        s_arp_tick_in_step = 0;
        const int arp_mode = pi(ARP_MODE);
        if (arp_mode != ARP_OFF) {
            arp_step(arp_mode, pi(ARP_OCT), cur_gate_ticks(arp_ticks));
        }
    }

    /* The beat grid free-runs with the clock, independent of the transport:
     * that is what lets the looper bar-lock while the sequencer is stopped. */
    if (--s_beat_tick <= 0) {
        s_beat_tick = kPpqn;
        on_beat();
        s_beat_in_bar = (s_beat_in_bar + 1) & 3;
    }

    seq_play_tick();
}

/* Mode/hold edge detection, polled every wake (<= 100 ms latency). */
void poll_edges() {
    const int seq_mode = pi(SEQ_MODE);
    const int arp_mode = pi(ARP_MODE);
    const bool hold = pv(ARP_HOLD) >= 0.5f;
    bool flush_arp = false;
    bool seq_changed = false;
    bool arp_changed = false;
    int prev_mode = SEQ_STOP;

    taskENTER_CRITICAL(&s_lock);
    if (seq_mode != s_seq_mode_prev) {
        seq_changed = true;
        prev_mode = s_seq_mode_prev;
        s_seq_mode_prev = seq_mode;
    }
    if (arp_mode != s_arp_mode_prev) {
        arp_changed = true;
        if (arp_mode == ARP_OFF) {
            /* Keys still down stay `owned` (their note-offs get swallowed
             * when they arrive); latched keys' offs already passed, so
             * settle their map bits now. */
            for (int i = 0; i < s_held_count; ++i) {
                if (!s_held[i].down) own_clear(s_held[i].note);
            }
            s_held_count = 0;
            flush_arp = true;
        }
        s_arp_idx = 0;
        s_arp_mode_prev = arp_mode;
    }
    if (hold != s_arp_hold_prev) {
        if (!hold) { /* latch off: drop notes whose keys are already up */
            int w = 0;
            for (int i = 0; i < s_held_count; ++i) {
                if (s_held[i].down) {
                    s_held[w++] = s_held[i];
                } else {
                    own_clear(s_held[i].note);
                }
            }
            s_held_count = w;
        }
        s_arp_hold_prev = hold;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (flush_arp) pend_flush();

    if (seq_changed) {
        /* rec keeps the transport rolling — overdub is the useful default,
         * and step input works from a stopped transport anyway. */
        if (seq_mode == SEQ_STOP) {
            seq_play_stop();
            s_countin_left = 0;
        } else if (prev_mode == SEQ_STOP) {
            /* A count-in only makes sense from a standing start, and only
             * when the transport is rewinding: resuming with MIDI continue
             * must land on the master's grid, not four beats later. */
            if (pv(COUNTIN) >= 0.5f && !s_suppress_reset) {
                s_countin_left = 5; /* four clicks, start on the fifth */
                s_beat_tick = 1; /* click the first beat on the next tick */
                s_beat_in_bar = 0;
                ESP_LOGI(TAG, "count-in: 4 beats");
            } else {
                seq_play_start(!s_suppress_reset);
            }
        }
        s_suppress_reset = false;
        ESP_LOGI(TAG, "seq %s (pattern %d)", kSeqModeNames[seq_mode],
                 seq_play_current_pattern() + 1);
    }
    if (arp_changed) {
        ESP_LOGI(TAG, "arp %s (x%d oct)", kArpModeNames[arp_mode], pi(ARP_OCT));
    }

    /* Live controls the sequencer engine owns rather than reads per tick. */
    const bool fill = pv(FILL) >= 0.5f;
    if (fill != s_fill_prev) {
        seq_play_set_fill(fill);
        s_fill_prev = fill;
    }
    const int pattern = pi(PATTERN);
    if (pattern != s_pattern_prev) {
        seq_play_select_pattern(pattern, false);
        s_pattern_prev = pattern;
    }
    const int scale = pi(SCALE);
    const int root = pi(ROOT);
    if (scale != s_scale_prev || root != s_root_prev) {
        seq_pattern_cfg_t cfg;
        seq_pattern_cfg_get(seqarp_edit_pattern(), &cfg);
        cfg.scale = (uint8_t)scale;
        cfg.root = (uint8_t)root;
        seq_pattern_cfg_set(seqarp_edit_pattern(), &cfg);
        s_scale_prev = scale;
        s_root_prev = root;
    }
    /* seq.steps mirrors the edited track's length in both directions: the
     * app can drive it as a knob, and selecting another track updates it. */
    const int edit_track = pi(EDIT_TRACK);
    const int steps = pi(SEQ_STEPS);
    seq_track_cfg_t tc;
    const int trk = (edit_track >= 1 && edit_track <= SEQ_TRACKS)
                        ? edit_track - 1
                        : 0;
    seq_track_cfg_get(seqarp_edit_pattern(), trk, &tc);
    if (edit_track != s_edit_track_prev) {
        s_edit_track_prev = edit_track;
        s_steps_prev = tc.length;
        ParamStore::instance().set(SEQ_PID_SEQ_STEPS, (float)tc.length,
                                   ParamOrigin::Internal);
    } else if (steps != s_steps_prev && steps >= 1) {
        tc.length = (uint16_t)steps;
        seq_track_cfg_set(seqarp_edit_pattern(), trk, &tc);
        s_steps_prev = steps;
    } else if (tc.length != s_steps_prev) {
        s_steps_prev = tc.length;
        ParamStore::instance().set(SEQ_PID_SEQ_STEPS, (float)tc.length,
                                   ParamOrigin::Internal);
    }
}

/* Recovered external clock: how many sub-ticks the task may emit now.
 * Bounded above by the clock bytes actually received (never run ahead of the
 * master) and below by kSlaveCatchUp (never lag by an audible amount). */
uint32_t slave_budget(uint32_t timer_ticks) {
    const uint32_t clocks = s_ext_clocks.load(std::memory_order_acquire);
    if (clocks != s_slave_clocks) s_slave_clocks = clocks;
    const uint32_t target = s_slave_clocks * kSubTicks;
    const int32_t owed = (int32_t)(target - s_slave_emitted);
    if (owed <= 0) return 0;
    uint32_t emit = timer_ticks;
    if ((int32_t)emit > owed) emit = (uint32_t)owed;
    if (owed - (int32_t)emit > kSlaveCatchUp) {
        emit = (uint32_t)(owed - kSlaveCatchUp); /* fell behind: close the gap */
    }
    s_slave_emitted += emit;
    return emit;
}

void clk_task(void* arg) {
    (void)arg;
    float tempo_applied = 0.0f;
    uint64_t period_us = 20833 / kSubTicks; /* 120 BPM until reconciled */
    bool timer_running = false;
    int src_prev = -1;
    int64_t last_poll_us = 0;
    /* External-clock stall watchdog state (see the check further down). */
    uint32_t seen_clocks = 0;
    int64_t last_clock_us = 0;
    bool ext_stalled = false;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

        const int src = pi(CLOCK_SRC);
        if (src != src_prev) { /* switching source restarts the recovery */
            s_slave_emitted = 0;
            s_slave_clocks = 0;
            s_ext_clocks.store(0, std::memory_order_release);
            s_ticks.store(0, std::memory_order_relaxed);
            src_prev = src;
            last_clock_us = 0;
            ext_stalled = false;
        }

        uint64_t want_us = period_us;
        if (src == CLK_INTERNAL) {
            const float tempo = pv(TEMPO);
            if (fabsf(tempo - tempo_applied) > 0.005f) {
                want_us =
                    (uint64_t)(60000000.0 / ((double)tempo * (double)kPpqn));
                tempo_applied = tempo;
            }
        } else {
            /* Pace the sub-tick timer from the measured clock interval so
             * interpolated ticks land where the master would have put them. */
            want_us = s_ext_period_us.load(std::memory_order_relaxed);
            if (want_us < 200) want_us = 200;      /* > 3000 BPM: nonsense */
            if (want_us > 100000) want_us = 100000; /* < 6 BPM */
            tempo_applied = 0.0f;
        }
        if (!timer_running || want_us != period_us) {
            period_us = want_us;
            if (timer_running) {
                esp_timer_restart(s_timer, period_us);
            } else {
                esp_timer_start_periodic(s_timer, period_us);
                timer_running = true;
            }
        }

        /* Transport flags from MIDI real-time, plus the arp kick. */
        const uint32_t flags = s_flags.exchange(0, std::memory_order_acq_rel);
        if (flags & kFlagStart) {
            s_slave_emitted = 0;
            s_slave_clocks = 0;
            s_ext_clocks.store(0, std::memory_order_release);
            s_arp_tick_in_step = kFireNow;
        }
        if (flags & kFlagContinue) s_suppress_reset = true;
        if (flags & (kFlagStart | kFlagContinue)) {
            ParamStore::instance().set(SEQ_PID_SEQ_MODE, (float)SEQ_PLAY);
        }
        if (flags & kFlagStop) {
            ParamStore::instance().set(SEQ_PID_SEQ_MODE, (float)SEQ_STOP);
        }

        /* Throttled: the task now wakes once per 96-PPQN tick (up to 480 Hz
         * at 300 BPM), and poll_edges walks the whole live-control surface —
         * running it per tick would burn CPU re-reading parameters that a
         * human moves at a few hertz. Transport flags bypass the throttle so
         * a start/stop is never late. */
        const int64_t now_us = esp_timer_get_time();
        if (flags != 0 || now_us - last_poll_us >= 20000) {
            last_poll_us = now_us;
            poll_edges();
        }

        /* External-clock stall watchdog.
         *
         * A master that stops without sending 0xFC — cable pulled, DAW
         * crashed, USB re-enumerated — leaves slave_budget() returning 0
         * forever, so process_tick() never runs again. That is where the
         * note-offs live: pend_tick() releases the arpeggiator's gates and
         * seq_play_tick() releases the sequencer's, so without this every
         * note sounding at the moment the clock died drones until something
         * else sends CC 123.
         *
         * Measured in wall time on purpose. The previous guard asked whether
         * a wake carried a task notification, which cannot work: the sub-tick
         * timer runs in *both* clock modes and notifies on every fire, so the
         * test was always false and the recovery never ran once. */
        if (src == CLK_MIDI) {
            const uint32_t clocks = s_ext_clocks.load(std::memory_order_acquire);
            if (clocks != seen_clocks) {
                seen_clocks = clocks;
                last_clock_us = now_us;
                ext_stalled = false;
            } else if (!ext_stalled && seen_clocks != 0 &&
                       now_us - last_clock_us >= kExtStallUs) {
                /* seen_clocks != 0 keeps this quiet when no master has ever
                 * been connected — selecting the MIDI clock source with
                 * nothing plugged in is a setup step, not a stall. A 0xFA
                 * start zeroes the counter, so it re-arms on the first byte
                 * of each transport run. */
                ext_stalled = true; /* one shot until the clock comes back */
                ESP_LOGW(TAG, "external clock stalled — releasing held notes");
                pend_flush();
                seq_play_stop();
                /* Reflect the transport so the app (and poll_edges' own edge
                 * detection) agree that it is no longer running. */
                if (pi(SEQ_MODE) != SEQ_STOP) {
                    ParamStore::instance().set(SEQ_PID_SEQ_MODE,
                                               (float)SEQ_STOP);
                }
            }
        } else {
            last_clock_us = 0;
            ext_stalled = false;
        }

        if (flags & kFlagKick) {
            /* First key of a chord with the step grid idle: fire the arp now
             * and re-align the grid to this instant. While the sequencer
             * plays, the arp quantizes to the running grid instead. */
            const int arp_mode = pi(ARP_MODE);
            if (arp_mode != ARP_OFF && !seq_play_running()) {
                s_arp_tick_in_step = 0;
                if (timer_running) esp_timer_restart(s_timer, period_us);
                const int st = cur_arp_ticks();
                arp_step(arp_mode, pi(ARP_OCT), cur_gate_ticks(st));
            }
        }

        uint32_t n = s_ticks.exchange(0, std::memory_order_acq_rel);
        if (src == CLK_MIDI) n = slave_budget(n);
        if (n > (uint32_t)kMaxBurst) { /* stalled stream resumed: no machine-gun */
            ESP_LOGD(TAG, "dropping %u backlogged ticks",
                     (unsigned)(n - kMaxBurst));
            n = kMaxBurst;
        }
        if (n == 0) continue; /* a stalled external clock is handled above */
        while (n-- > 0) process_tick();
    }
}

/* ---- input hooks (USB / serial-MIDI tasks) ---- */

int held_find(uint8_t note) { /* s_lock held */
    for (int i = 0; i < s_held_count; ++i) {
        if (s_held[i].note == note) return i;
    }
    return -1;
}

bool note_tap(uint8_t note, uint8_t vel, bool on, void* ctx) {
    (void)ctx;
    if (xTaskGetCurrentTaskHandle() == s_clk_task) return false; /* own note */

    bool consumed = false;
    bool kick = false;
    bool record = false;

    if (on) {
        const int arp_mode = pi(ARP_MODE);
        const bool hold = pv(ARP_HOLD) >= 0.5f;
        record = pi(SEQ_MODE) == SEQ_REC;
        taskENTER_CRITICAL(&s_lock);
        if (arp_mode != ARP_OFF) {
            if (hold && s_held_count > 0) {
                bool any_down = false;
                for (int i = 0; i < s_held_count; ++i) {
                    any_down = any_down || s_held[i].down;
                }
                if (!any_down) { /* fresh chord replaces the latched one */
                    for (int i = 0; i < s_held_count; ++i) {
                        own_clear(s_held[i].note);
                    }
                    s_held_count = 0;
                }
            }
            const int idx = held_find(note);
            if (idx >= 0) { /* retrigger: refresh */
                s_held[idx].vel = vel;
                s_held[idx].down = true;
                own_set(note);
                consumed = true;
            } else if (s_held_count < kMaxHeld) {
                kick = s_held_count == 0;
                s_held[s_held_count].note = note;
                s_held[s_held_count].vel = vel;
                s_held[s_held_count].down = true;
                ++s_held_count;
                own_set(note);
                consumed = true;
            }
            /* list full: not consumed — the note plays normally instead */
        }
        taskEXIT_CRITICAL(&s_lock);
    } else {
        const bool latch = pv(ARP_HOLD) >= 0.5f && pi(ARP_MODE) != ARP_OFF;
        taskENTER_CRITICAL(&s_lock);
        if (own_test(note)) {
            consumed = true;
            const int idx = held_find(note);
            if (idx < 0) {
                own_clear(note); /* arp went off mid-hold: settle the map */
            } else if (latch) {
                s_held[idx].down = false; /* keeps arping until a new chord */
            } else {
                own_clear(note);
                for (int i = idx; i < s_held_count - 1; ++i) {
                    s_held[i] = s_held[i + 1];
                }
                --s_held_count;
            }
        }
        taskEXIT_CRITICAL(&s_lock);
    }

    /* Recording happens outside the critical section: it touches the pattern
     * store, which takes its own lock. */
    if (record) seq_play_record_note(note, vel);

    if (kick) {
        s_flags.fetch_or(kFlagKick, std::memory_order_release);
        xTaskNotifyGive(s_clk_task);
    }
    return consumed;
}

void realtime_cb(uint8_t status, void* ctx) {
    (void)ctx;
    switch (status) {
        case 0xF8: { /* timing clock: 24 PPQN, only in external mode */
            if (pi(CLOCK_SRC) != CLK_MIDI) break;
            const uint64_t now = (uint64_t)esp_timer_get_time();
            const uint64_t last = s_ext_last_us.exchange(now,
                                                         std::memory_order_acq_rel);
            if (last != 0 && now > last) {
                const uint64_t dt = now - last;
                if (dt > 1000 && dt < 400000) { /* 6..3000 BPM, ignore glitches */
                    /* One-pole on the sub-tick period: a jittery USB-MIDI
                     * clock should not make the interpolation stutter. */
                    const uint32_t sub = (uint32_t)(dt / kSubTicks);
                    const uint32_t cur =
                        s_ext_period_us.load(std::memory_order_relaxed);
                    s_ext_period_us.store((cur * 3 + sub) / 4,
                                          std::memory_order_relaxed);
                }
            }
            s_ext_clocks.fetch_add(1, std::memory_order_release);
            xTaskNotifyGive(s_clk_task);
            break;
        }
        case 0xFA:
            s_ext_last_us.store(0, std::memory_order_release);
            s_flags.fetch_or(kFlagStart, std::memory_order_release);
            xTaskNotifyGive(s_clk_task);
            break;
        case 0xFB:
            s_flags.fetch_or(kFlagContinue, std::memory_order_release);
            xTaskNotifyGive(s_clk_task);
            break;
        case 0xFC:
            s_flags.fetch_or(kFlagStop, std::memory_order_release);
            xTaskNotifyGive(s_clk_task);
            break;
        default:
            break;
    }
}

void timer_cb(void* arg) {
    (void)arg;
    s_ticks.fetch_add(1, std::memory_order_relaxed);
    xTaskNotifyGive(s_clk_task);
}

void build_params() {
    int n = 0;
    s_params[n++] = {SEQ_PID_TEMPO, "seq.tempo", ParamType::Float,
                     ParamCurve::Linear, 30.0f, 300.0f, 120.0f, nullptr, 0};
    s_params[n++] = {SEQ_PID_CLOCK_SRC, "seq.clock", ParamType::Enum,
                     ParamCurve::Linear, 0.0f, 1.0f, 0.0f, kClockNames, 2};
    s_params[n++] = {SEQ_PID_DIV, "seq.div", ParamType::Enum,
                     ParamCurve::Linear, 0.0f, 5.0f, 3.0f, kDivNames, 6};
    s_params[n++] = {SEQ_PID_GATE, "seq.gate", ParamType::Float,
                     ParamCurve::Linear, 0.05f, 1.0f, 0.5f, nullptr, 0};
    s_params[n++] = {SEQ_PID_SWING, "seq.swing", ParamType::Float,
                     ParamCurve::Linear, 25.0f, 75.0f, 50.0f, nullptr, 0};
    s_params[n++] = {SEQ_PID_PATTERN, "seq.pattern", ParamType::Int,
                     ParamCurve::Linear, 0.0f, (float)(SEQ_PATTERNS - 1), 0.0f,
                     nullptr, 0};
    s_params[n++] = {SEQ_PID_SONG, "seq.song", ParamType::Bool,
                     ParamCurve::Linear, 0.0f, 1.0f, 0.0f, nullptr, 0};
    s_params[n++] = {SEQ_PID_SCALE, "seq.scale", ParamType::Enum,
                     ParamCurve::Linear, 0.0f, (float)(SEQ_SCALE_COUNT - 1),
                     0.0f, kScaleNames, SEQ_SCALE_COUNT};
    s_params[n++] = {SEQ_PID_ROOT, "seq.root", ParamType::Int,
                     ParamCurve::Linear, 0.0f, 11.0f, 0.0f, nullptr, 0};
    s_params[n++] = {SEQ_PID_FILL, "seq.fill", ParamType::Bool,
                     ParamCurve::Linear, 0.0f, 1.0f, 0.0f, nullptr, 0};
    s_params[n++] = {SEQ_PID_ACCENT, "seq.accent", ParamType::Float,
                     ParamCurve::Linear, 1.0f, 2.0f, 1.35f, nullptr, 0};
    s_params[n++] = {SEQ_PID_POS, "seq.pos", ParamType::Int,
                     ParamCurve::Linear, -1.0f, (float)(SEQ_MAX_STEPS - 1),
                     -1.0f, nullptr, 0};
    s_params[n++] = {SEQ_PID_CURPAT, "seq.curpat", ParamType::Int,
                     ParamCurve::Linear, 0.0f, (float)(SEQ_PATTERNS - 1), 0.0f,
                     nullptr, 0};
    s_params[n++] = {SEQ_PID_QUANT, "seq.quant", ParamType::Enum,
                     ParamCurve::Linear, 0.0f, 3.0f, 0.0f, kQuantNames, 4};
    s_params[n++] = {SEQ_PID_EDIT_TRACK, "seq.edit.track", ParamType::Int,
                     ParamCurve::Linear, 1.0f, (float)SEQ_TRACKS, 1.0f, nullptr,
                     0};
    s_params[n++] = {SEQ_PID_EDIT_STEP, "seq.edit.step", ParamType::Int,
                     ParamCurve::Linear, 0.0f, (float)(SEQ_MAX_STEPS - 1), 0.0f,
                     nullptr, 0};
    s_params[n++] = {SEQ_PID_ARP_MODE, "arp.mode", ParamType::Enum,
                     ParamCurve::Linear, 0.0f, 5.0f, 0.0f, kArpModeNames, 6};
    s_params[n++] = {SEQ_PID_ARP_OCT, "arp.octaves", ParamType::Int,
                     ParamCurve::Linear, 1.0f, 4.0f, 1.0f, nullptr, 0};
    s_params[n++] = {SEQ_PID_ARP_HOLD, "arp.hold", ParamType::Bool,
                     ParamCurve::Linear, 0.0f, 1.0f, 0.0f, nullptr, 0};
    s_params[n++] = {SEQ_PID_SEQ_MODE, "seq.mode", ParamType::Enum,
                     ParamCurve::Linear, 0.0f, 2.0f, 0.0f, kSeqModeNames, 3};
    s_params[n++] = {SEQ_PID_SEQ_STEPS, "seq.steps", ParamType::Int,
                     ParamCurve::Linear, 1.0f, (float)SEQ_MAX_STEPS,
                     (float)SEQ_DEFAULT_STEPS, nullptr, 0};
    s_params[n++] = {SEQ_PID_COUNTIN, "seq.countin", ParamType::Bool,
                     ParamCurve::Linear, 0.0f, 1.0f, 0.0f, nullptr, 0};

    for (int t = 0; t < SEQ_TRACKS; ++t) {
        char* mute = s_track_names[t * 2 + 0];
        char* solo = s_track_names[t * 2 + 1];
        /* Modulo for the compiler's benefit: `t` is a plain int, so
         * -Wformat-truncation assumes its full range against name[16]. */
        const unsigned tnum = (unsigned)(t + 1) % 100u;
        snprintf(mute, sizeof(s_track_names[0]), "trk%u.mute", tnum);
        snprintf(solo, sizeof(s_track_names[0]), "trk%u.solo", tnum);
        s_params[n++] = {(uint16_t)SEQ_PID_TRACK_MUTE(t), mute, ParamType::Bool,
                         ParamCurve::Linear, 0.0f, 1.0f, 0.0f, nullptr, 0};
        s_params[n++] = {(uint16_t)SEQ_PID_TRACK_SOLO(t), solo, ParamType::Bool,
                         ParamCurve::Linear, 0.0f, 1.0f, 0.0f, nullptr, 0};
    }
}

} // namespace

/* ---- preset-facing pattern access ---- */

void seqarp_set_beat_callback(seqarp_beat_fn fn, void* ctx) {
    s_beat_ctx = ctx;
    s_beat_cb = fn; /* pointer last: the callback is read without a lock */
}

int seqarp_ticks_per_beat(void) { return kPpqn; }
int seqarp_beat_in_bar(void) { return s_beat_in_bar; }

int seqarp_edit_pattern(void) {
    if (s_p[PATTERN] == nullptr) return 0;
    const int p = (int)(s_p[PATTERN]->load(std::memory_order_relaxed) + 0.5f);
    return (p >= 0 && p < SEQ_PATTERNS) ? p : 0;
}

size_t seqarp_pattern_export(int pattern, void* buf, size_t cap) {
    return seq_pattern_serialize(pattern, buf, cap);
}

void seqarp_pattern_reflect(int pattern) {
    if (pattern < 0 || pattern >= SEQ_PATTERNS) return;
    /* Reflect the pattern's feel into the live parameters so the app's knobs
     * show what is actually playing. s_*_prev is moved with them: poll_edges
     * compares against it, and without this the next poll would read the
     * *old* parameter values as an edit and write them back into the pattern
     * that was just loaded. */
    seq_pattern_cfg_t cfg;
    seq_pattern_cfg_get(pattern, &cfg);
    ParamStore& ps = ParamStore::instance();
    ps.set(SEQ_PID_SCALE, (float)cfg.scale, ParamOrigin::Preset);
    ps.set(SEQ_PID_ROOT, (float)cfg.root, ParamOrigin::Preset);
    ps.set(SEQ_PID_SWING, (float)cfg.swing, ParamOrigin::Preset);
    for (int t = 0; t < SEQ_TRACKS; ++t) {
        seq_track_cfg_t tc;
        seq_track_cfg_get(pattern, t, &tc);
        ps.set((uint16_t)SEQ_PID_TRACK_MUTE(t),
               (tc.flags & SEQ_TRACK_F_MUTE) ? 1.0f : 0.0f, ParamOrigin::Preset);
        ps.set((uint16_t)SEQ_PID_TRACK_SOLO(t),
               (tc.flags & SEQ_TRACK_F_SOLO) ? 1.0f : 0.0f, ParamOrigin::Preset);
    }
    s_scale_prev = cfg.scale;
    s_root_prev = cfg.root;
    s_steps_prev = -1;
}

bool seqarp_pattern_import(int pattern, const void* buf, size_t len) {
    const bool ok = seq_pattern_deserialize(pattern, buf, len);
    if (ok) seqarp_pattern_reflect(pattern);
    return ok;
}

size_t seqarp_pattern_max_bytes(void) { return seq_pattern_max_bytes(); }

esp_err_t seqarp_init(void) {
    const esp_err_t model_err = seq_model_init();
    if (model_err != ESP_OK) {
        ESP_LOGE(TAG, "pattern store unavailable — sequencer disabled");
        /* Carry on: the clock and the arpeggiator do not need it. */
    }

    build_params();
    ParamStore& ps = ParamStore::instance();
    const size_t added = ps.add(s_params, P_COUNT);
    if (added != P_COUNT) {
        ESP_LOGE(TAG, "registered %u/%d params", (unsigned)added, P_COUNT);
        return ESP_FAIL;
    }
    for (int i = 0; i < P_COUNT; ++i) s_p[i] = ps.valuePtr(s_params[i].id);

    seq_play_init();
    seq_play_bind_params();

    esp_timer_create_args_t targs = {};
    targs.callback = timer_cb;
    targs.dispatch_method = ESP_TIMER_TASK;
    targs.name = "seq_tick";
    targs.skip_unhandled_events = true; /* after a stall: no tick avalanche */
    esp_err_t err = esp_timer_create(&targs, &s_timer);
    if (err != ESP_OK) return err;

    const BaseType_t ok =
        xTaskCreatePinnedToCore(clk_task, "seq_clk", kClkTaskStack, nullptr,
                                kClkTaskPrio, &s_clk_task, 0);
    if (ok != pdPASS) {
        esp_timer_delete(s_timer);
        s_timer = nullptr;
        return ESP_FAIL;
    }

    midi_set_note_tap(note_tap, nullptr);
    midi_set_realtime_callback(realtime_cb, nullptr);

    ESP_LOGI(TAG,
             "up: %d PPQN clock (internal %g BPM or MIDI clock x%d), arp + "
             "%dx%d-step sequencer, %d patterns, %d params",
             kPpqn, (double)pv(TEMPO), kSubTicks, SEQ_TRACKS, SEQ_MAX_STEPS,
             SEQ_PATTERNS, P_COUNT);
    return ESP_OK;
}
