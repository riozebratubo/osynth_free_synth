/*
 * osynth — master FX bus (Sessions 10 + 11; bitcrush S17; filter S33;
 * drive / flanger / phaser / EQ / compressor / stereo / LFOs S34):
 *
 *   drive -> chorus -> flanger -> phaser -> delay -> granular -> reverb
 *         -> bitcrush -> filter -> EQ -> compressor -> stereo/output
 *
 * That order is the whole design of the bus, so it is worth stating why:
 *
 *  - Drive is first. Saturation belongs on the source, not on the tails;
 *    distorting a reverb reads as a fault, reverb on a distorted source
 *    reads as an effect. (Same argument S33 used to put the filter last.)
 *  - The three modulated-comb effects sit together, cheapest first, and
 *    ahead of the delay so their movement is what the repeats echo.
 *  - Filter then EQ then compressor: the compressor must react to the tone
 *    that was actually chosen, or its threshold moves every time the EQ does.
 *  - The compressor is late enough to catch the reverb tail. That is not an
 *    accident either — a sidechain duck that leaves the tail un-ducked does
 *    not read as a duck at all, it reads as a hole in the dry signal.
 *  - Stereo/output is last, because it is the only place where the *total*
 *    width of the mix is decided: ping-pong delay and the granular panner
 *    both throw energy wide upstream of it.
 *
 * Runs on the audio task, chained after the voice sum (main.cpp calls
 * fx_process() from the render callback; audio_io applies master volume and
 * the int16 conversion afterwards, so it scales dry and wet together).
 *
 * Memory: every line stores int16 — the whole output chain is 16-bit, so
 * the wet paths lose nothing audible, and the footprint (plus PSRAM cache
 * pressure on the S3) halves vs float. line_alloc() prefers PSRAM and falls
 * back to internal RAM; an effect whose lines cannot be allocated is
 * disabled with a warning (sink-fallback philosophy). The delay ceiling is
 * per target — 1.5 s on the S3 (PSRAM), 0.4 s on the classic ESP32
 * (internal-RAM budget) — and the registered fx.dly.time max reflects it.
 *
 * Every effect sits behind a dry/wet crossfade `mix`. Mixes are one-pole
 * smoothed per block (no zipper on CC sweeps) and the delay time glides
 * tape-style (repitches, never clicks). A fully-dry effect is skipped
 * entirely; once dry, its lines are zeroed incrementally (a few KB per
 * block) so stale audio can never replay on re-enable — that skip is what
 * keeps the always-on bus nearly free when unused.
 *
 * S35 adds `comp` to the delay, the granular delay and the reverb: the three
 * units whose wet path is decorrelated from the dry one, and so the three the
 * equal-gain crossfade above quietly costs 3 dB in the middle of the knob.
 * Two of them lose level at the far end as well. Off by default, opt-in per
 * unit; the whole argument is above mix_gains().
 *
 * Reverb is a Freeverb (Schroeder/Moorer): 8 parallel lowpass-feedback
 * combs + 4 series allpasses per channel, 44.1 kHz tunings scaled to the
 * build's sample rate, right channel offset for width.
 *
 * The granular delay (S11) captures the dry mono sum (plus feedback of its
 * own wet output) into one circular line and scatters parabolic-windowed
 * grains over it: each grain starts at a random extra delay (spray), plays
 * at a fixed per-grain rate (pitch, immutable once launched — so a live
 * pitch change never breaks a flying grain's bounds) and lands at a random
 * equal-power pan. The grain pool is a fixed per-target ceiling — the S10
 * budget figures were never recorded, so the worst case is bounded by
 * construction; when the pool is full a spawn is skipped, never stolen.
 *
 * S34 adds two things that are not effects:
 *
 *  - Note-division sync (fx.dly.div, fx.lfoN.sync), read from seqarp_bpm() —
 *    the tempo the clock is *running* at, so a delay locked to 1/8 stays
 *    locked when the instrument is slaved to a DAW's MIDI clock.
 *  - Two block-rate LFOs with one destination each. The S9 mod matrix is
 *    evaluated per voice against per-voice parameters, so nothing in the
 *    instrument could modulate the master bus; a tempo-locked filter sweep
 *    over a whole track was not expressible. They write normalized offsets
 *    into s_mod[] and every continuous parameter is read through pvm(),
 *    which folds the offset in against the parameter's own registered range
 *    and curve. Costs one load and one compare per parameter per block when
 *    nothing is modulating, which is the usual case.
 */
#include "fx.h"

#include <atomic>
#include <cmath>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "sdkconfig.h"

#include "drums.h"   /* the sidechain key: which drum slot sounded this block */
#include "seqarp.h"  /* seqarp_bpm() / beat grid: note-division sync (S34) */
#include "synth_config.h"
#include "synth_dsp.h"
#include "synth_params.h"
#include "synth_smooth.h"

static const char* TAG = "fx";

using osynth::ParamCurve;
using osynth::ParamDesc;
using osynth::ParamStore;
using osynth::ParamType;

