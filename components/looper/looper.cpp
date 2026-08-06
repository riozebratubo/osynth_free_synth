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
 *    playing until the punch lands. Stopping before the pass completes keeps
 *    the take and makes the rest of the pass silence (S32, ctl_pad_take):
 *    a stab or a fill over a long loop is a normal thing to want, and the
 *    alternative was throwing it away. Leaving rec via *play* still cancels
 *    the take — that one is a decoder-state constraint, not a policy; see
 *    audio_close_rec().
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
 * yet; a base cap sized from free PSRAM at init, x2 mono x2 4-track — see
 * the cap-policy comment below) and compacted to the actual
 * loop length once it closes (copy + pointer swap + render handshake +
 * free). A first take whose cap does not fit free PSRAM halves it until
 * it fits (the achieved ceiling is mirrored via loop.maxlen). Buffers are
 * PSRAM-only — no internal-RAM fallback (unlike the FX lines): a
 * loop-length buffer would eat the heap the voice pool and BLE live in.
 *
 * Streamed sets (S31, loop.store = sd) change where the bytes live and
 * nothing else: the transport, the punch-in semantics and the ADPCM format
 * are identical, and the render path picks the source per block off
 * s_streamed. What differs is that a track's bytes arrive through a ring
 * that a third task fills (loop_stream.h), so the render path can be *late*
 * rather than merely allocated-or-not. Two rules follow, and most of the
 * streamed code here exists to keep them:
 *  - A track whose window is dry mutes until the next loop start. ADPCM has
 *    no mid-stream re-entry, so playing the bytes that did arrive would
 *    leave the decoder wrong for the rest of the pass.
 *  - A take whose record ring overran is discarded whole. A loop with a
 *    hole in it is worse than no loop, and the user is standing there able
 *    to play it again.
 * Neither state is reachable in PSRAM mode, where a buffer is either there
 * or the take was refused up front.
 */
#include "looper.h"

#include "drums.h"
#include "seqarp.h"

#include <atomic>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "loop_adpcm.h"
#include "loop_store.h"
#include "loop_stream.h"
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

/* Cap policy (S20, sized from the free PSRAM pool since the P4 target): a
 * base cap for stereo/8-track mode, x2 for mono (loop.mono halves the bytes
 * per frame), x2 in 4-track mode (loop.tracks trades slots for length) — so
 * the absolute ceiling is 4x the base, and all three modes commit the same
 * total bytes for a full set (8 x base, or 4 x 2x base, ...).
 *
 * The base used to be a flat 40 s, chosen for an 8 MB S3: a full set at cap
 * is 8 x 40 s x 48 kB/s = ~15.4 MB against a ~7 MB free pool. That ~2x
 * over-commit was deliberate and is the thing worth preserving — most sets
 * are not eight tracks at the ceiling, and the first-take fallback below
 * halves gracefully for the ones that are. So rather than a seconds-per-MB
 * constant fitted to one board, the policy is stated directly:
 *
 *     a full track set at cap may commit kLoopCommitRatio x the free pool
 *
 * which rearranges to the base cap below. It reproduces ~38 s on the S3's
 * ~7 MB (i.e. today's behaviour), gives a 32 MB P4 board its clamped 160 s
 * base — 640 s at the mono/4-track ceiling — and gives a 2 MB quad-PSRAM S3
 * an honest ~10 s instead of advertising 40 s and then falling back to it
 * with a "PSRAM short" warning on the first take.
 *
 * Free, not installed: by the time looper_init() runs, main.cpp has already
 * brought up the FX buffers, the drum kit and the modular graph pool, so
 * what is left is what the looper can actually have. A build with the graph
 * compiled out therefore offers a longer loop, which is correct rather than
 * surprising.
 *
 * The clamp exists because neither end extrapolates forever: below the floor
 * a loop pedal stops being useful, and above the ceiling a single track no
 * longer fits any flash persistence region worth having. */
constexpr float kLoopCommitRatio = 2.0f;
constexpr float kLoopBaseCapMinS = 10.0f;
constexpr float kLoopBaseCapMaxS = 160.0f; /* reached around 30 MB free */
/* Widest ceiling the policy can ever produce. Only the static kParams table
 * uses it — a ParamDesc initialiser needs a constant, and registration then
 * narrows the three seconds-valued entries to this board's real numbers. */
constexpr float kLoopAbsMaxCeilS = kLoopBaseCapMaxS * 4.0f;

/* Live base cap. Constant-initialised to the historical value so it is valid
 * during static init (s_rec_limit below seeds itself from cap_frames), then
 * replaced by looper_init() once the heap can be measured. */
float s_base_cap_s = 40.0f;

inline float loop_abs_max_s() { return s_base_cap_s * 4.0f; }

constexpr uint32_t kMinFrames = SYNTH_SAMPLE_RATE / 4; /* 0.25 s */
constexpr uint32_t kMinTakeCapFrames =
    SYNTH_SAMPLE_RATE * 5; /* fallback floor: give up below 5 s */
constexpr uint32_t kCompactSlackFrames = 65536; /* compact if > 64 KB
                                                 * (stereo) / 32 KB (mono)
                                                 * of waste */

inline uint32_t cap_frames(bool mono, bool four_tracks) {
    return (uint32_t)(s_base_cap_s * SYNTH_SAMPLE_RATE) *
           (mono ? 2u : 1u) * (four_tracks ? 2u : 1u);
}

/* Base cap in seconds for `free_bytes` of PSRAM, clamped. One full set at
 * cap is LOOP_TRACKS x base x SYNTH_SAMPLE_RATE bytes (stereo ADPCM is
 * 1 B/frame, and the mono and 4-track modes trade one factor for the other,
 * so the total is the same in all three); allowing that to reach
 * kLoopCommitRatio x the pool and solving for base gives this. */
float base_cap_for_pool(size_t free_bytes) {
    float s = kLoopCommitRatio * (float)free_bytes /
              ((float)LOOP_TRACKS * (float)SYNTH_SAMPLE_RATE);
    /* Whole seconds: the app prints this with no decimals, and a cap of
     * 38.23 s displayed as "max loop 38 s" would be a promise the firmware
     * does not keep. Floor, so the number shown is always achievable. */
    s = (float)(int)s;
    if (s < kLoopBaseCapMinS) return kLoopBaseCapMinS;
    if (s > kLoopBaseCapMaxS) return kLoopBaseCapMaxS;
    return s;
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
#if SYNTH_LOOP_STREAM
const char* const kStoreNames[] = {"psram", "sd"};
#endif

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
     0.0f, kLoopAbsMaxCeilS, 0.0f, nullptr, 0}, /* read-only status, seconds;
                                                 * ranged for the ceiling —
                                                 * loop.maxlen is the live
                                                 * cap. Narrowed to this
                                                 * board's ceiling by
                                                 * register_params(). */
    {LOOP_PID_POS, "loop.pos", ParamType::Float, ParamCurve::Linear,
     0.0f, kLoopAbsMaxCeilS, 0.0f, nullptr, 0}, /* read-only status, seconds */
    {LOOP_PID_RECTRK, "loop.rectrk", ParamType::Int, ParamCurve::Linear,
     0.0f, (float)LOOP_TRACKS, 0.0f, nullptr, 0}, /* read-only, 0 = none */
    {LOOP_PID_MONO, "loop.mono", ParamType::Enum, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, kMonoNames, 2}, /* format of the *next* loop set;
                                        * defaults on — length beats the
                                        * stereo image for a loop pedal */
    {LOOP_PID_MAXLEN, "loop.maxlen", ParamType::Float, ParamCurve::Linear,
     0.0f, kLoopAbsMaxCeilS, kLoopBaseCapMaxS, nullptr, 0},
                                                     /* read-only live cap;
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

#if SYNTH_LOOP_STREAM
/* Storage backend for the next set (S31). Defaults to psram: it needs no
 * card, has no real-time dependency on one, and is what every existing
 * session expects — sd is the deliberate choice for a long take. */
const ParamDesc kStoreParams[] = {
    {LOOP_PID_STORE, "loop.store", ParamType::Enum, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, kStoreNames, 2},
};
constexpr size_t kStoreParamCount = 1;
#endif

