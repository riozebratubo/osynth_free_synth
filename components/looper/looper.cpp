/*
 * osynth — 8-track loop recorder (Session 15).
 *
 * Split of responsibilities (the house control/audio contract):
 *  - The audio task owns the transport: loop position, record state and the
 *    per-sample copy/mix in looper_process(). It never locks or allocates.
 *  - The `loop_ctl` task (core 0) owns all memory: it allocates a track's
 *    buffer *before* publishing the record command, and frees buffers only
 *    after the S6-style render handshake (two looper_process boundaries
 *    past the swap/detach, so no in-flight block can still hold a pointer).
 *    It also mirrors state into the read-only params (loop.len,
 *    loop.filled) and drops loop.mode back to play when a punch-in
 *    completes on its own — origin Internal, so BLE pushes the change to
 *    the app as EVT_PARAMS.
 *  - The ParamStore listener only sets flag bits and wakes loop_ctl; its
 *    own reflect writes are filtered by task handle (seqarp's guard).
 *
 * Control -> audio is a single-word command mailbox (latest wins — commands
 * are human-scale and only loop_ctl publishes); audio -> control is an
 * event bit set + task notify.
 *
 * Recording semantics:
 *  - First recording (no loop yet): starts immediately, the write cursor
 *    defines the loop; leaving rec (play or stop) closes it. Hitting the
 *    take's cap (s_rec_limit) closes it automatically. Takes shorter than
 *    kMinFrames are discarded.
 *  - With a loop: entering rec arms a punch-in at the next loop start
 *    (sample-accurate — blocks are split at the wrap), records exactly one
 *    pass, then falls back to play by itself. The track's old content keeps
 *    playing until the punch lands. Leaving rec before the pass completes
 *    cancels it (a partially overwritten track is marked empty — honest
 *    state beats a half-stale loop).
 *  - Recording replaces the selected track; the other filled tracks keep
 *    playing and are mixed in *after* the record tap, so they never bleed
 *    into the new take.
 *
 * Tracks are IMA-ADPCM in PSRAM since S20 (loop_adpcm.h): the audio task
 * encodes the record tap and decodes playback per block. That is safe
 * because the transport is strictly sequential from the loop start —
 * per-track decoder states reset at every wrap (and whenever the position
 * returns to 0), so there is no seeking, ever. Consequences the code
 * relies on:
 *  - A muted track (level 0) still decodes: skipping blocks would desync
 *    its decoder from the transport (ADPCM cannot re-enter mid-stream).
 *  - The S17 SIMD mix kernels are no longer used here — decode is a
 *    serial per-sample dependency chain; the PSRAM traffic is 4-8x lower
 *    instead.
 *
 * The first track is allocated at the policy cap (the length is not known
 * yet; 40 s stereo x2 mono x2 4-track, S20) and compacted to the actual
 * loop length once it closes (copy + pointer swap + render handshake +
 * free). A first take whose cap does not fit free PSRAM halves it until
 * it fits (the achieved ceiling is mirrored via loop.maxlen). Buffers are
 * PSRAM-only — no internal-RAM fallback (unlike the FX lines): a
 * loop-length buffer would eat the heap the voice pool and BLE live in.
 */
#include "looper.h"

#include "drums.h"
#include "seqarp.h"

#include <atomic>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "loop_adpcm.h"
#include "loop_store.h"
#include "synth_config.h"
#include "synth_params.h"

static const char* TAG = "looper";

#if CONFIG_SPIRAM

using osynth::ParamCurve;
using osynth::ParamDesc;
using osynth::ParamOrigin;
using osynth::ParamStore;
using osynth::ParamType;