namespace {

constexpr float kSr = (float)SYNTH_SAMPLE_RATE;
constexpr float kTwoPi = 6.28318530718f;

/* PSRAM, not a particular chip, is what sets these ceilings. */
#if CONFIG_SPIRAM
constexpr float kDelayMaxS = 1.5f; /* stereo int16 @ 48 kHz: 288 KB, PSRAM */
#else
constexpr float kDelayMaxS = 0.4f; /* 77 KB, classic internal-RAM budget */
#endif

constexpr float kChoBaseMs = 12.0f;
constexpr float kChoDepthMaxMs = 12.0f;

#if CONFIG_SPIRAM
constexpr float kGrnSizeMaxS = 0.5f;   /* grain length ceiling */
constexpr float kGrnSprayMaxS = 0.25f; /* random extra-delay ceiling */
constexpr int kGrainMax = 16;          /* fixed pool: bounds the worst case */
#else
constexpr float kGrnSizeMaxS = 0.25f;  /* classic: small internal buffer */
constexpr float kGrnSprayMaxS = 0.12f;
constexpr int kGrainMax = 8;
#endif

/* Flanger (S34): base delay plus the sweep it rides on. Tiny next to the
 * others — ~26 ms stereo is 5 KB — which is why it gets its own line instead
 * of sharing the chorus's: sharing would have coupled two independent
 * modulators to one write head. */
constexpr float kFlgBaseMaxMs = 12.0f;
constexpr float kFlgSweepMs = 10.0f;

constexpr int kPhsStagesMax = 12; /* allpass sections per channel */

/* ---- parameter set (order matches PIdx) ---- */

/* PIdx is internal indexing only — the on-wire form is the FX_PID_* id — so
 * unlike those, this may be reordered freely. Keep each unit's run together
 * and keep kParams below in exactly this order: tools/check_param_tables.py
 * verifies the two agree, because a short initializer list is silently
 * zero-filled and would register phantom parameters at id 0. */
enum PIdx {
    CHO_MIX, CHO_RATE, CHO_DEPTH,
    DLY_MIX, DLY_TIME, DLY_FB, DLY_TONE, DLY_PP, DLY_DIV, DLY_COMP,
    GRN_MIX, GRN_SIZE, GRN_DENS, GRN_PITCH, GRN_FB, GRN_SPRAY, GRN_COMP,
    REV_MIX, REV_SIZE, REV_DAMP, REV_COMP,
    CRUSH_MIX, CRUSH_BITS, CRUSH_DOWN,
    FLT_ON, FLT_TYPE, FLT_MODE, FLT_CUTOFF, FLT_RESO, FLT_DRIVE, FLT_SPREAD,
    FLT_VOWEL,
    DRV_MIX, DRV_MODE, DRV_DRIVE, DRV_TONE, DRV_LEVEL,
    PHS_MIX, PHS_STAGES, PHS_RATE, PHS_DEPTH, PHS_CENTER, PHS_FB, PHS_SPREAD,
    FLG_MIX, FLG_RATE, FLG_DEPTH, FLG_DELAY, FLG_FB, FLG_SPREAD,
    EQ_ON, EQ_LOW, EQ_LOFREQ, EQ_MID, EQ_MIDFREQ, EQ_MIDQ, EQ_HIGH, EQ_HIFREQ,
    COMP_ON, COMP_THRESH, COMP_RATIO, COMP_ATTACK, COMP_RELEASE, COMP_MAKEUP,
    COMP_MIX, COMP_KEY, COMP_SLOT,
    ST_WIDTH, ST_BASS, ST_MONO, ST_AMP, ST_PAN,
    LFO1_DEST, LFO1_WAVE, LFO1_RATE, LFO1_SYNC, LFO1_DEPTH, LFO1_PHASE,
    LFO2_DEST, LFO2_WAVE, LFO2_RATE, LFO2_SYNC, LFO2_DEPTH, LFO2_PHASE,
    P_COUNT
};

/* Same lists the engines register, same order — a filter should not mean
 * something different because it is on the master bus. Append-only. */
const char* const kFltModes[] = {"lp",   "bp", "hp", "notch",
                                 "peak", "ap", "bp norm"};
const char* const kFltTypes[] = {"svf 12", "svf 24", "ladder", "dual", "vowel"};

/* The first three are the graph Shaper's list, in its order and running the
 * same curves out of synth_dsp.h — a "fold" has to mean one thing in this
 * instrument, and these two units get A/B'd against each other constantly.
 * "tube" is appended: it biases the input before the curve, so it generates
 * the even harmonics the symmetric shapers cannot. */
const char* const kDrvModes[] = {"tanh", "fold", "clip", "tube"};

/* Note divisions. Entry 0 is "free" in both lists — a division of nothing —
 * which is what lets one enum replace a sync switch plus a division.
 *
 * Both are append-only: the index is the stored value. A trailing "." is
 * dotted (1.5x), a trailing "T" is a triplet (2/3x).
 *
 * The delay's list runs short-to-long because a delay measured in bars is
 * a different instrument; the LFO's runs long-to-short because a modulator
 * spanning eight bars is the whole point of having one. */
const char* const kDlyDivNames[] = {
    "free", "1/1",  "1/2.", "1/2",  "1/2T", "1/4.",  "1/4",  "1/4T",
    "1/8.", "1/8",  "1/8T", "1/16.", "1/16", "1/16T", "1/32"};
/* Beats (quarter notes) per repeat; [0] is unused. */
const float kDlyDivBeats[] = {
    0.0f,   4.0f,      3.0f,  2.0f,      4.0f / 3.0f, 1.5f, 1.0f, 2.0f / 3.0f,
    0.75f,  0.5f, 1.0f / 3.0f, 0.375f,   0.25f, 1.0f / 6.0f, 0.125f};

const char* const kLfoSyncNames[] = {
    "free", "8 bars", "4 bars", "2 bars", "1 bar", "1/2",  "1/4",
    "1/4T", "1/8",    "1/8T",   "1/16",   "1/16T", "1/32"};
/* Beats per cycle; [0] is unused. Bars are read as 4/4 — the sequencer has
 * no time signature, and its beat grid is the same 4-beat one the looper
 * bar-locks to. */
const float kLfoSyncBeats[] = {0.0f,        32.0f,       16.0f, 8.0f, 4.0f,
                               2.0f,        1.0f,        2.0f / 3.0f,
                               0.5f,        1.0f / 3.0f, 0.25f,
                               1.0f / 6.0f, 0.125f};

const char* const kLfoWaves[] = {"sine", "tri",    "saw up",
                                 "saw dn", "square", "s&h"};

/* The compressor's key. "mix" is the bus itself (glue); the rest is a drum
 * slot's trigger, which is what fx.comp.slot then picks. */
const char* const kCompKeys[] = {"mix", "drum"};

template <typename T, size_t N>
constexpr int count_of(const T (&)[N]) {
    return (int)N;
}

constexpr int kDlyDivCount = count_of(kDlyDivNames);
constexpr int kLfoSyncCount = count_of(kLfoSyncNames);
constexpr int kLfoWaveCount = count_of(kLfoWaves);
constexpr int kDrvModeCount = count_of(kDrvModes);
constexpr int kCompKeyCount = count_of(kCompKeys);
/* A name list and its value list that disagree would read one past the end
 * of the shorter one, from a value the app is entitled to send. */
static_assert(count_of(kDlyDivBeats) == kDlyDivCount, "delay division tables");
static_assert(count_of(kLfoSyncBeats) == kLfoSyncCount, "lfo sync tables");

/* ---- FX LFO destinations ----
 *
 * A curated list rather than "any FX parameter": the index is the stored
 * value, so the list is append-only, and every entry has to be something a
 * smoothly-moving control can actually do. That rules out the enums, the
 * switches and fx.crush.down (an integer hold count — ramping it produces a
 * staircase of aliasing, not a sweep).
 *
 * Order groups by unit and follows the signal chain, so the app's picker
 * reads like the FX page.
 *
 * The names are abbreviated to the point of terseness because they have to
 * *fit*: PARAM_INFO is a single frame, and ble_ctrl.cpp stops copying enum
 * names when the frame is full rather than splitting. An over-long list does
 * not fail — it arrives short, and the app draws a picker missing its last
 * few entries with nothing to indicate anything is wrong. The static_assert
 * below turns that into a build error; full names are in PARAM_MAP.md. */
constexpr const char* const kLfoDests[] = {
    "off",
    "drv mix", "drv amt",
    "cho dep", "cho rt",
    "flg mix", "flg dly", "flg rt",
    "phs mix", "phs frq", "phs rt",
    "dly mix", "dly tm",  "dly fb",
    "grn mix", "grn pit",
    "rev mix", "rev sz",
    "crs mix", "crs bit",
    "cutoff",  "reso",
    "eq mid",  "eq midf",
    "cmp thr",
    "width",   "amp",     "pan"};
/* Parallel to kLfoDests; -1 is "off". */
const int kLfoDestIdx[] = {
    -1,
    DRV_MIX,   DRV_DRIVE,
    CHO_DEPTH, CHO_RATE,
    FLG_MIX,   FLG_DELAY,  FLG_RATE,
    PHS_MIX,   PHS_CENTER, PHS_RATE,
    DLY_MIX,   DLY_TIME,   DLY_FB,
    GRN_MIX,   GRN_PITCH,
    REV_MIX,   REV_SIZE,
    CRUSH_MIX, CRUSH_BITS,
    FLT_CUTOFF, FLT_RESO,
    EQ_MID,    EQ_MIDFREQ,
    COMP_THRESH,
    ST_WIDTH,  ST_AMP,     ST_PAN};
constexpr int kLfoDestCount = count_of(kLfoDests);
static_assert(count_of(kLfoDestIdx) == kLfoDestCount, "lfo destination tables");

/* ---- PARAM_INFO frame budget ----
 *
 * One descriptor is one frame, and ble_ctrl.cpp fills it until it is full:
 * `if (n + len > limit) break;` — an enum list that does not fit is silently
 * *short*, and the app draws a picker missing its tail with no indication
 * that anything went missing. S31c hit exactly this and had to give up a
 * 34-entry enum over it. This list is the longest in the firmware and sits
 * close enough to the ceiling to be worth pinning down at build time.
 *
 * At the documented ATT MTU of 247 a notification carries 244 bytes
 * (BLE_PROTOCOL.md; kMaxFrame = 256 does not bind). The descriptor spends 22
 * on its fixed fields, then the parameter's own name, then the enum names,
 * every string NUL-terminated. `fx.lfo1.dest` is the longest name this
 * applies to.
 *
 * If this fires: shorten a name, or decide the destination is not worth its
 * bytes. Do NOT just raise the number — the frame is the frame. */
constexpr size_t cstr_len(const char* s) {
    size_t n = 0;
    while (s[n] != '\0') ++n;
    return n;
}

constexpr size_t enum_bytes(const char* const* names, int n) {
    size_t t = 0;
    for (int i = 0; i < n; ++i) t += cstr_len(names[i]) + 1;
    return t;
}

constexpr size_t kInfoFrame = 244;      /* ATT MTU 247 - 3 */
constexpr size_t kInfoFixed = 22;       /* id, type, curve, count, min/max/def */
constexpr size_t kLfoDestBudget =
    kInfoFrame - kInfoFixed - (cstr_len("fx.lfo1.dest") + 1);
static_assert(enum_bytes(kLfoDests, kLfoDestCount) <= kLfoDestBudget,
              "FX LFO destination names overflow one PARAM_INFO frame; the "
              "app would silently show a truncated list");

const ParamDesc kParams[P_COUNT] = {
    {FX_PID_CHO_MIX, "fx.cho.mix", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* 0.5 ~ classic chorus, 1 = pure vibrato */
    {FX_PID_CHO_RATE, "fx.cho.rate", ParamType::Float, ParamCurve::Exp,
     0.05f, 8.0f, 0.8f, nullptr, 0},
    {FX_PID_CHO_DEPTH, "fx.cho.depth", ParamType::Float, ParamCurve::Linear,
     0.0f, kChoDepthMaxMs, 3.5f, nullptr, 0}, /* ms */
    {FX_PID_DLY_MIX, "fx.dly.mix", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_DLY_TIME, "fx.dly.time", ParamType::Float, ParamCurve::Exp,
     0.02f, kDelayMaxS, 0.35f, nullptr, 0}, /* s; max = the line actually allocated */
    {FX_PID_DLY_FB, "fx.dly.fb", ParamType::Float, ParamCurve::Linear,
     0.0f, 0.95f, 0.35f, nullptr, 0},
    {FX_PID_DLY_TONE, "fx.dly.tone", ParamType::Float, ParamCurve::Exp,
     500.0f, 16000.0f, 4500.0f, nullptr, 0}, /* feedback LP: repeats darken */
    {FX_PID_DLY_PP, "fx.dly.pingpong", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    /* free by default: a delay that silently retimes itself the first time
     * the tempo moves is a surprise, and every preset saved before S34
     * loads with this at 0 and behaves exactly as it did. */
    {FX_PID_DLY_DIV, "fx.dly.div", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kDlyDivCount - 1), 0.0f /* free */, kDlyDivNames,
     kDlyDivCount},
    /* Crossfade law only — the delay's wet path is already unity (see
     * delay_process). Off by default like the other two. */
    {FX_PID_DLY_COMP, "fx.dly.comp", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_GRN_MIX, "fx.grn.mix", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_GRN_SIZE, "fx.grn.size", ParamType::Float, ParamCurve::Exp,
     0.02f, kGrnSizeMaxS, 0.09f, nullptr, 0}, /* s */
    {FX_PID_GRN_DENS, "fx.grn.dens", ParamType::Float, ParamCurve::Exp,
     1.0f, 64.0f, 12.0f, nullptr, 0}, /* grains/s; pool caps the overlap */
    {FX_PID_GRN_PITCH, "fx.grn.pitch", ParamType::Float, ParamCurve::Linear,
     -12.0f, 12.0f, 0.0f, nullptr, 0}, /* st, per grain at spawn */
    {FX_PID_GRN_FB, "fx.grn.fb", ParamType::Float, ParamCurve::Linear,
     0.0f, 0.9f, 0.25f, nullptr, 0}, /* wet mono back into the capture line */
    {FX_PID_GRN_SPRAY, "fx.grn.spray", ParamType::Float, ParamCurve::Linear,
     0.0f, kGrnSprayMaxS, 0.03f, nullptr, 0}, /* s of random extra delay */
    /* Window + pan make-up, and the sparse-setting duty. */
    {FX_PID_GRN_COMP, "fx.grn.comp", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_REV_MIX, "fx.rev.mix", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.15f, nullptr, 0}, /* subtle room out of the box */
    {FX_PID_REV_SIZE, "fx.rev.size", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.55f, nullptr, 0},
    {FX_PID_REV_DAMP, "fx.rev.damp", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.3f, nullptr, 0},
    /* Undoes the Freeverb staging's ~6 dB and takes `size` out of the level. */
    {FX_PID_REV_COMP, "fx.rev.comp", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_CRUSH_MIX, "fx.crush.mix", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* 1 = fully crushed */
    {FX_PID_CRUSH_BITS, "fx.crush.bits", ParamType::Float, ParamCurve::Linear,
     2.0f, 16.0f, 8.0f, nullptr, 0}, /* fractional bits allowed */
    {FX_PID_CRUSH_DOWN, "fx.crush.down", ParamType::Int, ParamCurve::Linear,
     1.0f, 32.0f, 1.0f, nullptr, 0}, /* sample-rate divider (hold) */
    /* The switch doubles as this unit's dry/wet: unit_gate() ramps it, so
     * toggling crossfades over a few blocks instead of stepping, and fully
     * off skips the stage. */
    {FX_PID_FLT_ON, "fx.flt.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_FLT_TYPE, "fx.flt.type", ParamType::Enum, ParamCurve::Linear,
     0.0f, 4.0f, 0.0f /* svf 12 */, kFltTypes, 5},
    {FX_PID_FLT_MODE, "fx.flt.mode", ParamType::Enum, ParamCurve::Linear,
     0.0f, 6.0f, 0.0f /* lp */, kFltModes, 7},
    {FX_PID_FLT_CUTOFF, "fx.flt.cutoff", ParamType::Float, ParamCurve::Exp,
     20.0f, 18000.0f, 18000.0f, nullptr, 0}, /* wide open: switching it on
                                                should not mute the mix */
    {FX_PID_FLT_RESO, "fx.flt.reso", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.1f, nullptr, 0},
    {FX_PID_FLT_DRIVE, "fx.flt.drive", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_FLT_SPREAD, "fx.flt.spread", ParamType::Float, ParamCurve::Linear,
     0.0f, 6.0f, 2.0f, nullptr, 0}, /* dual: passband width in octaves */
    {FX_PID_FLT_VOWEL, "fx.flt.vowel", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* vowel: morph a-e-i-o-u */

    /* ---- drive (S34) ---- */
    {FX_PID_DRV_MIX, "fx.drv.mix", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_DRV_MODE, "fx.drv.mode", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kDrvModeCount - 1), 0.0f /* tanh */, kDrvModes,
     kDrvModeCount},
    {FX_PID_DRV_DRIVE, "fx.drv.drive", ParamType::Float, ParamCurve::Exp,
     1.0f, 64.0f, 4.0f, nullptr, 0}, /* input gain into the curve */
    {FX_PID_DRV_TONE, "fx.drv.tone", ParamType::Float, ParamCurve::Exp,
     500.0f, 18000.0f, 18000.0f, nullptr, 0}, /* post-curve LP: tames fizz */
    {FX_PID_DRV_LEVEL, "fx.drv.level", ParamType::Float, ParamCurve::Linear,
     0.0f, 2.0f, 1.0f, nullptr, 0}, /* trim on top of the auto make-down */

    /* ---- phaser (S34) ---- */
    /* 0.5 is the classic phaser, as with the chorus: the notches come from
     * summing the swept allpass chain against the dry signal, and an allpass
     * on its own has flat magnitude. At 1 what is left is the feedback
     * resonance, which is a real sound but not the one most people mean. */
    {FX_PID_PHS_MIX, "fx.phs.mix", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    /* Int, not enum: the notch count is stages/2 and every value in between
     * is a usable sound, including the odd ones. */
    {FX_PID_PHS_STAGES, "fx.phs.stages", ParamType::Int, ParamCurve::Linear,
     2.0f, (float)kPhsStagesMax, 6.0f, nullptr, 0},
    {FX_PID_PHS_RATE, "fx.phs.rate", ParamType::Float, ParamCurve::Exp,
     0.02f, 8.0f, 0.3f, nullptr, 0},
    {FX_PID_PHS_DEPTH, "fx.phs.depth", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.7f, nullptr, 0}, /* sweep span around center, in octaves */
    {FX_PID_PHS_CENTER, "fx.phs.center", ParamType::Float, ParamCurve::Exp,
     100.0f, 4000.0f, 600.0f, nullptr, 0},
    {FX_PID_PHS_FB, "fx.phs.fb", ParamType::Float, ParamCurve::Linear,
     0.0f, 0.95f, 0.5f, nullptr, 0}, /* sharpens the notches */
    {FX_PID_PHS_SPREAD, "fx.phs.spread", ParamType::Float, ParamCurve::Linear,
     0.0f, 0.5f, 0.25f, nullptr, 0}, /* R LFO phase offset; 0.25 = quadrature */

    /* ---- flanger (S34) ---- */
    {FX_PID_FLG_MIX, "fx.flg.mix", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_FLG_RATE, "fx.flg.rate", ParamType::Float, ParamCurve::Exp,
     0.02f, 8.0f, 0.25f, nullptr, 0},
    {FX_PID_FLG_DEPTH, "fx.flg.depth", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.7f, nullptr, 0},
    {FX_PID_FLG_DELAY, "fx.flg.delay", ParamType::Float, ParamCurve::Exp,
     0.2f, kFlgBaseMaxMs, 1.0f, nullptr, 0}, /* ms, the manual/base tap */
    /* Signed: negative inverts the comb, moving the nulls to where the peaks
     * were. That hollow, through-zero-ish half of the sound is unreachable
     * with positive feedback at any setting, so the range has to cross zero. */
    {FX_PID_FLG_FB, "fx.flg.fb", ParamType::Float, ParamCurve::Linear,
     -0.95f, 0.95f, 0.7f, nullptr, 0},
    {FX_PID_FLG_SPREAD, "fx.flg.spread", ParamType::Float, ParamCurve::Linear,
     0.0f, 0.5f, 0.25f, nullptr, 0},

    /* ---- 3-band EQ (S34) ---- */
    {FX_PID_EQ_ON, "fx.eq.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_EQ_LOW, "fx.eq.low", ParamType::Float, ParamCurve::Linear,
     -18.0f, 18.0f, 0.0f, nullptr, 0}, /* dB, low shelf */
    {FX_PID_EQ_LOFREQ, "fx.eq.lofreq", ParamType::Float, ParamCurve::Exp,
     40.0f, 500.0f, 120.0f, nullptr, 0},
    {FX_PID_EQ_MID, "fx.eq.mid", ParamType::Float, ParamCurve::Linear,
     -18.0f, 18.0f, 0.0f, nullptr, 0}, /* dB, peaking bell */
    {FX_PID_EQ_MIDFREQ, "fx.eq.midfreq", ParamType::Float, ParamCurve::Exp,
     200.0f, 6000.0f, 1000.0f, nullptr, 0},
    {FX_PID_EQ_MIDQ, "fx.eq.midq", ParamType::Float, ParamCurve::Exp,
     0.3f, 6.0f, 1.0f, nullptr, 0},
    {FX_PID_EQ_HIGH, "fx.eq.high", ParamType::Float, ParamCurve::Linear,
     -18.0f, 18.0f, 0.0f, nullptr, 0}, /* dB, high shelf */
    {FX_PID_EQ_HIFREQ, "fx.eq.hifreq", ParamType::Float, ParamCurve::Exp,
     1500.0f, 16000.0f, 6000.0f, nullptr, 0},

    /* ---- compressor (S34) ---- */
    {FX_PID_COMP_ON, "fx.comp.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_COMP_THRESH, "fx.comp.thresh", ParamType::Float, ParamCurve::Linear,
     -48.0f, 0.0f, -18.0f, nullptr, 0}, /* dBFS */
    {FX_PID_COMP_RATIO, "fx.comp.ratio", ParamType::Float, ParamCurve::Exp,
     1.0f, 20.0f, 4.0f, nullptr, 0}, /* 20 is limiting, near enough */
    {FX_PID_COMP_ATTACK, "fx.comp.attack", ParamType::Float, ParamCurve::Exp,
     1.0f, 100.0f, 10.0f, nullptr, 0}, /* ms; floor is the block, see below */
    {FX_PID_COMP_RELEASE, "fx.comp.release", ParamType::Float, ParamCurve::Exp,
     10.0f, 1000.0f, 120.0f, nullptr, 0}, /* ms */
    {FX_PID_COMP_MAKEUP, "fx.comp.makeup", ParamType::Float, ParamCurve::Linear,
     0.0f, 24.0f, 0.0f, nullptr, 0}, /* dB */
    /* Parallel ("New York") compression: the wet path is the compressed one,
     * so 1 is a plain compressor and lower values blend the uncompressed
     * transients back underneath it. Defaults to 1 because a compressor that
     * did nothing when switched on would read as a bug. */
    {FX_PID_COMP_MIX, "fx.comp.mix", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0},
    {FX_PID_COMP_KEY, "fx.comp.key", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kCompKeyCount - 1), 0.0f /* mix */, kCompKeys,
     kCompKeyCount},
    {FX_PID_COMP_SLOT, "fx.comp.slot", ParamType::Int, ParamCurve::Linear,
     0.0f, (float)(DRUM_SLOTS - 1), 0.0f, nullptr, 0}, /* key = drum */

    /* ---- stereo + output (S34) ---- */
    {FX_PID_ST_WIDTH, "fx.st.width", ParamType::Float, ParamCurve::Linear,
     0.0f, 2.0f, 1.0f, nullptr, 0}, /* 0 = mono, 1 = unchanged, 2 = doubled */
    /* Below this, the mix is forced to mono. The floor doubles as "off":
     * 20 Hz is under everything this instrument makes. */
    {FX_PID_ST_BASS, "fx.st.bass", ParamType::Float, ParamCurve::Exp,
     20.0f, 400.0f, 20.0f, nullptr, 0},
    {FX_PID_ST_MONO, "fx.st.mono", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* mono-compatibility check */
    {FX_PID_ST_AMP, "fx.st.amp", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0}, /* bus trim; the tremolo destination */
    {FX_PID_ST_PAN, "fx.st.pan", ParamType::Float, ParamCurve::Linear,
     -1.0f, 1.0f, 0.0f, nullptr, 0}, /* the auto-pan destination */

    /* ---- FX LFOs (S34) ---- */
    {FX_PID_LFO1_DEST, "fx.lfo1.dest", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kLfoDestCount - 1), 0.0f /* off */, kLfoDests,
     kLfoDestCount},
    {FX_PID_LFO1_WAVE, "fx.lfo1.wave", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kLfoWaveCount - 1), 0.0f /* sine */, kLfoWaves,
     kLfoWaveCount},
    {FX_PID_LFO1_RATE, "fx.lfo1.rate", ParamType::Float, ParamCurve::Exp,
     0.02f, 20.0f, 1.0f, nullptr, 0}, /* Hz, when sync is free */
    {FX_PID_LFO1_SYNC, "fx.lfo1.sync", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kLfoSyncCount - 1), 0.0f /* free */, kLfoSyncNames,
     kLfoSyncCount},
    {FX_PID_LFO1_DEPTH, "fx.lfo1.depth", ParamType::Float, ParamCurve::Linear,
     -1.0f, 1.0f, 0.0f, nullptr, 0}, /* signed: negative inverts the shape */
    {FX_PID_LFO1_PHASE, "fx.lfo1.phase", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* offset within the cycle */
    {FX_PID_LFO2_DEST, "fx.lfo2.dest", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kLfoDestCount - 1), 0.0f, kLfoDests, kLfoDestCount},
    {FX_PID_LFO2_WAVE, "fx.lfo2.wave", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kLfoWaveCount - 1), 0.0f, kLfoWaves, kLfoWaveCount},
    {FX_PID_LFO2_RATE, "fx.lfo2.rate", ParamType::Float, ParamCurve::Exp,
     0.02f, 20.0f, 1.0f, nullptr, 0},
    {FX_PID_LFO2_SYNC, "fx.lfo2.sync", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kLfoSyncCount - 1), 0.0f, kLfoSyncNames, kLfoSyncCount},
    {FX_PID_LFO2_DEPTH, "fx.lfo2.depth", ParamType::Float, ParamCurve::Linear,
     -1.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_LFO2_PHASE, "fx.lfo2.phase", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.25f, nullptr, 0}, /* offset from LFO1 by default */
};

