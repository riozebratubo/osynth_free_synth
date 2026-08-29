/*
 * osynth — sampler: recording into kit pads (Session 44). Contract in
 * sampler.h; the seam with the player is drums_priv.h.
 *
 * Everything the audio task does here is in sampler_capture(): convert one
 * block to mono, push it into the pre-roll ring, and — while a take is
 * running — append it to the staging buffer and update a peak. No allocation,
 * no locks, no file I/O, and no decision that needs more than two atomics.
 * The rest of this file runs on the sampler control task, which is where
 * trimming, slicing, publishing, undo and storage all live.
 *
 * ---------------------------------------------------------------------------
 * Why the parameter set is as small as it is
 *
 * ParamStore::kMaxParams is 448 and the live peak was already ~394 before this
 * session (see the tally in synth_params.h). The obvious shape for the per-pad
 * playback settings — `drum7.mode`, `drum7.rev`, `drum7.start`, `drum7.choke`
 * — is 64 more parameters, which does not fit, and would not have been right
 * even if it did: a parameter does not follow a kit switch, so those four
 * would describe pad 7 of whichever kit happened to be bound. They are kit
 * data, they live in drum_sample_t, and they travel over OP_KIT_EDIT. What is
 * left here is the 24 controls that genuinely belong to *the recorder* rather
 * than to a pad, which lands the total at ~418 with room to spare.
 * ---------------------------------------------------------------------------
 */
#include "sampler.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_io.h"
#include "drum_kit.h"
#include "drums_priv.h"
#include "synth_config.h"
#include "synth_line.h"
#include "synth_params.h"
#include "synth_smooth.h"

static const char* TAG = "sampler";

using osynth::ParamCurve;
using osynth::ParamDesc;
using osynth::ParamOrigin;
using osynth::ParamStore;
using osynth::ParamType;
using osynth::dsp::Smooth;
using osynth::dsp::smooth_lin;