namespace {

/* Cap policy (S20): base 40 s (stereo, 8-track mode), x2 for mono
 * (loop.mono halves the bytes per frame), x2 in 4-track mode (loop.tracks
 * trades slots for length) — 160 s mono/4-track ceiling. At the full cap
 * a track costs ~1.9 MB (3.8 MB in 4-track mode), so ~4 (resp. ~2) tracks
 * fit the ~7 MB PSRAM pool at cap and everything fits at half the cap;
 * the graceful first-take fallback below covers the shortfalls. */
constexpr float kLoopBaseCapS = 40.0f;
constexpr float kLoopAbsMaxS = kLoopBaseCapS * 4.0f; /* mono + 4-track */
constexpr uint32_t kMinFrames = SYNTH_SAMPLE_RATE / 4; /* 0.25 s */
constexpr uint32_t kMinTakeCapFrames =
    SYNTH_SAMPLE_RATE * 5; /* fallback floor: give up below 5 s */
constexpr uint32_t kCompactSlackFrames = 65536; /* compact if > 64 KB
                                                 * (stereo) / 32 KB (mono)
                                                 * of waste */

constexpr uint32_t cap_frames(bool mono, bool four_tracks) {
    return (uint32_t)(kLoopBaseCapS * SYNTH_SAMPLE_RATE) *
           (mono ? 2u : 1u) * (four_tracks ? 2u : 1u);
}

/* PSRAM/storage bytes of one track (S20: ADPCM — stereo 1 B/frame, mono
 * two frames per byte with the odd tail padding a nibble). */
inline size_t track_bytes_for(uint32_t frames, bool mono) {
    return mono ? ((size_t)frames + 1) / 2 : (size_t)frames;
}

constexpr int kCtlTaskPrio = 5; /* control plane, core 0 (ARCHITECTURE.md) */
constexpr int kCtlTaskStack = 4096;

enum { MODE_STOP = 0, MODE_PLAY, MODE_REC };
enum { CLR_NONE = 0, CLR_TRACK, CLR_ALL };

const char* const kModeNames[] = {"stop", "play", "rec"};
const char* const kClearNames[] = {"none", "track", "all"};
const char* const kMonoNames[] = {"stereo", "mono"};
const char* const kTracksNames[] = {"8", "4"};

const ParamDesc kParams[] = {
    {LOOP_PID_TRACK, "loop.track", ParamType::Int, ParamCurve::Linear,
     1.0f, (float)LOOP_TRACKS, 1.0f, nullptr, 0},
    {LOOP_PID_MODE, "loop.mode", ParamType::Enum, ParamCurve::Linear,
     0.0f, 2.0f, 0.0f, kModeNames, 3},
    {LOOP_PID_CLEAR, "loop.clear", ParamType::Enum, ParamCurve::Linear,
     0.0f, 2.0f, 0.0f, kClearNames, 3}, /* command; snaps back to none */
    {LOOP_PID_FILLED, "loop.filled", ParamType::Int, ParamCurve::Linear,
     0.0f, 255.0f, 0.0f, nullptr, 0}, /* read-only status bitmask */
    {LOOP_PID_LEN, "loop.len", ParamType::Float, ParamCurve::Linear,
     0.0f, kLoopAbsMaxS, 0.0f, nullptr, 0}, /* read-only status, seconds;
                                             * ranged for the ceiling —
                                             * loop.maxlen is the live cap */
    {LOOP_PID_POS, "loop.pos", ParamType::Float, ParamCurve::Linear,
     0.0f, kLoopAbsMaxS, 0.0f, nullptr, 0}, /* read-only status, seconds */
    {LOOP_PID_RECTRK, "loop.rectrk", ParamType::Int, ParamCurve::Linear,
     0.0f, (float)LOOP_TRACKS, 0.0f, nullptr, 0}, /* read-only, 0 = none */
    {LOOP_PID_MONO, "loop.mono", ParamType::Enum, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, kMonoNames, 2}, /* format of the *next* loop set;
                                        * defaults on — length beats the
                                        * stereo image for a loop pedal */
    {LOOP_PID_MAXLEN, "loop.maxlen", ParamType::Float, ParamCurve::Linear,
     0.0f, kLoopAbsMaxS, kLoopBaseCapS, nullptr, 0}, /* read-only live cap;
                                                      * default = base cap,
                                                      * max = ceiling —
                                                      * clients compute
                                                      * default x2 per
                                                      * enabled toggle */
    {LOOP_PID_SYNC, "loop.sync", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {LOOP_PID_COUNTIN, "loop.countin", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0}, /* on: hitting rec mid-bar is the norm */
    {LOOP_PID_ARMED, "loop.armed", ParamType::Int, ParamCurve::Linear,
     0.0f, 8.0f, 0.0f, nullptr, 0}, /* read-only: beats until rec starts */
    {LOOP_PID_TRACKMODE, "loop.tracks", ParamType::Enum, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, kTracksNames, 2}, /* cap/UI policy for the *next*
                                          * set; never blocks loop.track.
                                          * Defaults to 4 — with mono also
                                          * on the out-of-the-box cap is the
                                          * 160 s ceiling */
    {LOOP_PID_LEVEL(0), "loop.lvl1", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0},
    {LOOP_PID_LEVEL(1), "loop.lvl2", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0},
    {LOOP_PID_LEVEL(2), "loop.lvl3", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0},
    {LOOP_PID_LEVEL(3), "loop.lvl4", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0},
    {LOOP_PID_LEVEL(4), "loop.lvl5", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0},
    {LOOP_PID_LEVEL(5), "loop.lvl6", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0},
    {LOOP_PID_LEVEL(6), "loop.lvl7", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0},
    {LOOP_PID_LEVEL(7), "loop.lvl8", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0},
};
constexpr size_t kParamCount = sizeof(kParams) / sizeof(kParams[0]);

#if SYNTH_ENABLE_LOOP_PERSIST
/* Trigger params (S16): the registered max is the backend's real slot
 * ceiling, so the app's controls pick up the right range from PARAM_INFO. */
constexpr float kSlotMax = SYNTH_LOOP_STORE_SD
                               ? (float)(LOOP_STORE_SLOTS_SD - 1)
                               : (float)(LOOP_STORE_SLOTS_FLASH - 1);
const ParamDesc kPersistParams[] = {
    {LOOP_PID_SAVE, "loop.save", ParamType::Int, ParamCurve::Linear,
     0.0f, kSlotMax, 0.0f, nullptr, 0}, /* trigger: writing saves that slot */
    {LOOP_PID_LOAD, "loop.load", ParamType::Int, ParamCurve::Linear,
     0.0f, kSlotMax, 0.0f, nullptr, 0}, /* trigger: writing loads that slot */
};
constexpr size_t kPersistParamCount =
    sizeof(kPersistParams) / sizeof(kPersistParams[0]);
#endif

const std::atomic<float>* s_lvl[LOOP_TRACKS];

/* ---- shared state (ownership per the header comment) ---- */

std::atomic<uint8_t*> s_buf[LOOP_TRACKS]; /* ADPCM bytes (stereo 1 B/frame,
                                           * mono 2 frames/B); loop_ctl
                                           * writes the pointers */
size_t s_cap_bytes[LOOP_TRACKS];          /* bytes allocated; loop_ctl only —
                                           * bytes, not frames: the frame
                                           * size follows the set format */
std::atomic<bool> s_mono{true};           /* live set format; loop_ctl
                                           * latches it while audio is
                                           * detached or the set is empty.
                                           * Seeded from the registered
                                           * loop.mono/loop.tracks defaults
                                           * (both on) until it does */
std::atomic<uint32_t> s_rec_limit{cap_frames(true, true)};
                                          /* first-take ceiling in frames —
                                           * the policy cap, or less after
                                           * the alloc fallback; loop_ctl
                                           * publishes before kCmdRec */
std::atomic<uint32_t> s_loop_frames{0};   /* L; audio closes, loop_ctl resets */
std::atomic<uint8_t> s_filled{0};         /* audio sets bits, loop_ctl clears */
std::atomic<uint32_t> s_render_seq{0};    /* ticks at end of looper_process */
std::atomic<int> s_rec_trk_live{-1};      /* track audio is writing, -1 none */
std::atomic<uint32_t> s_pos{0};           /* transport position, frames */

/* command mailbox: loop_ctl -> audio, latest wins */
constexpr uint32_t kCmdStop = 1u << 8;
constexpr uint32_t kCmdPlay = 2u << 8;
constexpr uint32_t kCmdRec = 3u << 8; /* low byte = track */
constexpr uint32_t kCmdDetach = 4u << 8;
std::atomic<uint32_t> s_cmd{0};

/* event bits: audio -> loop_ctl */
constexpr uint32_t kEvtState = 1u << 0;    /* len / filled changed */
constexpr uint32_t kEvtAutoPlay = 1u << 1; /* rec self-ended: mode -> play */
std::atomic<uint32_t> s_evt{0};

/* flag bits: listener -> loop_ctl */
constexpr uint32_t kFlagMode = 1u << 0;
constexpr uint32_t kFlagClear = 1u << 1;
constexpr uint32_t kFlagSave = 1u << 2;
constexpr uint32_t kFlagLoad = 1u << 3;
constexpr uint32_t kFlagMono = 1u << 4;
constexpr uint32_t kFlagArmTick = 1u << 5; /* a count-in beat passed */
constexpr uint32_t kFlagArmFire = 1u << 6; /* countdown done: start rec */
std::atomic<uint32_t> s_flags{0};

TaskHandle_t s_ctl_task = nullptr;

/* ---- audio-task-only transport state ---- */

bool a_playing = false;
bool a_rec = false;
bool a_rec_pending = false;
int a_rec_trk = 0;
uint32_t a_pos = 0;

/* ADPCM codec state (S20). Decoders track the transport: reset whenever
 * a_pos returns to 0 (wrap, stop, rec start, first-take close) — the only
 * discontinuities the transport has. The encoder pair serves the one
 * recording track; reset when a take starts (rec-now or punch at wrap). */
osynth::adpcm::Ch a_dec[LOOP_TRACKS][2];
osynth::adpcm::Ch a_enc[2];

inline void audio_reset_dec() {
    for (int t = 0; t < LOOP_TRACKS; ++t) {
        a_dec[t][0] = osynth::adpcm::Ch{};
        a_dec[t][1] = osynth::adpcm::Ch{};
    }
}

inline void audio_reset_enc() {
    a_enc[0] = osynth::adpcm::Ch{};
    a_enc[1] = osynth::adpcm::Ch{};
}

inline int16_t f2i16(float v) {
    int32_t s = (int32_t)(v * 32767.0f);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return (int16_t)s;
}

inline void audio_raise(uint32_t bits) {
    s_evt.fetch_or(bits, std::memory_order_release);
    if (s_ctl_task != nullptr) xTaskNotifyGive(s_ctl_task);
}

/* Rec teardown when a transport command interrupts it (audio task). */
void audio_close_rec() {
    const uint32_t L = s_loop_frames.load(std::memory_order_relaxed);
    if (L == 0 && a_rec) {
        /* first recording: leaving rec closes the loop (or discards a
         * too-short take); either way the loop restarts at the top */
        if (a_pos >= kMinFrames) {
            s_loop_frames.store(a_pos, std::memory_order_release);
            s_filled.fetch_or((uint8_t)(1u << a_rec_trk),
                              std::memory_order_release);
        }
        a_pos = 0;
        audio_reset_dec();
        audio_raise(kEvtState);
    } else if (L > 0 && a_rec) {
        /* punch-in canceled mid-pass: partially overwritten -> empty */
        s_filled.fetch_and((uint8_t)~(1u << a_rec_trk),
                           std::memory_order_release);
        audio_raise(kEvtState);
    }
    a_rec = false;
    a_rec_pending = false;
    s_rec_trk_live.store(-1, std::memory_order_release);
}

void audio_apply_cmd(uint32_t cmd) {
    switch (cmd & 0xFF00u) {
        case kCmdStop:
            audio_close_rec();
            a_playing = false;
            a_pos = 0;
            audio_reset_dec();
            break;
        case kCmdPlay:
            audio_close_rec();
            a_playing = true;
            if (a_pos == 0) audio_reset_dec(); /* fresh start from the top */
            break;
        case kCmdRec: {
            if (a_rec || a_rec_pending) break; /* loop_ctl guards; be safe */
            a_rec_trk = (int)(cmd & 0xFFu) % LOOP_TRACKS;
            /* no loop yet, or stopped (stop rewinds to the top): record from
             * here; otherwise punch in at the next wrap */
            if (s_loop_frames.load(std::memory_order_relaxed) == 0 ||
                (!a_playing && a_pos == 0)) {
                a_rec = true;
                a_pos = 0;
                audio_reset_dec();
                audio_reset_enc();
                s_rec_trk_live.store(a_rec_trk, std::memory_order_release);
            } else {
                a_rec_pending = true; /* encoder resets at the wrap */
            }
            a_playing = true;
            break;
        }
        case kCmdDetach:
            a_rec = false;
            a_rec_pending = false;
            a_playing = false;
            a_pos = 0;
            audio_reset_dec();
            s_rec_trk_live.store(-1, std::memory_order_release);
            break;
        default:
            break;
    }
}

/* ---- loop_ctl task: allocation, params mirror, clears ---- */

int s_ctl_mode = MODE_STOP; /* last transport we commanded */
uint32_t s_ctl_len = 0;     /* last loop length we saw (for the close log) */

/* Two looper_process boundaries: no in-flight block still holds pointers
 * loaded before a swap/detach (the S6 protocol). Returns false when the audio
 * task never got there — every caller frees something afterwards, so they
 * must leak instead. A few MB of PSRAM is recoverable by clearing the set (or
 * a reboot); decoding ADPCM out of a freed buffer in the render loop is not.
 *
 * The unsigned difference is deliberate. Comparing against `seq0 + 2`
 * returned immediately for one 500 ms window every ~66 days, when the block
 * counter wraps past UINT32_MAX and `seq0 + 2` becomes a small number that
 * the current, still-large value compares greater than. */
bool ctl_handshake() {
    const uint32_t seq0 = s_render_seq.load(std::memory_order_acquire);
    for (int i = 0; i < 250; ++i) { /* 500 ms cap */
        if (s_render_seq.load(std::memory_order_acquire) - seq0 >= 2) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGE(TAG, "render handshake timed out");
    return false;
}

/* Drops every track buffer, freeing only what the audio task has provably let
 * go of. Used by the two paths that replace the whole set (clear-all, load). */
void ctl_release_all(bool settled) {
    for (int t = 0; t < LOOP_TRACKS; ++t) {
        uint8_t* p = s_buf[t].load(std::memory_order_relaxed);
        s_buf[t].store(nullptr, std::memory_order_release);
        s_cap_bytes[t] = 0;
        if (p != nullptr && settled) heap_caps_free(p);
    }
    if (!settled) {
        ESP_LOGE(TAG, "track buffers leaked rather than freed under a render "
                      "that would not yield");
    }
}

bool ctl_alloc(int trk, uint32_t frames, bool mono) {
    const size_t need = track_bytes_for(frames, mono);
    if (s_buf[trk].load(std::memory_order_relaxed) != nullptr &&
        s_cap_bytes[trk] >= need) {
        return true; /* reuse */
    }
    uint8_t* p = (uint8_t*)heap_caps_malloc(
        need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) return false;
    uint8_t* old = s_buf[trk].load(std::memory_order_relaxed);
    s_buf[trk].store(p, std::memory_order_release);
    s_cap_bytes[trk] = need;
    if (old != nullptr) {
        /* only reachable if a track ever needs to grow — clear-all frees
         * buffers detached, so this is a safety net; handshake anyway */
        if (ctl_handshake()) {
            heap_caps_free(old);
        } else {
            ESP_LOGE(TAG, "track %d: old buffer leaked (handshake)", trk + 1);
        }
    }
    return true;
}

void ctl_mirror_state() {
    ParamStore& ps = ParamStore::instance();
    const uint32_t L = s_loop_frames.load(std::memory_order_acquire);
    const uint8_t filled = s_filled.load(std::memory_order_acquire);
    ps.set(LOOP_PID_LEN, (float)L / (float)SYNTH_SAMPLE_RATE,
           ParamOrigin::Internal);
    ps.set(LOOP_PID_FILLED, (float)filled, ParamOrigin::Internal);
    /* The one line that says whether the looper thinks anything is recorded.
     * It only prints when the state actually changes — a take closing, a
     * clear, a load — so it is a handful of lines per session, and it is the
     * difference between "the firmware never set the bit" and "the app never
     * received it" when a control that depends on loop.filled looks wrong. */
    ESP_LOGI(TAG, "state: filled=0x%02x len=%.2f s", filled,
             (float)L / (float)SYNTH_SAMPLE_RATE);
}

/* loop.maxlen follows the cap policy (loop.mono x loop.tracks): the cap of
 * the *next* first take (once a loop exists the length is set and the cap
 * is moot). Origin Internal, so BLE pushes the change and the app's
 * "max n s" hint updates live (S19). During a fallback-capped first take
 * ctl_handle_mode overrides this with the achieved ceiling; the state
 * event at close (or cancel/clear) re-mirrors the nominal value. */
void ctl_mirror_maxlen() {
    ParamStore& ps = ParamStore::instance();
    const bool mono = ps.get(LOOP_PID_MONO) > 0.5f;
    const bool four = ps.get(LOOP_PID_TRACKMODE) > 0.5f;
    ps.set(LOOP_PID_MAXLEN,
           (float)cap_frames(mono, four) / (float)SYNTH_SAMPLE_RATE,
           ParamOrigin::Internal);
}

/* Transport telemetry (S18): loop.pos / loop.rectrk, published on the timed
 * ctl wake while the transport runs (~4 Hz; the app interpolates between
 * updates) and once more on the way to idle. Change-filtered, so a stopped
 * synth generates no BLE traffic. */
float s_last_pos = -1.0f;
int s_last_rectrk = -1;

void ctl_mirror_pos() {
    ParamStore& ps = ParamStore::instance();
    /* stop rewinds to the top; publish 0 without waiting for the audio task
     * to apply the command (the mailbox may not have been drained yet) */
    const uint32_t frames = s_ctl_mode == MODE_STOP
                                ? 0
                                : s_pos.load(std::memory_order_acquire);
    const float pos = (float)frames / (float)SYNTH_SAMPLE_RATE;
    const int live = s_rec_trk_live.load(std::memory_order_acquire);
    const int rectrk = live < 0 ? 0 : live + 1;
    if (pos != s_last_pos) {
        ps.set(LOOP_PID_POS, pos, ParamOrigin::Internal);
        s_last_pos = pos;
    }
    if (rectrk != s_last_rectrk) {
        ps.set(LOOP_PID_RECTRK, (float)rectrk, ParamOrigin::Internal);
        s_last_rectrk = rectrk;
    }
}

/* Shrink an over-allocated buffer (the first track is allocated at the
 * format's cap) down to the actual loop length. Skips a track the audio
 * task is writing; swapping under a *playing* track is safe (copy, publish,
 * handshake, free — a reader only ever sees one whole buffer). */
void ctl_compact() {
    const uint32_t L = s_loop_frames.load(std::memory_order_acquire);
    if (L == 0 || s_ctl_mode == MODE_REC) return;
    const bool mono = s_mono.load(std::memory_order_relaxed);
    const size_t slack = track_bytes_for(kCompactSlackFrames, mono);
    for (int t = 0; t < LOOP_TRACKS; ++t) {
        uint8_t* old = s_buf[t].load(std::memory_order_relaxed);
        const size_t need = track_bytes_for(L, mono);
        if (old == nullptr || s_cap_bytes[t] <= need + slack) continue;
        if (s_rec_trk_live.load(std::memory_order_acquire) == t) continue;
        uint8_t* p = (uint8_t*)heap_caps_malloc(
            need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p == nullptr) return; /* keep the big buffer; not an error */
        const size_t old_bytes = s_cap_bytes[t];
        memcpy(p, old, need);
        s_buf[t].store(p, std::memory_order_release);
        s_cap_bytes[t] = need;
        if (!ctl_handshake()) {
            /* The point of compacting is to give PSRAM back; leaking the old
             * buffer here gives none back at all, but it is still the only
             * safe answer while the audio task may hold the pointer. */
            ESP_LOGE(TAG, "track %d: compaction leaked the old buffer", t + 1);
            continue;
        }
        heap_caps_free(old);
        ESP_LOGI(TAG, "track %d compacted to %.2f s (%u KB freed)", t + 1,
                 (float)L / SYNTH_SAMPLE_RATE,
                 (unsigned)((old_bytes - need) / 1024));
    }
}

/* ---- start alignment (S24): loop.sync / loop.countin ----
 * Entering rec with either on does not start the take; it arms one. The
 * seq/arp beat callback then counts down and issues the real rec command on
 * a downbeat, so the loop's first sample lands on the grid.
 *
 * The countdown lives on loop_ctl, not in the callback: the callback runs on
 * the clock task and must stay short, and starting a take allocates. */
/* Atomic because two tasks share them: the countdown is decremented by
 * beat_cb() on the seq_clk task, and armed or cancelled by loop_ctl. As plain
 * ints, a stop pressed on the very beat the countdown expired could race the
 * decrement and either start a take the user had just cancelled or leave
 * loop.armed showing a countdown that no longer exists. */
std::atomic<int> s_arm_beats{0};   /* beats still to wait; 0 = not armed */
std::atomic<bool> s_arm_fire{false}; /* countdown done, take not started yet */
std::atomic<bool> s_arm_click{false};
std::atomic<int> s_arm_track{0};
/* Transport the arm interrupted, so a take that cannot start has somewhere
 * honest to land. Plain int: armed and read on loop_ctl, nowhere else. */
int s_arm_prev_mode = MODE_STOP;

void ctl_arm_cancel() {
    const bool was_armed =
        s_arm_beats.exchange(0, std::memory_order_acq_rel) != 0;
    /* Clearing the fire flag is what makes cancelling win a race with the
     * last beat: kFlagArmFire may already be latched in s_flags, and the
     * handler only starts a take if this flag is still set. */
    const bool was_firing = s_arm_fire.exchange(false, std::memory_order_acq_rel);
    if (!was_armed && !was_firing) return;
    ParamStore::instance().set(LOOP_PID_ARMED, 0.0f, ParamOrigin::Internal);
}

/* Clock task. Short by construction: click, decrement, and hand the start
 * over to loop_ctl. */
void beat_cb(int beat_in_bar, void*) {
    /* Compare-exchange rather than a plain decrement: if loop_ctl zeroed the
     * countdown between the load and here, the arm is gone and this beat must
     * not resurrect it. */
    int left = s_arm_beats.load(std::memory_order_acquire);
    for (;;) {
        if (left <= 0) return;
        if (s_arm_beats.compare_exchange_weak(left, left - 1,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            break;
        }
    }
    if (left - 1 <= 0) {
        /* Deliberately no click on this beat. It is the downbeat the take
         * opens on, and the four counts have already been given on the four
         * beats before it — clicking here would be counting to five, and it
         * put the last tick at sample zero of the loop. */
        s_arm_fire.store(true, std::memory_order_release);
        s_flags.fetch_or(kFlagArmFire, std::memory_order_release);
    } else {
        if (s_arm_click.load(std::memory_order_relaxed)) {
            drums_click(beat_in_bar == 0);
        }
        s_flags.fetch_or(kFlagArmTick, std::memory_order_release);
    }
    if (s_ctl_task != nullptr) xTaskNotifyGive(s_ctl_task);
}

/* Begins a take on `trk` immediately: allocation, then the audio command.
 * Split out of ctl_handle_mode so the armed path can reuse it verbatim when
 * its countdown reaches zero. Returns false if it could not start (already
 * logged and the mode reverted). */
bool ctl_start_rec(int trk) {
    ParamStore& ps = ParamStore::instance();
    const uint32_t L = s_loop_frames.load(std::memory_order_acquire);
    bool mono = s_mono.load(std::memory_order_relaxed);
    if (L == 0) {
        /* the first take latches the set format (S19) and its cap
         * (S20); audio is not touching buffers while no loop
         * exists. If the policy cap does not fit free PSRAM, halve
         * until it does (graceful fallback) — the achieved ceiling
         * is the take's limit and is mirrored so the app shows it. */
        mono = ps.get(LOOP_PID_MONO) > 0.5f;
        s_mono.store(mono, std::memory_order_release);
        const bool four = ps.get(LOOP_PID_TRACKMODE) > 0.5f;
        const uint32_t cap = cap_frames(mono, four);
        uint32_t got = cap;
        while (!ctl_alloc(trk, got, mono)) {
            if (got / 2 < kMinTakeCapFrames) {
                got = 0;
                break;
            }
            got /= 2;
        }
        if (got == 0) {
            ESP_LOGW(TAG,
                     "track %d: no PSRAM even for %.0f s %s — rec "
                     "rejected (clear tracks first)",
                     trk + 1,
                     (float)kMinTakeCapFrames / SYNTH_SAMPLE_RATE,
                     mono ? "mono" : "stereo");
            ps.set(LOOP_PID_MODE, (float)s_ctl_mode,
                   ParamOrigin::Internal);
            return false;
        }
        s_rec_limit.store(got, std::memory_order_release);
        if (got < cap) {
            ESP_LOGW(TAG,
                     "track %d: PSRAM short — this take is capped "
                     "at %.0f s (policy cap %.0f s)",
                     trk + 1, (float)got / SYNTH_SAMPLE_RATE,
                     (float)cap / SYNTH_SAMPLE_RATE);
            ps.set(LOOP_PID_MAXLEN,
                   (float)got / (float)SYNTH_SAMPLE_RATE,
                   ParamOrigin::Internal);
        }
    } else if (!ctl_alloc(trk, L, mono)) {
        ESP_LOGW(TAG,
                 "track %d: no PSRAM for %.1f s %s — rec rejected "
                 "(clear tracks or record a shorter loop)",
                 trk + 1, (float)L / SYNTH_SAMPLE_RATE,
                 mono ? "mono" : "stereo");
        ps.set(LOOP_PID_MODE, (float)s_ctl_mode, ParamOrigin::Internal);
        return false;
    }
    s_cmd.store(kCmdRec | (uint32_t)trk, std::memory_order_release);
    s_ctl_mode = MODE_REC;
    return true;
}

void ctl_handle_mode() {
    ParamStore& ps = ParamStore::instance();
    const int want = (int)(ps.get(LOOP_PID_MODE) + 0.5f);
    if (want == s_ctl_mode) return;
    /* Any transport change abandons a pending count-in. */
    if (want != MODE_REC) ctl_arm_cancel();
    switch (want) {
        case MODE_REC: {
            int trk = (int)(ps.get(LOOP_PID_TRACK) + 0.5f) - 1;
            if (trk < 0 || trk >= LOOP_TRACKS) trk = 0;
            /* Aligned start: arm rather than record. The take begins on a
             * downbeat, counted down by the seq/arp beat callback. loop.mode
             * stays at rec throughout so the transport UI reads correctly;
             * loop.armed is what says "waiting", and loop.rectrk stays 0
             * until the audio task really starts writing. */
            const bool want_count = ps.get(LOOP_PID_COUNTIN) > 0.5f;
            const bool want_sync = ps.get(LOOP_PID_SYNC) > 0.5f;
            if (want_count || want_sync) {
                s_arm_track.store(trk, std::memory_order_relaxed);
                s_arm_click.store(want_count, std::memory_order_relaxed);
                /* Count-in is four clicks and the take opens on the fifth
                 * beat, so the last tick has a whole beat to decay and the
                 * loop starts on the "1" rather than on the "4".
                 *
                 * Sync alone waits for the next downbeat. seqarp_beat_in_bar
                 * is the index the *next* beat will carry (it is bumped after
                 * the callback), so 0 means one beat away and 1 means a full
                 * bar — hence the modulo rather than a plain subtraction,
                 * which used to fire on beat 3 of 4. */
                const int bib = seqarp_beat_in_bar();
                const int beats = want_count ? 5 : (((4 - bib) % 4) + 1);
                s_arm_fire.store(false, std::memory_order_relaxed);
                s_arm_beats.store(beats, std::memory_order_release);
                s_arm_prev_mode = s_ctl_mode; /* where a failed take lands */
                s_ctl_mode = MODE_REC;
                ps.set(LOOP_PID_ARMED, (float)beats, ParamOrigin::Internal);
                ESP_LOGI(TAG, "track %d armed: %d beat(s)%s", trk + 1, beats,
                         want_count ? " (count-in)" : " (sync)");
                break;
            }
            if (!ctl_start_rec(trk)) return;
            break;
        }
        case MODE_PLAY:
            s_cmd.store(kCmdPlay, std::memory_order_release);
            s_ctl_mode = MODE_PLAY;
            break;
        case MODE_STOP:
        default:
            s_cmd.store(kCmdStop, std::memory_order_release);
            s_ctl_mode = MODE_STOP;
            break;
    }
}

void ctl_handle_clear() {
    ctl_arm_cancel();
    ParamStore& ps = ParamStore::instance();
    const int what = (int)(ps.get(LOOP_PID_CLEAR) + 0.5f);
    if (what == CLR_TRACK) {
        int trk = (int)(ps.get(LOOP_PID_TRACK) + 0.5f) - 1;
        if (trk < 0 || trk >= LOOP_TRACKS) trk = 0;
        s_filled.fetch_and((uint8_t)~(1u << trk), std::memory_order_release);
        /* buffer kept for reuse — the loop length is unchanged */
        ctl_mirror_state();
    } else if (what == CLR_ALL) {
        s_cmd.store(kCmdDetach, std::memory_order_release);
        ctl_release_all(ctl_handshake());
        s_filled.store(0, std::memory_order_release);
        s_loop_frames.store(0, std::memory_order_release);
        s_ctl_len = 0;
        s_ctl_mode = MODE_STOP;
        ps.set(LOOP_PID_MODE, (float)MODE_STOP, ParamOrigin::Internal);
        ctl_mirror_state();
        ctl_mirror_maxlen(); /* the policy cap applies again */
        ESP_LOGI(TAG, "all tracks cleared, loop length reset");
    }
    if (what != CLR_NONE) {
        ps.set(LOOP_PID_CLEAR, (float)CLR_NONE, ParamOrigin::Internal);
    }
}

#if SYNTH_ENABLE_LOOP_PERSIST

/* Shared save/load preconditions. Flash ops stall XIP code fetches for the
 * whole render chain, so that backend demands a stopped transport; a rec in
 * flight is refused on any backend (its buffer is being written). */
bool ctl_persist_allowed(const char* op) {
    if (s_ctl_mode == MODE_REC) {
        ESP_LOGW(TAG, "%s refused: finish (or cancel) the recording first", op);
        return false;
    }
    if (loop_store_needs_stopped() && s_ctl_mode != MODE_STOP) {
        ESP_LOGW(TAG,
                 "%s refused: stop the loop first (flash writes stall the "
                 "render chain)",
                 op);
        return false;
    }
    if (!loop_store_ready()) {
        ESP_LOGW(TAG, "%s refused: %s backend not available", op,
                 loop_store_backend_name());
        return false;
    }
    return true;
}

void ctl_handle_save() {
    ParamStore& ps = ParamStore::instance();
    const int slot = (int)(ps.get(LOOP_PID_SAVE) + 0.5f);
    const uint32_t L = s_loop_frames.load(std::memory_order_acquire);
    const uint8_t filled = s_filled.load(std::memory_order_acquire);
    if (L == 0 || filled == 0) {
        ESP_LOGW(TAG, "save refused: nothing recorded");
        return;
    }
    if (!ctl_persist_allowed("save")) return;
    uint8_t* bufs[LOOP_TRACKS];
    int count = 0;
    for (int t = 0; t < LOOP_TRACKS; ++t) {
        bufs[t] = s_buf[t].load(std::memory_order_relaxed);
        if (((filled >> t) & 1) != 0) {
            if (bufs[t] == nullptr) {
                ESP_LOGE(TAG, "save aborted: track %d filled but unallocated",
                         t + 1);
                return;
            }
            ++count;
        }
    }
    /* the stored codec follows the live set's format — the buffers *are*
     * that format, byte for byte (S20) */
    const bool mono = s_mono.load(std::memory_order_relaxed);
    const esp_err_t err = loop_store_save(slot, L, filled, bufs, mono);
    if (err == ESP_OK) {
        const size_t bytes = (size_t)count * track_bytes_for(L, mono);
        ESP_LOGI(TAG, "saved slot %d: %.2f s, %d track(s), %u KB %sadpcm (%s)",
                 slot, (float)L / SYNTH_SAMPLE_RATE, count,
                 (unsigned)(bytes / 1024), mono ? "mono " : "",
                 loop_store_backend_name());
    } else {
        ESP_LOGW(TAG, "save failed: %s", esp_err_to_name(err));
    }
}

void ctl_handle_load() {
    ParamStore& ps = ParamStore::instance();
    const int slot = (int)(ps.get(LOOP_PID_LOAD) + 0.5f);
    if (!ctl_persist_allowed("load")) return;
    uint32_t new_len = 0;
    uint8_t stored = 0;
    bool mono = false;
    esp_err_t err = loop_store_probe(slot, &new_len, &stored, &mono);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "load: slot %d has no valid set (%s)", slot,
                 esp_err_to_name(err));
        return;
    }
    /* replace the live set: detach audio, free, then fill from storage */
    s_cmd.store(kCmdDetach, std::memory_order_release);
    ctl_release_all(ctl_handshake());
    s_filled.store(0, std::memory_order_release);
    s_loop_frames.store(0, std::memory_order_release);
    s_mono.store(mono, std::memory_order_release); /* set format = the blob's */
    uint8_t loaded = 0;
    int packed = 0;
    for (int t = 0; t < LOOP_TRACKS; ++t) {
        if (((stored >> t) & 1) == 0) continue;
        if (!ctl_alloc(t, new_len, mono)) {
            ESP_LOGW(TAG, "load: PSRAM exhausted at track %d — partial load",
                     t + 1);
            break;
        }
        err = loop_store_read_track(slot, packed++,
                                    s_buf[t].load(std::memory_order_relaxed),
                                    new_len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "load: track %d read failed (%s)", t + 1,
                     esp_err_to_name(err));
            break;
        }
        loaded |= (uint8_t)(1u << t);
    }
    s_loop_frames.store(loaded != 0 ? new_len : 0, std::memory_order_release);
    s_filled.store(loaded, std::memory_order_release);
    s_ctl_len = loaded != 0 ? new_len : 0;
    s_ctl_mode = MODE_STOP;
    ps.set(LOOP_PID_MODE, (float)MODE_STOP, ParamOrigin::Internal);
    if (loaded != 0) {
        /* the loaded set's format becomes the live one — reflect it so the
         * app's toggle and max-length hint match what is actually playing
         * (our own writes skip the listener; mirror maxlen directly) */
        ps.set(LOOP_PID_MONO, mono ? 1.0f : 0.0f, ParamOrigin::Internal);
        ctl_mirror_maxlen();
    }
    ctl_mirror_state();
    if (loaded != 0) {
        ESP_LOGI(TAG, "loaded slot %d: %.2f s, %d %s track(s) — press play",
                 slot, (float)new_len / SYNTH_SAMPLE_RATE,
                 __builtin_popcount(loaded), mono ? "mono" : "stereo");
    }
}

#endif /* SYNTH_ENABLE_LOOP_PERSIST */

void ctl_task(void* arg) {
    (void)arg;
    ParamStore& ps = ParamStore::instance();
    for (;;) {
        /* timed wake while the transport runs — the position mirror at the
         * bottom is the only periodic work; everything else stays
         * notify-driven (and a stopped looper blocks indefinitely) */
        ulTaskNotifyTake(pdTRUE, s_ctl_mode == MODE_STOP
                                     ? portMAX_DELAY
                                     : pdMS_TO_TICKS(250));
        /* events before flags: an event is necessarily raised before any
         * command that reacts to it (causality — see the ctl_compact guard) */
        const uint32_t evt = s_evt.exchange(0, std::memory_order_acquire);
        if (evt & kEvtState) {
            const uint32_t L = s_loop_frames.load(std::memory_order_acquire);
            if (evt & kEvtAutoPlay) {
                s_ctl_mode = MODE_PLAY;
                ps.set(LOOP_PID_MODE, (float)MODE_PLAY, ParamOrigin::Internal);
            } else if (s_ctl_mode == MODE_REC) {
                /* user left rec via play/stop; shadow follows the param */
                s_ctl_mode = (int)(ps.get(LOOP_PID_MODE) + 0.5f);
            }
            ctl_mirror_state();
            /* a first take just closed or was canceled — if its cap had
             * been reduced by the alloc fallback, show the policy cap
             * again (change-free re-mirrors are cheap and human-rate) */
            ctl_mirror_maxlen();
            if (L > 0 && s_ctl_len == 0) {
                const bool mono = s_mono.load(std::memory_order_relaxed);
                ESP_LOGI(TAG, "loop closed: %.2f s (%u KB per %s track, "
                         "adpcm)",
                         (float)L / SYNTH_SAMPLE_RATE,
                         (unsigned)(track_bytes_for(L, mono) / 1024),
                         mono ? "mono" : "stereo");
            }
            s_ctl_len = L;
            ctl_compact();
        }
        const uint32_t flags = s_flags.exchange(0, std::memory_order_acquire);
        /* Transport first. A stop and the last count-in beat can land in the
         * same wake, and the user's stop has to win: ctl_handle_mode() ->
         * ctl_arm_cancel() clears s_arm_fire, so the armed-take branch below
         * finds nothing to start. Handled the other way round (as it was),
         * the take opened and was stopped again a moment later — audible as a
         * blip on a track the user had just cancelled. */
        if (flags & kFlagMode) ctl_handle_mode();
        if (flags & kFlagClear) ctl_handle_clear();
        if (flags & kFlagArmTick) {
            ParamStore::instance().set(
                LOOP_PID_ARMED,
                (float)s_arm_beats.load(std::memory_order_acquire),
                ParamOrigin::Internal);
        }
        if ((flags & kFlagArmFire) &&
            s_arm_fire.exchange(false, std::memory_order_acq_rel)) {
            ParamStore::instance().set(LOOP_PID_ARMED, 0.0f,
                                       ParamOrigin::Internal);
            /* The countdown ran on the clock task; the take starts here,
             * where allocating is allowed. */
            if (!ctl_start_rec(s_arm_track.load(std::memory_order_acquire))) {
                /* ctl_start_rec's own revert writes s_ctl_mode back into
                 * loop.mode — and arming had already set that to rec, so the
                 * revert was a no-op and the looper was left claiming to
                 * record with no take open: rec pressed again did nothing
                 * (want == s_ctl_mode), compaction stayed disabled, and
                 * save/load answered "finish the recording first". Fall back
                 * to the transport the arm interrupted. No command is issued:
                 * the audio task was never told to record, so it is still in
                 * exactly that state and only the shadow and the parameter
                 * need to agree with it again. */
                ESP_LOGW(TAG, "armed take could not start — back to %s",
                         kModeNames[s_arm_prev_mode]);
                s_ctl_mode = s_arm_prev_mode;
                ps.set(LOOP_PID_MODE, (float)s_ctl_mode, ParamOrigin::Internal);
            }
        }
        if (flags & kFlagMono) ctl_mirror_maxlen();
#if SYNTH_ENABLE_LOOP_PERSIST
        if (flags & kFlagSave) ctl_handle_save();
        if (flags & kFlagLoad) ctl_handle_load();
#endif
        ctl_mirror_pos();
    }
}

/* Any control task; must stay short — set a flag and wake loop_ctl. Our own
 * reflect writes run on loop_ctl itself and are filtered by task handle. */
void param_listener(uint16_t id, float value, ParamOrigin origin, void*) {
    (void)value;
    (void)origin;
    if (xTaskGetCurrentTaskHandle() == s_ctl_task) return;
    uint32_t bit = 0;
    if (id == LOOP_PID_MODE) {
        bit = kFlagMode;
    } else if (id == LOOP_PID_CLEAR) {
        bit = kFlagClear;
    } else if (id == LOOP_PID_MONO || id == LOOP_PID_TRACKMODE) {
        bit = kFlagMono; /* only re-mirrors loop.maxlen — format and cap
                          * are latched when the next first take starts */
#if SYNTH_ENABLE_LOOP_PERSIST
    } else if (id == LOOP_PID_SAVE) {
        bit = kFlagSave;
    } else if (id == LOOP_PID_LOAD) {
        bit = kFlagLoad;
#endif
    } else {
        return;
    }
    s_flags.fetch_or(bit, std::memory_order_release);
    if (s_ctl_task != nullptr) xTaskNotifyGive(s_ctl_task);
}

} // namespace