const std::atomic<float>* s_p[P_COUNT];

inline float pv(PIdx i) { return s_p[i]->load(std::memory_order_relaxed); }

/* ---- LFO modulation offsets (S34) ----
 *
 * Normalized: +1 means "one full registered range up". Rewritten once per
 * block by lfo_update() and read through pvm() — never by pv(), which stays
 * the raw read for the enums, switches and integers that have no meaningful
 * in-between value. */
float s_mod[P_COUNT];

/* Parameter value with this block's modulation folded in, clamped to the
 * parameter's own registered range.
 *
 * The offset is applied in the parameter's own domain, which is why this
 * consults kParams rather than taking a range argument: adding a linear
 * offset to an Exp-curve control (cutoff, times, rates) would spend nearly
 * the whole sweep in the top octave — the same argument synth_smooth.h makes
 * for having two flavours of smoother. */
inline float pvm(PIdx i) {
    const float base = s_p[i]->load(std::memory_order_relaxed);
    const float off = s_mod[i];
    if (SYNTH_LIKELY(off == 0.0f)) return base;
    const ParamDesc& d = kParams[i];
    const float v = (d.curve == ParamCurve::Exp)
                        ? base * exp2f(off * log2f(d.max / d.min))
                        : base + off * (d.max - d.min);
    return fminf(fmaxf(v, d.min), d.max);
}

/* ---- int16 circular delay line ---- */

struct Line {
    int16_t* buf = nullptr;
    uint32_t len = 0;
    uint32_t w = 0; /* next write index */
};

size_t s_bytes_spiram = 0;
size_t s_bytes_internal = 0;