namespace {

constexpr float kSr = (float)SYNTH_SAMPLE_RATE;

/* Pre-roll depth. Fixed rather than derived from smp.preroll because the ring
 * has to already contain the history by the time anyone asks for it — a
 * parameter can only choose how much of it to *use*. 250 ms covers a BLE round
 * trip and a slow thumb with room over; at mono int16 it is 24 KB. */
constexpr uint32_t kPrerollMs = 250;
constexpr uint32_t kRingLen = (uint32_t)(kPrerollMs * SYNTH_SAMPLE_RATE / 1000);

/* Trim floor: -60 dBFS, below which a tail is room tone rather than sound. */
constexpr float kTrimFloor = 0.001f;
/* Leave this much of the fade in when trimming, so a decaying tail is cut at
 * a genuinely inaudible point rather than at the first sample under the floor
 * — cutting on the crossing itself is where a click comes from. */
constexpr uint32_t kTrimTailMs = 12;
/* Shortest slice the transient detector will emit, and the shortest take worth
 * committing at all. Below this a "sample" is a click. */
constexpr uint32_t kMinTakeMs = 15;

/* Every commit fades this long in and out. Not optional and not exposed: a
 * take starts and ends wherever the threshold, the trim and the slicer put it,
 * which is essentially never at a zero crossing, and the step left otherwise
 * is audible on every single hit. Two milliseconds is under the ear's
 * resolution for a transient and still 96 samples to get to zero in. */
constexpr uint32_t kEdgeFadeMs = 2;

constexpr int kStateTaskStack = 4096;
constexpr int kCtlTaskPrio = 4;

/* Tempo, for the count-in. Read by id rather than through seqarp.h because
 * the dependency only runs the other way: seqarp PRIV_REQUIREs drums (its
 * tracks drive the drum bus), so drums cannot require seqarp back. The id is
 * SEQ_PID_TEMPO in components/seqarp/include/seqarp.h, which is the source of
 * truth if it ever moves. A build with the sequencer compiled out returns 0
 * from get() and the fallback below applies. */
constexpr uint16_t kSeqTempoPid = 0x0400;
constexpr float kFallbackBpm = 120.0f;

enum : int { SRC_INPUT = 0, SRC_BUS = 1 };
enum : int { SLICE_EVEN = 0, SLICE_TRANSIENT = 1 };

const char* const kSrcNames[] = {"input", "bus"};
const char* const kSliceModeNames[] = {"even", "transient"};

/* ---- parameters ---- */

enum PIdx {
    P_SRC, P_ARM, P_REC, P_ERASE, P_UNDO, P_THRESH, P_TRIM, P_NORM,
    P_PREROLL, P_MAXSEC, P_SLICES, P_SLICEMODE, P_MONITOR, P_COUNTIN,
    P_SAVE, P_GAIN, P_COPYFROM, P_COPYKIT, P_COPYTO, P_DUPKIT,
    P_STATE, P_POS, P_FREE, P_PEAK,
    P_COUNT
};

const std::atomic<float>* s_p[P_COUNT] = {};

inline float pv(int i) {
    return s_p[i] != nullptr ? s_p[i]->load(std::memory_order_relaxed) : 0.0f;
}

/* ---- the pool ---- */

constexpr size_t kPoolTotal = (size_t)SYNTH_SAMPLE_POOL_KB * 1024u;
std::atomic<size_t> s_pool_used{0};

/* ---- the pre-roll ring (audio task writes, control task reads at commit) ---- */

int16_t* s_ring = nullptr;
uint32_t s_ring_w = 0;      /* next write index */
uint32_t s_ring_filled = 0; /* valid samples behind the head, capped at len */

/* ---- staging ---- */

int16_t* s_stage = nullptr;
uint32_t s_stage_cap = 0;             /* frames it can hold */
std::atomic<uint32_t> s_take_len{0};  /* frames written so far */
std::atomic<float> s_take_peak{0.0f};

/* ---- state ---- */

std::atomic<int> s_state{SAMPLER_IDLE};
std::atomic<int> s_arm_slot{-1};
std::atomic<bool> s_gate{false};
/* 0 none, 1 the gate was released, 2 the staging buffer filled up. */
std::atomic<int> s_stop_req{0};
/* Control-task work, each latched by the parameter listener and drained in
 * ctl_task(). Separate atomics rather than a queue: every one of them is
 * idempotent and "the most recent request wins" is the behaviour you want
 * from a button that can be pressed twice. */
std::atomic<int> s_req_erase{-1};
std::atomic<bool> s_req_undo{false};
std::atomic<int> s_req_copy_to{-1};
std::atomic<int> s_req_dup_kit{-1};
std::atomic<bool> s_req_save{false};
std::atomic<bool> s_req_countin{false};

TaskHandle_t s_ctl_task = nullptr;

Smooth s_sm_gain;

/* Why a take was refused, latched for the log so the same reason is not
 * printed once per block by a player leaning on the button. */
int s_last_refusal = 0;

/* ---- undo ----
 *
 * One operation deep, but an operation may be several pads: slicing a take
 * across eight of them is one gesture and has to undo as one. The stash holds
 * what was displaced, blocks included, so undoing is a swap rather than a
 * re-allocation — which is what makes it work when the pool is full, i.e.
 * exactly when a mistaken overwrite is most likely and least affordable. */
struct UndoOp {
    bool valid = false;
    /* Zero, not -1, and the reason is memory rather than meaning.
     *
     * This struct is a kilobyte. A single non-zero member initialiser makes
     * the whole object non-zero-initialised, which moves it out of `.bss` and
     * into `.data` — and on the ESP32-P4 those two land in *different regions*:
     * `.bss` goes to sram_high (384 KB, mostly free) while `.data` shares
     * sram_low with all the IRAM code (175 KB, and it was 5 KB from full when
     * this was written). `int kit = -1` cost 1036 bytes of the scarcest memory
     * in the build and bought nothing: `valid` is what every reader checks, and
     * a stray 0 here addresses the factory kit, which drums_slot_replace()
     * refuses outright. See tools/iram_budget.py for how to see this happen. */
    int kit = 0;
    int count = 0;
    int slots[DRUM_SLOTS] = {};
    drum_sample_t prev[DRUM_SLOTS] = {};
};
UndoOp s_undo;

/* ---- small helpers ---- */

/* Shared since S46 (synth_line.h): the clamp already ran in the float domain
 * here, but a NaN passes both of its compares untouched and the conversion of
 * one is undefined. */
using osynth::dsp::f2i16;

inline uint32_t ms_to_frames(float ms) {
    if (ms < 0.0f) ms = 0.0f;
    return (uint32_t)(ms * 0.001f * kSr);
}

/* ---- monitoring while armed ----
 *
 * Sampling something you cannot hear is guesswork, and the control that fixes
 * it — `in.route` — lives on a different page of the app and has three other
 * values that all mean something. So arming a pad borrows it: if the input is
 * routed nowhere, put it on `mon` (heard, never recorded into a looper take)
 * for as long as the recorder is armed, and put back exactly what was there
 * when it goes idle.
 *
 * Borrowed, not set: a player who has already routed the input to `fx` or
 * `dry` is monitoring it on purpose and through their own effects, and moving
 * that under them would change what they are about to sample. Only `off` is
 * taken as "nobody has an opinion".
 *
 * `smp.src = bus` never borrows anything — the bus is the thing already coming
 * out of the speakers.
 *
 * The origin on both writes is load-bearing, not decoration. in.route is a
 * persisted setting, and ParamOrigin::Internal is what keeps a borrowed value
 * out of NVS (persist.cpp's param_listener). Without that, a reset taken while
 * a pad was still armed stored `mon` permanently, and the player's `off` came
 * back as monitoring on the next boot — which is how S45c found it. Anything
 * added here that moves a persisted parameter temporarily has to carry the same
 * origin for the same reason. */
constexpr uint16_t kInRoutePid = 0x0008; /* PID_LINE_IN_ROUTE */
constexpr float kInRouteMon = 1.0f;
int s_route_saved = -1; /* -1 = not borrowed */

void monitor_hold(bool on) {
    ParamStore& ps = ParamStore::instance();
    if (on) {
        if (s_route_saved >= 0) return;
        if (pv(P_MONITOR) < 0.5f) return;
        if ((int)(pv(P_SRC) + 0.5f) == SRC_BUS) return;
        const ParamDesc* d = ps.describe(kInRoutePid);
        if (d == nullptr) return; /* a build with no audio input */
        const int cur = (int)(ps.get(kInRoutePid) + 0.5f);
        if (cur != 0) return; /* already monitoring, on the player's terms */
        s_route_saved = cur;
        ps.set(kInRoutePid, kInRouteMon, ParamOrigin::Internal);
    } else {
        if (s_route_saved < 0) return;
        ps.set(kInRoutePid, (float)s_route_saved, ParamOrigin::Internal);
        s_route_saved = -1;
    }
}

void publish_state(int st) {
    s_state.store(st, std::memory_order_release);
    ParamStore::instance().set(SMP_PID_STATE, (float)st, ParamOrigin::Internal);
    monitor_hold(st != SAMPLER_IDLE);
}

/* The readouts the app draws the transport from. Pushed from the control task
 * because ParamStore::set() notifies listeners synchronously, which is not
 * something the audio task may do. */
void publish_progress() {
    ParamStore& ps = ParamStore::instance();
    const uint32_t len = s_take_len.load(std::memory_order_relaxed);
    /* Against the take ceiling the player actually set, not the buffer: a
     * progress bar that crawls to an eighth and then stops is not telling
     * anyone what they wanted to know. */
    uint32_t capf = (uint32_t)(pv(P_MAXSEC) * kSr);
    if (capf == 0 || capf > s_stage_cap) capf = s_stage_cap;
    const float cap = (float)(capf ? capf : 1u);
    ps.set(SMP_PID_POS, (float)len / cap, ParamOrigin::Internal);
    ps.set(SMP_PID_PEAK, s_take_peak.load(std::memory_order_relaxed),
           ParamOrigin::Internal);
    ps.set(SMP_PID_FREE,
           (float)((kPoolTotal - s_pool_used.load(std::memory_order_relaxed)) /
                   1024u),
           ParamOrigin::Internal);
}

/* Allocates the staging buffer on the first arm and keeps it. Deferred rather
 * than claimed at init because it is 768 KB at the default ceiling and a build
 * whose owner never records a pad should not be paying for it — the looper
 * sizes itself from the free pool, so that memory is not idle, it is loop
 * time. */
bool ensure_stage() {
    if (s_stage != nullptr) return true;
    if (SYNTH_SAMPLE_KITS == 0) return false;
    const uint32_t want = (uint32_t)(SYNTH_SAMPLE_MAX_SEC * SYNTH_SAMPLE_RATE);
    s_stage = (int16_t*)heap_caps_malloc((size_t)want * 2, MALLOC_CAP_SPIRAM);
    if (s_stage == nullptr) {
        ESP_LOGE(TAG, "no PSRAM for a %u s staging buffer (%u KB) — recording "
                      "is unavailable",
                 (unsigned)SYNTH_SAMPLE_MAX_SEC, (unsigned)(want * 2 / 1024));
        return false;
    }
    s_stage_cap = want;
    ESP_LOGI(TAG, "staging buffer: %u s (%u KB PSRAM)",
             (unsigned)SYNTH_SAMPLE_MAX_SEC, (unsigned)(want * 2 / 1024));
    return true;
}

} // namespace