extern "C" esp_err_t looper_init(void) {
    ParamStore& ps = ParamStore::instance();
    size_t params = kParamCount;
    if (ps.add(kParams, kParamCount) != kParamCount) {
        ESP_LOGE(TAG, "param registration failed");
        return ESP_FAIL;
    }
#if SYNTH_ENABLE_LOOP_PERSIST
    if (ps.add(kPersistParams, kPersistParamCount) != kPersistParamCount) {
        ESP_LOGE(TAG, "persist param registration failed");
        return ESP_FAIL;
    }
    params += kPersistParamCount;
    ESP_ERROR_CHECK(loop_store_init()); /* never fails; logs its backend */
#endif
    for (int t = 0; t < LOOP_TRACKS; ++t) {
        s_lvl[t] = ps.valuePtr((uint16_t)LOOP_PID_LEVEL(t));
        s_buf[t].store(nullptr, std::memory_order_relaxed);
        s_cap_bytes[t] = 0;
    }
    if (xTaskCreatePinnedToCore(ctl_task, "loop_ctl", kCtlTaskStack, nullptr,
                                kCtlTaskPrio, &s_ctl_task, 0) != pdPASS) {
        return ESP_FAIL;
    }
    if (ps.addListener(param_listener, nullptr) < 0) return ESP_FAIL;
    /* loop.mono and loop.tracks default on, so the live cap out of the box is
     * not loop.maxlen's registered default (the base cap — what clients
     * multiply per enabled toggle). Mirror once so the value a fresh app read
     * gets is the real one, without waiting for a toggle. */
    ctl_mirror_maxlen();
    /* The seq/arp clock drives loop.sync and loop.countin. seqarp_init() runs
     * before this (main.cpp), so the subscription is live from here on. */
    seqarp_set_beat_callback(beat_cb, nullptr);
    ESP_LOGI(TAG,
             "up: %d tracks, adpcm in PSRAM (~%u KB/s stereo, half mono), "
             "cap %.0f s x2 mono x2 4-track (<= %.0f s), free PSRAM %u KB "
             "(largest block %u KB), %u params, punch-in at loop start%s",
             LOOP_TRACKS, (unsigned)(SYNTH_SAMPLE_RATE / 1024),
             kLoopBaseCapS, kLoopAbsMaxS,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) /
                        1024),
             (unsigned)params,
             SYNTH_ENABLE_LOOP_PERSIST ? ", save/load enabled" : "");
    return ESP_OK;
}