bool line_alloc(Line& l, uint32_t len) {
    int16_t* p = nullptr;
#if CONFIG_SPIRAM
    p = (int16_t*)heap_caps_calloc(len, sizeof(int16_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (p == nullptr) {
        p = (int16_t*)heap_caps_calloc(len, sizeof(int16_t),
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p == nullptr) return false;
    }
    (esp_ptr_external_ram(p) ? s_bytes_spiram : s_bytes_internal) +=
        (size_t)len * sizeof(int16_t);
    l.buf = p;
    l.len = len;
    l.w = 0;
    return true;
}

inline void line_push(Line& l, float v) {
    int32_t s = (int32_t)(v * 32767.0f);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    l.buf[l.w] = (int16_t)s;
    if (++l.w == l.len) l.w = 0;
}

/* Oldest sample — a delay of exactly len; read before line_push overwrites. */
inline float line_tap(const Line& l) {
    return (float)l.buf[l.w] * (1.0f / 32768.0f);
}

/* x[n - d] with fractional d in [1, len-3], linear interpolation; call
 * before pushing sample n. */
inline float line_read_frac(const Line& l, float d) {
    const uint32_t di = (uint32_t)d;
    const float frac = d - (float)di;
    uint32_t i0 = l.w + l.len - di;
    if (i0 >= l.len) i0 -= l.len;
    const uint32_t i1 = (i0 == 0) ? l.len - 1 : i0 - 1;
    const float a = (float)l.buf[i0];
    return (a + frac * ((float)l.buf[i1] - a)) * (1.0f / 32768.0f);
}

/* ---- per-effect dry/wet gate with incremental bypass scrub ---- */

constexpr float kMixSlew = 0.08f;      /* per block: ~90 ms for a full swing */
constexpr float kSilent = 1e-3f;
constexpr uint32_t kScrubChunk = 4096; /* samples zeroed per block while dry */

struct UnitState {
    float mix = 0.0f; /* smoothed */
    bool active = false;
    bool scrubbing = false;
    uint8_t sl = 0;   /* scrub cursor: line index / offset */
    uint32_t sp = 0;
};

/* Zeroes up to kScrubChunk samples across the unit's lines; true when done. */
bool SYNTH_RENDER_IRAM scrub_step(UnitState& u, Line* const* lines, size_t n) {
    uint32_t budget = kScrubChunk;
    while (budget > 0 && u.sl < n) {
        Line* l = lines[u.sl];
        if (l->buf == nullptr || u.sp >= l->len) {
            u.sl++;
            u.sp = 0;
            continue;
        }
        const uint32_t left = l->len - u.sp;
        const uint32_t c = (budget < left) ? budget : left;
        memset(l->buf + u.sp, 0, c * sizeof(int16_t));
        u.sp += c;
        budget -= c;
    }
    return u.sl >= n;
}

/* Smooths the unit's mix toward `target`. Returns the block mix when the
 * effect should process, or < 0 when it is dry — the caller returns
 * immediately (resetting its filter states); the tail lines are zeroed
 * here, one chunk per block, so re-enabling never replays stale audio. */
float SYNTH_RENDER_IRAM unit_gate(UnitState& u, float target,
                                  Line* const* lines, size_t n) {
    u.mix += kMixSlew * (target - u.mix);
    if (u.mix > kSilent || target > kSilent) {
        u.active = true;
        u.scrubbing = false;
        return u.mix;
    }
    if (u.active) {
        u.active = false;
        u.mix = 0.0f;
        u.scrubbing = true;
        u.sl = 0;
        u.sp = 0;
    }
    if (u.scrubbing && scrub_step(u, lines, n)) u.scrubbing = false;
    return -1.0f;
}

/* ---- optional level compensation: fx.dly.comp / fx.grn.comp / fx.rev.comp
 *      (S35) ----
 *
 * Every unit on this bus crossfades with `dry + m*(wet - dry)`, i.e. gains
 * (1-m) and m. That is equal-*gain*, and it is the right law when the wet path
 * is a phase-coherent relative of the dry one — the chorus, the flanger and
 * the phaser all sum against the dry signal on purpose, and their notches are
 * the effect. It is the wrong law for a wet path that is decorrelated from the
 * dry: those sum in power, so the output runs at sqrt((1-m)^2 + m^2), which is
 * 3 dB down at m = 0.5 and only recovers at the ends of the knob. The delay,
 * the granular delay and the reverb are the three decorrelated units here,
 * which is why these three and no others get the switch.
 *
 * Two of them lose level at the *far* end too, for reasons that have nothing
 * to do with the crossfade — the Freeverb gain staging and the granular
 * window/pan arithmetic — so `comp` also applies a per-unit wet make-up. Those
 * are derived where they are used; only the crossfade law lives here.
 *
 * What compensation deliberately does NOT do:
 *  - chase the level with an RMS servo. It would pump against the master
 *    filter, fight the compressor two stages downstream, and undo the
 *    granular's density normalization, which is doing this job properly.
 *  - restore what a 100 % wet mix genuinely removes. With no dry left there
 *    is no transient to make up; boosting the tail is not the same signal.
 *  - touch ping-pong's routing. Its left line is fed only by feedback, so at
 *    100 % wet the left channel really is `fb` down — and that asymmetry is
 *    the effect, not a gain error.
 *
 * Cost when off: identical arithmetic, two multiplies and an add instead of a
 * subtract, a multiply and an add. (The endpoints stay exact: m = 0 gives
 * 1*dry + 0*wet, m = 1 gives 0*dry + 1*wet.) Cost when on: two sqrtf per unit
 * per block, outside the sample loop.
 *
 * Headroom: the make-up is real gain arriving at the output stage's
 * soft_clip(). In float it costs nothing — a scalar multiply adds rounding
 * noise around -144 dB — but a patch already near full scale will engage the
 * clipper sooner with it on. audio_io_get_stats() reports out_peak and
 * soft_clips; that is the pair to watch. Note also that the wet lines store
 * int16, so make-up applied here lifts a line's quantization floor along with
 * its signal — which is why the reverb's constants (kRevInGain and friends)
 * are the place to buy headroom back, not this. */
/* `makeup` is the unit's fixed wet staging *times* whatever compensation it
 * asked for — the reverb's kRevWet rides in here either way — so a caller with
 * no staging of its own passes 1.0 and a caller with compensation off passes
 * exactly what it passed before the switch existed. */
struct MixGains {
    float dry, wet;
};

inline MixGains mix_gains(float m, bool comp, float makeup) {
    if (!comp) return {1.0f - m, m * makeup};
    /* fmaxf guards sqrtf against a negative from float error at the top of
     * the mix smoother's travel; pvm() already clamps `m` to 0..1. */
    return {sqrtf(fmaxf(0.0f, 1.0f - m)), sqrtf(m) * makeup};
}

/* ---- drive: waveshaper on the finished mix (S34) ----
 *
 * The graph's Shaper node, moved to where it can reach the drums and the
 * looper. First in the chain, for the reason at the top of this file.
 *
 * `drive` is peak-compensated the same way the Shaper compensates it, so
 * turning it up changes the timbre and not the level — otherwise the control
 * doubles as a volume knob and the whole mix has to be re-gained every time
 * it moves. Compensation is *peak*, not RMS: RMS is supposed to rise, that
 * is what distortion does, and the sink downstream is peak-limited.
 *
 * The tone lowpass sits after the curve rather than before it. Before, it
 * would only change which harmonics get generated; after, it removes the
 * fizz the curve just made, which is the control anyone actually wants. */

constexpr float kDrvTubeBias = 0.35f;

struct DriveFx {
    float lpl = 0.0f, lpr = 0.0f;
    osynth::dsp::Smooth s_drive, s_tone, s_level;
    UnitState u;
};

DriveFx s_drv;

/* The mode is a template argument so the curve is picked once per block
 * instead of once per sample: the same reason graph_render.cpp hoists the
 * Shaper's switch out of its kernel. */
template <int MODE>
inline float drive_shape(float x, float g, float bias0) {
    if (MODE == 1) return osynth::dsp::fold(x * g);
    if (MODE == 2) return osynth::dsp::soft_clip(x * g);
    if (MODE == 3) {
        return osynth::dsp::fast_tanh(x * g + kDrvTubeBias) - bias0;
    }
    return osynth::dsp::fast_tanh(x * g);
}

template <int MODE>
void SYNTH_RENDER_IRAM drive_kernel(DriveFx& d, float* __restrict__ bl,
                                    float* __restrict__ br, size_t frames,
                                    float g, float a, float norm, float bias0,
                                    float m) {
    for (size_t i = 0; i < frames; ++i) {
        const float wl = drive_shape<MODE>(bl[i], g, bias0) * norm;
        const float wr = drive_shape<MODE>(br[i], g, bias0) * norm;
        d.lpl += a * (wl - d.lpl);
        d.lpr += a * (wr - d.lpr);
        bl[i] += m * (d.lpl - bl[i]);
        br[i] += m * (d.lpr - br[i]);
    }
}

void SYNTH_RENDER_IRAM drive_process(float* __restrict__ bl,
                                     float* __restrict__ br, size_t frames) {
    DriveFx& d = s_drv;
    const float m = unit_gate(d.u, pvm(DRV_MIX), nullptr, 0);
    if (m < 0.0f) {
        d.lpl = d.lpr = 0.0f;
        return;
    }

    const int mode = (int)pv(DRV_MODE);
    const float g = osynth::dsp::smooth_exp(d.s_drive, pvm(DRV_DRIVE));
    const float a =
        1.0f - expf(-kTwoPi * osynth::dsp::smooth_exp(d.s_tone, pvm(DRV_TONE)) /
                    kSr);
    const float lvl = osynth::dsp::smooth_lin(d.s_level, pvm(DRV_LEVEL));

    /* Per-mode peak normalization, computed once for the block. fold and
     * clip are bounded by construction and need none. */
    float norm = lvl;
    float bias0 = 0.0f;
    if (mode == 0) {
        norm = lvl / osynth::dsp::fast_tanh(g);
    } else if (mode == 3) {
        /* The tube curve is asymmetric, which is the point of it — and the
         * reason this has to take the *larger* of the two excursions. The
         * negative one is bigger (the bias pushes that side further down the
         * curve), and normalizing on the positive peak alone would send a
         * full-scale input past -1 and into the output-stage clipper. */
        bias0 = osynth::dsp::fast_tanh(kDrvTubeBias);
        const float pos = osynth::dsp::fast_tanh(g + kDrvTubeBias) - bias0;
        const float neg = bias0 - osynth::dsp::fast_tanh(kDrvTubeBias - g);
        norm = lvl / fmaxf(pos, neg);
    }

    switch (mode) {
        case 1: drive_kernel<1>(d, bl, br, frames, g, a, norm, bias0, m); break;
        case 2: drive_kernel<2>(d, bl, br, frames, g, a, norm, bias0, m); break;
        case 3: drive_kernel<3>(d, bl, br, frames, g, a, norm, bias0, m); break;
        default: drive_kernel<0>(d, bl, br, frames, g, a, norm, bias0, m); break;
    }
}

/* ---- chorus: one modulated tap per channel, right LFO in quadrature ---- */

constexpr uint32_t kChoLen =
    (uint32_t)((kChoBaseMs + kChoDepthMaxMs + 2.0f) * 0.001f * kSr) + 4;

struct ChorusFx {
    Line l, r;
    float phase = 0.0f;
    float dl = -1.0f, dr = -1.0f; /* block-boundary tap delays; < 0 = snap */
    osynth::dsp::Smooth s_depth;
    UnitState u;
    bool ok = false;
};

ChorusFx s_cho;

void SYNTH_RENDER_IRAM chorus_process(float* __restrict__ bl,
                                      float* __restrict__ br, size_t frames) {
    ChorusFx& c = s_cho;
    if (!c.ok) return;
    Line* const lines[] = {&c.l, &c.r};
    const float m = unit_gate(c.u, pvm(CHO_MIX), lines, 2);
    if (m < 0.0f) {
        c.dl = c.dr = -1.0f;
        return;
    }

    c.phase += pvm(CHO_RATE) * (float)frames / kSr;
    c.phase -= (float)(int)c.phase;
    float pr = c.phase + 0.25f;
    pr -= (float)(int)pr;

    /* Target tap delays for this block boundary; the per-sample ramp between
     * boundaries keeps the delay continuous (the LFO is grossly oversampled
     * by the 1.33 ms block rate). */
    const float base = kChoBaseMs * 0.001f * kSr;
    const float depth =
        osynth::dsp::smooth_lin(c.s_depth, pvm(CHO_DEPTH)) * 0.001f * kSr;
    const float tl = base + depth * (0.5f + 0.5f * sinf(kTwoPi * c.phase));
    const float tr = base + depth * (0.5f + 0.5f * sinf(kTwoPi * pr));
    if (c.dl < 0.0f) {
        c.dl = tl;
        c.dr = tr;
    }
    const float stepl = (tl - c.dl) / (float)frames;
    const float stepr = (tr - c.dr) / (float)frames;

    float dl = c.dl, dr = c.dr;
    for (size_t i = 0; i < frames; ++i) {
        dl += stepl;
        dr += stepr;
        const float wl = line_read_frac(c.l, dl);
        const float wr = line_read_frac(c.r, dr);
        line_push(c.l, bl[i]);
        line_push(c.r, br[i]);
        bl[i] += m * (wl - bl[i]);
        br[i] += m * (wr - br[i]);
    }
    c.dl = tl;
    c.dr = tr;
}

/* ---- flanger: short modulated delay with signed feedback (S34) ----
 *
 * The chorus one section up is the same topology with the feedback removed
 * and a ten-times-longer base tap, and that is exactly why this is a
 * separate unit and not a preset of it: at 12 ms the comb teeth are too
 * close together to hear individually (it reads as detuning), at 1 ms they
 * are a resonant filter. Its own line, too — sharing the chorus's would have
 * coupled two independent modulators to one write head.
 *
 * Feedback is signed. Negative inverts the comb, putting nulls where the
 * peaks were; that hollow half of the sound is unreachable with positive
 * feedback at any setting. */

constexpr uint32_t kFlgLen =
    (uint32_t)((kFlgBaseMaxMs + kFlgSweepMs + 2.0f) * 0.001f * kSr) + 4;

struct FlangerFx {
    Line l, r;
    float phase = 0.0f;
    float dl = -1.0f, dr = -1.0f; /* block-boundary tap delays; < 0 = snap */
    osynth::dsp::Smooth s_depth, s_delay, s_fb;
    UnitState u;
    bool ok = false;
};

FlangerFx s_flg;

void SYNTH_RENDER_IRAM flanger_process(float* __restrict__ bl,
                                       float* __restrict__ br, size_t frames) {
    FlangerFx& f = s_flg;
    if (!f.ok) return;
    Line* const lines[] = {&f.l, &f.r};
    const float m = unit_gate(f.u, pvm(FLG_MIX), lines, 2);
    if (m < 0.0f) {
        f.dl = f.dr = -1.0f;
        return;
    }

    f.phase += pvm(FLG_RATE) * (float)frames / kSr;
    f.phase -= (float)(int)f.phase;
    float pr = f.phase + pvm(FLG_SPREAD);
    pr -= (float)(int)pr;

    const float base =
        osynth::dsp::smooth_exp(f.s_delay, pvm(FLG_DELAY)) * 0.001f * kSr;
    const float sweep =
        osynth::dsp::smooth_lin(f.s_depth, pvm(FLG_DEPTH)) * kFlgSweepMs *
        0.001f * kSr;
    const float fb = osynth::dsp::smooth_lin(f.s_fb, pvm(FLG_FB));

    /* Same per-sample ramp between block-boundary targets the chorus uses:
     * the LFO is grossly oversampled by the block rate, and a stepped tap
     * would click on every boundary. Floor at 1 sample — line_read_frac()
     * needs at least that much to interpolate against. */
    const float tl = fmaxf(1.0f, base + sweep * (0.5f + 0.5f * sinf(kTwoPi * f.phase)));
    const float tr = fmaxf(1.0f, base + sweep * (0.5f + 0.5f * sinf(kTwoPi * pr)));
    if (f.dl < 0.0f) {
        f.dl = tl;
        f.dr = tr;
    }
    const float stepl = (tl - f.dl) / (float)frames;
    const float stepr = (tr - f.dr) / (float)frames;

    float dl = f.dl, dr = f.dr;
    for (size_t i = 0; i < frames; ++i) {
        dl += stepl;
        dr += stepr;
        const float wl = line_read_frac(f.l, dl);
        const float wr = line_read_frac(f.r, dr);
        /* Saturate the feedback path, unlike the delay: a 1 ms comb at 0.95
         * has an order of magnitude more resonant gain than a 350 ms one, and
         * without this the runaway lands as a hard clamp inside line_push(). */
        line_push(f.l, osynth::dsp::soft_clip(bl[i] + fb * wl));
        line_push(f.r, osynth::dsp::soft_clip(br[i] + fb * wr));
        bl[i] += m * (wl - bl[i]);
        br[i] += m * (wr - br[i]);
    }
    f.dl = tl;
    f.dr = tr;
}

/* ---- phaser: swept allpass chain (S34) ----
 *
 * No delay line at all — which is the point. A flanger's notches are
 * harmonically spaced (they are a comb); a phaser's are not, because each
 * first-order allpass contributes its own frequency-dependent phase shift.
 * That is a different sound, not a longer or shorter version of the same one.
 *
 * `stages` is the number of allpass sections; notches appear at roughly half
 * that count. Odd values are allowed and useful — the notch pattern is
 * asymmetric rather than wrong.
 *
 * Coefficients are per block, not per sample: at 750 blocks/s against an LFO
 * capped at 8 Hz the sweep is oversampled ~90x, and a per-sample tanf() would
 * cost more than the whole rest of the unit. */

struct PhaserFx {
    float st[2][kPhsStagesMax] = {};
    float fbl = 0.0f, fbr = 0.0f;
    float phase = 0.0f;
    int stages = 0; /* what the state above was last run with */
    osynth::dsp::Smooth s_center, s_depth, s_fb;
    UnitState u;
};

PhaserFx s_phs;

inline float phs_coef(float fc) {
    fc = fminf(fmaxf(fc, 20.0f), kSr * 0.45f);
    const float t = tanf(3.14159265f * fc / kSr);
    return (t - 1.0f) / (t + 1.0f);
}

void SYNTH_RENDER_IRAM phaser_process(float* __restrict__ bl,
                                      float* __restrict__ br, size_t frames) {
    PhaserFx& p = s_phs;
    const float m = unit_gate(p.u, pvm(PHS_MIX), nullptr, 0);
    if (m < 0.0f) {
        memset(p.st, 0, sizeof(p.st));
        p.fbl = p.fbr = 0.0f;
        p.stages = 0;
        return;
    }

    int stages = (int)pv(PHS_STAGES);
    if (stages < 1) stages = 1;
    if (stages > kPhsStagesMax) stages = kPhsStagesMax;
    if (stages != p.stages) {
        /* Sections that were not running hold whatever they last saw, which
         * on re-entry is a burst of stale audio. Clear the lot: changing the
         * stage count is a timbre edit, not something to interpolate. */
        memset(p.st, 0, sizeof(p.st));
        p.fbl = p.fbr = 0.0f;
        p.stages = stages;
    }

    const float center = osynth::dsp::smooth_exp(p.s_center, pvm(PHS_CENTER));
    const float depth = osynth::dsp::smooth_lin(p.s_depth, pvm(PHS_DEPTH));
    const float fb = osynth::dsp::smooth_lin(p.s_fb, pvm(PHS_FB));

    p.phase += pvm(PHS_RATE) * (float)frames / kSr;
    p.phase -= (float)(int)p.phase;
    float pr = p.phase + pvm(PHS_SPREAD);
    pr -= (float)(int)pr;

    /* Sweep is symmetric in the log domain — +-2 octaves around center at
     * full depth — because that is what reads as an even sweep by ear. */
    const float al = phs_coef(center * exp2f(2.0f * depth *
                                             sinf(kTwoPi * p.phase)));
    const float ar = phs_coef(center * exp2f(2.0f * depth * sinf(kTwoPi * pr)));

    float* __restrict__ sl = p.st[0];
    float* __restrict__ sr = p.st[1];
    for (size_t i = 0; i < frames; ++i) {
        /* Feedback is *subtracted*, and that is not a taste decision. A first
         * order allpass has H(1) = 1, so the chain is +1 at DC whatever the
         * stage count and whatever the sweep is doing; adding the feedback
         * would put a 1/(1-fb) resonance — 26 dB at fb 0.95 — permanently on
         * DC, where there is nothing musical to resonate and plenty of
         * subsonic energy to run away with. Subtracting makes DC 1/(1+fb),
         * i.e. attenuated, and moves the peak to where the chain reaches -1,
         * which is inside the swept band. (An odd stage count then puts one
         * peak at Nyquist instead; harmless, and the reason the default is
         * even.) */
        float xl = bl[i] - fb * p.fbl;
        float xr = br[i] - fb * p.fbr;
        for (int k = 0; k < stages; ++k) {
            const float yl = al * xl + sl[k];
            sl[k] = xl - al * yl;
            xl = yl;
            const float yr = ar * xr + sr[k];
            sr[k] = xr - ar * yr;
            xr = yr;
        }
        /* The chain is allpass, so |loop gain| = |fb| < 1 and this cannot
         * diverge — but at 0.95 the closed loop still peaks around +26 dB at
         * the resonant frequency, which is well past what the int16 stages
         * downstream can take. Saturating the stored feedback bounds it. */
        p.fbl = osynth::dsp::soft_clip(xl);
        p.fbr = osynth::dsp::soft_clip(xr);
        bl[i] += m * (xl - bl[i]);
        br[i] += m * (xr - br[i]);
    }
}

/* ---- delay: stereo (or cross-fed ping-pong), damped feedback ---- */

constexpr float kTimeSlew = 0.025f; /* per block: tape-style glide, ~110 ms */

struct DelayFx {
    Line l, r;
    float lpl = 0.0f, lpr = 0.0f; /* feedback tone filters */
    float t = -1.0f;              /* smoothed time in samples; < 0 = snap */
    osynth::dsp::Smooth s_fb, s_tone;
    UnitState u;
    bool ok = false;
};

DelayFx s_dly;

void SYNTH_RENDER_IRAM delay_process(float* __restrict__ bl,
                                     float* __restrict__ br, size_t frames) {
    DelayFx& d = s_dly;
    if (!d.ok) return;
    Line* const lines[] = {&d.l, &d.r};
    const float m = unit_gate(d.u, pvm(DLY_MIX), lines, 2);
    if (m < 0.0f) {
        d.lpl = d.lpr = 0.0f;
        d.t = -1.0f;
        return;
    }

    /* Note-division sync (S34). The tempo comes from the clock's live tick
     * period, not from seq.tempo, so a delay locked to 1/8 stays locked when
     * the instrument is slaved to a DAW. A division longer than the line can
     * hold clamps to the line — on the classic ESP32, whose ceiling is 0.4 s,
     * that is anything from 1/4 down at ordinary tempos. */
    int div = (int)pv(DLY_DIV);
    if (div < 0 || div >= kDlyDivCount) div = 0;
    float tt = (div > 0) ? kDlyDivBeats[div] * (60.0f / fmaxf(seqarp_bpm(), 1.0f)) * kSr
                         : pvm(DLY_TIME) * kSr;
    const float tmax = (float)(d.l.len - 4);
    tt = fminf(fmaxf(tt, 2.0f), tmax);
    if (d.t < 0.0f) d.t = tt;
    const float t1 = d.t + kTimeSlew * (tt - d.t);
    const float tstep = (t1 - d.t) / (float)frames;

    /* Feedback and tone run per sample inside the loop below: a raw jump
     * would step the whole tail, so both are smoothed (S21). The delay time
     * already has its own tape-style glide above. */
    const float fb = osynth::dsp::smooth_lin(d.s_fb, pvm(DLY_FB));
    const float a =
        1.0f - expf(-kTwoPi * osynth::dsp::smooth_exp(d.s_tone, pvm(DLY_TONE)) /
                    kSr);
    const bool pp = pv(DLY_PP) >= 0.5f;

    /* Crossfade law only, make-up 1.0: the tap is a full-level copy of what
     * was pushed in — the tone lowpass sits in the feedback path, not on the
     * output — so at 100 % wet this unit is already at unity and there is
     * nothing to make up. What `comp` fixes is the 3 dB scoop in the middle of
     * the mix knob, where an echo decorrelated from the dry signal partially
     * cancels the dry gain rather than adding to it.
     *
     * Feedback is not compensated either. A sustained source through a 0.95
     * tail runs 1/(1-fb^2) — 10 dB up — and pulling that back down would be
     * removing the effect, not a gain error in it. */
    const MixGains mg = mix_gains(m, pv(DLY_COMP) >= 0.5f, 1.0f);

    float t = d.t;
    for (size_t i = 0; i < frames; ++i) {
        t += tstep;
        const float wl = line_read_frac(d.l, t);
        const float wr = line_read_frac(d.r, t);
        d.lpl += a * (wl - d.lpl);
        d.lpr += a * (wr - d.lpr);
        if (pp) { /* cross-feedback: echoes alternate right, left, right… */
            const float mono = 0.5f * (bl[i] + br[i]);
            line_push(d.l, fb * d.lpr);
            line_push(d.r, mono + fb * d.lpl);
        } else {
            line_push(d.l, bl[i] + fb * d.lpl);
            line_push(d.r, br[i] + fb * d.lpr);
        }
        bl[i] = mg.dry * bl[i] + mg.wet * wl;
        br[i] = mg.dry * br[i] + mg.wet * wr;
    }
    d.t = t1;
}

/* ---- granular delay: windowed grains scattered over a mono capture line ---- */

/* Worst-case reach behind the write head: a +12 st grain needs size*(2-1)
 * of lead room (its delay shrinks by rate-1 per output sample), plus the
 * spray ceiling; a -12 st grain's delay grows by at most size/2, which the
 * same bound covers. */
constexpr uint32_t kGrnLen =
    (uint32_t)((kGrnSizeMaxS + kGrnSprayMaxS) * kSr) + 16;

struct Grain {
    float d0 = 0.0f;    /* start delay behind the write head, samples */
    float dstep = 0.0f; /* delay drift per output sample: 1 - rate */
    float pinc = 0.0f;  /* window-phase increment: 1 / nt */
    float gl = 0.0f, gr = 0.0f; /* pan gains, density normalization baked in */
    float gm = 0.0f;            /* mono gain for the feedback path */
    uint32_t e = 0, nt = 0;     /* elapsed / total samples; e >= nt = free */
};

/* Make-up for fx.grn.comp. Two fixed losses, both structural, neither of them
 * something the spawn-time `norm` addresses:
 *
 *  - the parabolic window 4p(1-p) has RMS sqrt(8/15), i.e. -2.7 dB. `norm`
 *    tracks the expected *overlap*, not the shape of what is overlapping.
 *  - grains land at a random equal-power pan, so with theta uniform on
 *    [0, pi/2] each output channel carries E[cos^2 theta] = 0.5 of a grain's
 *    power: another 3 dB under a dry signal that is full level in both.
 *
 * Together that is 1/(0.730*0.707) = +5.7 dB, and it is the bulk of what the
 * unit loses at 100 % wet.
 *
 * The third term is the sparse case. `norm` divides by sqrt(max(1, dens*size))
 * — the max() means it never compensates *upward*, so below one grain of
 * expected overlap the unit is simply silent part of the time and gets no
 * help at all. sqrt(duty) is that correction. It is capped, because a 5 %-duty
 * setting is a sparse effect on purpose and does not want its grains 13 dB
 * hotter to average out; past the cap the level is allowed to fall away. */
constexpr float kGrnWinRms = 0.73030f;   /* sqrt(8/15), RMS of 4p(1-p) */
constexpr float kGrnPanRms = 0.70711f;   /* sqrt(E[cos^2]) over [0, pi/2] */
constexpr float kGrnCompMax = 4.0f;      /* +12 dB; duty ~0.23 and denser */

struct GranularFx {
    Line line; /* mono capture: dry input sum + fb * wet */
    Grain g[kGrainMax];
    float acc = 0.0f; /* spawn accumulator, in grains */
    uint32_t rng = 0x9e3779b9u;
    osynth::dsp::Smooth s_fb;
    UnitState u;
    bool ok = false;
};

GranularFx s_grn;

inline float grn_rand01(GranularFx& g) {
    uint32_t x = g.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g.rng = x;
    return (float)(x >> 8) * (1.0f / 16777216.0f);
}

void granular_spawn(GranularFx& g, float size_s, float pitch_st, float spray_s,
                    float dens) {
    Grain* slot = nullptr;
    for (int i = 0; i < kGrainMax; ++i) {
        if (g.g[i].e >= g.g[i].nt) {
            slot = &g.g[i];
            break;
        }
    }
    if (slot == nullptr) return; /* pool full: skip, never steal */

    const float rate = exp2f(pitch_st * (1.0f / 12.0f));
    uint32_t nt = (uint32_t)(size_s * kSr);
    if (nt < 64) nt = 64;

    /* Start delay: read margin + spray jitter, plus lead room so a
     * pitched-up grain never overruns the write head; clamped so a
     * pitched-down grain never falls off the far end of the line. The
     * per-sample delay is d0 + e*dstep (recomputed from the integer sample
     * count, not accumulated — float accumulation over a 24k-sample grain
     * could drift by whole samples and break these bounds). */
    float d0 = 3.0f + spray_s * kSr * grn_rand01(g);
    if (rate > 1.0f) d0 += (float)nt * (rate - 1.0f);
    const float dmax =
        (float)(g.line.len - 4) - (float)nt * fmaxf(1.0f - rate, 0.0f);
    if (d0 > dmax) d0 = dmax;

    /* Keep the wet level even as density/size change: expected overlap is
     * dens*size grains; uncorrelated grains sum ~ sqrt. */
    const float norm = 1.0f / sqrtf(fmaxf(1.0f, dens * size_s));
    const float pan = grn_rand01(g) * (0.25f * kTwoPi); /* equal power */

    slot->d0 = d0;
    slot->dstep = 1.0f - rate;
    slot->pinc = 1.0f / (float)nt;
    slot->gl = norm * cosf(pan);
    slot->gr = norm * sinf(pan);
    slot->gm = norm;
    slot->e = 0;
    slot->nt = nt;
}

void SYNTH_RENDER_IRAM granular_process(float* __restrict__ bl,
                                        float* __restrict__ br,
                                        size_t frames) {
    GranularFx& g = s_grn;
    if (!g.ok) return;
    Line* const lines[] = {&g.line};
    const float m = unit_gate(g.u, pvm(GRN_MIX), lines, 1);
    if (m < 0.0f) {
        for (int i = 0; i < kGrainMax; ++i) g.g[i].nt = 0;
        g.acc = 0.0f;
        return;
    }

    const float size = pvm(GRN_SIZE);
    const float dens = pvm(GRN_DENS);
    const float pitch = pvm(GRN_PITCH);
    const float spray = pvm(GRN_SPRAY);
    /* size/dens/pitch/spray are read at spawn time, so a jump only shapes
     * the *next* grain — no smoothing needed. The feedback gain is applied
     * per sample into the capture line, so it is smoothed (S21). */
    const float fb = osynth::dsp::smooth_lin(g.s_fb, pvm(GRN_FB));

    /* Read from the same per-block size/dens the spawns below use, so the
     * make-up and the grains it is scaling always agree about the duty. */
    float makeup = 1.0f;
    const bool comp = pv(GRN_COMP) >= 0.5f;
    if (comp) {
        const float duty = fminf(1.0f, dens * size);
        makeup = fminf(kGrnCompMax,
                       1.0f / (kGrnWinRms * kGrnPanRms *
                               sqrtf(fmaxf(duty, 1e-3f))));
    }
    const MixGains mg = mix_gains(m, comp, makeup);

    /* Spawns land on block boundaries (1.33 ms grid — spray jitters the
     * audible onsets anyway). */
    g.acc += dens * (float)frames / kSr;
    while (g.acc >= 1.0f) {
        g.acc -= 1.0f;
        granular_spawn(g, size, pitch, spray, dens);
    }

    for (size_t i = 0; i < frames; ++i) {
        float wl = 0.0f, wr = 0.0f, wm = 0.0f;
        for (int k = 0; k < kGrainMax; ++k) {
            Grain& gn = g.g[k];
            if (gn.e >= gn.nt) continue;
            const float e = (float)gn.e;
            const float p = e * gn.pinc;
            const float w = 4.0f * p * (1.0f - p); /* parabolic window */
            const float s = line_read_frac(g.line, gn.d0 + e * gn.dstep) * w;
            wl += s * gn.gl;
            wr += s * gn.gr;
            wm += s * gn.gm;
            gn.e++;
        }
        /* The capture line takes the *uncompensated* wet through `wm`: the
         * feedback loop's gain is fx.grn.fb and nothing else, or toggling the
         * switch would move the loop toward runaway as a side effect. */
        line_push(g.line, 0.5f * (bl[i] + br[i]) + fb * wm);
        bl[i] = mg.dry * bl[i] + mg.wet * wl;
        br[i] = mg.dry * br[i] + mg.wet * wr;
    }
}

/* ---- reverb: Freeverb (8 LP-feedback combs + 4 allpasses per channel) ---- */

constexpr uint32_t kCombTune[8] = {1116, 1188, 1277, 1356,
                                   1422, 1491, 1557, 1617};
constexpr uint32_t kApTune[4] = {556, 441, 341, 225};
constexpr uint32_t kSpread = 23; /* right-channel offset, pre-scale */

/* 44.1 kHz tuning -> this build's sample rate. */
constexpr uint32_t rv(uint32_t n44) {
    return (uint32_t)(((uint64_t)n44 * SYNTH_SAMPLE_RATE + 22050u) / 44100u);
}

/* Gain staging for the int16 lines: the input is padded into the combs and
 * the comb sum padded again into the allpasses, made up at the wet output.
 * The product kRevInGain * kRevPreAp * kRevWet = 0.045 equals Freeverb's
 * fixedgain (0.015) * scalewet (3), so the perceived level is the classic
 * one while every stored signal keeps real headroom above the 16-bit floor. */
constexpr float kRevInGain = 0.06f;
constexpr float kRevPreAp = 0.25f;
constexpr float kRevWet = 3.0f;

/* Make-up for fx.rev.comp.
 *
 * The classic staging above is also the reason a 100 % wet Freeverb has always
 * been much quieter than its input. Against broadband material each comb has
 * power gain 1/(1 - fb^2), and the eight of them are tuned to mutually prime
 * lengths so their outputs sum in power rather than amplitude. The wet path
 * therefore runs at
 *
 *     kRevRefGain * sqrt(8) / sqrt(1 - fb^2)
 *
 * against a mono-correlated input — about 0.49, or -6.2 dB, at the default
 * size, and it moves with `size` because fb does. Inverting it does two
 * things: it puts the wet back at dry level, and it makes `size` a room
 * control instead of a loudness control (a bigger room currently arrives
 * louder as well as longer, which is most of why the size knob is hard to
 * audition).
 *
 * Damping is deliberately left out. It removes real energy from the tail,
 * and compensating a control whose entire job is to darken the reverb would
 * turn it into a tone control; bright material stays a little under unity at
 * high damp, which is the honest answer.
 *
 * The clamp bounds what is otherwise an unbounded 1/sqrt as fb approaches 1.
 * At the registered size range fb runs 0.70..0.98 and the make-up runs
 * 2.80..0.78, so the clamp is slack — it exists for the modulated case, where
 * an LFO on fx.rev.size drives fb, and for the arithmetic, not for the knob. */
constexpr float kRevRefGain = kRevInGain * kRevPreAp * kRevWet * 2.0f;
constexpr float kRevCombSum = 2.8284271f; /* sqrt(8) */
constexpr float kRevCompMin = 0.25f;
constexpr float kRevCompMax = 4.0f;

struct Comb {
    Line line;
    float store = 0.0f; /* damping LP state */
};

struct ReverbFx {
    Comb cl[8], cr[8];
    Line al[4], ar[4];
    osynth::dsp::Smooth s_fb, s_damp;
    UnitState u;
    bool ok = false;
};

ReverbFx s_rev;
Line* s_rev_lines[24]; /* every line, for the bypass scrub */

inline float comb_next(Comb& c, float in, float fb, float damp) {
    const float out = line_tap(c.line);
    c.store = out + damp * (c.store - out);
    line_push(c.line, in + fb * c.store);
    return out;
}

inline float allpass_next(Line& l, float in) {
    const float b = line_tap(l);
    line_push(l, in + 0.5f * b);
    return b - in;
}

void SYNTH_RENDER_IRAM reverb_process(float* __restrict__ bl,
                                      float* __restrict__ br, size_t frames) {
    ReverbFx& v = s_rev;
    if (!v.ok) return;
    const float m = unit_gate(v.u, pvm(REV_MIX), s_rev_lines, 24);
    if (m < 0.0f) {
        for (int i = 0; i < 8; ++i) v.cl[i].store = v.cr[i].store = 0.0f;
        return;
    }

    /* Both feed the comb loop per sample: a raw jump steps the running tail
     * (a size change is audible as a click on a long decay). Smoothed S21. */
    const float fb =
        0.70f + 0.28f * osynth::dsp::smooth_lin(v.s_fb, pvm(REV_SIZE));
    const float damp =
        0.95f * osynth::dsp::smooth_lin(v.s_damp, pvm(REV_DAMP));

    /* kRevWet is folded into the wet gain here rather than multiplied per
     * sample below, which is where the make-up joins it. */
    float makeup = kRevWet;
    const bool comp = pv(REV_COMP) >= 0.5f;
    if (comp) {
        const float g = sqrtf(fmaxf(1.0f - fb * fb, 1e-4f)) /
                        (kRevRefGain * kRevCombSum);
        makeup = kRevWet * fminf(fmaxf(g, kRevCompMin), kRevCompMax);
    }
    const MixGains mg = mix_gains(m, comp, makeup);

    for (size_t i = 0; i < frames; ++i) {
        const float in = (bl[i] + br[i]) * kRevInGain;
        float sl = 0.0f, sr = 0.0f;
        for (int c = 0; c < 8; ++c) {
            sl += comb_next(v.cl[c], in, fb, damp);
            sr += comb_next(v.cr[c], in, fb, damp);
        }
        sl *= kRevPreAp;
        sr *= kRevPreAp;
        for (int a = 0; a < 4; ++a) {
            sl = allpass_next(v.al[a], sl);
            sr = allpass_next(v.ar[a], sr);
        }
        bl[i] = mg.dry * bl[i] + mg.wet * sl;
        br[i] = mg.dry * br[i] + mg.wet * sr;
    }
}

/* ---- bitcrush: bit-depth quantize + sample-rate divide (S17) ----
 *
 * Master lo-fi, last in the chain so the crunch prints on everything.
 * Quantize step is 2^(bits-1) (fractional bits allowed — the step just
 * lands between powers of two); the rate divider holds each sampled value
 * for `down` output samples (naive zero-order hold: the aliasing IS the
 * effect). No delay lines, so the bypass gate has nothing to scrub. */

struct CrushFx {
    float hl = 0.0f, hr = 0.0f; /* held (quantized) samples */
    uint32_t cnt = 0;           /* samples until the next capture */
    osynth::dsp::Smooth s_bits;
    UnitState u;
};

CrushFx s_crush;

void SYNTH_RENDER_IRAM crush_process(float* __restrict__ bl,
                                     float* __restrict__ br, size_t frames) {
    CrushFx& c = s_crush;
    const float m = unit_gate(c.u, pvm(CRUSH_MIX), nullptr, 0);
    if (m < 0.0f) {
        c.hl = c.hr = 0.0f;
        c.cnt = 0;
        return;
    }

    /* bits is already a log-domain control, so it smooths linearly; the
     * rate divider is an integer hold count and cannot be ramped. */
    const float q = exp2f(osynth::dsp::smooth_lin(c.s_bits, pvm(CRUSH_BITS)) -
                          1.0f);
    const float iq = 1.0f / q;
    int32_t down = (int32_t)pv(CRUSH_DOWN);
    if (down < 1) down = 1;

    for (size_t i = 0; i < frames; ++i) {
        if (c.cnt == 0) {
            c.hl = floorf(bl[i] * q + 0.5f) * iq;
            c.hr = floorf(br[i] * q + 0.5f) * iq;
            c.cnt = (uint32_t)down;
        }
        c.cnt--;
        bl[i] += m * (c.hl - bl[i]);
        br[i] += m * (c.hr - br[i]);
    }
}

/* ---- master filter (S33): the voice filter family, run on the bus ----
 *
 * Everything the engines can do to one voice, applied to the finished mix —
 * which means it also filters the drums and the looper, neither of which
 * goes anywhere near a voice filter. That is the reason it exists: a
 * build-up that closes down the *whole* track was not previously expressible
 * anywhere in this synth.
 *
 * Cheap by construction: two channels once per block, against eight voices
 * once per block upstream. One coefficient build serves both channels —
 * they share every parameter and differ only in their filter state, so
 * there is no per-voice cutoff spread to pay for here.
 *
 * Placed last in the chain, after the reverb and the crusher, so the sweep
 * takes the tails and the quantization noise with it. A filter before the
 * reverb leaves a bright tail hanging over a closed filter, which sounds
 * like a mistake rather than an effect. */

struct FilterFx {
    osynth::dsp::Filt l, r;
    osynth::dsp::Smooth s_cut, s_reso, s_drive, s_spread, s_vowel;
    UnitState u;
};

FilterFx s_flt;

void SYNTH_RENDER_IRAM filter_process(float* __restrict__ bl,
                                      float* __restrict__ br, size_t frames) {
    FilterFx& c = s_flt;
    const float m = unit_gate(c.u, pv(FLT_ON), nullptr, 0);
    if (m < 0.0f) {
        /* Off: drop the filter states so switching back on cannot replay a
         * resonant ring from whatever was playing minutes ago. */
        c.l = osynth::dsp::Filt{};
        c.r = osynth::dsp::Filt{};
        return;
    }

    const osynth::dsp::FiltCoef fc = osynth::dsp::filt_coef(
        (osynth::dsp::FltType)(int)pv(FLT_TYPE),
        (osynth::dsp::SvfMode)(int)pv(FLT_MODE),
        osynth::dsp::smooth_exp(c.s_cut, pvm(FLT_CUTOFF)),
        osynth::dsp::smooth_lin(c.s_reso, pvm(FLT_RESO)),
        osynth::dsp::smooth_lin(c.s_drive, pvm(FLT_DRIVE)),
        osynth::dsp::smooth_lin(c.s_spread, pvm(FLT_SPREAD)),
        osynth::dsp::smooth_lin(c.s_vowel, pvm(FLT_VOWEL)), kSr);

    for (size_t i = 0; i < frames; ++i) {
        bl[i] += m * (osynth::dsp::filt_next(c.l, fc, bl[i]) - bl[i]);
        br[i] += m * (osynth::dsp::filt_next(c.r, fc, br[i]) - br[i]);
    }
}

/* ---- 3-band EQ (S34): RBJ shelves + a sweepable bell ----
 *
 * Not a second master filter. The S33 filter is a resonant, sweepable,
 * performance control that removes whole bands; this is a corrective one that
 * tilts them, and the two are wanted at the same time often enough that
 * sharing would have been the wrong economy.
 *
 * Biquads rather than the SVF family upstream: a shelf is what an EQ is for,
 * and the Cytomic SVF has no shelving output. Direct Form I — with float
 * state and coefficients rebuilt every block, DF1's larger state is the
 * cheaper end of the trade against DF2's coefficient-change transients.
 *
 * A band parked at 0 dB is skipped outright, so a flat EQ with fx.eq.on set
 * costs one compare per band. That matters: "on but flat" is the state the
 * unit spends most of its life in while someone reaches for one knob. */

struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
};

struct BiquadState {
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
};

inline float bq_next(BiquadState& s, const Biquad& c,
                                       float x) {
    const float y =
        c.b0 * x + c.b1 * s.x1 + c.b2 * s.x2 - c.a1 * s.y1 - c.a2 * s.y2;
    s.x2 = s.x1;
    s.x1 = x;
    s.y2 = s.y1;
    s.y1 = y;
    return y;
}

/* RBJ cookbook. `shelf` picks low (-1) / high (+1); Q is the bell's, and is
 * ignored by the shelves (both run at the S = 1 slope, the one that does not
 * overshoot). */
Biquad bq_shelf(float f0, float gain_db, int shelf) {
    const float A = powf(10.0f, gain_db * (1.0f / 40.0f));
    const float w0 = kTwoPi * fminf(f0, kSr * 0.45f) / kSr;
    const float cw = cosf(w0);
    const float alpha = sinf(w0) * 0.5f * 1.41421356f; /* S = 1 */
    const float tsa = 2.0f * sqrtf(A) * alpha;
    const float ap = A + 1.0f, am = A - 1.0f;
    Biquad c;
    float a0;
    if (shelf < 0) {
        c.b0 = A * (ap - am * cw + tsa);
        c.b1 = 2.0f * A * (am - ap * cw);
        c.b2 = A * (ap - am * cw - tsa);
        a0 = ap + am * cw + tsa;
        c.a1 = -2.0f * (am + ap * cw);
        c.a2 = ap + am * cw - tsa;
    } else {
        c.b0 = A * (ap + am * cw + tsa);
        c.b1 = -2.0f * A * (am + ap * cw);
        c.b2 = A * (ap + am * cw - tsa);
        a0 = ap - am * cw + tsa;
        c.a1 = 2.0f * (am - ap * cw);
        c.a2 = ap - am * cw - tsa;
    }
    const float ia = 1.0f / a0;
    c.b0 *= ia; c.b1 *= ia; c.b2 *= ia; c.a1 *= ia; c.a2 *= ia;
    return c;
}

Biquad bq_peak(float f0, float gain_db, float q) {
    const float A = powf(10.0f, gain_db * (1.0f / 40.0f));
    const float w0 = kTwoPi * fminf(f0, kSr * 0.45f) / kSr;
    const float cw = cosf(w0);
    const float alpha = sinf(w0) / (2.0f * fmaxf(q, 0.05f));
    Biquad c;
    const float a0 = 1.0f + alpha / A;
    const float ia = 1.0f / a0;
    c.b0 = (1.0f + alpha * A) * ia;
    c.b1 = (-2.0f * cw) * ia;
    c.b2 = (1.0f - alpha * A) * ia;
    c.a1 = (-2.0f * cw) * ia;
    c.a2 = (1.0f - alpha / A) * ia;
    return c;
}

/* Below this a band is the identity to well under the 16-bit floor, so
 * building and running it would be pure cost. */
constexpr float kEqFlatDb = 0.05f;

struct EqFx {
    BiquadState lo[2], mid[2], hi[2];
    bool on_lo = false, on_mid = false, on_hi = false; /* last block's skips */
    osynth::dsp::Smooth s_lo, s_lof, s_mid, s_midf, s_midq, s_hi, s_hif;
    UnitState u;
};

EqFx s_eq;

void SYNTH_RENDER_IRAM eq_process(float* __restrict__ bl,
                                  float* __restrict__ br, size_t frames) {
    EqFx& e = s_eq;
    const float m = unit_gate(e.u, pv(EQ_ON), nullptr, 0);
    if (m < 0.0f) {
        e.lo[0] = e.lo[1] = BiquadState{};
        e.mid[0] = e.mid[1] = BiquadState{};
        e.hi[0] = e.hi[1] = BiquadState{};
        e.on_lo = e.on_mid = e.on_hi = false;
        return;
    }

    const float glo = osynth::dsp::smooth_lin(e.s_lo, pvm(EQ_LOW));
    const float gmid = osynth::dsp::smooth_lin(e.s_mid, pvm(EQ_MID));
    const float ghi = osynth::dsp::smooth_lin(e.s_hi, pvm(EQ_HIGH));
    const bool do_lo = fabsf(glo) > kEqFlatDb;
    const bool do_mid = fabsf(gmid) > kEqFlatDb;
    const bool do_hi = fabsf(ghi) > kEqFlatDb;

    /* A band that has just gone flat stops being run, and its state would sit
     * there holding whatever it last saw — for minutes, if the knob is left
     * alone — to be emitted as a transient the moment it comes back. Clear on
     * the falling edge. Harmless at the crossing itself: a band within
     * 0.05 dB of flat is the identity, so its state *is* the recent input. */
    if (e.on_lo && !do_lo) e.lo[0] = e.lo[1] = BiquadState{};
    if (e.on_mid && !do_mid) e.mid[0] = e.mid[1] = BiquadState{};
    if (e.on_hi && !do_hi) e.hi[0] = e.hi[1] = BiquadState{};
    e.on_lo = do_lo;
    e.on_mid = do_mid;
    e.on_hi = do_hi;

    if (!do_lo && !do_mid && !do_hi) return; /* on, but flat */

    Biquad clo, cmid, chi;
    if (do_lo) {
        clo = bq_shelf(osynth::dsp::smooth_exp(e.s_lof, pvm(EQ_LOFREQ)), glo, -1);
    }
    if (do_mid) {
        cmid = bq_peak(osynth::dsp::smooth_exp(e.s_midf, pvm(EQ_MIDFREQ)), gmid,
                       osynth::dsp::smooth_exp(e.s_midq, pvm(EQ_MIDQ)));
    }
    if (do_hi) {
        chi = bq_shelf(osynth::dsp::smooth_exp(e.s_hif, pvm(EQ_HIFREQ)), ghi, 1);
    }

    for (size_t i = 0; i < frames; ++i) {
        float l = bl[i], r = br[i];
        if (do_lo) {
            l = bq_next(e.lo[0], clo, l);
            r = bq_next(e.lo[1], clo, r);
        }
        if (do_mid) {
            l = bq_next(e.mid[0], cmid, l);
            r = bq_next(e.mid[1], cmid, r);
        }
        if (do_hi) {
            l = bq_next(e.hi[0], chi, l);
            r = bq_next(e.hi[1], chi, r);
        }
        bl[i] += m * (l - bl[i]);
        br[i] += m * (r - br[i]);
    }
}

/* ---- compressor with a selectable key (S34) ----
 *
 * The instrument had no dynamics at all before this, only the output-stage
 * soft clip. Two jobs, one unit, because they differ in exactly one place:
 *
 *  - key = mix: the detector listens to the bus. Glue.
 *  - key = drum: the detector listens to a drum slot's *trigger*, through a
 *    synthetic decay envelope. Sidechain ducking — and the reason the whole
 *    thing is placed late enough in the chain to catch the reverb tail, since
 *    a duck that leaves the tail up reads as a hole rather than a pump.
 *
 * There is no audio path from the drum bus to key on: drums_pre_fx() has
 * already summed its send into the mix by the time this runs, and the
 * un-sent portion never passes here at all. The trigger tap is both cheaper
 * and better behaved — it keys on the slot you chose, at the velocity it
 * actually played, with no bleed from anything else in the kit.
 *
 * Detection is per sample; the *gain* is computed once per block from the
 * block's peak envelope and ramped linearly across it. That is the same
 * block-boundary-target / per-sample-ramp shape the chorus and the delay
 * use, and it keeps one log and one exp per block out of the sample loop.
 * The cost is that gain movement lands on a 1.33 ms grid, which is why
 * fx.comp.attack has a 1 ms floor: below the block period the number would
 * be a promise the structure cannot keep. Peak, not RMS, and peak of the
 * block rather than its last sample — a transient that arrives mid-block
 * must not be able to slip through ungained. */

constexpr float kCompKnee = 6.0f; /* dB, fixed: the ducker covers hard-knee
                                   * territory and a knee control on top of
                                   * ratio is a second way to say the same
                                   * thing */
constexpr float kCompFloor = 1e-6f;

struct CompFx {
    float env = 0.0f;  /* detector */
    float key = 0.0f;  /* synthetic drum-key envelope */
    float gain = 1.0f; /* smoothed, block-boundary */
    osynth::dsp::Smooth s_thresh, s_ratio, s_atk, s_rel, s_makeup, s_mix;
    UnitState u;
};

CompFx s_comp;

/* The trigger's stand-in for a kick's amplitude envelope: without one, a
 * single-sample impulse would never charge the attack one-pole and the
 * attack control would do nothing. 60 ms is a kick's body. */
constexpr float kCompKeyDecayMs = 60.0f;

void SYNTH_RENDER_IRAM comp_process(float* __restrict__ bl,
                                    float* __restrict__ br, size_t frames) {
    CompFx& c = s_comp;
    const float m = unit_gate(c.u, pv(COMP_ON), nullptr, 0);
    if (m < 0.0f) {
        c.env = c.key = 0.0f;
        c.gain = 1.0f;
        return;
    }

    const float thresh = osynth::dsp::smooth_lin(c.s_thresh, pvm(COMP_THRESH));
    const float ratio = osynth::dsp::smooth_exp(c.s_ratio, pvm(COMP_RATIO));
    const float atk_ms = osynth::dsp::smooth_exp(c.s_atk, pvm(COMP_ATTACK));
    const float rel_ms = osynth::dsp::smooth_exp(c.s_rel, pvm(COMP_RELEASE));
    const float makeup = osynth::dsp::smooth_lin(c.s_makeup, pvm(COMP_MAKEUP));
    const float mix = osynth::dsp::smooth_lin(c.s_mix, pvm(COMP_MIX));

    const float ka = 1.0f - expf(-1.0f / (atk_ms * 0.001f * kSr));
    const float kr = 1.0f - expf(-1.0f / (rel_ms * 0.001f * kSr));
    const float kkey = expf(-1.0f / (kCompKeyDecayMs * 0.001f * kSr));

    /* Pass 1: detector. Nothing is written to the bus yet — the gain below is
     * derived from this whole block's peak, so it cannot be applied until the
     * block has been seen. */
    float peak = 0.0f;
    if ((int)pv(COMP_KEY) == 1) {
        uint16_t at = 0;
        const uint8_t vel = drums_block_hit((int)pv(COMP_SLOT), &at);
        const float lvl = (float)vel * (1.0f / 127.0f);
        /* Micro-timing can in principle push a hit past this block's end, and
         * the tap reports it only once. Firing at the last sample is one
         * block early at worst; dropping it would lose the duck entirely. */
        if ((size_t)at >= frames) at = (uint16_t)(frames - 1);
        for (size_t i = 0; i < frames; ++i) {
            if (vel != 0 && (size_t)at == i) c.key = lvl;
            const float t = c.key;
            c.key *= kkey;
            c.env += ((t > c.env) ? ka : kr) * (t - c.env);
            if (c.env > peak) peak = c.env;
        }
    } else {
        c.key = 0.0f;
        for (size_t i = 0; i < frames; ++i) {
            const float al = fabsf(bl[i]), ar = fabsf(br[i]);
            const float t = (al > ar) ? al : ar;
            c.env += ((t > c.env) ? ka : kr) * (t - c.env);
            if (c.env > peak) peak = c.env;
        }
    }

    /* Gain computer, in dB, with a fixed soft knee. */
    const float db = 20.0f * log10f(fmaxf(peak, kCompFloor));
    const float over = db - thresh;
    const float slope = 1.0f - 1.0f / fmaxf(ratio, 1.0f);
    float gr;
    if (over <= -0.5f * kCompKnee) {
        gr = 0.0f;
    } else if (over >= 0.5f * kCompKnee) {
        gr = slope * over;
    } else {
        const float t = over + 0.5f * kCompKnee;
        gr = slope * t * t / (2.0f * kCompKnee);
    }
    const float target = powf(10.0f, (makeup - gr) * (1.0f / 20.0f));

    /* Parallel blend folded into the gain: the wet path is dry*gain, so
     * dry + mix*(dry*gain - dry) is one multiply per sample, not three.
     * `m` rides on top so switching the unit on crossfades rather than
     * stepping, exactly like every dry/wet unit here. */
    const float wet = m * mix;
    const float g0 = 1.0f + wet * (c.gain - 1.0f);
    const float g1 = 1.0f + wet * (target - 1.0f);
    const float gstep = (g1 - g0) / (float)frames;

    float g = g0;
    for (size_t i = 0; i < frames; ++i) {
        g += gstep;
        bl[i] *= g;
        br[i] *= g;
    }
    c.gain = target;
}

/* ---- stereo + output (S34) ----
 *
 * Last in the chain, because it is the only place where the total width of
 * the mix is decided: ping-pong delay, the granular panner and the chorus's
 * quadrature LFO all throw energy wide upstream of here, and none of them
 * knew about each other.
 *
 * `amp` and `pan` live here rather than in audio_io's master volume for one
 * reason: the FX LFOs need somewhere to put tremolo and auto-pan, and the
 * master volume is a user-facing control that a modulator has no business
 * moving. `pan` is a balance, not a panner — it only ever attenuates a side.
 * A master stage that boosts one channel to centre something is applying
 * gain nobody asked for, right where the headroom is tightest.
 *
 * Neutral settings return immediately. Because the test runs on the
 * *smoothed* values, easing back to neutral fades out rather than dropping
 * out, and a modulated amp or pan keeps the unit alive on its own. */

constexpr float kStNeutralEps = 1e-4f;

struct StereoFx {
    float lp = 0.0f; /* bass-mono crossover state, on the side signal */
    osynth::dsp::Smooth s_width, s_bass, s_amp, s_pan;
};

StereoFx s_st;

void SYNTH_RENDER_IRAM stereo_process(float* __restrict__ bl,
                                      float* __restrict__ br, size_t frames) {
    StereoFx& s = s_st;
    /* Mono is a width of zero fed through the same smoother, not a branch
     * around it: switching it on has to collapse the image over a few blocks
     * like every other control here, not step. */
    const bool mono = pv(ST_MONO) >= 0.5f;
    const float width = osynth::dsp::smooth_lin(
        s.s_width, mono ? 0.0f : pvm(ST_WIDTH));
    const float bass = osynth::dsp::smooth_exp(s.s_bass, pvm(ST_BASS));
    const float amp = osynth::dsp::smooth_lin(s.s_amp, pvm(ST_AMP));
    const float pan = osynth::dsp::smooth_lin(s.s_pan, pvm(ST_PAN));

    const bool do_width = fabsf(width - 1.0f) > kStNeutralEps;
    const bool do_bass = bass > 20.0f + kStNeutralEps;
    const bool do_amp = fabsf(amp - 1.0f) > kStNeutralEps;
    const bool do_pan = fabsf(pan) > kStNeutralEps;
    if (!do_width && !do_bass && !do_amp && !do_pan && !mono) {
        s.lp = 0.0f;
        return;
    }

    /* Balance, never boost: unity on the near side, attenuate the far one. */
    const float gl = amp * ((pan > 0.0f) ? (1.0f - pan) : 1.0f);
    const float gr = amp * ((pan < 0.0f) ? (1.0f + pan) : 1.0f);
    const float a = 1.0f - expf(-kTwoPi * bass / kSr);

    for (size_t i = 0; i < frames; ++i) {
        const float mid = 0.5f * (bl[i] + br[i]);
        float side = 0.5f * (bl[i] - br[i]);
        if (do_bass) {
            /* Keep only the part of the side signal above the crossover:
             * everything below it collapses to the middle, which is what
             * makes the low end survive a mono fold-down. */
            s.lp += a * (side - s.lp);
            side -= s.lp;
        }
        side *= width;
        bl[i] = (mid + side) * gl;
        br[i] = (mid - side) * gr;
    }
}

/* ---- FX LFOs (S34) ----
 *
 * Two block-rate LFOs, one destination each, writing normalized offsets into
 * s_mod[] for pvm() to fold in. They exist because the S9 mod matrix is
 * evaluated per voice against per-voice parameters: nothing in the instrument
 * could modulate the master bus, so a tempo-locked filter sweep over a whole
 * track — drums, looper and all — was not expressible anywhere.
 *
 * Rate locking, and how far it goes. In sync mode the rate comes from
 * seqarp_bpm(), so it follows tempo changes and an external MIDI clock
 * exactly. Phase is re-anchored on every downbeat of the same free-running
 * beat grid the looper bar-locks to, which is why a "1 bar" LFO stays on the
 * bar instead of drifting: between downbeats the phase accumulates smoothly
 * at block rate, and at each one it is snapped to where the elapsed bar count
 * says it should be. The snap is at most one block wide and lands on the
 * cycle boundary, where a saw's discontinuity already is.
 *
 * What it does *not* do is follow the sequencer's transport: the grid free-
 * runs, so bar 0 is wherever the clock started, not where you pressed play.
 * fx.lfoN.phase is the manual alignment for that.
 */

struct FxLfo {
    float phase = 0.0f;
    float sh = 0.0f; /* sample & hold: the currently held value */
    uint32_t rng = 0x2545f491u;
};

FxLfo s_lfo[2];
int s_prev_beat = -1; /* downbeat edge detector */
/* Bar counter for the multi-bar cycles, wrapped at the longest one. Every
 * division in kLfoSyncBeats completes a whole number of cycles in 8 bars, so
 * wrapping here is exact — and it keeps the float below out of the range
 * where its fractional part would start quantizing (a free-running counter
 * gets there in a few weeks of uptime). */
constexpr uint32_t kLfoBarWrap = 8;
uint32_t s_bar = 0;

inline float lfo_rand(FxLfo& l) {
    uint32_t x = l.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    l.rng = x;
    return (float)(x >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

/* Bipolar, -1..1, from a phase already wrapped into [0, 1). */
inline float lfo_shape(int wave, float p, FxLfo& l, bool wrapped) {
    switch (wave) {
        case 1: return 1.0f - 4.0f * fabsf(p - 0.5f);  /* tri */
        case 2: return 2.0f * p - 1.0f;                /* saw up */
        case 3: return 1.0f - 2.0f * p;                /* saw down */
        case 4: return (p < 0.5f) ? 1.0f : -1.0f;      /* square */
        case 5:                                        /* s&h */
            if (wrapped) l.sh = lfo_rand(l);
            return l.sh;
        default: return sinf(kTwoPi * p);              /* sine */
    }
}

void SYNTH_RENDER_IRAM lfo_update(size_t frames) {
    memset(s_mod, 0, sizeof(s_mod));

    /* One downbeat edge per bar. The beat grid ticks at 96 PPQN on the clock
     * task; at 1.33 ms blocks against beats no shorter than 200 ms (the 300
     * BPM ceiling) an edge cannot be missed. */
    const int beat = seqarp_beat_in_bar();
    const bool downbeat = (beat == 0 && s_prev_beat != 0);
    s_prev_beat = beat;
    if (downbeat) s_bar = (s_bar + 1) % kLfoBarWrap;

    const float bpm = fmaxf(seqarp_bpm(), 1.0f);

    for (int i = 0; i < 2; ++i) {
        FxLfo& l = s_lfo[i];
        const PIdx p_dest = (i == 0) ? LFO1_DEST : LFO2_DEST;
        const PIdx p_wave = (i == 0) ? LFO1_WAVE : LFO2_WAVE;
        const PIdx p_rate = (i == 0) ? LFO1_RATE : LFO2_RATE;
        const PIdx p_sync = (i == 0) ? LFO1_SYNC : LFO2_SYNC;
        const PIdx p_depth = (i == 0) ? LFO1_DEPTH : LFO2_DEPTH;
        const PIdx p_phase = (i == 0) ? LFO1_PHASE : LFO2_PHASE;

        int sync = (int)pv(p_sync);
        if (sync < 0 || sync >= kLfoSyncCount) sync = 0;
        float hz;
        if (sync == 0) {
            hz = pv(p_rate);
        } else {
            const float beats = kLfoSyncBeats[sync];
            hz = bpm / (60.0f * beats);
            if (downbeat) {
                /* Snap to where the elapsed bar count puts this cycle. Four
                 * beats to the bar, so bar B is B*4/beats cycles in. */
                float c = (float)s_bar * 4.0f / beats;
                l.phase = c - floorf(c);
            }
        }

        l.phase += hz * (float)frames / kSr;
        const bool wrapped = l.phase >= 1.0f; /* s&h steps once per cycle */
        l.phase -= floorf(l.phase);

        const int dest = (int)pv(p_dest);
        if (dest <= 0 || dest >= kLfoDestCount) continue;
        const float depth = pv(p_depth);
        if (depth == 0.0f) continue;

        float ph = l.phase + pv(p_phase);
        ph -= floorf(ph);
        const float v = lfo_shape((int)pv(p_wave), ph, l, wrapped);

        const int idx = kLfoDestIdx[dest];
        if (idx < 0) continue;
        if (idx == ST_AMP) {
            /* Tremolo ducks, it never boosts. A bipolar offset on a level
             * that already sits at its maximum would spend half its cycle
             * clamped, and the other half asking the output stage for gain
             * that is not there. Negative depth still inverts the shape —
             * it just inverts a downward one. */
            const float sv = (depth < 0.0f) ? -v : v;
            s_mod[idx] += 0.5f * (sv - 1.0f) * fabsf(depth);
        } else {
            s_mod[idx] += v * depth;
        }
    }
}

bool s_up = false;

} // namespace

esp_err_t fx_init(void) {
    ParamStore& ps = ParamStore::instance();
    const size_t added = ps.add(kParams, P_COUNT);
    if (added != P_COUNT) {
        ESP_LOGE(TAG, "registered %u/%u params", (unsigned)added,
                 (unsigned)P_COUNT);
        return ESP_FAIL;
    }
    for (size_t i = 0; i < P_COUNT; ++i) {
        s_p[i] = ps.valuePtr(kParams[i].id);
    }

    s_cho.ok = line_alloc(s_cho.l, kChoLen) && line_alloc(s_cho.r, kChoLen);
    if (!s_cho.ok) ESP_LOGW(TAG, "chorus disabled: line alloc failed");

    const uint32_t dlen = (uint32_t)(kDelayMaxS * kSr) + 8;
    s_dly.ok = line_alloc(s_dly.l, dlen) && line_alloc(s_dly.r, dlen);
    if (!s_dly.ok) ESP_LOGW(TAG, "delay disabled: line alloc failed");

    s_grn.ok = line_alloc(s_grn.line, kGrnLen);
    if (!s_grn.ok) ESP_LOGW(TAG, "granular disabled: line alloc failed");

    s_rev.ok = true;
    size_t nl = 0;
    for (int i = 0; i < 8; ++i) {
        s_rev.ok = s_rev.ok && line_alloc(s_rev.cl[i].line, rv(kCombTune[i]));
        s_rev.ok =
            s_rev.ok && line_alloc(s_rev.cr[i].line, rv(kCombTune[i] + kSpread));
        s_rev_lines[nl++] = &s_rev.cl[i].line;
        s_rev_lines[nl++] = &s_rev.cr[i].line;
    }
    for (int i = 0; i < 4; ++i) {
        s_rev.ok = s_rev.ok && line_alloc(s_rev.al[i], rv(kApTune[i]));
        s_rev.ok = s_rev.ok && line_alloc(s_rev.ar[i], rv(kApTune[i] + kSpread));
        s_rev_lines[nl++] = &s_rev.al[i];
        s_rev_lines[nl++] = &s_rev.ar[i];
    }
    if (!s_rev.ok) ESP_LOGW(TAG, "reverb disabled: line alloc failed");

    s_flg.ok = line_alloc(s_flg.l, kFlgLen) && line_alloc(s_flg.r, kFlgLen);
    if (!s_flg.ok) ESP_LOGW(TAG, "flanger disabled: line alloc failed");

    s_up = true;
    ESP_LOGI(TAG,
             "fx bus up: drive -> chorus -> flanger -> phaser -> delay -> "
             "granular -> reverb -> crush -> filter -> eq -> comp -> stereo, "
             "%u params, "
             "delay max %.2f s, %d grains / %.2f s window, buffers %u KB "
             "PSRAM + %u KB internal",
             (unsigned)P_COUNT, (double)kDelayMaxS, kGrainMax,
             (double)kGrnLen / (double)kSr, (unsigned)(s_bytes_spiram / 1024),
             (unsigned)(s_bytes_internal / 1024));
    return ESP_OK;
}

void SYNTH_RENDER_IRAM fx_process(float* l, float* r, size_t frames) {
    if (!s_up || l == nullptr || r == nullptr) return;
    /* First: every unit below reads its parameters through pvm(), which
     * consumes what this writes. */
    lfo_update(frames);

    drive_process(l, r, frames);
    chorus_process(l, r, frames);
    flanger_process(l, r, frames);
    phaser_process(l, r, frames);
    delay_process(l, r, frames);
    granular_process(l, r, frames);
    reverb_process(l, r, frames);
    crush_process(l, r, frames);
    filter_process(l, r, frames);
    eq_process(l, r, frames);
    comp_process(l, r, frames);
    stereo_process(l, r, frames);
}