/* ======================= pool ========================================== */

void* sampler_pool_alloc(size_t bytes) {
    if (bytes == 0) return nullptr;
    size_t used = s_pool_used.load(std::memory_order_relaxed);
    for (;;) {
        if (used + bytes > kPoolTotal) return nullptr;
        if (s_pool_used.compare_exchange_weak(used, used + bytes,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
            break;
        }
    }
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (p == nullptr) {
        /* The budget said yes and the heap said no — PSRAM is shared with the
         * looper and the FX lines, so this is reachable and is not a bug. Give
         * the reservation back rather than leaking it out of the accounting,
         * which would shrink the pool for the rest of the session. */
        s_pool_used.fetch_sub(bytes, std::memory_order_acq_rel);
        return nullptr;
    }
    return p;
}

void sampler_pool_free(void* p, size_t bytes) {
    if (p == nullptr) return;
    heap_caps_free(p);
    /* Guarded: a double free would wrap the counter to an enormous value and
     * refuse every future allocation, turning a small bug into a dead
     * feature. */
    size_t used = s_pool_used.load(std::memory_order_relaxed);
    const size_t give = bytes > used ? used : bytes;
    s_pool_used.fetch_sub(give, std::memory_order_acq_rel);
}

size_t sampler_pool_used(void) {
    return s_pool_used.load(std::memory_order_relaxed);
}

size_t sampler_pool_total(void) { return kPoolTotal; }

/* ======================= capture (audio task) ========================== */

namespace {

/* Copies `n` frames of pre-roll out of the ring into the front of staging.
 * Audio task, at the moment a take begins. */
uint32_t take_preroll(uint32_t n) {
    if (n > s_ring_filled) n = s_ring_filled;
    if (n > s_stage_cap) n = s_stage_cap;
    if (n == 0) return 0;
    /* The ring's oldest wanted sample sits `n` behind the write head. */
    uint32_t rd = (s_ring_w + kRingLen - n) % kRingLen;
    const uint32_t first = (rd + n <= kRingLen) ? n : (kRingLen - rd);
    memcpy(s_stage, s_ring + rd, (size_t)first * 2);
    if (first < n) memcpy(s_stage + first, s_ring, (size_t)(n - first) * 2);
    return n;
}

} // namespace

void SYNTH_RENDER_IRAM sampler_capture(const float* l, const float* r,
                                       size_t frames) {
    if (s_ring == nullptr) return;
    if (frames > SYNTH_BLOCK_SIZE) frames = SYNTH_BLOCK_SIZE;

    /* ---- one block of mono source, whichever source is selected ---- */
    float src[SYNTH_BLOCK_SIZE];
    const int source = (int)(pv(P_SRC) + 0.5f);
    if (source == SRC_BUS) {
        for (size_t i = 0; i < frames; ++i) src[i] = (l[i] + r[i]) * 0.5f;
    } else if (!audio_io_in_mono(src, frames)) {
        memset(src, 0, frames * sizeof(float));
    }
    /* Record trim, smoothed: this is a gain the player rides while listening,
     * so stepping it per block would print a zipper into the take itself. */
    const float g = smooth_lin(s_sm_gain, pv(P_GAIN));

    /* ---- the ring, unconditionally ----
     *
     * The whole value of pre-roll is that it is already there when the button
     * is finally pressed, so this cannot be gated on the state. It is a
     * bounded copy of one block whatever else is happening. */
    for (size_t i = 0; i < frames; ++i) {
        s_ring[s_ring_w] = f2i16(src[i] * g);
        if (++s_ring_w >= kRingLen) s_ring_w = 0;
    }
    if (s_ring_filled < kRingLen) {
        const uint32_t room = kRingLen - s_ring_filled;
        s_ring_filled += (frames < room) ? (uint32_t)frames : room;
    }

    /* Acquire, and it matters: the control task allocates the staging buffer
     * and then publishes the state with a release store (publish_state). A
     * relaxed load here would let this task observe SAMPLER_WAITING while
     * `s_stage` and `s_stage_cap` were still the values from before the
     * allocation — a null pointer and a zero length, written into below. */
    int st = s_state.load(std::memory_order_acquire);
    if (SYNTH_LIKELY(st != SAMPLER_WAITING && st != SAMPLER_RECORDING)) return;
    if (SYNTH_UNLIKELY(s_stage == nullptr || s_stage_cap == 0)) return;

    size_t from = 0;

    /* ---- threshold: the take starts on the sound, not on the gate ---- */
    if (st == SAMPLER_WAITING) {
        const float thresh = pv(P_THRESH);
        size_t hit = 0;
        bool found = false;
        if (thresh <= 0.0f) {
            found = true;
        } else {
            for (size_t i = 0; i < frames; ++i) {
                if (fabsf(src[i] * g) >= thresh) {
                    hit = i;
                    found = true;
                    break;
                }
            }
        }
        if (!found) return; /* still waiting; the ring kept rolling above */
        /* Pre-roll is taken relative to the crossing, and the crossing is
         * `frames - hit` samples behind the ring's *current* head — this block
         * has already been pushed. Rewinding by that much is what makes a
         * threshold-armed take include the attack that tripped it rather than
         * starting one block late. */
        const uint32_t want = ms_to_frames(pv(P_PREROLL)) +
                              (uint32_t)(frames - hit);
        const uint32_t got = take_preroll(want);
        s_take_len.store(got, std::memory_order_relaxed);
        s_take_peak.store(0.0f, std::memory_order_relaxed);
        s_state.store(SAMPLER_RECORDING, std::memory_order_release);
        /* Everything from `hit` on is already in what take_preroll() copied,
         * so this block must not be appended a second time. */
        from = frames;
        st = SAMPLER_RECORDING;
    }

    /* ---- append ----
     *
     * `smp.maxsec` is the *player's* ceiling and the staging buffer is the
     * build's; the take stops at whichever comes first. Two limits rather than
     * one because they answer different questions: the buffer says what the
     * hardware can hold, and the parameter says how much of a pad you meant to
     * fill. Setting it short is also the fastest way to keep a pool of 128
     * pads from being eaten by four of them. */
    uint32_t cap = (uint32_t)(pv(P_MAXSEC) * kSr);
    if (cap == 0 || cap > s_stage_cap) cap = s_stage_cap;
    uint32_t len = s_take_len.load(std::memory_order_relaxed);
    float peak = s_take_peak.load(std::memory_order_relaxed);
    for (size_t i = from; i < frames && len < cap; ++i, ++len) {
        const float x = src[i] * g;
        const float a = fabsf(x);
        if (a > peak) peak = a;
        s_stage[len] = f2i16(x);
    }
    s_take_len.store(len, std::memory_order_relaxed);
    s_take_peak.store(peak, std::memory_order_relaxed);

    if (SYNTH_UNLIKELY(len >= cap)) {
        /* Out of room. Stop here rather than wrapping: a sampler that silently
         * kept the *last* eight seconds of a long take would hand back audio
         * nobody meant to keep. */
        s_stop_req.store(2, std::memory_order_release);
        s_state.store(SAMPLER_COMMITTING, std::memory_order_release);
        if (s_ctl_task != nullptr) xTaskNotifyGive(s_ctl_task);
    }
}

bool sampler_recording(void) {
    const int st = s_state.load(std::memory_order_relaxed);
    return st == SAMPLER_RECORDING || st == SAMPLER_WAITING;
}

/* ======================= commit (control task) ========================= */

namespace {

/* Peak of a staged range. */
float range_peak(uint32_t a, uint32_t b) {
    float p = 0.0f;
    for (uint32_t i = a; i < b; ++i) {
        const float v = fabsf((float)s_stage[i]);
        if (v > p) p = v;
    }
    return p * (1.0f / 32768.0f);
}

/* Last sample above the trim floor, searching back from `b`. */
uint32_t trim_tail(uint32_t a, uint32_t b) {
    const int16_t floor_i = (int16_t)(kTrimFloor * 32767.0f);
    uint32_t e = b;
    while (e > a) {
        const int16_t v = s_stage[e - 1];
        if (v > floor_i || v < -floor_i) break;
        --e;
    }
    if (e < b) {
        /* Give the fade its tail back — see kTrimTailMs. */
        const uint32_t pad = ms_to_frames((float)kTrimTailMs);
        e = (b - e > pad) ? e + pad : b;
    }
    return e;
}

/* Copies one staged range into a fresh pool block, normalising and fading the
 * edges on the way. Returns nullptr when the pool is full. */
int16_t* bake(uint32_t a, uint32_t b, bool normalise, uint32_t* out_frames) {
    if (b <= a) return nullptr;
    const uint32_t n = b - a;
    int16_t* dst = (int16_t*)sampler_pool_alloc((size_t)n * 2);
    if (dst == nullptr) return nullptr;

    float scale = 1.0f;
    if (normalise) {
        const float p = range_peak(a, b);
        /* Normalise the samples rather than the slot's gain: a quiet take
         * scaled at playback keeps a quiet take's bit depth, and the point of
         * doing this at all is that a mic three feet away lands 30 dB down.
         * The ceiling is just under full scale so the interpolator in the
         * voice loop cannot overshoot into the clipper. */
        if (p > 1e-6f) scale = 0.99f / p;
        if (scale > 64.0f) scale = 64.0f; /* silence stays silence */
    }

    const uint32_t fade = ms_to_frames((float)kEdgeFadeMs);
    const uint32_t fin = (fade * 2 < n) ? fade : n / 2;
    for (uint32_t i = 0; i < n; ++i) {
        float v = (float)s_stage[a + i] * scale;
        if (fin > 0) {
            if (i < fin) v *= (float)i / (float)fin;
            else if (i >= n - fin) v *= (float)(n - i) / (float)fin;
        }
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        dst[i] = (int16_t)v;
    }
    *out_frames = n;
    return dst;
}

/* Onset positions inside [a,b), for smp.slicemode = transient.
 *
 * A short-window energy envelope compared against its own running mean: an
 * onset is where the window jumps well above where the signal has been
 * sitting. Deliberately simple — this is picking hit points in a bar someone
 * just played, not transcribing it, and a detector that needs tuning
 * parameters is a detector that will be wrong on the next kit. Everything it
 * can get wrong is fixed by the player choosing `even` instead. */
int find_onsets(uint32_t a, uint32_t b, uint32_t* out, int max) {
    if (max <= 0) return 0;
    constexpr uint32_t kWinMs = 5;
    const uint32_t win = ms_to_frames((float)kWinMs);
    if (win == 0 || b <= a + win) return 0;
    const uint32_t min_gap = ms_to_frames(50.0f);

    int n = 0;
    out[n++] = a; /* the first slice always starts at the take's start */
    float avg = 0.0f;
    bool primed = false;
    uint32_t last = a;
    for (uint32_t p = a; p + win <= b && n < max; p += win) {
        float e = 0.0f;
        for (uint32_t i = 0; i < win; ++i) {
            const float v = (float)s_stage[p + i] * (1.0f / 32768.0f);
            e += v * v;
        }
        e = sqrtf(e / (float)win);
        if (!primed) {
            avg = e;
            primed = true;
            continue;
        }
        /* 3x the running mean, and never within min_gap of the previous
         * onset: a snare's body would otherwise be sliced off its own attack. */
        if (e > avg * 3.0f && e > 0.02f && p > last + min_gap) {
            out[n++] = p;
            last = p;
        }
        /* Slow follower, so one loud hit does not blind the detector to the
         * next; fast enough that a decaying tail stops re-triggering it. */
        avg += (e - avg) * 0.25f;
    }
    return n;
}

/* Frees whatever the undo stash is holding. Called when a new operation
 * displaces it — this is the moment the pool gets that memory back, and the
 * reason a stashed pad is not a leak. */
void undo_release() {
    if (!s_undo.valid) return;
    for (int i = 0; i < s_undo.count; ++i) {
        drum_sample_t& s = s_undo.prev[i];
        if (s.owned != nullptr) sampler_pool_free(s.owned, s.owned_bytes);
        s.owned = nullptr;
    }
    s_undo = UndoOp{};
}

void undo_begin(int kit) {
    undo_release();
    s_undo.kit = kit;
    s_undo.count = 0;
}

void undo_record(int slot, const drum_sample_t& prev) {
    if (s_undo.count >= DRUM_SLOTS) return;
    s_undo.slots[s_undo.count] = slot;
    s_undo.prev[s_undo.count] = prev;
    ++s_undo.count;
    s_undo.valid = true;
}

/* Builds the drum_sample_t for a freshly baked block and publishes it, folding
 * the displaced pad into the undo stash. */
esp_err_t publish(int kit, int slot, int16_t* data, uint32_t frames,
                  const char* name, int index_in_take) {
    drum_sample_t fresh = {};
    fresh.data = (const uint8_t*)data;
    fresh.frames = frames;
    fresh.rate = SYNTH_SAMPLE_RATE;
    fresh.gain = 1.0f;
    fresh.pan = 0.0f;
    fresh.format = DRUM_FMT_PCM16;
    fresh.play_mode = DRUM_PLAY_ONESHOT;
    fresh.start_ofs = 0.0f;
    fresh.owned = data;
    fresh.owned_bytes = (size_t)frames * 2;
    /* Keep whatever note the pad already answered to, so a re-recorded pad
     * stays where the sequencer and the MIDI map already point. An empty pad
     * falls back to the chromatic-from-GM-kick layout the WAV loader uses. */
    const drum_kit_t* k = drums_kit_at(kit);
    const uint8_t prev_note = (k != nullptr && k->slots[slot].note != 0)
                                  ? k->slots[slot].note
                                  : (uint8_t)(36 + slot);
    fresh.note = prev_note;
    if (name != nullptr && name[0] != '\0') {
        strlcpy(fresh.name, name, DRUM_SLOT_NAME_MAX);
    } else if (index_in_take > 0) {
        snprintf(fresh.name, DRUM_SLOT_NAME_MAX, "slice%d", index_in_take);
    } else {
        snprintf(fresh.name, DRUM_SLOT_NAME_MAX, "pad%d", slot + 1);
    }

    drum_sample_t old = {};
    const esp_err_t err = drums_slot_replace(kit, slot, &fresh, &old);
    if (err != ESP_OK) {
        /* Nothing was published, so this block is ours to give back. */
        sampler_pool_free(data, (size_t)frames * 2);
        return err;
    }
    undo_record(slot, old);
    return ESP_OK;
}

/* Runs on the control task once the audio task has asked for it.
 *
 * There is a window here where the audio task may still be inside the block
 * that set s_stop_req, appending to staging. That is safe rather than merely
 * unlikely: it only ever writes *forward* from s_take_len, and everything
 * below reads [0, end) where end came from that same atomic. The worst case is
 * committing a take a fraction of a block shorter than what was captured,
 * which is not something anyone can hear at the end of a gesture that has
 * already finished. */
void commit_take(int reason) {
    const int kit = drums_kit_index();
    const int slot = s_arm_slot.load(std::memory_order_acquire);
    uint32_t len = s_take_len.load(std::memory_order_acquire);

    publish_state(SAMPLER_COMMITTING);

    if (slot < 0 || slot >= DRUM_SLOTS || !drums_kit_is_user(kit)) {
        ESP_LOGW(TAG, "take dropped: no writable destination (kit %d slot %d)",
                 kit, slot);
        publish_state(s_arm_slot.load(std::memory_order_relaxed) >= 0
                          ? SAMPLER_ARMED
                          : SAMPLER_IDLE);
        return;
    }
    if (len < ms_to_frames((float)kMinTakeMs)) {
        ESP_LOGW(TAG, "take dropped: %u frames is below the %u ms floor",
                 (unsigned)len, (unsigned)kMinTakeMs);
        publish_state(SAMPLER_ARMED);
        return;
    }

    uint32_t end = pv(P_TRIM) >= 0.5f ? trim_tail(0, len) : len;
    if (end <= 0) end = len;
    const bool norm = pv(P_NORM) >= 0.5f;
    int slices = (int)(pv(P_SLICES) + 0.5f);
    if (slices < 1) slices = 1;
    if (slices > DRUM_SLOTS) slices = DRUM_SLOTS;

    undo_begin(kit);

    int made = 0;
    if (slices == 1) {
        uint32_t frames = 0;
        int16_t* data = bake(0, end, norm, &frames);
        if (data == nullptr) {
            ESP_LOGE(TAG, "take dropped: pool full (%u/%u KB)",
                     (unsigned)(sampler_pool_used() / 1024),
                     (unsigned)(kPoolTotal / 1024));
        } else if (publish(kit, slot, data, frames, nullptr, 0) == ESP_OK) {
            made = 1;
        }
    } else {
        uint32_t marks[DRUM_SLOTS + 1];
        int n = 0;
        if ((int)(pv(P_SLICEMODE) + 0.5f) == SLICE_TRANSIENT) {
            n = find_onsets(0, end, marks, slices);
        }
        if (n < 2) {
            /* Either `even` was chosen, or the detector found nothing it was
             * confident about. Falling back rather than refusing: the player
             * asked for eight pads and eight equal pads is a defensible answer
             * to a bar with no obvious transients. */
            n = slices;
            for (int i = 0; i < n; ++i) {
                marks[i] = (uint32_t)((uint64_t)end * (uint32_t)i / (uint32_t)n);
            }
        }
        marks[n] = end;
        for (int i = 0; i < n; ++i) {
            const int dst = (slot + i) % DRUM_SLOTS;
            uint32_t frames = 0;
            int16_t* data = bake(marks[i], marks[i + 1], norm, &frames);
            if (data == nullptr) {
                ESP_LOGE(TAG, "slice %d dropped: pool full", i + 1);
                break;
            }
            if (publish(kit, dst, data, frames, nullptr, i + 1) != ESP_OK) break;
            ++made;
        }
    }

    if (made > 0) {
        ESP_LOGI(TAG,
                 "kit %d: %d pad(s) from a %u ms take%s%s (pool %u/%u KB)",
                 kit, made, (unsigned)(end * 1000u / SYNTH_SAMPLE_RATE),
                 norm ? ", normalised" : "",
                 reason == 2 ? ", stopped at the ceiling" : "",
                 (unsigned)(sampler_pool_used() / 1024),
                 (unsigned)(kPoolTotal / 1024));
    }
    s_take_len.store(0, std::memory_order_relaxed);
    publish_progress();
    publish_state(SAMPLER_ARMED);
}

/* ---- count-in ----
 *
 * Runs on the control task so it can sleep between beats. The gate is already
 * held by the time this starts; what it delays is the transition into
 * WAITING, which is the state that actually opens the recorder. */
void run_countin(int beats) {
    ParamStore& ps = ParamStore::instance();
    float bpm = ps.get(kSeqTempoPid);
    if (bpm < 20.0f || bpm > 400.0f) bpm = kFallbackBpm;
    const int period_ms = (int)(60000.0f / bpm);
    for (int i = 0; i < beats; ++i) {
        if (!s_gate.load(std::memory_order_acquire)) return; /* released */
        drums_click(i == 0);
        vTaskDelay(pdMS_TO_TICKS(period_ms));
    }
    if (s_gate.load(std::memory_order_acquire) &&
        s_state.load(std::memory_order_acquire) == SAMPLER_ARMED) {
        s_take_len.store(0, std::memory_order_relaxed);
        s_take_peak.store(0.0f, std::memory_order_relaxed);
        publish_state(SAMPLER_WAITING);
    }
}

void ctl_task(void* arg) {
    (void)arg;
    for (;;) {
        /* A timeout rather than a plain block: while a take runs, the app's
         * meter and progress bar come from here. 100 ms is ten updates a
         * second, which is a smooth bar and a tenth of what the BLE parameter
         * batch could carry. */
        const bool recording = sampler_recording();
        ulTaskNotifyTake(pdTRUE, recording ? pdMS_TO_TICKS(100)
                                           : portMAX_DELAY);

        const int stop = s_stop_req.exchange(0, std::memory_order_acq_rel);
        if (stop != 0) commit_take(stop);

        if (s_req_countin.exchange(false, std::memory_order_acq_rel)) {
            run_countin((int)(pv(P_COUNTIN) + 0.5f));
        }

        const int erase = s_req_erase.exchange(-1, std::memory_order_acq_rel);
        if (erase >= 0) (void)sampler_erase(drums_kit_index(), erase);

        if (s_req_undo.exchange(false, std::memory_order_acq_rel)) {
            (void)sampler_undo();
        }

        const int copy_to = s_req_copy_to.exchange(-1, std::memory_order_acq_rel);
        if (copy_to >= 0) {
            const int from_kit = (int)(pv(P_COPYKIT) + 0.5f);
            const int from_slot = (int)(pv(P_COPYFROM) + 0.5f);
            (void)sampler_copy_pad(from_kit, from_slot, drums_kit_index(),
                                   copy_to);
        }

        const int dup = s_req_dup_kit.exchange(-1, std::memory_order_acq_rel);
        if (dup >= 0) (void)sampler_dup_kit(drums_kit_index(), dup);

        if (s_req_save.exchange(false, std::memory_order_acq_rel)) {
            const int kit = drums_kit_index();
            const drum_kit_t* k = drums_kit_at(kit);
            if (k != nullptr && drums_kit_is_user(kit)) {
                const esp_err_t err = drum_kit_save_user(kit, k);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "kit %d save failed (%s)", kit,
                             esp_err_to_name(err));
                }
            }
        }

        if (sampler_recording()) publish_progress();
    }
}