/* Widest value loop.len / loop.pos / loop.maxlen can take on this build.
 * The streamed backend is bounded by the card, not the pool, so its ceiling
 * is the policy limit in loop_stream.h rather than 4x the PSRAM base cap —
 * and the params have to be registered wide enough for it or a long streamed
 * loop would report a clamped position. */
inline float loop_param_max_s() {
#if SYNTH_LOOP_STREAM
    const float sd = LOOP_STREAM_MAX_S;
    return loop_abs_max_s() > sd ? loop_abs_max_s() : sd;
#else
    return loop_abs_max_s();
#endif
}

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
/* Live set's storage (S31). Latched by loop_ctl alongside s_mono, under the
 * same rule: only with the set empty or the audio detached. The audio task
 * reads it once per block to pick the track source. */
std::atomic<bool> s_streamed{false};
/* Audio -> loop_ctl: the record ring overflowed, so the take on the card has
 * a hole in it and must be discarded at close. Set by the audio task, read
 * and cleared by loop_ctl. */
std::atomic<bool> s_stream_overrun{false};

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
constexpr uint32_t kEvtPad = 1u << 2;      /* punch-in stopped: tail to write */
std::atomic<uint32_t> s_evt{0};

/* Handover for kEvtPad (S32). A punch-in the transport stops keeps what it
 * recorded and the rest of its pass becomes silence — but "the rest" can be
 * minutes of samples, so the audio task only says where it stopped and in
 * what encoder state, and loop_ctl writes it. Plain fields: the audio task
 * fills them before s_evt's release fetch_or and loop_ctl reads them after
 * the matching acquire exchange, which is the same handover every other
 * event payload here uses. */
int s_pad_trk = -1;   /* -1 = nothing to pad */
uint32_t s_pad_from = 0;
osynth::adpcm::Ch s_pad_enc[2];

/* flag bits: listener -> loop_ctl */
constexpr uint32_t kFlagMode = 1u << 0;
constexpr uint32_t kFlagClear = 1u << 1;
constexpr uint32_t kFlagSave = 1u << 2;
constexpr uint32_t kFlagLoad = 1u << 3;
constexpr uint32_t kFlagMono = 1u << 4;
constexpr uint32_t kFlagArmTick = 1u << 5; /* a count-in beat passed */
constexpr uint32_t kFlagArmFire = 1u << 6; /* countdown done: start rec */
constexpr uint32_t kFlagExport = 1u << 7;  /* a track read is waiting (S33) */
std::atomic<uint32_t> s_flags{0};

TaskHandle_t s_ctl_task = nullptr;

/* ---- export handover (S33): any task -> loop_ctl -> back ----
 *
 * A track's bytes are reachable only from loop_ctl (looper.h says why), and
 * the caller is a BLE command task that wants a couple of kilobytes and an
 * answer. So the request travels as one struct through the flag/notify path
 * every other control message uses, and the caller blocks on s_export_done
 * until loop_ctl has filled it in.
 *
 * s_export_mutex is what makes a single struct enough: it is held across the
 * whole round trip, so there is never more than one request in flight and
 * loop_ctl can write its results straight back into it. The wait is
 * deliberately untimed — a request that timed out would leave loop_ctl about
 * to write into a buffer its caller had moved on from, and the honest fix for
 * that is not to have two. loop_ctl always answers: ctl_handle_export() gives
 * the semaphore on every path, and its longest single operation (a save) is
 * seconds, not forever. */
struct ExportReq {
    bool read;      /* false: info; true: read */
    int source;     /* LOOPER_EXPORT_* */
    int slot;
    int track;      /* read only, 0-based */
    uint32_t offset;
    uint32_t len;
    uint8_t* dst;
    looper_export_info_t* info;
    uint32_t got;   /* out: bytes actually read */
    esp_err_t err;  /* out */
};
ExportReq s_export_req{};
SemaphoreHandle_t s_export_mutex = nullptr; /* one caller at a time */
SemaphoreHandle_t s_export_done = nullptr;  /* loop_ctl -> that caller */

/* ---- audio-task-only transport state ---- */

bool a_playing = false;
bool a_rec = false;
bool a_rec_pending = false;
int a_rec_trk = 0;
uint32_t a_pos = 0;

/* Streamed-mode transport state (audio task only).
 *  a_win_off — bytes of the current pass already released from the track's
 *              window. Every non-starved track holds the same value; a
 *              starved one stops advancing, which is exactly why it cannot
 *              rejoin before the wrap.
 *  a_starved — tracks muted for the rest of this pass because their window
 *              ran dry. ADPCM has no mid-stream re-entry (loop_adpcm.h), so
 *              muting to the wrap is the only honest recovery. */
#if SYNTH_LOOP_STREAM
uint32_t a_win_off[LOOP_TRACKS] = {};
uint8_t a_starved = 0;
#endif

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

/* Streamed mode: forget where the windows were. Called on every transport
 * jump to the top that is *not* a natural wrap (stop, detach, a take
 * starting). Those all come from loop_ctl, which re-primes the windows with
 * loop_stream_rewind() — the audio task only has to stop trusting the
 * offsets it had. The starved mask is cleared rather than set: after a
 * rewind the windows are at the pass start, which is exactly where a_pos is,
 * so there is nothing to sit out. */
inline void audio_reset_windows() {
#if SYNTH_LOOP_STREAM
    for (int t = 0; t < LOOP_TRACKS; ++t) a_win_off[t] = 0;
    a_starved = 0;
#endif
}

/* Streamed mode: mute track `t` and hand its window back to loop_io to be
 * re-primed from the loop start. Called wherever a take has just made the
 * window meaningless — the file behind it is about to be replaced, or the
 * transport advanced a whole pass without consuming it because the track was
 * the one being recorded. loop_ctl re-opens the file (loop_stream_add_track)
 * and clears the bit; the track rejoins at the next wrap, where every
 * decoder resets anyway.
 *
 * This is deliberately not an underrun: nothing was late. Counting it as one
 * would make every punch-in look like a card that cannot keep up. */