extern "C" void SYNTH_RENDER_IRAM looper_process(float* __restrict__ l,
                                                 float* __restrict__ r,
                                                 size_t frames) {
    const uint32_t cmd = s_cmd.exchange(0, std::memory_order_acq_rel);
    if (cmd != 0) audio_apply_cmd(cmd);

    uint32_t L = s_loop_frames.load(std::memory_order_relaxed);
    if (!a_playing || (L == 0 && !a_rec)) {
        /* idle, or "playing" with nothing recorded yet */
        s_pos.store(a_pos, std::memory_order_relaxed);
        s_render_seq.fetch_add(1, std::memory_order_release);
        return;
    }

    /* set format (S19): stable while buffers are attached — loop_ctl only
     * flips it with the set empty or the audio detached */
    const bool mono = s_mono.load(std::memory_order_relaxed);

    size_t i = 0;
    while (i < frames) {
        const uint32_t limit =
            (L > 0) ? L : s_rec_limit.load(std::memory_order_relaxed);
        size_t n = frames - i;
        if ((uint32_t)n > limit - a_pos) n = limit - a_pos;

        /* record tap first: the live synth only, before playback is mixed —
         * encoded straight into the ADPCM track (S20) */
        if (a_rec) {
            uint8_t* tb = s_buf[a_rec_trk].load(std::memory_order_acquire);
            if (tb != nullptr) {
                if (mono) {
                    /* fold the stereo bus; (l+r)/2 never exceeds the wider
                     * channel, so the fold cannot clip on its own. Nibble
                     * placement follows the absolute frame position, so odd
                     * loop lengths stay consistent across passes. */
                    for (size_t k = 0; k < n; ++k) {
                        const uint8_t nib = osynth::adpcm::encode(
                            a_enc[0], f2i16(0.5f * (l[i + k] + r[i + k])));
                        const uint32_t p = a_pos + (uint32_t)k;
                        if ((p & 1u) == 0) {
                            tb[p >> 1] = (uint8_t)(nib << 4);
                        } else {
                            tb[p >> 1] |= nib;
                        }
                    }
                } else {
                    uint8_t* w = tb + a_pos;
                    for (size_t k = 0; k < n; ++k) {
                        const uint8_t nl =
                            osynth::adpcm::encode(a_enc[0], f2i16(l[i + k]));
                        const uint8_t nr =
                            osynth::adpcm::encode(a_enc[1], f2i16(r[i + k]));
                        *w++ = (uint8_t)((nl << 4) | nr);
                    }
                }
            }
        }

        if (L > 0) {
            const uint8_t mask = s_filled.load(std::memory_order_acquire);
            for (int t = 0; t < LOOP_TRACKS; ++t) {
                if ((mask & (1u << t)) == 0) continue;
                if (a_rec && t == a_rec_trk) continue;
                const uint8_t* tb = s_buf[t].load(std::memory_order_acquire);
                if (tb == nullptr) continue;
                const float g = s_lvl[t]->load(std::memory_order_relaxed) *
                                (1.0f / 32768.0f);
                /* no level-0 skip: the decoder must stay in lock-step with
                 * the transport — ADPCM cannot re-enter mid-stream */
                if (mono) {
                    osynth::adpcm::Ch& c = a_dec[t][0];
                    for (size_t k = 0; k < n; ++k) {
                        const uint32_t p = a_pos + (uint32_t)k;
                        const uint8_t b = tb[p >> 1];
                        const uint8_t nib =
                            (p & 1u) ? (uint8_t)(b & 0x0F) : (uint8_t)(b >> 4);
                        const float s =
                            (float)osynth::adpcm::decode(c, nib) * g;
                        l[i + k] += s;
                        r[i + k] += s;
                    }
                } else {
                    osynth::adpcm::Ch& cl = a_dec[t][0];
                    osynth::adpcm::Ch& cr = a_dec[t][1];
                    const uint8_t* p8 = tb + a_pos;
                    for (size_t k = 0; k < n; ++k) {
                        const uint8_t b = *p8++;
                        l[i + k] += (float)osynth::adpcm::decode(
                                        cl, (uint8_t)(b >> 4)) * g;
                        r[i + k] += (float)osynth::adpcm::decode(
                                        cr, (uint8_t)(b & 0x0F)) * g;
                    }
                }
            }
        }

        a_pos += (uint32_t)n;
        i += n;

        if (a_pos >= limit) {
            if (L > 0) {
                a_pos = 0;
                audio_reset_dec(); /* every decoder re-enters at the top */
                if (a_rec) {
                    /* punch-in completed one full pass */
                    a_rec = false;
                    s_filled.fetch_or((uint8_t)(1u << a_rec_trk),
                                      std::memory_order_release);
                    s_rec_trk_live.store(-1, std::memory_order_release);
                    audio_raise(kEvtState | kEvtAutoPlay);
                }
                if (a_rec_pending) {
                    a_rec_pending = false;
                    a_rec = true;
                    audio_reset_enc(); /* the punch take starts at frame 0 */
                    s_rec_trk_live.store(a_rec_trk, std::memory_order_release);
                }
            } else {
                /* first recording hit the cap: close the loop here */
                s_loop_frames.store(limit, std::memory_order_release);
                s_filled.fetch_or((uint8_t)(1u << a_rec_trk),
                                  std::memory_order_release);
                a_rec = false;
                a_pos = 0;
                audio_reset_dec();
                s_rec_trk_live.store(-1, std::memory_order_release);
                audio_raise(kEvtState | kEvtAutoPlay);
                L = limit;
            }
        }
    }
    s_pos.store(a_pos, std::memory_order_relaxed);
    s_render_seq.fetch_add(1, std::memory_order_release);
}

#else /* !CONFIG_SPIRAM — classic ESP32: no RAM for even one stereo second */

extern "C" esp_err_t looper_init(void) {
    ESP_LOGI(TAG, "unavailable on this target (needs PSRAM)");
    return ESP_OK;
}

extern "C" void looper_process(float* l, float* r, size_t frames) {
    (void)l;
    (void)r;
    (void)frames;
}

#endif