/* ---- parameter listener ----
 *
 * Runs on whoever called set() — the BLE command task, the MIDI task, a preset
 * load. So it does the minimum: latch a request into an atomic and wake the
 * control task. Nothing here allocates, blocks or touches a kit. */
void param_listener(uint16_t id, float value, ParamOrigin origin, void* ctx) {
    (void)origin;
    (void)ctx;
    switch (id) {
        case SMP_PID_ARM: {
            const int slot = (int)(value + 0.5f);
            s_arm_slot.store(slot >= 0 && slot < DRUM_SLOTS ? slot : -1,
                             std::memory_order_release);
            const int st = s_state.load(std::memory_order_acquire);
            /* Re-aiming mid-take is not a thing: the destination is latched
             * when the take commits, and moving it under a running recorder
             * would land the audio somewhere the player did not watch it go. */
            if (st == SAMPLER_IDLE || st == SAMPLER_ARMED) {
                publish_state(s_arm_slot.load(std::memory_order_relaxed) >= 0
                                  ? SAMPLER_ARMED
                                  : SAMPLER_IDLE);
            }
            break;
        }
        case SMP_PID_REC: {
            const bool on = value >= 0.5f;
            const bool was = s_gate.exchange(on, std::memory_order_acq_rel);
            if (on == was) break;
            if (on) {
                if (s_ring == nullptr || !ensure_stage()) {
                    if (s_last_refusal != 1) {
                        ESP_LOGW(TAG, "record refused: no sample memory on "
                                      "this build");
                        s_last_refusal = 1;
                    }
                    break;
                }
                if (s_arm_slot.load(std::memory_order_acquire) < 0) {
                    if (s_last_refusal != 2) {
                        ESP_LOGW(TAG, "record refused: arm a pad first "
                                      "(smp.arm)");
                        s_last_refusal = 2;
                    }
                    break;
                }
                s_last_refusal = 0;
                if ((int)(pv(P_COUNTIN) + 0.5f) > 0) {
                    s_req_countin.store(true, std::memory_order_release);
                } else {
                    s_take_len.store(0, std::memory_order_relaxed);
                    s_take_peak.store(0.0f, std::memory_order_relaxed);
                    publish_state(SAMPLER_WAITING);
                }
            } else {
                const int st = s_state.load(std::memory_order_acquire);
                if (st == SAMPLER_RECORDING) {
                    s_stop_req.store(1, std::memory_order_release);
                    s_state.store(SAMPLER_COMMITTING, std::memory_order_release);
                } else if (st == SAMPLER_WAITING) {
                    /* Released before the threshold ever tripped: no take. */
                    publish_state(SAMPLER_ARMED);
                    break;
                } else {
                    break;
                }
            }
            if (s_ctl_task != nullptr) xTaskNotifyGive(s_ctl_task);
            break;
        }
        case SMP_PID_ERASE: {
            const int slot = (int)(value + 0.5f);
            if (slot >= 0 && slot < DRUM_SLOTS) {
                s_req_erase.store(slot, std::memory_order_release);
                if (s_ctl_task != nullptr) xTaskNotifyGive(s_ctl_task);
            }
            break;
        }
        case SMP_PID_UNDO:
            if (value >= 0.5f) {
                s_req_undo.store(true, std::memory_order_release);
                if (s_ctl_task != nullptr) xTaskNotifyGive(s_ctl_task);
            }
            break;
        case SMP_PID_COPYTO: {
            const int slot = (int)(value + 0.5f);
            if (slot >= 0 && slot < DRUM_SLOTS) {
                s_req_copy_to.store(slot, std::memory_order_release);
                if (s_ctl_task != nullptr) xTaskNotifyGive(s_ctl_task);
            }
            break;
        }
        case SMP_PID_DUPKIT: {
            const int kit = (int)(value + 0.5f);
            if (kit > 0) {
                s_req_dup_kit.store(kit, std::memory_order_release);
                if (s_ctl_task != nullptr) xTaskNotifyGive(s_ctl_task);
            }
            break;
        }
        case SMP_PID_SAVE:
            if (value >= 0.5f) {
                s_req_save.store(true, std::memory_order_release);
                if (s_ctl_task != nullptr) xTaskNotifyGive(s_ctl_task);
            }
            break;
        default:
            break;
    }
}