inline void audio_hold_track(int t) {
#if SYNTH_LOOP_STREAM
    if (!s_streamed.load(std::memory_order_relaxed)) return;
    const uint8_t bit = (uint8_t)(1u << t);
    /* a_starved doubles as "already held": the render path calls this on the
     * recording track every block, and an unconditional atomic RMW there
     * would be a per-block cost for a state that changes once per take. */
    if ((a_starved & bit) != 0) return;
    a_starved |= bit;
    osynth::loopstream::g_hold.fetch_or(bit, std::memory_order_release);
#else
    (void)t;
#endif
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

/* Rec teardown when a transport command interrupts it (audio task).
 *
 * `keep_partial` decides what happens to a punch-in that has not finished its
 * pass: kept and padded out with silence (S32), or discarded as it always
 * was. Only stop passes true, and the reason is not policy but decoder state.
 * Stop rewinds to the top and resets every decoder, so a track brought into
 * the mix afterwards is read from frame 0 with a decoder that starts there.
 * Play leaves the transport where it is: turning a track's filled bit on
 * mid-pass would decode the rest of that pass with a decoder still holding
 * the state it had at the last wrap — noise, not the take. */
void audio_close_rec(bool keep_partial) {
    const uint32_t L = s_loop_frames.load(std::memory_order_relaxed);
    if (L == 0 && a_rec) {
        /* first recording: leaving rec closes the loop (or discards a
         * too-short take); either way the loop restarts at the top */
        const bool kept = a_pos >= kMinFrames;
        if (kept) {
            s_loop_frames.store(a_pos, std::memory_order_release);
            s_filled.fetch_or((uint8_t)(1u << a_rec_trk),
                              std::memory_order_release);
        }
        a_pos = 0;
        audio_reset_dec();
        audio_reset_windows();
        /* After the window reset, which clears every hold: this track now has
         * a file on the card but no reader until loop_ctl opens one. */
        if (kept) audio_hold_track(a_rec_trk);
        audio_raise(kEvtState);
    } else if (L > 0 && a_rec) {
        /* Punch-in ended by the transport. The take stands: it keeps what was
         * played into it and the rest of the pass becomes silence, which
         * loop_ctl writes (see ctl_pad_take — the remainder is far too much
         * to encode here). Until it has, the track's filled bit stays clear,
         * and that is also what keeps a half-written track out of the mix. */
        s_filled.fetch_and((uint8_t)~(1u << a_rec_trk),
                           std::memory_order_release);
        if (keep_partial && a_pos > 0) {
            s_pad_trk = a_rec_trk;
            s_pad_from = a_pos;
            s_pad_enc[0] = a_enc[0];
            s_pad_enc[1] = a_enc[1];
            audio_raise(kEvtState | kEvtPad);
        } else {
            /* Nothing landed at all, or the transport is not rewinding (see
             * above): either way the track goes back to empty. */
            audio_raise(kEvtState);
        }
    }
    a_rec = false;
    a_rec_pending = false;
    s_rec_trk_live.store(-1, std::memory_order_release);
}

void audio_apply_cmd(uint32_t cmd) {
    switch (cmd & 0xFF00u) {
        case kCmdStop:
            audio_close_rec(true); /* the take stands; loop_ctl pads its tail */
            a_playing = false;
            a_pos = 0;
            audio_reset_dec();
            audio_reset_windows();
            break;
        case kCmdPlay:
            audio_close_rec(false);
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
                audio_reset_windows();
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
            audio_reset_windows();
            s_rec_trk_live.store(-1, std::memory_order_release);
            break;
        default:
            break;
    }
}

/* ---- loop_ctl task: allocation, params mirror, clears ---- */

/* How often a stopped looper wakes to ask whether the card is still there.
 * Slow on purpose: it is the cost of having no card-detect pin, and a second
 * either way is imperceptible against pulling a card out of a socket. */
constexpr uint32_t kCardPollMs = 1000;

int s_ctl_mode = MODE_STOP; /* last transport we commanded */
uint32_t s_ctl_len = 0;     /* last loop length we saw (for the close log) */
/* Track the open take is on. s_rec_trk_live is already -1 by the time the
 * close event reaches us, and the streamed backend still has a file to
 * finish — this is the only record of which one. -1 = no take open. */
int s_ctl_rec_trk = -1;

#if SYNTH_LOOP_STREAM
/* Would the *next* set stream? loop.store is a policy for the next set, like
 * loop.mono; the live set's answer is s_streamed. Availability is re-checked
 * here rather than cached: the card can be pulled between sets. */
bool ctl_want_streamed() {
    return ParamStore::instance().get(LOOP_PID_STORE) > 0.5f &&
           loop_stream_ready();
}
#endif

/* Recording ceiling in frames for a first take. The PSRAM cap comes from the
 * pool (cap_frames); a streamed take is bounded by the card, so it gets the
 * policy limit from loop_stream.h instead. `streamed` is passed rather than
 * re-derived: the caller either knows the live set's answer (s_streamed) or
 * is predicting the next one (ctl_want_streamed), and those differ while a
 * set is open. */
uint32_t ctl_take_cap(bool mono, bool four, bool streamed) {
#if SYNTH_LOOP_STREAM
    if (streamed) {
        return (uint32_t)(LOOP_STREAM_MAX_S * (float)SYNTH_SAMPLE_RATE);
    }
#else
    (void)streamed;
#endif
    return cap_frames(mono, four);
}

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
#if SYNTH_LOOP_STREAM
    /* Here rather than at the handful of call sites, because "the shape of
     * the set changed" and "mirror the shape of the set" are the same event,
     * and a manifest that misses one of those sites is worse than none: it
     * would describe a set that no longer exists. Costs one small file write
     * at human rate, and only for a set that lives on the card. */
    if (s_streamed.load(std::memory_order_relaxed)) {
        loop_stream_save_manifest(L, s_mono.load(std::memory_order_relaxed),
                                  filled);
    }
#endif
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
    /* In sd mode the mono/4-track doublings do not move this: the cap is the
     * card's, not the pool's. They still decide the format, so the toggles
     * stay meaningful — they just stop being length policy. */
#if SYNTH_LOOP_STREAM
    const bool streamed = ctl_want_streamed();
#else
    const bool streamed = false;
#endif
    ps.set(LOOP_PID_MAXLEN,
           (float)ctl_take_cap(mono, four, streamed) / (float)SYNTH_SAMPLE_RATE,
           ParamOrigin::Internal);
}

/* Transport telemetry (S18): loop.pos / loop.rectrk, published on the timed
 * ctl wake while the transport runs (~4 Hz; the app interpolates between
 * updates) and once more on the way to idle. Change-filtered, so a stopped
 * synth generates no BLE traffic. */
float s_last_pos = -1.0f;
int s_last_rectrk = -1;
#if SYNTH_LOOP_STREAM
uint32_t s_last_underruns = 0;
#endif

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
#if SYNTH_LOOP_STREAM
    /* Nothing to compact: a streamed set owns no per-track buffer, only the
     * fixed windows, and those are sized for throughput rather than length. */
    if (s_streamed.load(std::memory_order_relaxed)) return;
#endif
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
#if SYNTH_LOOP_STREAM
    if (L == 0) {
        /* The first take latches the storage the same way it latches the
         * format — the set is empty, so nothing has to be moved. A card that
         * has gone away since the toggle was flipped simply lands back on
         * psram rather than failing the take. */
        const bool asked = ps.get(LOOP_PID_STORE) > 0.5f;
        /* Probe, not the cached answer: a card inserted since the last set
         * should work, and one removed must not be recorded to. */
        bool want = asked && loop_stream_probe_card();
        if (asked && !want) {
            ESP_LOGW(TAG, "no card for a streamed set — recording to PSRAM");
        }
        if (want) {
            mono = ps.get(LOOP_PID_MONO) > 0.5f;
            s_mono.store(mono, std::memory_order_release);
            if (loop_stream_begin_set(mono) != ESP_OK) {
                ESP_LOGW(TAG, "sd set could not be opened — recording to PSRAM");
                want = false;
            }
        }
        s_streamed.store(want, std::memory_order_release);
        if (asked && !want) {
            /* Say so in the parameter, not just the log: the app's switch is
             * a promise about where this set is going, and it has just been
             * broken. Origin Internal, so it reaches the app as an event and
             * the listener's task filter keeps it from looping back here. */
            ps.set(LOOP_PID_STORE, 0.0f, ParamOrigin::Internal);
            ctl_mirror_maxlen(); /* the PSRAM cap applies after all */
        }
    }
    if (s_streamed.load(std::memory_order_relaxed)) {
        /* No allocation at all: the take streams to the card and playback
         * comes back through the fixed windows. */
        s_stream_overrun.store(false, std::memory_order_release);
        if (loop_stream_open_record(trk, L) != ESP_OK) {
            ESP_LOGW(TAG, "track %d: sd take could not be opened — rec rejected",
                     trk + 1);
            ps.set(LOOP_PID_MODE, (float)s_ctl_mode, ParamOrigin::Internal);
            return false;
        }
        if (L == 0) {
            s_rec_limit.store(ctl_take_cap(mono, false, true),
                              std::memory_order_release);
        }
        s_ctl_rec_trk = trk;
        s_cmd.store(kCmdRec | (uint32_t)trk, std::memory_order_release);
        s_ctl_mode = MODE_REC;
        return true;
    }
#endif
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
    s_ctl_rec_trk = trk;
    s_cmd.store(kCmdRec | (uint32_t)trk, std::memory_order_release);
    s_ctl_mode = MODE_REC;
    return true;
}

#if SYNTH_LOOP_STREAM
/* Finishes a streamed take once the audio task has raised the close event.
 * The audio task has already decided whether the take counts (it sets or
 * clears the track's filled bit); this makes the card agree with that, and
 * overrides it when the record ring overflowed — a take with a hole in it is
 * not a take, however complete the transport thought it was. */
void ctl_finish_streamed_take() {
    if (s_ctl_rec_trk < 0) return;
    const int trk = s_ctl_rec_trk;
    s_ctl_rec_trk = -1;
    const uint8_t filled = s_filled.load(std::memory_order_acquire);
    bool keep = ((filled >> trk) & 1) != 0;
    if (s_stream_overrun.exchange(false, std::memory_order_acq_rel)) {
        ESP_LOGW(TAG,
                 "track %d discarded: the card could not keep up with the "
                 "take (record ring overran)",
                 trk + 1);
        keep = false;
    }
    if (loop_stream_close_record(trk, keep) != ESP_OK && keep) {
        ESP_LOGW(TAG, "track %d: the take did not reach the card", trk + 1);
        keep = false;
    }
    if (!keep) {
        s_filled.fetch_and((uint8_t)~(1u << trk), std::memory_order_release);
    }
    if (!keep) {
        /* The take is gone and the track is empty, but its hold is still on
         * (the audio task set it when the pass closed). Nothing will lift it
         * otherwise, and a later punch-in on this track would find it muted
         * before it ever started. */
        loop_stream_release_track(trk);
        return;
    }
    /* Give the new track a reader. Only this track is touched — the others
     * are mid-pass with the audio task reading their windows, and resetting
     * a ring under a live reader is exactly what loop_stream_add_track()'s
     * resync handshake exists to avoid. The first take also gets here, and
     * it is what decides the loop length. */
    const uint32_t L = s_loop_frames.load(std::memory_order_acquire);
    if (L == 0) return;
    loop_stream_set_length(L);
    if (loop_stream_add_track(trk) != ESP_OK) {
        ESP_LOGW(TAG, "track %d: recorded but could not be re-opened to play",
                 trk + 1);
        s_filled.fetch_and((uint8_t)~(1u << trk), std::memory_order_release);
    }
}
#endif

/* ---- the tail of a punch-in the transport stopped (S32) ----
 *
 * Stopping mid-pass used to throw the take away, on the grounds that a
 * partially overwritten track is not a track. Keeping it is the more useful
 * answer — a stab, a fill, one phrase over a long loop — and what it needs is
 * the rest of the pass to be silence.
 *
 * Silence in IMA-ADPCM is not a byte value: the decoder is a predictor, and a
 * run of zero nibbles from an arbitrary state walks it to a frozen non-zero
 * sample, i.e. DC, not quiet. It *is* silence from one state — predictor 0,
 * step index 0 — where the zero nibble decodes to zero and leaves the state
 * alone. So the tail here encodes real samples until the encoder reaches that
 * fixed point, and only then is the remainder plain zero bytes.
 *
 * The samples it encodes are a short fade rather than an immediate zero. The
 * signal is wherever it was when the button was pressed; cutting it in one
 * sample makes the encoder chase the step down through its largest steps and
 * ring on the way, which is both a click and a long convergence. Fading from
 * the encoder's own last reconstructed sample costs nothing and removes both. */
constexpr uint32_t kPadFadeFrames = 240; /* 5 ms at 48 kHz */
constexpr uint32_t kPadMaxFrames = 512;  /* fade + convergence, with margin */
uint8_t s_pad_buf[kPadMaxFrames];        /* stereo packs one byte per frame */

/* Encodes the tail into s_pad_buf, advancing `enc`. Returns the frames it
 * covers and writes the byte count. Stops once the fade is done and the
 * encoder has settled — and never on a half-used mono byte, so whatever
 * follows starts on a byte boundary. */
uint32_t ctl_pad_tail(osynth::adpcm::Ch enc[2], bool mono, uint32_t room,
                      uint32_t* out_bytes) {
    const int32_t p0 = enc[0].pred;
    const int32_t p1 = enc[1].pred;
    uint32_t n = 0;
    while (n < room && n < kPadMaxFrames) {
        const bool settled =
            n >= kPadFadeFrames && enc[0].pred == 0 && enc[0].index == 0 &&
            (mono || (enc[1].pred == 0 && enc[1].index == 0));
        if (settled && !(mono && (n & 1u) != 0)) break;
        int16_t s0 = 0, s1 = 0;
        if (n < kPadFadeFrames) {
            const int32_t w = (int32_t)(kPadFadeFrames - n);
            s0 = (int16_t)(p0 * w / (int32_t)kPadFadeFrames);
            s1 = (int16_t)(p1 * w / (int32_t)kPadFadeFrames);
        }
        if (mono) {
            const uint8_t nib = osynth::adpcm::encode(enc[0], s0);
            if ((n & 1u) == 0) {
                s_pad_buf[n >> 1] = (uint8_t)(nib << 4);
            } else {
                s_pad_buf[n >> 1] |= nib;
            }
        } else {
            const uint8_t nl = osynth::adpcm::encode(enc[0], s0);
            const uint8_t nr = osynth::adpcm::encode(enc[1], s1);
            s_pad_buf[n] = (uint8_t)((nl << 4) | nr);
        }
        ++n;
    }
    *out_bytes = mono ? ((n + 1u) >> 1) : n;
    return n;
}

/* Writes that tail where the take is, and only then publishes the track. */
void ctl_pad_take() {
    const int trk = s_pad_trk;
    s_pad_trk = -1;
    if (trk < 0 || trk >= LOOP_TRACKS) return;
    const uint32_t L = s_loop_frames.load(std::memory_order_acquire);
    uint32_t from = s_pad_from;
    if (L == 0 || from == 0 || from >= L) return;
    const bool mono = s_mono.load(std::memory_order_relaxed);
    osynth::adpcm::Ch enc[2] = {s_pad_enc[0], s_pad_enc[1]};
    /* Mono packs two frames per byte, and the byte holding `from` is already
     * written with its low nibble left at zero — the render path assigns the
     * whole byte on an even frame and ORs the odd one in. Mirror that zero
     * nibble through the encoder state (decode() and encode() move the same
     * state) so the tail continues from where the decoder will really be, and
     * begin on the next whole byte. Both backends are then appending. */
    if (mono && (from & 1u) != 0) {
        osynth::adpcm::decode(enc[0], 0);
        if (++from >= L) return;
    }
    uint32_t bytes = 0;
    const uint32_t n = ctl_pad_tail(enc, mono, L - from, &bytes);
#if SYNTH_LOOP_STREAM
    if (s_streamed.load(std::memory_order_relaxed)) {
        /* Only the tail reaches the card. The pass's remaining bytes are the
         * zeros the settled decoder reads as silence, and loop_stream serves
         * those for any file that ends before its pass does — writing them
         * out would cost megabytes and seconds for no audible difference. */
        if (loop_stream_pad_record(s_pad_buf, bytes) != ESP_OK) {
            ESP_LOGW(TAG, "track %d: tail did not reach the card — take lost",
                     trk + 1);
            return; /* bit stays clear: the streamed close then discards it */
        }
    } else
#endif
    {
        uint8_t* tb = s_buf[trk].load(std::memory_order_acquire);
        if (tb == nullptr) return;
        /* The filled bit has been clear since the audio task closed the take,
         * so the render path is already skipping this track — the handshake
         * is for the block that may have read the mask just before that. */
        ctl_handshake();
        memcpy(tb + (mono ? (from >> 1) : from), s_pad_buf, bytes);
        const uint32_t end = from + n;
        if (end < L) {
            const uint32_t zoff = mono ? (end >> 1) : end;
            const uint32_t zend = mono ? ((L + 1u) >> 1) : L;
            memset(tb + zoff, 0, zend - zoff);
        }
    }
    s_filled.fetch_or((uint8_t)(1u << trk), std::memory_order_release);
    ESP_LOGI(TAG, "track %d: take stopped at %.2f s, silent to %.2f s",
             trk + 1, (float)s_pad_from / SYNTH_SAMPLE_RATE,
             (float)L / SYNTH_SAMPLE_RATE);
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
#if SYNTH_LOOP_STREAM
            /* Stop rewinds the transport to the top, so the windows have to
             * go back there too — otherwise the next play would serve
             * whatever the streams had prefetched at the moment of the stop.
             * The handshake is what makes resetting the rings safe: past it,
             * no in-flight block is still reading them. */
            if (s_streamed.load(std::memory_order_relaxed) &&
                s_loop_frames.load(std::memory_order_acquire) != 0) {
                if (ctl_handshake()) loop_stream_rewind();
            }
#endif
            break;
    }
}