const ParamDesc kParams[P_COUNT] = {
    {SMP_PID_SRC, "smp.src", ParamType::Enum, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, kSrcNames, 2},
    /* -1 is a real value here, not a sentinel bolted on: "nothing armed" is
     * the state the pads are in almost all of the time, and the app needs to
     * be able to say it. */
    {SMP_PID_ARM, "smp.arm", ParamType::Int, ParamCurve::Linear,
     -1.0f, (float)(DRUM_SLOTS - 1), -1.0f, nullptr, 0},
    {SMP_PID_REC, "smp.rec", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {SMP_PID_ERASE, "smp.erase", ParamType::Int, ParamCurve::Linear,
     -1.0f, (float)(DRUM_SLOTS - 1), -1.0f, nullptr, 0},
    {SMP_PID_UNDO, "smp.undo", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {SMP_PID_THRESH, "smp.thresh", ParamType::Float, ParamCurve::Linear,
     0.0f, 0.5f, 0.0f, nullptr, 0},
    {SMP_PID_TRIM, "smp.trim", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0},
    {SMP_PID_NORM, "smp.norm", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0},
    {SMP_PID_PREROLL, "smp.preroll", ParamType::Float, ParamCurve::Linear,
     0.0f, (float)kPrerollMs, 120.0f, nullptr, 0},
    {SMP_PID_MAXSEC, "smp.maxsec", ParamType::Float, ParamCurve::Linear,
     0.1f, (float)SYNTH_SAMPLE_MAX_SEC, (float)SYNTH_SAMPLE_MAX_SEC,
     nullptr, 0},
    {SMP_PID_SLICES, "smp.slices", ParamType::Int, ParamCurve::Linear,
     1.0f, (float)DRUM_SLOTS, 1.0f, nullptr, 0},
    {SMP_PID_SLICEMODE, "smp.slicemode", ParamType::Enum, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f /* transient */, kSliceModeNames, 2},
    {SMP_PID_MONITOR, "smp.monitor", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0},
    {SMP_PID_COUNTIN, "smp.countin", ParamType::Int, ParamCurve::Linear,
     0.0f, 8.0f, 0.0f, nullptr, 0},
    {SMP_PID_SAVE, "smp.save", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {SMP_PID_GAIN, "smp.gain", ParamType::Float, ParamCurve::Linear,
     0.0f, 8.0f, 1.0f, nullptr, 0},
    {SMP_PID_COPYFROM, "smp.copyfrom", ParamType::Int, ParamCurve::Linear,
     -1.0f, (float)(DRUM_SLOTS - 1), -1.0f, nullptr, 0},
    {SMP_PID_COPYKIT, "smp.copykit", ParamType::Int, ParamCurve::Linear,
     -1.0f, (float)SYNTH_SAMPLE_KITS, -1.0f, nullptr, 0},
    {SMP_PID_COPYTO, "smp.copyto", ParamType::Int, ParamCurve::Linear,
     -1.0f, (float)(DRUM_SLOTS - 1), -1.0f, nullptr, 0},
    {SMP_PID_DUPKIT, "smp.dupkit", ParamType::Int, ParamCurve::Linear,
     -1.0f, (float)SYNTH_SAMPLE_KITS, -1.0f, nullptr, 0},
    /* Read-only below. There is no flag for that in ParamDesc — it is a
     * convention the app follows — but writing one costs nothing worse than
     * being overwritten by the next publish, which is why they are safe to
     * expose on a store that has no notion of direction. */
    {SMP_PID_STATE, "smp.state", ParamType::Int, ParamCurve::Linear,
     0.0f, 4.0f, 0.0f, nullptr, 0},
    {SMP_PID_POS, "smp.pos", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {SMP_PID_FREE, "smp.free", ParamType::Float, ParamCurve::Linear,
     0.0f, (float)(kPoolTotal / 1024u ? kPoolTotal / 1024u : 1u),
     (float)(kPoolTotal / 1024u), nullptr, 0},
    {SMP_PID_PEAK, "smp.peak", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
};

} // namespace

/* ======================= editing ======================================= */

esp_err_t sampler_erase(int kit, int slot) {
    if (slot < 0 || slot >= DRUM_SLOTS) return ESP_ERR_INVALID_ARG;
    if (!drums_kit_is_user(kit)) return ESP_ERR_NOT_SUPPORTED;
    drum_kit_t* k = drums_kit_at(kit);
    if (k == nullptr) return ESP_ERR_INVALID_ARG;
    if (k->slots[slot].data == nullptr) return ESP_OK; /* already empty */

    const drum_sample_t empty = {};
    drum_sample_t old = {};
    const esp_err_t err = drums_slot_replace(kit, slot, &empty, &old);
    if (err != ESP_OK) return err;

    undo_begin(kit);
    undo_record(slot, old);
    ESP_LOGI(TAG, "kit %d pad %d erased (pool %u/%u KB)", kit, slot + 1,
             (unsigned)(sampler_pool_used() / 1024),
             (unsigned)(kPoolTotal / 1024));
    publish_progress();
    return ESP_OK;
}

esp_err_t sampler_undo(void) {
    if (!s_undo.valid || s_undo.count == 0) {
        ESP_LOGW(TAG, "nothing to undo");
        return ESP_ERR_NOT_FOUND;
    }
    const int kit = s_undo.kit;
    /* Take a copy and clear the stash first: publishing below hands us back
     * the blocks that are being displaced, and those are what the *next* undo
     * would restore — so the stash must not still be describing the state we
     * are in the middle of leaving. */
    UndoOp op = s_undo;
    s_undo = UndoOp{};

    int restored = 0;
    for (int i = 0; i < op.count; ++i) {
        drum_sample_t replaced = {};
        if (drums_slot_replace(kit, op.slots[i], &op.prev[i], &replaced) !=
            ESP_OK) {
            /* Could not publish: the block we were putting back is still ours,
             * and dropping it here would leak it out of the pool accounting. */
            if (op.prev[i].owned != nullptr) {
                sampler_pool_free(op.prev[i].owned, op.prev[i].owned_bytes);
            }
            continue;
        }
        if (replaced.owned != nullptr) {
            sampler_pool_free(replaced.owned, replaced.owned_bytes);
        }
        ++restored;
    }
    ESP_LOGI(TAG, "undo: %d pad(s) restored on kit %d (pool %u/%u KB)",
             restored, kit, (unsigned)(sampler_pool_used() / 1024),
             (unsigned)(kPoolTotal / 1024));
    publish_progress();
    return restored > 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t sampler_copy_pad(int from_kit, int from_slot, int to_kit,
                           int to_slot) {
    if (from_kit < 0) from_kit = drums_kit_index();
    if (from_slot < 0 || from_slot >= DRUM_SLOTS) return ESP_ERR_INVALID_ARG;
    if (to_slot < 0 || to_slot >= DRUM_SLOTS) return ESP_ERR_INVALID_ARG;
    if (!drums_kit_is_user(to_kit)) return ESP_ERR_NOT_SUPPORTED;
    const drum_kit_t* src = drums_kit_at(from_kit);
    if (src == nullptr) return ESP_ERR_INVALID_ARG;
    const drum_sample_t& s = src->slots[from_slot];
    if (s.data == nullptr || s.frames == 0) return ESP_ERR_NOT_FOUND;

    /* Its own block, always. Two kits sharing one would make erasing either of
     * them a use-after-free in the other, and a "copy" that aliases is a
     * copy whose semantics change the first time someone re-records. */
    const size_t bytes =
        (size_t)s.frames * (s.format == DRUM_FMT_PCM16 ? 2u : 1u);
    void* data = sampler_pool_alloc(bytes);
    if (data == nullptr) {
        ESP_LOGE(TAG, "copy refused: pool full (%u KB needed)",
                 (unsigned)(bytes / 1024));
        return ESP_ERR_NO_MEM;
    }
    memcpy(data, s.data, bytes);

    drum_sample_t fresh = s;
    fresh.data = (const uint8_t*)data;
    fresh.owned = data;
    fresh.owned_bytes = bytes;
    /* The destination keeps its own note, so copying a sound onto a pad does
     * not silently move where the sequencer's lane points. */
    const drum_kit_t* dst = drums_kit_at(to_kit);
    if (dst != nullptr && dst->slots[to_slot].note != 0) {
        fresh.note = dst->slots[to_slot].note;
    }

    drum_sample_t old = {};
    const esp_err_t err = drums_slot_replace(to_kit, to_slot, &fresh, &old);
    if (err != ESP_OK) {
        sampler_pool_free(data, bytes);
        return err;
    }
    undo_begin(to_kit);
    undo_record(to_slot, old);
    ESP_LOGI(TAG, "copied kit %d pad %d -> kit %d pad %d (%u KB)", from_kit,
             from_slot + 1, to_kit, to_slot + 1, (unsigned)(bytes / 1024));
    publish_progress();
    return ESP_OK;
}

esp_err_t sampler_dup_kit(int from_kit, int to_kit) {
    if (from_kit == to_kit) return ESP_ERR_INVALID_ARG;
    if (!drums_kit_is_user(to_kit)) return ESP_ERR_NOT_SUPPORTED;
    const drum_kit_t* src = drums_kit_at(from_kit);
    drum_kit_t* dst = drums_kit_at(to_kit);
    if (src == nullptr || dst == nullptr) return ESP_ERR_INVALID_ARG;

    /* Size the whole thing first. A kit that duplicates half way and then runs
     * out is worse than one that refuses: the player would have to work out
     * which pads made it. */
    size_t need = 0;
    for (int i = 0; i < DRUM_SLOTS; ++i) {
        const drum_sample_t& s = src->slots[i];
        if (s.data == nullptr || s.frames == 0) continue;
        need += (size_t)s.frames * (s.format == DRUM_FMT_PCM16 ? 2u : 1u);
    }
    if (sampler_pool_used() + need > kPoolTotal) {
        ESP_LOGE(TAG, "duplicate refused: needs %u KB, %u KB free",
                 (unsigned)(need / 1024),
                 (unsigned)((kPoolTotal - sampler_pool_used()) / 1024));
        return ESP_ERR_NO_MEM;
    }

    /* Clear the destination one pad at a time through the same protocol
     * everything else uses, so a kit being duplicated onto while it is the
     * bound one is not a special case. */
    for (int i = 0; i < DRUM_SLOTS; ++i) {
        if (dst->slots[i].data == nullptr) continue;
        const drum_sample_t empty = {};
        drum_sample_t old = {};
        if (drums_slot_replace(to_kit, i, &empty, &old) == ESP_OK &&
            old.owned != nullptr) {
            sampler_pool_free(old.owned, old.owned_bytes);
        }
    }
    undo_release(); /* the pads just dropped are gone; do not offer them back */

    int copied = 0;
    for (int i = 0; i < DRUM_SLOTS; ++i) {
        const drum_sample_t& s = src->slots[i];
        if (s.data == nullptr || s.frames == 0) continue;
        const size_t bytes =
            (size_t)s.frames * (s.format == DRUM_FMT_PCM16 ? 2u : 1u);
        void* data = sampler_pool_alloc(bytes);
        if (data == nullptr) break;
        memcpy(data, s.data, bytes);
        drum_sample_t fresh = s;
        fresh.data = (const uint8_t*)data;
        fresh.owned = data;
        fresh.owned_bytes = bytes;
        drum_sample_t old = {};
        if (drums_slot_replace(to_kit, i, &fresh, &old) != ESP_OK) {
            sampler_pool_free(data, bytes);
            break;
        }
        ++copied;
    }
    for (int i = 0; i < DRUM_SLOTS; ++i) dst->mix[i] = src->mix[i];
    snprintf(dst->name, DRUM_KIT_NAME_MAX, "%.*s copy",
             DRUM_KIT_NAME_MAX - 6, src->name);
    drums_kit_mark_dirty(to_kit);
    ESP_LOGI(TAG, "kit %d duplicated onto kit %d (%d pads, %u KB)", from_kit,
             to_kit, copied, (unsigned)(need / 1024));
    publish_progress();
    return ESP_OK;
}

/* ======================= init ========================================== */

esp_err_t sampler_init(void) {
    ParamStore& ps = ParamStore::instance();
    const size_t added = ps.add(kParams, P_COUNT);
    if (added != P_COUNT) {
        ESP_LOGE(TAG, "registered %u/%u params", (unsigned)added,
                 (unsigned)P_COUNT);
        return ESP_FAIL;
    }
    for (size_t i = 0; i < P_COUNT; ++i) s_p[i] = ps.valuePtr(kParams[i].id);
    ps.addListener(param_listener, nullptr);

    if (SYNTH_SAMPLE_KITS == 0) {
        ESP_LOGI(TAG, "no sample kits on this build — the recorder is "
                      "registered but will refuse every take");
        return ESP_OK;
    }

    /* The ring is the one allocation made unconditionally at init, because it
     * has to be filling before anyone thinks about arming. 24 KB. */
    s_ring = (int16_t*)heap_caps_malloc((size_t)kRingLen * 2, MALLOC_CAP_SPIRAM);
    if (s_ring == nullptr) {
        ESP_LOGE(TAG, "no PSRAM for the %u ms pre-roll ring — recording is "
                      "unavailable",
                 (unsigned)kPrerollMs);
        return ESP_OK; /* the sink-fallback rule: never fail the boot */
    }
    memset(s_ring, 0, (size_t)kRingLen * 2);

    if (xTaskCreatePinnedToCore(ctl_task, "smp_ctl", kStateTaskStack, nullptr,
                                kCtlTaskPrio, &s_ctl_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "control task not started — recording is unavailable");
        s_ctl_task = nullptr;
        heap_caps_free(s_ring);
        s_ring = nullptr;
        return ESP_OK;
    }

    ESP_LOGI(TAG,
             "up: %d kits x %d pads, %u KB pool, %u s max take, %u ms "
             "pre-roll, storage %s",
             SYNTH_SAMPLE_KITS, DRUM_SLOTS, (unsigned)(kPoolTotal / 1024),
             (unsigned)SYNTH_SAMPLE_MAX_SEC, (unsigned)kPrerollMs,
             drum_kit_storage_name());
    return ESP_OK;
}