/* Back to "nothing recorded": detach the audio, drop every buffer, and reset
 * the loop length so the next first take latches the format and the full
 * policy cap again (S19/S20). Shared by clear-all and by the track clear that
 * happens to empty the set — those two land in the same state, so they take
 * the same path. */
void ctl_reset_empty_set() {
    ParamStore& ps = ParamStore::instance();
    s_cmd.store(kCmdDetach, std::memory_order_release);
    ctl_release_all(ctl_handshake());
#if SYNTH_LOOP_STREAM
    /* Detached and handshaken above, so the scratch files and rings have no
     * reader left — the same precondition ctl_release_all() needs. The next
     * first take latches loop.store again from scratch. With no card mounted
     * the wipe inside costs nothing: the VFS has no /sd registered, so the
     * removes fail immediately instead of timing out against the bus. */
    loop_stream_end_set();
    s_streamed.store(false, std::memory_order_release);
    s_ctl_rec_trk = -1;
#endif
    s_filled.store(0, std::memory_order_release);
    s_loop_frames.store(0, std::memory_order_release);
    s_ctl_len = 0;
    s_ctl_mode = MODE_STOP;
    ps.set(LOOP_PID_MODE, (float)MODE_STOP, ParamOrigin::Internal);
    ctl_mirror_state();
    ctl_mirror_maxlen(); /* the policy cap applies again */
}

#if SYNTH_LOOP_STORE_SD
/* The card is not answering (loop_store.h). Nothing is mid-flight — this only
 * runs on an idle wake — so the job is to give up what is held on it, in that
 * order: the streamed set's file handles first, because unmounting frees the
 * FATFS context they point into, and only then the mount itself.
 *
 * The set itself survives. Its tracks are files, and a card in someone's hand
 * still has them — so this suspends rather than clears, and putting the card
 * back brings the audio with it (ctl_card_back). Meanwhile the loop keeps its
 * length and its filled mask and plays silence, which is the honest state: the
 * tracks exist, they are simply not reachable. A PSRAM set is untouched; it
 * never needed the card. */
void ctl_card_lost() {
#if SYNTH_LOOP_STREAM
    if (s_streamed.load(std::memory_order_relaxed) &&
        !loop_stream_suspended()) {
        ESP_LOGW(TAG, "card removed — the streamed set is suspended, and its "
                      "tracks come back when the card does");
        loop_stream_suspend_set();
    }
#endif
    loop_store_card_gone();
}

#if SYNTH_LOOP_STREAM
/* Picks up a set left on the card — by the last session, by a power cut, or
 * by another osynth. Only ever with the looper empty: adopting over a live set
 * would silently discard whatever is in PSRAM, and there is no gesture here
 * that asks for that. Quiet when the card has nothing to offer, because it is
 * called speculatively (at init, and whenever loop.store is switched to sd
 * with nothing recorded). */
bool ctl_adopt_card_set() {
    if (s_loop_frames.load(std::memory_order_acquire) != 0) return false;
    if (s_filled.load(std::memory_order_acquire) != 0) return false;
    if (!loop_stream_probe_card()) return false;
    uint32_t frames = 0;
    uint8_t filled = 0;
    bool mono = true;
    if (!loop_stream_load_manifest(&frames, &mono, &filled)) return false;
    if (loop_stream_adopt_set(frames, mono, filled) != ESP_OK) return false;
    ParamStore& ps = ParamStore::instance();
    s_mono.store(mono, std::memory_order_release);
    s_streamed.store(true, std::memory_order_release);
    s_loop_frames.store(frames, std::memory_order_release);
    s_filled.store(filled, std::memory_order_release);
    s_ctl_len = frames;
    s_ctl_rec_trk = -1;
    /* The set's own format and storage become the live ones, exactly as a
     * slot load adopts what it loaded. Internal, so the app is told rather
     * than asked, and the listener's task filter keeps it from coming back. */
    ps.set(LOOP_PID_MONO, mono ? 1.0f : 0.0f, ParamOrigin::Internal);
    ps.set(LOOP_PID_STORE, 1.0f, ParamOrigin::Internal);
    ctl_mirror_state();
    ctl_mirror_maxlen();
    ESP_LOGI(TAG, "picked up the set on the card: %.2f s, %d %s track(s) — "
                  "press play",
             (float)frames / SYNTH_SAMPLE_RATE, __builtin_popcount(filled),
             mono ? "mono" : "stereo");
    return true;
}
#endif

/* A card is mounted again. If a set is waiting for one, this is where it finds
 * out whether it is *its* card. */
void ctl_card_back() {
#if SYNTH_LOOP_STREAM
    if (!loop_stream_suspended()) return;
    if (loop_stream_resume_set(s_filled.load(std::memory_order_acquire))) {
        ESP_LOGI(TAG, "card back — the streamed set plays again");
        return;
    }
    /* A different card, or one this set's files are no longer on. Nothing can
     * be recovered and the loop.filled the app is showing is now a lie, so
     * the set goes rather than sitting there permanently silent. */
    ESP_LOGW(TAG, "the streamed set's tracks are not on this card — clearing");
    ctl_reset_empty_set();
#endif
}
#endif

void ctl_handle_clear() {
    ctl_arm_cancel();
    ParamStore& ps = ParamStore::instance();
    const int what = (int)(ps.get(LOOP_PID_CLEAR) + 0.5f);
    if (what == CLR_TRACK) {
        int trk = (int)(ps.get(LOOP_PID_TRACK) + 0.5f) - 1;
        if (trk < 0 || trk >= LOOP_TRACKS) trk = 0;
        const uint8_t bit = (uint8_t)(1u << trk);
        const uint8_t before =
            s_filled.fetch_and((uint8_t)~bit, std::memory_order_acq_rel);
        const uint8_t left = (uint8_t)(before & ~bit);
        /* Clearing the *last* filled track leaves no loop at all, and a length
         * with nothing behind it is not a length: the next first take would
         * still be bound by a loop nobody can hear, and the app would still
         * show the old ceiling instead of the cap. A take in flight is being
         * recorded against that length, so it keeps it (the punch is the loop
         * now); loop_ctl's mode is the honest test — both the first take and a
         * pending punch sit in MODE_REC. */
        if ((before & bit) != 0 && left == 0 && s_ctl_mode != MODE_REC) {
            ctl_reset_empty_set();
            ESP_LOGI(TAG, "track %d cleared (last one), loop length reset",
                     trk + 1);
        } else {
#if SYNTH_LOOP_STREAM
            /* The audio task has already stopped mixing this track (its
             * filled bit is clear above), so dropping the reader and the
             * file is safe without a handshake. */
            if (s_streamed.load(std::memory_order_relaxed)) {
                loop_stream_clear_track(trk);
            }
#endif
            /* buffer kept for reuse — the loop length is unchanged */
            ctl_mirror_state();
        }
    } else if (what == CLR_ALL) {
        ctl_reset_empty_set();
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
#if SYNTH_LOOP_STREAM
    /* A streamed set has no PSRAM buffers to hand loop_store, and the slot
     * blob format wants the whole set in memory at once — which is the one
     * thing this mode exists to avoid. The tracks are already files on the
     * card (/sd/osynth/liveN.olt); copying them into a slot is a card-to-card
     * job for a later session, not something to fake here. Refusing is the
     * honest answer, and it names where the audio actually is. */
    if (s_streamed.load(std::memory_order_relaxed)) {
        ESP_LOGW(TAG,
                 "%s refused: this set streams from the card — its tracks are "
                 "/sd/osynth/liveN.olt (slot save/load is psram-only)",
                 op);
        return false;
    }
#endif
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

/* ---- export (S33): serving a recorded track to a control client ----
 *
 * All three of these run on loop_ctl, off the ExportReq the caller left
 * behind. See looper.h for what an export is allowed to see and the header
 * comment on ExportReq for how the request gets here. */

/* The live set's packing, as a codec number. */
inline uint8_t live_codec(bool mono) {
    return mono ? (uint8_t)LOOPER_EXPORT_CODEC_ADPCM_MONO
                : (uint8_t)LOOPER_EXPORT_CODEC_ADPCM;
}

/* Whether an export may run at all right now. Both refusals are the same two
 * the persistence paths make, for the same two reasons: a take in flight is
 * writing the very bytes being read, and the flash backend cannot be read
 * from while the render chain is being fed off it. A live (PSRAM or card) set
 * touches no flash and is therefore free to be downloaded while it plays. */
esp_err_t ctl_export_allowed(int source) {
    if (s_ctl_mode == MODE_REC) return ESP_ERR_INVALID_STATE;
#if SYNTH_ENABLE_LOOP_PERSIST
    if (source == LOOPER_EXPORT_SLOT && loop_store_needs_stopped() &&
        s_ctl_mode != MODE_STOP) {
        return ESP_ERR_INVALID_STATE;
    }
#else
    (void)source;
#endif
    return ESP_OK;
}

esp_err_t ctl_export_info(int source, int slot, looper_export_info_t* out) {
    memset(out, 0, sizeof(*out));
    out->sample_rate = (uint32_t)SYNTH_SAMPLE_RATE;
    if (source == LOOPER_EXPORT_LIVE) {
        const uint32_t L = s_loop_frames.load(std::memory_order_acquire);
        const bool mono = s_mono.load(std::memory_order_relaxed);
        out->codec = live_codec(mono);
        out->loop_frames = L;
        if (L == 0) return ESP_OK; /* no set: `filled` stays 0 */
        out->filled = s_filled.load(std::memory_order_acquire);
        out->track_bytes = (uint32_t)track_bytes_for(L, mono);
        return ESP_OK;
    }
    if (source != LOOPER_EXPORT_SLOT) return ESP_ERR_INVALID_ARG;
#if SYNTH_ENABLE_LOOP_PERSIST
    if (slot < 0 || slot >= loop_store_slots()) return ESP_ERR_INVALID_ARG;
    if (!loop_store_ready()) return ESP_ERR_INVALID_STATE;
    loop_store_info_t si;
    const esp_err_t err = loop_store_slot_info(slot, &si);
    /* A slot nobody has saved to is an ordinary answer, not a failure: an
     * empty `filled` says so and the app greys its button. */
    if (err == ESP_ERR_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    out->loop_frames = si.loop_frames;
    out->sample_rate = si.sample_rate;
    out->track_bytes = si.track_bytes;
    out->filled = si.filled;
    out->codec = si.codec;
    return ESP_OK;
#else
    (void)slot;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t ctl_export_read(const ExportReq& r, uint32_t* got) {
    *got = 0;
    if (r.track < 0 || r.track >= LOOP_TRACKS) return ESP_ERR_INVALID_ARG;
    if (r.len == 0) return ESP_OK;
    if (r.source == LOOPER_EXPORT_LIVE) {
        const uint32_t L = s_loop_frames.load(std::memory_order_acquire);
        const uint8_t filled = s_filled.load(std::memory_order_acquire);
        if (L == 0 || ((filled >> r.track) & 1) == 0) return ESP_ERR_NOT_FOUND;
        const bool mono = s_mono.load(std::memory_order_relaxed);
        const uint32_t total = (uint32_t)track_bytes_for(L, mono);
        if (r.offset >= total) return ESP_OK; /* end of track */
        const uint32_t n = r.len < total - r.offset ? r.len : total - r.offset;
#if SYNTH_LOOP_STREAM
        if (s_streamed.load(std::memory_order_relaxed)) {
            return loop_stream_export_read(r.track, r.offset, r.dst, n, got);
        }
#endif
        /* PSRAM set: loop_ctl owns these pointers, so reading one here needs
         * no handshake — and the audio task only ever *writes* the track it
         * is recording, which ctl_export_allowed() has already ruled out. */
        const uint8_t* src = s_buf[r.track].load(std::memory_order_relaxed);
        if (src == nullptr) return ESP_ERR_NOT_FOUND;
        memcpy(r.dst, src + r.offset, n);
        *got = n;
        return ESP_OK;
    }
    if (r.source != LOOPER_EXPORT_SLOT) return ESP_ERR_INVALID_ARG;
#if SYNTH_ENABLE_LOOP_PERSIST
    if (r.slot < 0 || r.slot >= loop_store_slots()) return ESP_ERR_INVALID_ARG;
    loop_store_info_t si;
    esp_err_t err = loop_store_slot_info(r.slot, &si);
    if (err != ESP_OK) return err;
    if (((si.filled >> r.track) & 1) == 0) return ESP_ERR_NOT_FOUND;
    if (r.offset >= si.track_bytes) return ESP_OK; /* end of track */
    /* Only *stored* tracks are in the blob, packed in ascending track order
     * (loop_store.h), so the track number has to be counted into an index. */
    int packed = 0;
    for (int t = 0; t < r.track; ++t) packed += (si.filled >> t) & 1;
    const uint32_t n =
        r.len < si.track_bytes - r.offset ? r.len : si.track_bytes - r.offset;
    err = loop_store_read_slot_bytes(r.slot, packed, r.offset, r.dst, n);
    if (err == ESP_OK) *got = n;
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void ctl_handle_export() {
    ExportReq& r = s_export_req;
    r.got = 0;
    r.err = ctl_export_allowed(r.source);
    if (r.err == ESP_OK) {
        r.err = r.read ? ctl_export_read(r, &r.got)
                       : ctl_export_info(r.source, r.slot, r.info);
    }
    /* Unconditional, and the last thing this function does: the caller is
     * blocked on it with no timeout, so no path here may return without it. */
    xSemaphoreGive(s_export_done);
}

/* Caller side of the handover — any task but loop_ctl. */
esp_err_t export_call(ExportReq& req) {
    if (s_export_mutex == nullptr || s_ctl_task == nullptr) {
        return ESP_ERR_INVALID_STATE; /* looper_init has not run */
    }
    if (xTaskGetCurrentTaskHandle() == s_ctl_task) {
        return ESP_ERR_INVALID_STATE; /* would wait for itself */
    }
    xSemaphoreTake(s_export_mutex, portMAX_DELAY);
    s_export_req = req;
    s_flags.fetch_or(kFlagExport, std::memory_order_release);
    xTaskNotifyGive(s_ctl_task);
    xSemaphoreTake(s_export_done, portMAX_DELAY);
    const esp_err_t err = s_export_req.err;
    req.got = s_export_req.got;
    xSemaphoreGive(s_export_mutex);
    return err;
}

void ctl_task(void* arg) {
    (void)arg;
    ParamStore& ps = ParamStore::instance();
    for (;;) {
        /* timed wake while the transport runs — the position mirror at the
         * bottom is the only periodic work; everything else stays
         * notify-driven. A stopped looper blocks indefinitely, unless there
         * is a card to keep an eye on: with no card-detect pin, presence is
         * something this task has to ask about, and idle is the only time it
         * may (see the poll at the bottom). */
#if SYNTH_LOOP_STORE_SD
        const TickType_t idle_wait = pdMS_TO_TICKS(kCardPollMs);
#else
        const TickType_t idle_wait = portMAX_DELAY;
#endif
        ulTaskNotifyTake(pdTRUE, s_ctl_mode == MODE_STOP
                                     ? idle_wait
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
            /* Before the streamed close, which reads loop.filled to decide
             * whether the take is kept and closes the file this writes to. */
            if (evt & kEvtPad) ctl_pad_take();
#if SYNTH_LOOP_STREAM
            /* Before the mirror: this is what settles whether the take
             * counted, and loop.filled must not be published twice. */
            if (s_streamed.load(std::memory_order_relaxed)) {
                ctl_finish_streamed_take();
            }
#endif
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
            } else {
                /* Say so even though nothing moved. This is the one transport
                 * change the firmware makes on its own initiative, seconds
                 * after the write that armed it, and until now it was the one
                 * that announced nothing: loop.mode was left at whatever the
                 * app had written before the count-in. Anything that leaves a
                 * client's view stale across those seconds — a dropped
                 * notification, a reconnect, the fallback below rewriting
                 * loop.store — therefore showed a stopped transport over a
                 * running take, with no later event to correct it. Re-asserting
                 * is free: ParamStore does not change-filter, so this always
                 * reaches the client, and a client already showing rec sees no
                 * edge and does no work. */
                ps.set(LOOP_PID_MODE, (float)MODE_REC, ParamOrigin::Internal);
            }
        }
        if (flags & kFlagMono) {
            ctl_mirror_maxlen();
#if SYNTH_LOOP_STREAM
            /* Switching to sd with nothing recorded is the closest thing the
             * surface has to "use what is on the card", so it is treated as
             * that. Cheap and quiet when the card has no set on it, and never
             * reached once anything is recorded — kFlagMono also covers
             * loop.mono and loop.tracks, which must not go looking for one. */
            if (ParamStore::instance().get(LOOP_PID_STORE) > 0.5f) {
                ctl_adopt_card_set();
            }
#endif
        }
#if SYNTH_ENABLE_LOOP_PERSIST
        if (flags & kFlagSave) ctl_handle_save();
        if (flags & kFlagLoad) ctl_handle_load();
#endif
        /* After save/load on purpose: a client that saves a set and then asks
         * to download it in the same breath gets the set it just wrote. */
        if (flags & kFlagExport) ctl_handle_export();
        ctl_mirror_pos();
#if SYNTH_LOOP_STREAM
        /* The one line that says the card is the problem. Rate-limited by
         * the ~4 Hz wake and by only printing on a change, so a set that
         * streams cleanly says nothing at all. */
        if (s_streamed.load(std::memory_order_relaxed)) {
            const uint32_t u = loop_stream_underruns();
            if (u != s_last_underruns) {
                ESP_LOGW(TAG,
                         "sd cannot keep up: %u window underrun(s) — the "
                         "affected track drops out until the next loop start",
                         (unsigned)u);
                s_last_underruns = u;
            }
        }
#endif
#if SYNTH_LOOP_STORE_SD
        /* Idle only, as the card lifecycle asks: a running transport is
         * either reading the card or about to, and a status probe in the
         * middle of that tells us nothing its own I/O errors would not. This
         * is also what mounts a re-inserted card — loop_store backs the
         * attempts off, so an empty slot settles into one try every 30 s. */
        if (s_ctl_mode == MODE_STOP) {
            switch (loop_store_poll_card()) {
                case LOOP_STORE_CARD_OK: ctl_card_back(); break;
                case LOOP_STORE_CARD_LOST: ctl_card_lost(); break;
                case LOOP_STORE_CARD_NONE:
                    /* Not a removal — see loop_store.h. A suspended set waits
                     * here for as long as it takes; nothing is torn down on
                     * the strength of "nothing is mounted right now". */
                    break;
            }
        }
#endif
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
    } else if (id == LOOP_PID_MONO || id == LOOP_PID_TRACKMODE
#if SYNTH_LOOP_STREAM
               || id == LOOP_PID_STORE
#endif
    ) {
        bit = kFlagMono; /* only re-mirrors loop.maxlen — format, storage and
                          * cap are latched when the next first take starts */
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

    /* Size the cap policy to the PSRAM actually left for the looper, before
     * the parameters are registered — the app reads the ceiling from
     * loop.len / loop.pos and the base cap from loop.maxlen's *default*
     * (PARAM_INFO metadata), so those three have to be truthful at
     * registration or the "max n s" texts describe a board that isn't this
     * one. main.cpp calls this after fx/drums/engines, so the measurement is
     * taken once the other PSRAM consumers have had their turn. */
    s_base_cap_s =
        base_cap_for_pool(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    s_rec_limit.store(cap_frames(true, true), std::memory_order_relaxed);

    /* ParamStore::add copies the descriptor, so patching a local copy is
     * enough — and keeps kParams a single readable table. */
    ParamDesc live[kParamCount];
    memcpy(live, kParams, sizeof(live));
    for (ParamDesc& d : live) {
        if (d.id == LOOP_PID_LEN || d.id == LOOP_PID_POS) {
            d.max = loop_param_max_s();
        } else if (d.id == LOOP_PID_MAXLEN) {
            d.max = loop_param_max_s();
            d.def = s_base_cap_s;
        }
    }

    size_t params = kParamCount;
    if (ps.add(live, kParamCount) != kParamCount) {
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
#if SYNTH_LOOP_STREAM
    /* After loop_store_init(): the streamed backend shares its mount. The
     * param is registered whether or not a card is in the slot right now —
     * one can be inserted later, and ctl_want_streamed() re-checks per set.
     * A ring allocation that fails leaves loop_stream_ready() false, and the
     * switch then falls back to psram with a log line rather than failing a
     * take. */
    ESP_ERROR_CHECK(loop_stream_init());
    if (ps.add(kStoreParams, kStoreParamCount) != kStoreParamCount) {
        ESP_LOGE(TAG, "loop.store registration failed");
        return ESP_FAIL;
    }
    params += kStoreParamCount;
    /* Probed here rather than in the summary line below: probing mounts the
     * card through loop_store_mount(), which has one logical caller, and past
     * the xTaskCreate below that caller is loop_ctl. It also seeds what
     * loop_stream_ready() reports, so the first loop.maxlen mirror already
     * knows whether sd is on offer. */
    const bool card_now = loop_stream_probe_card();
#endif
    for (int t = 0; t < LOOP_TRACKS; ++t) {
        s_lvl[t] = ps.valuePtr((uint16_t)LOOP_PID_LEVEL(t));
        s_buf[t].store(nullptr, std::memory_order_relaxed);
        s_cap_bytes[t] = 0;
    }
    /* Before the task: ctl_handle_export() gives s_export_done the moment it
     * sees a request, so both must exist by the time loop_ctl can run. */
    s_export_mutex = xSemaphoreCreateMutex();
    s_export_done = xSemaphoreCreateBinary();
    if (s_export_mutex == nullptr || s_export_done == nullptr) {
        ESP_LOGE(TAG, "export handover allocation failed");
        return ESP_FAIL;
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
#if SYNTH_LOOP_STREAM
    /* A set left on the card is picked up here, before anything can be
     * recorded over it — so a synth that is switched off mid-session comes
     * back with the loop it had. Still on the main task and still before
     * loop_ctl exists, which keeps loop_store's single-caller rule; and the
     * audio task is not running yet, so publishing a loop for it to find is
     * exactly as safe as a slot load is. */
    ctl_adopt_card_set();
#endif
    /* The seq/arp clock drives loop.sync and loop.countin. seqarp_init() runs
     * before this (main.cpp), so the subscription is live from here on. */
    seqarp_set_beat_callback(beat_cb, nullptr);
    ESP_LOGI(TAG,
             "up: %d tracks, adpcm in PSRAM (~%u KB/s stereo, half mono), "
             "cap %.0f s x2 mono x2 4-track (<= %.0f s, sized from the free "
             "pool at init), free PSRAM %u KB "
             "(largest block %u KB), %u params, punch-in at loop start%s",
             LOOP_TRACKS, (unsigned)(SYNTH_SAMPLE_RATE / 1024),
             s_base_cap_s, loop_abs_max_s(),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) /
                        1024),
             (unsigned)params,
             SYNTH_ENABLE_LOOP_PERSIST ? ", save/load enabled" : "");
#if SYNTH_LOOP_STREAM
    ESP_LOGI(TAG,
             "loop.store: psram (the cap above) or sd (streamed from the "
             "card, up to %.0f s) — %s right now",
             LOOP_STREAM_MAX_S,
             card_now ? "card ready" : "no card, psram only");
#endif
    return ESP_OK;
}

extern "C" esp_err_t looper_export_info(int source, int slot,
                                        looper_export_info_t* out) {
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    ExportReq req{};
    req.read = false;
    req.source = source;
    req.slot = slot;
    req.info = out;
    return export_call(req);
}

extern "C" esp_err_t looper_export_read(int source, int slot, int track,
                                        uint32_t offset, uint8_t* dst,
                                        uint32_t len, uint32_t* out_read) {
    if (out_read == nullptr) return ESP_ERR_INVALID_ARG;
    *out_read = 0;
    if (dst == nullptr) return ESP_ERR_INVALID_ARG;
    ExportReq req{};
    req.read = true;
    req.source = source;
    req.slot = slot;
    req.track = track;
    req.offset = offset;
    req.len = len;
    req.dst = dst;
    const esp_err_t err = export_call(req);
    *out_read = req.got;
    return err;
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
#if SYNTH_LOOP_STREAM
    /* Track source (S31): same latching rule as the format above. */
    const bool streamed = s_streamed.load(std::memory_order_relaxed);
#endif

    size_t i = 0;
    while (i < frames) {
        const uint32_t limit =
            (L > 0) ? L : s_rec_limit.load(std::memory_order_relaxed);
        size_t n = frames - i;
        if ((uint32_t)n > limit - a_pos) n = limit - a_pos;

        /* record tap first: the live synth only, before playback is mixed —
         * encoded straight into the ADPCM track (S20) */
#if SYNTH_LOOP_STREAM
        if (a_rec && streamed) {
            namespace ls = osynth::loopstream;
            /* Worst case one byte per frame (stereo); mono needs half that
             * and never more. Checking the whole segment up front keeps the
             * inner loops branch-free and means a hole is always a whole
             * segment, never a torn frame. */
            if (ls::rec_space() >= (uint32_t)n) {
                if (mono) {
                    for (size_t k = 0; k < n; ++k) {
                        const uint8_t nib = osynth::adpcm::encode(
                            a_enc[0], f2i16(0.5f * (l[i + k] + r[i + k])));
                        /* nibble phase follows the absolute frame position,
                         * exactly as the PSRAM packing does */
                        if (((a_pos + (uint32_t)k) & 1u) == 0) {
                            ls::rec_put((uint8_t)(nib << 4));
                        } else {
                            ls::rec_or_last(nib);
                        }
                    }
                } else {
                    for (size_t k = 0; k < n; ++k) {
                        const uint8_t nl =
                            osynth::adpcm::encode(a_enc[0], f2i16(l[i + k]));
                        const uint8_t nr =
                            osynth::adpcm::encode(a_enc[1], f2i16(r[i + k]));
                        ls::rec_put((uint8_t)((nl << 4) | nr));
                    }
                }
            } else {
                /* The card did not drain the ring in time. The take now has
                 * a hole, so it is not a take — loop_ctl discards it at
                 * close rather than keeping a loop with a gap in it. */
                s_stream_overrun.store(true, std::memory_order_release);
            }
        } else
#endif
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
                if (a_rec && t == a_rec_trk) {
                    /* The take is overwriting this track, so it is not mixed
                     * (same as PSRAM mode). Streamed, that also means a whole
                     * pass goes by without its window being consumed — the
                     * window would be a pass behind by the time the punch
                     * ends, so hand it back now. */
                    audio_hold_track(t);
                    continue;
                }
#if SYNTH_LOOP_STREAM
                if (streamed) {
                    namespace ls = osynth::loopstream;
                    const uint8_t bit = (uint8_t)(1u << t);
                    /* muted for the rest of the pass; waiting for loop_io to
                     * put the window back at the loop start; or waiting for
                     * loop_ctl to open the file a take just replaced. None of
                     * the three is an underrun — see audio_hold_track. */
                    if ((a_starved & bit) != 0) continue;
                    if (((ls::g_resync.load(std::memory_order_acquire) |
                          ls::g_hold.load(std::memory_order_acquire)) &
                         bit) != 0) {
                        a_starved |= bit;
                        continue;
                    }
                    /* Bytes this segment needs: the ones holding frames
                     * [a_pos, a_pos + n). Mono packs two frames per byte, so
                     * the last frame's byte counts even when only its high
                     * nibble is used. */
                    const uint32_t end = a_pos + (uint32_t)n;
                    const uint32_t need =
                        (mono ? ((end + 1u) >> 1) : end) - a_win_off[t];
                    if (ls::play_avail(t) < need) {
                        /* Window dry. Decoding the bytes that *are* there
                         * would leave this track's decoder at an unknown
                         * state for the rest of the pass, so it drops out
                         * instead and rejoins at the wrap. */
                        a_starved |= bit;
                        ls::g_resync.fetch_or(bit, std::memory_order_release);
                        ls::g_underruns.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    const float g = s_lvl[t]->load(std::memory_order_relaxed) *
                                    (1.0f / 32768.0f);
                    if (mono) {
                        osynth::adpcm::Ch& c = a_dec[t][0];
                        for (size_t k = 0; k < n; ++k) {
                            const uint32_t p = a_pos + (uint32_t)k;
                            const uint8_t b =
                                ls::play_at(t, (p >> 1) - a_win_off[t]);
                            const uint8_t nib = (p & 1u) ? (uint8_t)(b & 0x0F)
                                                         : (uint8_t)(b >> 4);
                            const float s =
                                (float)osynth::adpcm::decode(c, nib) * g;
                            l[i + k] += s;
                            r[i + k] += s;
                        }
                    } else {
                        osynth::adpcm::Ch& cl = a_dec[t][0];
                        osynth::adpcm::Ch& cr = a_dec[t][1];
                        for (size_t k = 0; k < n; ++k) {
                            const uint8_t b = ls::play_at(
                                t, a_pos + (uint32_t)k - a_win_off[t]);
                            l[i + k] += (float)osynth::adpcm::decode(
                                            cl, (uint8_t)(b >> 4)) * g;
                            r[i + k] += (float)osynth::adpcm::decode(
                                            cr, (uint8_t)(b & 0x0F)) * g;
                        }
                    }
                    /* Release only whole bytes: with mono at an odd end
                     * frame the last byte still holds the next frame's
                     * nibble, so it stays in the window one more segment. */
                    const uint32_t done = mono ? (end >> 1) : end;
                    ls::play_consume(t, done - a_win_off[t]);
                    a_win_off[t] = done;
                    continue;
                }
#endif
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
#if SYNTH_LOOP_STREAM
                if (streamed) {
                    namespace ls = osynth::loopstream;
                    /* Close the pass in the windows too: an odd mono length
                     * leaves a half-used tail byte that the next pass must
                     * not see, so release exactly the pass, not the frames.
                     * loop_io wraps each file at the same boundary, which is
                     * what keeps the two sides describing the same sample. */
                    const uint32_t pass = mono ? ((L + 1u) >> 1) : L;
                    const uint8_t live = s_filled.load(std::memory_order_acquire);
                    for (int t = 0; t < LOOP_TRACKS; ++t) {
                        if ((a_starved & (1u << t)) != 0) continue;
                        if ((live & (1u << t)) == 0) continue;
                        if (a_rec && t == a_rec_trk) continue;
                        if (pass > a_win_off[t]) {
                            ls::play_consume(t, pass - a_win_off[t]);
                        }
                    }
                    for (int t = 0; t < LOOP_TRACKS; ++t) a_win_off[t] = 0;
                    /* Every track gets its pass back: the ones loop_io has
                     * not re-primed yet are caught again by the resync check
                     * at the top of the next segment. */
                    a_starved = 0;
                }
#endif
                a_pos = 0;
                audio_reset_dec(); /* every decoder re-enters at the top */
                if (a_rec) {
                    /* punch-in completed one full pass */
                    a_rec = false;
                    s_filled.fetch_or((uint8_t)(1u << a_rec_trk),
                                      std::memory_order_release);
                    /* streamed: its file is being replaced — no reader until
                     * loop_ctl re-opens it (the a_starved reset just above
                     * cleared the hold this track was already under) */
                    audio_hold_track(a_rec_trk);
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
                audio_reset_windows();
                /* after the reset, or it would be cleared again */
                audio_hold_track(a_rec_trk);
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

extern "C" esp_err_t looper_export_info(int, int, looper_export_info_t*) {
    return ESP_ERR_NOT_SUPPORTED;
}

extern "C" esp_err_t looper_export_read(int, int, int, uint32_t, uint8_t*,
                                        uint32_t, uint32_t* out_read) {
    if (out_read != nullptr) *out_read = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
