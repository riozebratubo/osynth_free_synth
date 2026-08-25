/*
 * osynth — master FX bus (Sessions 10 + 11; bitcrush S17; filter S33;
 * drive / flanger / phaser / EQ / compressor / stereo / LFOs S34; vocoder
 * S38; the two noise-reduction units S39):
 *
 *   anr -> nr -> vocoder -> drive -> chorus -> flanger -> phaser -> delay
 *       -> granular -> reverb -> bitcrush -> filter -> EQ -> compressor
 *       -> stereo/output
 *
 * That order is the whole design of the bus, so it is worth stating why:
 *
 *  - The noise reduction is first, and anr before nr, because it is a
 *    *source* cleanup rather than an effect: nothing below it should be
 *    asked to work on top of a room's air conditioning, and an estimator
 *    cannot learn a noise floor that a gate downstream has already ducked
 *    away. The full argument is above FX_PID_ANR_ON in fx.h.
 *  - The vocoder is next, because it decides what the sound *is*, so
 *    everything after it colours the spoken result rather than the carrier.
 *  - Drive leads the effects proper. Saturation belongs on the source, not
 *    on the tails;
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

#include "esp_attr.h" /* NOINLINE_ATTR: the microphone chain, below */
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "sdkconfig.h"

#include "audio_io.h" /* the vocoder's modulator: the selected audio input (S38) */
#include "synth_voice.h" /* voice_manager_block_note(): the vocoder's retrigger */
#include "drums.h"   /* the sidechain key: which drum slot sounded this block */
#include "seqarp.h"  /* seqarp_bpm() / beat grid: note-division sync (S34) */
#include "synth_config.h"
#include "synth_dsp.h"
#include "synth_line.h"
#include "synth_params.h"
#include "synth_reverb_algo.h"
#include "synth_smooth.h"
#include "synth_warn.h"

#include "fx_fft.h" /* the mic NR's transform (S42) */

/* The reverb algorithms. Below synth_config.h on purpose: SYNTH_ENABLE_FX_GPL
 * is defined there, and an unguarded fx_gpl.h would drag GPL declarations
 * into a build that deliberately excludes them. */
#include "fx_reverb_wet.h" /* algorithm 1, MIT, always built */
#if SYNTH_ENABLE_FX_GPL
#include "fx_gpl.h" /* algorithms 2 and 3, GPL-3, opt-in */
#endif

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

/* Vocoder band count (S38). A CPU budget, not a memory one — 3 SVFs and a
 * follower per band per sample — but gated on the same PSRAM proxy for "the
 * bigger chip", since the classic ESP32 is the part with no headroom left
 * after the voices. Up here rather than beside the vocoder's DSP because
 * kParams registers the range and is declared long before it. */
#if CONFIG_SPIRAM
constexpr int kVocBandsMax = 16;
#else
constexpr int kVocBandsMax = 10;
#endif
constexpr int kVocBandsMin = 4;
/* Adaptive noise reduction (S39). Two SVFs per band per sample — one per
 * channel — plus a block-rate follower, so the ceiling is a CPU budget like
 * the vocoder's above and gated on the same PSRAM proxy for "the bigger
 * chip". The default is deliberately below the ceiling: past about a dozen
 * bands the profile gets *finer* than the noise it is describing, and a
 * finer profile mostly buys the artefact this class of algorithm is known
 * for — isolated bands opening and closing on their own, which sounds like
 * wind chimes in the background. */
#if CONFIG_SPIRAM
constexpr int kAnrBandsMax = 16;
constexpr int kAnrBandsDef = 12;
#else
constexpr int kAnrBandsMax = 10;
constexpr int kAnrBandsDef = 8;
#endif
constexpr int kAnrBandsMin = 4;


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
/* The `_ON` switch leads each unit's run rather than trailing it: the app
 * builds an FX panel by walking the registry in registration order, so this
 * is what puts the bypass at the top-left of the card instead of after the
 * knobs it governs. */
enum PIdx {
    MNR_ON, MNR_SRC, MNR_AMOUNT, MNR_FLOOR, MNR_ADAPT, MNR_LEARN,
    ANR_ON, ANR_SRC, ANR_AMOUNT, ANR_FLOOR, ANR_BANDS, ANR_LOW, ANR_HIGH,
    ANR_ADAPT, ANR_ATTACK, ANR_RELEASE, ANR_LEARN,
    NR_ON, NR_SRC, NR_HPF, NR_HUM, NR_THRESH, NR_RATIO, NR_FLOOR, NR_ATTACK,
    NR_HOLD, NR_RELEASE,
    VOC_ON, VOC_MIX, VOC_BANDS, VOC_LOW, VOC_HIGH, VOC_Q, VOC_ATTACK,
    VOC_RELEASE, VOC_SHIFT, VOC_SIB, VOC_GATE, VOC_NORM, VOC_CLARITY,
    VOC_LEVEL, VOC_CARRIER, VOC_FREEZE, VOC_LOOP,
    CHO_ON, CHO_MIX, CHO_RATE, CHO_DEPTH,
    DLY_ON, DLY_MIX, DLY_TIME, DLY_FB, DLY_TONE, DLY_PP, DLY_DIV, DLY_COMP,
    GRN_ON, GRN_MIX, GRN_SIZE, GRN_DENS, GRN_PITCH, GRN_FB, GRN_SPRAY,
    GRN_COMP,
    REV_ON, REV_ALGO, REV_MIX, REV_SIZE, REV_DAMP, REV_COMP, REV_PRE, REV_TONE,
    REV_WIDTH, REV_DIFF, REV_EARLY,
    CRUSH_ON, CRUSH_MIX, CRUSH_BITS, CRUSH_DOWN,
    FLT_ON, FLT_TYPE, FLT_MODE, FLT_CUTOFF, FLT_RESO, FLT_DRIVE, FLT_SPREAD,
    FLT_VOWEL,
    DRV_ON, DRV_MIX, DRV_MODE, DRV_DRIVE, DRV_TONE, DRV_LEVEL,
    PHS_ON, PHS_MIX, PHS_STAGES, PHS_RATE, PHS_DEPTH, PHS_CENTER, PHS_FB,
    PHS_SPREAD,
    FLG_ON, FLG_MIX, FLG_RATE, FLG_DEPTH, FLG_DELAY, FLG_FB, FLG_SPREAD,
    EQ_ON, EQ_LOW, EQ_LOFREQ, EQ_MID, EQ_MIDFREQ, EQ_MIDQ, EQ_HIGH, EQ_HIFREQ,
    COMP_ON, COMP_THRESH, COMP_RATIO, COMP_ATTACK, COMP_RELEASE, COMP_MAKEUP,
    COMP_MIX, COMP_KEY, COMP_SLOT,
    ST_WIDTH, ST_BASS, ST_MONO, ST_AMP, ST_PAN,
    LIM_ON, LIM_CEIL, LIM_GAIN, LIM_RELEASE,
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

/* What a noise-reduction unit is looking at (S39b). Shared by both units —
 * one list, one meaning — and append-only like every other enum here, with
 * entry 0 the behaviour they had before the control existed.
 *
 * `input` is the answer to "clean my microphone without putting a denoiser
 * across my synth": the unit runs on the block audio_io mixed in at the fx
 * position and adds only the difference back — so nothing reaching the bus is
 * a function of anything but the input. It needs `in.route` = fx, and
 * is inert otherwise — the reasoning is above FX_PID_ANR_SRC in fx.h.
 *
 * It is also the *default* for both units (S39c), which is why entry 0 being
 * the pre-control behaviour is now only an enum-ordering rule and no longer
 * describes what a fresh patch does. Cleaning a microphone is what anyone who
 * switches one of these on is trying to do; denoising the synth's own bus is
 * the specialist case, and it is the one that has a held pad to lose. */
const char* const kNrSrcs[] = {"bus", "input"};

/* Mains hum (S39). Append-only, and the two entries are regions of the world
 * rather than a frequency knob: 50 and 60 Hz is the whole list, nobody is
 * hunting for 53, and a notch that can be mistuned is a notch that will be. */
const char* const kNrHum[] = {"off", "50 Hz", "60 Hz"};

/* How much noise the `noise` and `bus+noise` carriers add (S43). Not 0.5,
 * which is what this was, and the difference is not a taste call: noise and a
 * pitched carrier are not comparable at equal amplitude through a filter
 * bank. A saw carries energy only at its harmonics, so most bands see very
 * little of it; white noise feeds every band fully. Measured through this
 * bank, noise at 0.5 produced 1.19x the output of a saw at 0.4 -- so
 * `bus+noise` was mostly noise, and every preset using it ran into the output
 * clipper. At 0.25 it sits around 0.6x the bus: a supplement for the
 * unpitched consonants, which is what it is for, rather than a takeover. */
constexpr float kVocNoiseGain = 0.25f;

/* Vocoder carrier (S38). Append-only. `noise` alone is what makes a whisper
 * or an unpitched consonant work — a band bank can only shape what it is
 * given, and a silent or very thin synth gives the vowels nothing to land
 * on. */
const char* const kVocCarriers[] = {"bus", "noise", "bus+noise"};

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

/* Reverb algorithms (S36). Append-only, and the two GPL-licensed entries sit
 * at the end so CONFIG_OSYNTH_FX_GPL=n can shorten the list instead of
 * punching a hole in it — the index is the stored value, so removing a middle
 * entry would renumber the tail and change what saved patches mean. The
 * reasoning in full is in fx.h above FX_PID_REV_ALGO. */
const char* const kRevAlgos[] = {"freeverb", "wetreverb",
#if SYNTH_ENABLE_FX_GPL
                                 "mverb", "duskverb",
#endif
};

template <typename T, size_t N>
constexpr int count_of(const T (&)[N]) {
    return (int)N;
}

constexpr int kDlyDivCount = count_of(kDlyDivNames);
constexpr int kLfoSyncCount = count_of(kLfoSyncNames);
constexpr int kLfoWaveCount = count_of(kLfoWaves);
constexpr int kDrvModeCount = count_of(kDrvModes);
constexpr int kCompKeyCount = count_of(kCompKeys);
constexpr int kNrHumCount = count_of(kNrHum);
constexpr int kNrSrcCount = count_of(kNrSrcs);
constexpr int kRevAlgoCount = count_of(kRevAlgos);
enum RevAlgo { kAlgoFreeverb = 0, kAlgoWet = 1, kAlgoMVerb = 2, kAlgoDusk = 3 };

/* Shared pre/post stages around whichever algorithm is selected.
 *
 * kRevToneOpen is the wet lowpass's "off" position and doubles as its
 * registered maximum, so the default *is* the bypass: above Nyquist there is
 * nothing left for a one-pole to remove, and the code skips the filter
 * outright rather than running a coefficient that rounds to unity. */
constexpr float kRevPreMaxMs = 120.0f;
constexpr float kRevToneOpen = 20000.0f;
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
    /* ---- mic noise reduction (S42) ---- */
    {FX_PID_MNR_ON, "fx.mnr.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_MNR_SRC, "fx.mnr.src", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kNrSrcCount - 1), 1.0f /* input */, kNrSrcs, kNrSrcCount},
    {FX_PID_MNR_AMOUNT, "fx.mnr.amount", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.6f, nullptr, 0}, /* 1 = subtract 3x the estimated floor */
    {FX_PID_MNR_FLOOR, "fx.mnr.floor", ParamType::Float, ParamCurve::Linear,
     -48.0f, 0.0f, -24.0f, nullptr, 0}, /* dB, the deepest a bin may be cut */
    {FX_PID_MNR_ADAPT, "fx.mnr.adapt", ParamType::Float, ParamCurve::Exp,
     0.2f, 20.0f, 1.5f, nullptr, 0},    /* s — the minimum-tracking window */
    {FX_PID_MNR_LEARN, "fx.mnr.learn", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},

    {FX_PID_ANR_ON, "fx.anr.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_ANR_SRC, "fx.anr.src", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kNrSrcCount - 1), 1.0f /* input */, kNrSrcs, kNrSrcCount},
    {FX_PID_ANR_AMOUNT, "fx.anr.amount", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.6f, nullptr, 0}, /* 1 = subtract 3x the estimated floor */
    {FX_PID_ANR_FLOOR, "fx.anr.floor", ParamType::Float, ParamCurve::Linear,
     -48.0f, 0.0f, -20.0f, nullptr, 0}, /* dB, the deepest a band may be cut */
    {FX_PID_ANR_BANDS, "fx.anr.bands", ParamType::Int, ParamCurve::Linear,
     (float)kAnrBandsMin, (float)kAnrBandsMax, (float)kAnrBandsDef, nullptr, 0},
    {FX_PID_ANR_LOW, "fx.anr.low", ParamType::Float, ParamCurve::Exp,
     40.0f, 400.0f, 120.0f, nullptr, 0},      /* first crossover, Hz */
    {FX_PID_ANR_HIGH, "fx.anr.high", ParamType::Float, ParamCurve::Exp,
     2000.0f, 16000.0f, 9000.0f, nullptr, 0}, /* last crossover, Hz */
    {FX_PID_ANR_ADAPT, "fx.anr.adapt", ParamType::Float, ParamCurve::Exp,
     0.5f, 60.0f, 3.0f, nullptr, 0},   /* s — how fast the floor may rise */
    {FX_PID_ANR_ATTACK, "fx.anr.attack", ParamType::Float, ParamCurve::Exp,
     1.0f, 100.0f, 5.0f, nullptr, 0},    /* ms — a band reopening */
    {FX_PID_ANR_RELEASE, "fx.anr.release", ParamType::Float, ParamCurve::Exp,
     5.0f, 1000.0f, 150.0f, nullptr, 0}, /* ms — a band closing */
    {FX_PID_ANR_LEARN, "fx.anr.learn", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_NR_ON, "fx.nr.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_NR_SRC, "fx.nr.src", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kNrSrcCount - 1), 1.0f /* input */, kNrSrcs, kNrSrcCount},
    {FX_PID_NR_HPF, "fx.nr.hpf", ParamType::Float, ParamCurve::Exp,
     20.0f, 400.0f, 80.0f, nullptr, 0}, /* the registered minimum is the bypass */
    {FX_PID_NR_HUM, "fx.nr.hum", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kNrHumCount - 1), 0.0f, kNrHum, kNrHumCount},
    {FX_PID_NR_THRESH, "fx.nr.thresh", ParamType::Float, ParamCurve::Linear,
     -80.0f, 0.0f, -24.0f, nullptr, 0}, /* dB, peak — see nr_process() */
    {FX_PID_NR_RATIO, "fx.nr.ratio", ParamType::Float, ParamCurve::Exp,
     1.0f, 20.0f, 4.0f, nullptr, 0},    /* downward expansion below thresh */
    {FX_PID_NR_FLOOR, "fx.nr.floor", ParamType::Float, ParamCurve::Linear,
     -60.0f, 0.0f, -24.0f, nullptr, 0}, /* dB: duck this far and no further */
    {FX_PID_NR_ATTACK, "fx.nr.attack", ParamType::Float, ParamCurve::Exp,
     1.0f, 100.0f, 3.0f, nullptr, 0},   /* ms — opening */
    {FX_PID_NR_HOLD, "fx.nr.hold", ParamType::Float, ParamCurve::Linear,
     0.0f, 1000.0f, 150.0f, nullptr, 0}, /* ms; 0 is a real setting, so linear */
    {FX_PID_NR_RELEASE, "fx.nr.release", ParamType::Float, ParamCurve::Exp,
     5.0f, 1000.0f, 200.0f, nullptr, 0}, /* ms — closing */
    {FX_PID_VOC_ON, "fx.voc.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_VOC_MIX, "fx.voc.mix", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0}, /* fully wet: the point is the voice */
    {FX_PID_VOC_BANDS, "fx.voc.bands", ParamType::Int, ParamCurve::Linear,
     (float)kVocBandsMin, (float)kVocBandsMax, (float)kVocBandsMax, nullptr, 0},
    {FX_PID_VOC_LOW, "fx.voc.low", ParamType::Float, ParamCurve::Exp,
     50.0f, 1000.0f, 150.0f, nullptr, 0},  /* lowest band centre, Hz */
    {FX_PID_VOC_HIGH, "fx.voc.high", ParamType::Float, ParamCurve::Exp,
     1000.0f, 16000.0f, 7000.0f, nullptr, 0}, /* highest band centre, Hz */
    {FX_PID_VOC_Q, "fx.voc.q", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.5f, nullptr, 0}, /* 0.5 = adjacent skirts meet (flat) */
    {FX_PID_VOC_ATTACK, "fx.voc.attack", ParamType::Float, ParamCurve::Exp,
     0.5f, 200.0f, 3.0f, nullptr, 0},  /* ms — fast enough for consonants */
    {FX_PID_VOC_RELEASE, "fx.voc.release", ParamType::Float, ParamCurve::Exp,
     1.0f, 500.0f, 40.0f, nullptr, 0}, /* ms — slow enough not to chatter */
    {FX_PID_VOC_SHIFT, "fx.voc.shift", ParamType::Float, ParamCurve::Linear,
     -12.0f, 12.0f, 0.0f, nullptr, 0}, /* formant shift, semitones */
    {FX_PID_VOC_SIB, "fx.voc.sib", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.35f, nullptr, 0},   /* consonants over the bank */
    /* 0.02, not the 0.06 this defaulted to through S42. The control is an
     * absolute floor subtracted from every band envelope, and speech runs
     * band envelopes of roughly 0.02-0.09 in this unit's domain, so 0.06
     * (a 0.012 floor) was standing in the middle of the voice rather than
     * under it: measured against a speech-like modulator it left 38-45% of
     * the spectral envelope, and the two thirds it removed is precisely the
     * information that makes words legible. 0.02 keeps 77-81% and still cuts
     * the between-words leakage about threefold in a quiet room.
     *
     * Turn it back up when the room is the problem rather than the words --
     * that is what it is for -- but reach for fx.mnr first, which was built
     * to remove noise from *under* the voice instead of gating around it. */
    {FX_PID_VOC_GATE, "fx.voc.gate", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.02f, nullptr, 0},   /* modulator noise floor */
    /* On by default: without it every one of the four controls above is also
     * a large volume control, which is the thing that made this unit hard to
     * set up. See the normalization note in vocoder_process(). */
    {FX_PID_VOC_NORM, "fx.voc.norm", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0},
    /* Off by default: it is the classic-vocoder/legible-speech fork, and the
     * classic sound is what the unit is for out of the box. The preset card
     * turns it on for the sets that are about words. */
    {FX_PID_VOC_CLARITY, "fx.voc.clarity", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    /* 2.5, down from the 4 that stood while fx.voc.gate defaulted to 0.06.
     * The two are coupled and have to move together: the gate decides how
     * much of the modulator's envelope reaches the bank at all, so opening it
     * from 0.06 to 0.02 roughly doubled what arrives here. Left at 4 the unit
     * ran its own soft_clip() backstop on ordinary speech, which is the
     * "harsh and hissy" this pair was reported as. */
    {FX_PID_VOC_LEVEL, "fx.voc.level", ParamType::Float, ParamCurve::Linear,
     0.0f, 16.0f, 2.5f, nullptr, 0},   /* make-up; a vocoder starts quiet */
    {FX_PID_VOC_CARRIER, "fx.voc.carrier", ParamType::Enum, ParamCurve::Linear,
     0.0f, 2.0f, 0.0f /* bus */, kVocCarriers, 3},
    {FX_PID_VOC_FREEZE, "fx.voc.freeze", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    /* Off = the phrase plays once per note and then the vocoder falls silent
     * over a still-sounding carrier; on = it repeats until the note ends. Off
     * by default because a spoken phrase said twice is a stutter, and speech
     * is what the capture is for; a rhythmic syllable is the case that wants
     * the other setting. */
    {FX_PID_VOC_LOOP, "fx.voc.loop", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    /* Bypass. Gates the mix below rather than replacing it — see the
     * enable-switch note in fx.h. */
    {FX_PID_CHO_ON, "fx.cho.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_CHO_MIX, "fx.cho.mix", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* 0.5 ~ classic chorus, 1 = pure vibrato */
    {FX_PID_CHO_RATE, "fx.cho.rate", ParamType::Float, ParamCurve::Exp,
     0.05f, 8.0f, 0.8f, nullptr, 0},
    {FX_PID_CHO_DEPTH, "fx.cho.depth", ParamType::Float, ParamCurve::Linear,
     0.0f, kChoDepthMaxMs, 3.5f, nullptr, 0}, /* ms */
    {FX_PID_DLY_ON, "fx.dly.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
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
    {FX_PID_GRN_ON, "fx.grn.on", ParamType::Bool, ParamCurve::Linear,
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
    {FX_PID_REV_ON, "fx.rev.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_REV_ALGO, "fx.rev.algo", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kRevAlgoCount - 1), 0.0f /* freeverb */, kRevAlgos,
     kRevAlgoCount},
    {FX_PID_REV_MIX, "fx.rev.mix", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.15f, nullptr, 0}, /* subtle room out of the box */
    {FX_PID_REV_SIZE, "fx.rev.size", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.55f, nullptr, 0},
    {FX_PID_REV_DAMP, "fx.rev.damp", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.3f, nullptr, 0},
    /* Undoes the Freeverb staging's ~6 dB and takes `size` out of the level. */
    {FX_PID_REV_COMP, "fx.rev.comp", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    /* The shared front and back of the unit, outside the algorithm: a
     * pre-delay in, then a wet-only tilt and width out. Every default is
     * the exact bypass of its stage (0 ms, filter open, width unity), so a
     * patch saved before S36 renders sample-for-sample as it did — and so
     * freeverb, which gained all three for free, is unchanged until asked. */
    {FX_PID_REV_PRE, "fx.rev.pre", ParamType::Float, ParamCurve::Linear,
     0.0f, kRevPreMaxMs, 0.0f, nullptr, 0}, /* ms */
    {FX_PID_REV_TONE, "fx.rev.tone", ParamType::Float, ParamCurve::Exp,
     500.0f, kRevToneOpen, kRevToneOpen, nullptr, 0}, /* wet LP; max = off */
    {FX_PID_REV_WIDTH, "fx.rev.width", ParamType::Float, ParamCurve::Linear,
     0.0f, 2.0f, 1.0f, nullptr, 0}, /* wet only: 0 mono, 1 as rendered */
    /* Ignored by freeverb, which has neither a diffusion coefficient worth
     * exposing (its allpasses are fixed at 0.5) nor an early field. */
    {FX_PID_REV_DIFF, "fx.rev.diff", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.7f, nullptr, 0},
    {FX_PID_REV_EARLY, "fx.rev.early", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.4f, nullptr, 0}, /* 0 all late, 1 all early */
    {FX_PID_CRUSH_ON, "fx.crush.on", ParamType::Bool, ParamCurve::Linear,
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
    {FX_PID_DRV_ON, "fx.drv.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
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
    {FX_PID_PHS_ON, "fx.phs.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
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
    {FX_PID_FLG_ON, "fx.flg.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
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

    /* ---- 3-band EQ (S34; re-voiced S40) ----
     *
     * The shelf corner frequencies are the thing to get right here, and the
     * S34 numbers had them wrong in a way that read as "the EQ does nothing".
     * An RBJ shelf reaches *half* its gain (in dB) at f0 and its full gain
     * about two octaves past it, so where the knob points is not where the
     * boost lands:
     *
     *   lofreq 120, +12 dB  ->  +6.0 dB at 120 Hz, +0.8 dB at 250, ~0 by 500,
     *                           i.e. the whole lift is under 60 Hz, which is
     *                           under everything a phone, a laptop or the
     *                           ES8311 speaker can reproduce. The old 40 Hz
     *                           floor was worse still: it does nothing at all
     *                           on any of them.
     *   hifreq 6000, +12 dB ->  +2.1 dB at 4 kHz but +10.8 at 10 k and +12 at
     *                           16 k. That octave holds no instrument content
     *                           and does hold the int16 quantization hash of
     *                           the reverb and granular lines immediately
     *                           upstream, so the control read as a noise knob.
     *
     * Re-centred so the audible half of each shelf lands on the knob:
     * lofreq 250 puts +12 dB at +11.2 dB where 120 Hz actually is, and
     * hifreq 3000 puts the lift on presence (+8.9 dB at 4 k) rather than on
     * air. The ends still reach the old extremes — 60 Hz and 12 kHz are past
     * where either shelf has anything left to move.
     *
     * Gains are +/-12 rather than +/-18. Three reasons, in order: 36 dB across
     * an 84 px dial is ~1.5 dB per pixel, which is why the mid stepped rather
     * than swept; +18 dB into a bell at Q 6 is a resonance, not a correction,
     * and this unit is the corrective one (the S33 filter is the performance
     * one); and the boost is real gain arriving at the output soft clip two
     * stages later. Nothing musical is lost — a master EQ asked for more than
     * 12 dB is fixing something that wants fixing upstream.
     *
     * Note for existing patches: presets store sparsely, so every patch that
     * never touched the EQ silently inherits the new lofreq/hifreq defaults.
     * That is the intent — those patches were carrying shelves that did
     * nothing — but it does mean a patch A/B'd across this change is not
     * comparing the same instrument. Stored gains beyond +/-12 dB clamp. */
    {FX_PID_EQ_ON, "fx.eq.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_EQ_LOW, "fx.eq.low", ParamType::Float, ParamCurve::Linear,
     -12.0f, 12.0f, 0.0f, nullptr, 0}, /* dB, low shelf */
    {FX_PID_EQ_LOFREQ, "fx.eq.lofreq", ParamType::Float, ParamCurve::Exp,
     60.0f, 800.0f, 250.0f, nullptr, 0},
    {FX_PID_EQ_MID, "fx.eq.mid", ParamType::Float, ParamCurve::Linear,
     -12.0f, 12.0f, 0.0f, nullptr, 0}, /* dB, peaking bell */
    {FX_PID_EQ_MIDFREQ, "fx.eq.midfreq", ParamType::Float, ParamCurve::Exp,
     200.0f, 6000.0f, 1000.0f, nullptr, 0},
    {FX_PID_EQ_MIDQ, "fx.eq.midq", ParamType::Float, ParamCurve::Exp,
     0.3f, 6.0f, 1.0f, nullptr, 0},
    {FX_PID_EQ_HIGH, "fx.eq.high", ParamType::Float, ParamCurve::Linear,
     -12.0f, 12.0f, 0.0f, nullptr, 0}, /* dB, high shelf */
    {FX_PID_EQ_HIFREQ, "fx.eq.hifreq", ParamType::Float, ParamCurve::Exp,
     1200.0f, 12000.0f, 3000.0f, nullptr, 0},

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

    /* ---- master limiter (S43) ---- */
    {FX_PID_LIM_ON, "fx.lim.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    /* -2 dBFS, not 0, and the number is not arbitrary: soft_clip()'s knee is
     * at 0.8, which is -1.94 dBFS, so a ceiling set above it hands the sink
     * a signal the sink then shapes anyway. Just under the knee is where
     * this unit is the only thing acting on the peaks. */
    {FX_PID_LIM_CEIL, "fx.lim.ceil", ParamType::Float, ParamCurve::Linear,
     -24.0f, 0.0f, -2.0f, nullptr, 0}, /* dBFS */
    {FX_PID_LIM_GAIN, "fx.lim.gain", ParamType::Float, ParamCurve::Linear,
     0.0f, 24.0f, 0.0f, nullptr, 0},   /* dB into the ceiling — loudness */
    {FX_PID_LIM_RELEASE, "fx.lim.release", ParamType::Float, ParamCurve::Exp,
     10.0f, 1000.0f, 150.0f, nullptr, 0}, /* ms; attack is zero by design */

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

/* Moved to synth_core/synth_line.h in S36 so components/fx_gpl can build its
 * two reverbs on the same primitive without either component including the
 * other's sources. Pulled back in unqualified here, so every use below reads
 * exactly as it did before the move. */
using osynth::dsp::Line;
using osynth::dsp::line_alloc;
using osynth::dsp::line_push;
using osynth::dsp::line_read;
using osynth::dsp::line_read_frac;
using osynth::dsp::line_tap;

/* ---- per-effect dry/wet gate with incremental bypass scrub ---- */

/* The enable switch (S36) folded into a unit's dry/wet target.
 *
 * `on` is read with pv() and not pvm(): a Bool has no meaningful in-between
 * value and is not a legal LFO destination. The result still goes through
 * unit_gate(), so flipping the switch crossfades over the same ~90 ms a mix
 * sweep would, rather than stepping.
 *
 * Zeroing the target rather than branching around the whole unit is what
 * keeps the older contract intact: a unit sitting at mix 0 costs nothing
 * whatever the switch says, and the bypass scrub still runs, so re-enabling
 * a reverb never replays the tail it had when you switched it off. */
inline float gated(float on, float mix) { return on >= 0.5f ? mix : 0.0f; }


constexpr float kMixSlew = 0.08f;      /* per block: ~90 ms for a full swing */
constexpr float kSilent = 1e-3f;

/* Samples zeroed per block while dry — a budget for the *whole bus*, not for
 * each unit.
 *
 * It used to be per unit, and every unit on the chain is called every block,
 * so the ceiling was the sum of all of them rather than this number. That is
 * not a corner case: the mixes all decay with the same kMixSlew, so switching
 * several units off together — a preset that clears the enable switches, an
 * app "bypass all" — drops them through kSilent within a block or two of each
 * other and starts every scrub at once. Delay (144 016 samples across two
 * lines), granular (36 016), reverb (~39 000 for freeverb, more for the GPL
 * algorithms), chorus and flanger overlap for the first several blocks: five
 * units x 8 KB = 40 KB of PSRAM memset inside a 1.33 ms block, on top of the
 * bus, the voices, the drums and the looper. That is over budget on an S3 and
 * tight on a P4 — an underrun caused by the machinery that exists to stop a
 * click.
 *
 * Shared, the common case is unchanged: one unit switched off still gets the
 * whole 4096 and finishes in exactly the blocks it used to. Only the pile-up
 * is bounded, and it costs elapsed time rather than deadline — the scrubs
 * serialise, so total time is the same sum of samples over the same rate no
 * matter how they are ordered.
 *
 * What that lengthens is the window in which re-enabling a unit can still
 * replay stale audio, because unit_gate() abandons a scrub the moment the
 * target comes back up. Worst case moves from ~48 ms (delay alone) to ~73 ms
 * (delay, then granular, then reverb). Both are far under the ~90 ms the mix
 * crossfade takes to become audible on the way back in, which is the deadline
 * that actually matters here. */
constexpr uint32_t kScrubChunk = 4096;

/* What is left of this block's kScrubChunk. Refreshed at the top of
 * fx_process(); drawn down by every scrub_step() in the chain, including the
 * reverb's second cursor. Audio task only. */
uint32_t s_scrub_budget = 0;

struct UnitState {
    float mix = 0.0f; /* smoothed */
    bool active = false;
    bool scrubbing = false;
    uint8_t sl = 0;   /* scrub cursor: line index / offset */
    uint32_t sp = 0;
};

/* Zeroes as much of the unit's lines as this block's shared budget allows;
 * true when the unit is done. A block whose budget was already spent by an
 * earlier unit makes no progress and returns false, which is exactly what a
 * caller wants: the unit stays dry and tries again next block. */
bool SYNTH_RENDER_IRAM scrub_step(UnitState& u, Line* const* lines, size_t n) {
    uint32_t budget = s_scrub_budget;
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
    s_scrub_budget = budget;
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

/* ---- adaptive noise reduction (S39): subtract whatever never stops ----
 *
 * A per-band spectral subtractor. The bus is split into `bands` sections;
 * each keeps an estimate of its own *steady* level, and each is attenuated by
 * however much of what is currently in it looks like that estimate. Speech,
 * notes and transients move; a fan, a hiss floor and a spinning disk do not,
 * and that difference is the entire algorithm.
 *
 * Reconstruction is by *residual*, not by summing the bank back up:
 *
 *     y = x + sum_k (g_k - 1) * band_k(x)
 *
 * which buys the one property that matters for a unit that spends most of its
 * life doing nothing: with every g_k at 1 the output is the input, sample for
 * sample — no filterbank colouration, no phase smear to explain away.
 *
 * ---- the bank (S39c) ----
 *
 * That residual form only pays off if the bands sum back to x, because with
 * every g_k at `fx.anr.floor` the output is
 *
 *     x + (gmin - 1) * sum_k band_k(x)
 *
 * and whatever the bank fails to account for is *not attenuated at all*. It
 * is the floor of the whole unit, not a detail. The first version of this
 * bank was a Butterworth lowpass, a run of constant-Q unity-peak bandpasses
 * and a Butterworth highpass, on the reasoning that skirts crossing at -3 dB
 * sum back to something flat. They do not: adjacent bandpasses cross at
 * different phases, and measured on a real recording that bank summed to
 * -9 dB at 100 Hz, +3 dB through the midrange and -5 dB at 8 kHz, leaving
 * 18% of the input outside the bank entirely. The unit could not cut by more
 * than about 8 dB however `fx.anr.floor` was set — and past -10 dB, setting
 * it *deeper* made the output louder, because the bands that summed hot were
 * being over-subtracted while the residue sat there untouched.
 *
 * So the bank telescopes instead. bands-1 Butterworth lowpasses, geometrically
 * spaced, and each band is the difference of two neighbouring ones:
 *
 *     band_0     = LP_0(x)
 *     band_k     = LP_k(x) - LP_k-1(x)
 *     band_n-1   = x - LP_n-2(x)
 *
 * The sum is x identically — an algebraic identity, not a filter design, so
 * it holds sample for sample at every frequency, for any spacing, any band
 * count, any Q. All g_k at 1 gives x; all g_k at gmin gives gmin*x exactly;
 * `fx.anr.floor` means what it says. `low` and `high` are still the first and
 * last crossover, naming the same two frequencies they named before, and the
 * bank still covers the whole spectrum — rumble under `low` and hiss over
 * `high` are precisely where noise lives. It is also *cheaper*: bands-1
 * filters per channel where the old bank ran bands.
 *
 * What it costs is selectivity. Neighbouring bands overlap heavily — at
 * twelve bands a band's neighbour is only ~2.5 dB down at its centre — so the
 * profile is a blurred picture of the spectrum rather than a sharp one. That
 * is a good trade twice over: noise floors are smooth in frequency, so there
 * is little detail to lose, and a gain curve that cannot vary sharply between
 * neighbours is exactly the curve that does not produce the isolated
 * opening-and-closing bands this class of algorithm is known for. It is also
 * why `fx.anr.bands` now barely changes the result — 8, 12 and 16 land within
 * 0.1 dB of each other on stationary noise.
 *
 * ---- the estimator ----
 *
 * A sliding-window minimum over the band's level, held in kAnrSubWins buckets
 * that between them cover `fx.anr.adapt`. The floor of a band is its minimum
 * over a few seconds, not its average: an average includes the speech, the
 * minimum does not.
 *
 * The level it takes the minimum *of* is smoothed first (kAnrEstTauS), and
 * that one-pole is what makes the whole unit work. Without it the minimum is
 * taken over raw per-block magnitudes, and a block is 1.33 ms — a sixth of a
 * cycle at the bottom crossover. What such a block measures is not the level
 * of the band, it is where in its waveform the band happened to be, and the
 * minimum of *that* over a few thousand blocks is a number far below anything
 * present in the signal. Measured on a stationary recording the gap between a
 * band's median block and its minimum block ran from 5 dB at the top of the
 * bank to 28 dB at the bottom, so the unit believed the noise floor was up to
 * 28 dB quieter than it was and subtracted next to nothing — while kAnrBias,
 * one constant, tried to correct a bias that was not one number but twelve.
 * With 150 ms of smoothing ahead of it that spread is 1-3 dB across the whole
 * bank, which is a bias small enough and uniform enough for kAnrBias to be
 * the fixed factor it was always documented as.
 *
 * Only the *estimator* reads the smoothed level. The gain below reads the raw
 * block magnitude, because that half has to move at the speed of a syllable:
 * running the gain off the smoothed level too costs another 5 dB of noise cut
 * and, worse, holds each band shut for 150 ms into every word.
 *
 * Buckets rather than the two the first version held, so the first usable
 * profile lands one bucket after switch-on rather than one whole window — at
 * the default `adapt` that is 0.75 s instead of 8 s of a unit that is on,
 * lit, and audibly doing nothing.
 *
 * Two things guard it. They were written for `src` = bus, where a held pad
 * looks exactly like a fan, and they are not dropped when the unit is pointed
 * at the input instead: a sung note, a bowed string and a guitar drone are all
 * steady for longer than a window, and a denoiser that learns one removes it.
 * The source changes what is at stake, not whether the test is right:
 *
 *   - a band is offered to the bucket only while it is within
 *     kAnrSignalRatio of the current estimate. Above that it is signal, and
 *     signal has no business in a noise profile.
 *   - if a whole window goes by with nothing offered in any bucket —
 *     something loud has been sitting in that band the entire time — the
 *     estimate may climb toward the raw minimum, by no more than kAnrCreep
 *     per kAnrCreepRefS. Without an escape the unit locks out completely on
 *     any input that starts loud; with an unbounded one, a long pad is
 *     learned and fades away under the player's hands. Bounded, a pad 40 dB
 *     up takes the better part of a minute to be mistaken for noise, while a
 *     floor that genuinely rises 6 dB is tracked in one window — that one is
 *     the offer test's job, not the creep's, which is why slowing the creep
 *     down does not slow down tracking a floor that really moved.
 *
 *     Per *second*, note, not per bucket: the rate at which a held note is
 *     mistaken for a fan is a promise to the player, and it must not change
 *     just because `fx.anr.adapt` was moved. Anchored to the bucket instead,
 *     shortening `adapt` from 8 s to 3 s would have made that creep ten times
 *     faster and eaten a held pad in nine seconds.
 *
 * The estimate carries kAnrBias, because the minimum of a fluctuating noise
 * sits below its average and subtracting the minimum would leave most of the
 * noise behind. That is the classic minimum-statistics bias correction, at a
 * fixed factor rather than the derived one: the derivation needs the variance
 * of the estimator, and one number tuned by ear is worth more here than three
 * that are exactly right about a Gaussian assumption the input does not obey.
 *
 * Nothing is subtracted from a band until a window has closed over it, and a
 * band is not primed at all until something turns up in it. The unit listens
 * for `adapt` seconds after being switched on and only then starts working,
 * which is the difference between switching it on mid-sentence and having the
 * sentence disappear; the per-band half of that is what makes switching it on
 * over a *silent* bus — the usual case, since that is when anyone sets this up
 * — behave the same way instead of priming every estimate to zero and then
 * rejecting every real signal for being too far above it. `fx.anr.learn` is the short cut: held, the window drops
 * to kAnrLearnMs and the offer test is waived, so a second of held button in
 * a quiet room is a complete profile.
 *
 * ---- rates ----
 *
 * Everything except the filters runs once per block: one mean-|x| accumulator
 * per band, then the estimate, the gain and its attack/release, then a linear
 * ramp of (g - 1) across the *following* block. Same shape as the compressor,
 * same reason — the divides stay out of the sample loop — and the same
 * consequence, that gain movement lands on a 1.33 ms grid, which is why
 * `fx.anr.attack` has a 1 ms floor. The one-block lag is deliberate: the
 * alternative is running the whole bank twice to look 1.33 ms further into a
 * noise floor that is by definition not changing.
 */

constexpr float kAnrSignalRatio = 4.0f; /* +12 dB over the estimate is signal */
constexpr float kAnrBias = 1.5f;        /* minimum -> average, see above */
constexpr float kAnrCreep = 2.0f;       /* ceiling on a locked-out band's climb */
constexpr float kAnrCreepRefS = 8.0f;   /* ...per this many seconds, not per bucket */
constexpr float kAnrLearnMs = 80.0f;    /* the window while `learn` is held */
constexpr float kAnrOversub = 3.0f;     /* what `amount` = 1 subtracts */
constexpr float kAnrXoverK = 1.41421356f; /* Butterworth: every crossover */
constexpr float kAnrEstTauS = 0.15f;    /* the smoother ahead of the minimum */
constexpr int kAnrSubWins = 4;          /* buckets the window is split into */
constexpr float kAnrHuge = 1e30f;       /* "this bucket took nothing" */
constexpr float kAnrSeed = 1e-6f;       /* keeps the offer test alive at zero */
constexpr float kAnrEps = 1e-7f;      /* under this a band is empty, not quiet */

struct AnrBand {
    /* The band's *crossover* lowpass, not the band itself: the bank is a
     * telescoping difference (see above), so band k is this lowpass minus the
     * one below it. The top band has no filter of its own and leaves these
     * unused. */
    osynth::dsp::Svf l, r;
    osynth::dsp::SvfCoef c;
    float acc = 0.0f;      /* sum |band| over the block, both channels */
    float sm = 0.0f;       /* acc/frames, smoothed — what the estimator sees */
    float noise = 0.0f;    /* the estimate, biased, in the units of acc */
    /* Brace-initialised by hand, so the count is not free to drift: a short
     * initialiser list would zero-fill the rest, and a bucket holding 0 reads
     * as "this window measured a floor of nothing" rather than as empty. */
    static_assert(kAnrSubWins == 4, "buck[] initialiser lists one per bucket");
    float buck[kAnrSubWins] = {kAnrHuge, kAnrHuge, kAnrHuge, kAnrHuge};
    float raw = kAnrHuge;  /* this bucket's minimum, offered or not */
    float g = 1.0f;        /* smoothed gain, block boundary */
    float d = 0.0f;        /* (g - 1), ramped across the block */
    float dstep = 0.0f;
    /* Per band, not per unit. A band with nothing in it has nothing to
     * estimate from, and priming it anyway from a silent bus is what used to
     * wedge the whole unit — see the estimator note above. */
    bool primed = false;  /* the estimate holds something */
    bool settled = false; /* ...and a window has closed over it */
};

struct AnrFx {
    AnrBand b[kAnrBandsMax];
    int n = 0;
    /* Doubles as the "has run since the last reset" flag: -1 both requests a
     * coefficient build and says the state below is already clean. */
    int c_bands = -1;
    float c_low = 0.0f, c_high = 0.0f;
    uint32_t win_cnt = 0;
    osynth::dsp::Smooth s_amount, s_floor;
    UnitState u;
};

/* One block of the input, stereo, exactly as audio_io mixed it into the bus
 * (S39b). Shared by both units because neither holds it across a call: each
 * fills it, uses it and is done, and the two run one after the other on the
 * audio task. Live only inside anr_process() and nr_process(). */
float s_nr_src_l[SYNTH_BLOCK_SIZE];
float s_nr_src_r[SYNTH_BLOCK_SIZE];

AnrFx s_anr;

void anr_rebuild(AnrFx& a, int bands, float low, float high) {
    if (bands < kAnrBandsMin) bands = kAnrBandsMin;
    if (bands > kAnrBandsMax) bands = kAnrBandsMax;
    /* Two crossovers with a couple of octaves between them; below this the
     * ratio arithmetic divides by ~0. */
    if (high < low * 4.0f) high = low * 4.0f;

    /* `bands` bands need bands-1 crossovers, and `low`/`high` are the first
     * and the last of them — the same thing they named before, since the old
     * bank's end members were a lowpass at `low` and a highpass at `high`. */
    const float ratio = powf(high / low, 1.0f / (float)(bands - 2));
    float f = low;
    for (int i = 0; i < bands - 1; ++i) {
        a.b[i].c = osynth::dsp::svf_coef_k(f, kAnrXoverK, kSr);
        f *= ratio;
    }
    a.n = bands;
}

void SYNTH_RENDER_IRAM anr_process(float* __restrict__ bl,
                                   float* __restrict__ br, size_t frames) {
    AnrFx& a = s_anr;
    const float m = unit_gate(a.u, pv(ANR_ON), nullptr, 0);
    if (m < 0.0f) {
        /* Off: drop the banks and the profile, once, on the way down. A
         * profile describes a room, and the room it is switched back on in is
         * the only one worth describing. */
        if (a.c_bands >= 0) {
            for (int k = 0; k < kAnrBandsMax; ++k) a.b[k] = AnrBand{};
            a.c_bands = -1;
            a.win_cnt = 0;
        }
        return;
    }

    const int bands = (int)pv(ANR_BANDS);
    const float low = pvm(ANR_LOW);
    const float high = pvm(ANR_HIGH);
    if (bands != a.c_bands || low != a.c_low || high != a.c_high) {
        /* A moved crossover leaves every estimate describing a band that no
         * longer exists, so the profile goes with the coefficients rather
         * than being carried across to quietly mean something else. */
        for (int k = 0; k < kAnrBandsMax; ++k) {
            a.b[k].noise = 0.0f;
            a.b[k].sm = 0.0f;
            for (int w = 0; w < kAnrSubWins; ++w) a.b[k].buck[w] = kAnrHuge;
            a.b[k].raw = kAnrHuge;
            a.b[k].primed = false;
            a.b[k].settled = false;
        }
        a.win_cnt = 0;
        anr_rebuild(a, bands, low, high);
        a.c_bands = bands;
        a.c_low = low;
        a.c_high = high;
    }
    const int n = a.n;

    const float amount = osynth::dsp::smooth_lin(a.s_amount, pvm(ANR_AMOUNT));
    const float gmin = powf(
        10.0f,
        osynth::dsp::smooth_lin(a.s_floor, pvm(ANR_FLOOR)) * (1.0f / 20.0f));
    const float sub = amount * kAnrOversub;

    /* One block is the clock for everything below the filters. */
    const float blk_s = (float)frames / kSr;
    const float ka =
        1.0f - expf(-blk_s / fmaxf(pvm(ANR_ATTACK) * 0.001f, 1e-4f));
    const float kr =
        1.0f - expf(-blk_s / fmaxf(pvm(ANR_RELEASE) * 0.001f, 1e-4f));

    const bool learn = pv(ANR_LEARN) >= 0.5f;
    const float win_s = learn ? (kAnrLearnMs * 0.001f) : pvm(ANR_ADAPT);
    /* The window is held as kAnrSubWins buckets rather than two, so the first
     * profile lands one *bucket* after switch-on instead of one whole window.
     * The look-back is still `adapt`; only the granularity changed. */
    uint32_t win = (uint32_t)(win_s / (float)kAnrSubWins / blk_s);
    if (win < 1) win = 1;
    /* One-pole ahead of the minimum tracker. Without it the "minimum" is the
     * minimum of the *waveform's* wander inside 1.33 ms, which at the bottom
     * of the bank is 25 dB below the level it is supposed to be measuring. */
    const float k_est = 1.0f - expf(-blk_s / kAnrEstTauS);
    /* kAnrCreep is a rate and has to stay one: anchored to the bucket it would
     * speed up whenever `adapt` was shortened, and the promise in the note
     * above — a pad 40 dB up surviving the better part of a minute — is wall
     * clock. */
    const float creep = powf(kAnrCreep, (float)win * blk_s / kAnrCreepRefS);

    /* What this unit is looking at (S39b). In `bus` mode these alias bl/br,
     * which is deliberate and is why they are not themselves __restrict__: a
     * pointer *based on* a restrict-qualified one is allowed to read what that
     * one writes. In `input` mode they are the scratch, and the two objects
     * are genuinely disjoint. */
    const float* sl = bl;
    const float* sr = br;
    bool live = true;
    if ((int)pv(ANR_SRC) == 1) {
        live = frames <= SYNTH_BLOCK_SIZE &&
               audio_io_in_fx_block(s_nr_src_l, s_nr_src_r, frames);
        sl = s_nr_src_l;
        sr = s_nr_src_r;
    }

    /* The bank, and the correction it earned last block. The source sample is
     * read into a local first: every band has to analyse the *input*, not what
     * the bands ahead of it have already been subtracted from. */
    if (live) {
        for (size_t i = 0; i < frames; ++i) {
            const float xl = sl[i], xr = sr[i];
            float cl = 0.0f, cr = 0.0f;
            /* The running crossover output, which is also band k-1's upper
             * edge. Starting it at zero is what makes band 0 a plain
             * lowpass. */
            float pl = 0.0f, pr = 0.0f;
            for (int k = 0; k < n - 1; ++k) {
                AnrBand& b = a.b[k];
                const float ll = osynth::dsp::svf_next(
                    b.l, b.c, osynth::dsp::SvfMode::Lp, xl);
                const float lr = osynth::dsp::svf_next(
                    b.r, b.c, osynth::dsp::SvfMode::Lp, xr);
                const float yl = ll - pl;
                const float yr = lr - pr;
                pl = ll;
                pr = lr;
                b.acc += fabsf(yl) + fabsf(yr);
                cl += b.d * yl;
                cr += b.d * yr;
                b.d += b.dstep;
            }
            {
                /* Whatever the last crossover did not take. This is the term
                 * that closes the telescope: every band below has already
                 * cancelled against its neighbour, so what is left of the
                 * sum is exactly x. */
                AnrBand& b = a.b[n - 1];
                const float yl = xl - pl;
                const float yr = xr - pr;
                b.acc += fabsf(yl) + fabsf(yr);
                cl += b.d * yl;
                cr += b.d * yr;
                b.d += b.dstep;
            }
            /* The band sum *is* the correction — (g - 1) times each band — so
             * this one line is both "replace the bus" and "replace only the
             * input's contribution to it". Which of the two it does was
             * settled above, by where xl came from. */
            bl[i] += m * cl;
            br[i] += m * cr;
        }
    } else {
        /* `input` with nothing arriving. No correction to add, and the banks
         * are dropped so a source that comes back does not meet state charged
         * before it went away. The per-band pass below still runs, over an
         * accumulator of zero, which is what unprimes the estimates. */
        for (int k = 0; k < n; ++k) {
            a.b[k].l = osynth::dsp::Svf{};
            a.b[k].r = osynth::dsp::Svf{};
            a.b[k].d = 0.0f;
        }
    }

    const bool boundary = (++a.win_cnt >= win);
    if (boundary) a.win_cnt = 0;

    const float inv = 1.0f / (2.0f * (float)frames);
    for (int k = 0; k < n; ++k) {
        AnrBand& b = a.b[k];
        const float mag = b.acc * inv;
        b.acc = 0.0f;

        if (!b.primed) {
            /* Seeded from whatever is there, loud or not — the offer test
             * below needs a non-zero estimate to be a test at all, and the
             * first window to close replaces this with a real minimum. But
             * only from *something*: an empty band is not a quiet one, and
             * an estimate of zero rejects every signal that follows it. */
            if (mag > kAnrEps) {
                b.sm = mag;
                b.noise = mag * kAnrBias;
                for (int w = 0; w < kAnrSubWins; ++w) b.buck[w] = mag;
                b.raw = mag;
                b.primed = true;
            }
        } else {
            /* Everything the estimator looks at is the *smoothed* level; the
             * gain below still reads the raw one, because that is the half
             * that has to move at the speed of a syllable. */
            b.sm += k_est * (mag - b.sm);
            if (b.sm < b.raw) b.raw = b.sm;
            if (learn || b.sm < b.noise * kAnrSignalRatio + kAnrSeed) {
                if (b.sm < b.buck[0]) b.buck[0] = b.sm;
            }
            if (boundary) {
                float best = kAnrHuge;
                for (int w = 0; w < kAnrSubWins; ++w) {
                    if (b.buck[w] < best) best = b.buck[w];
                }
                if (best < kAnrHuge) {
                    b.noise = best * kAnrBias;
                } else {
                    /* A whole window with nothing plausible anywhere in it.
                     * Climb, but no faster than kAnrCreep — see the estimator
                     * note above. */
                    b.noise = fminf(b.raw * kAnrBias, b.noise * creep);
                }
                for (int w = kAnrSubWins - 1; w > 0; --w) {
                    b.buck[w] = b.buck[w - 1];
                }
                b.buck[0] = kAnrHuge;
                b.raw = kAnrHuge;
                /* A window that closed on silence did not measure a floor,
                 * it measured the absence of one. Throwing it away and
                 * re-priming is the difference between an input that arrives
                 * late being cleaned up and being ignored forever. */
                if (b.noise <= kAnrEps) {
                    b.primed = false;
                    b.settled = false;
                } else {
                    b.settled = true;
                }
            }
        }

        /* Wiener-ish: what fraction of this band is not accounted for by the
         * estimate. Floored, because a band taken to silence is what makes
         * the residual noise sound like wind chimes rather than like a room;
         * leaving 20 dB of it in place is what a listener hears as "quiet". */
        float g = 1.0f;
        if (b.settled && mag > kAnrEps) {
            g = (mag - sub * b.noise) / mag;
            if (g < gmin) g = gmin;
            else if (g > 1.0f) g = 1.0f;
        }
        b.g += ((g > b.g) ? ka : kr) * (g - b.g);
        b.dstep = (b.g - 1.0f - b.d) / (float)frames;
    }
}

/* ---- noise reduction (S39): high-pass, hum notch, downward expander ----
 *
 * The fixed half of the pair, and the one that behaves the way the DSP inside
 * a USB microphone behaves: three stages, no learning, every number chosen by
 * the player. It is what the adaptive unit above cannot be — predictable —
 * and that is its entire job description.
 *
 *   fx.nr.hpf     a Butterworth high-pass. Desk rumble, footfalls, the low
 *                 end of a laptop fan, and the thump of a hand landing on the
 *                 same table as the microphone. The registered minimum (20 Hz)
 *                 *is* the bypass: below audibility there is nothing for a
 *                 two-pole to remove, so the stage is skipped outright rather
 *                 than run at a setting that rounds to unity — the same
 *                 convention fx.rev.tone uses at the other end of the range.
 *   fx.nr.hum     notches at the mains frequency and its first two harmonics.
 *                 Q of 20, which is narrow enough to leave a bass line alone
 *                 and deep enough to take out what an unbalanced lead picks
 *                 up from a wall wart. Off by default: a notch nobody needs
 *                 is a hole in the spectrum nobody asked for.
 *   the expander  everything below fx.nr.thresh is pushed down at
 *                 fx.nr.ratio, no further than fx.nr.floor, and not until
 *                 fx.nr.hold has run out.
 *
 * ---- where the threshold has to sit (S39c) ----
 *
 * `thresh` is the one number in this unit that cannot be guessed from first
 * principles, because it is an absolute level and the thing it has to sit
 * above is a property of the room and the gain, not of the algorithm. It
 * shipped at -45 dBFS, which is a sensible figure for a quiet line source and
 * a useless one for the microphone this unit exists to clean: osynth's own
 * mic at `in.micgain` 1.33 in an ordinary room puts this detector at about
 * -29 dBFS on the ambience alone. A threshold 16 dB underneath that is never
 * crossed from below, so `hold` re-armed on every block, the expander sat
 * fully open for the entire recording and the unit did — measurably, on a
 * 10 s capture — 0.4 dB of nothing.
 *
 * -24 dBFS is the default now: above that mic's floor with a few dB to spare,
 * below anything anyone would call speech. It is still a number the player
 * has to own — halve the gain and it wants moving with it — but it is now
 * wrong in the direction that is audible and adjustable rather than the
 * direction that looks like a broken feature. Note that it is a *peak*
 * detector reading after the high-pass, so it sits a few dB above the RMS
 * level the master meter shows.
 *
 * `floor` is the control that separates this from a gate, and it is the one
 * worth understanding. A gate closes; a room that goes absolutely silent
 * between words does not sound like a quiet room, it sounds like a broken
 * connection, and the moment it reopens the noise arrives as an audible
 * *swell*. Capping the attenuation at -24 dB leaves the room present and
 * simply distant, which is what "quiet" sounds like to a listener. Take
 * `floor` to -60 dB and this is a gate again, deliberately.
 *
 * `hold` is the other half of that. Speech ends in unvoiced consonants at a
 * fraction of the energy of the vowel before them, and an expander with no
 * hold eats every one of them — the classic word-ending chop. A hold of
 * 150 ms keeps the unit open across it and across the gaps *inside* a phrase,
 * which are shorter than the gaps between phrases by a wide enough margin for
 * one number to tell them apart.
 *
 * The detector runs on the *filtered* signal, which is why the filters are
 * first: a footfall is nearly all energy below 80 Hz, and a gate that opens
 * on it has opened for something nobody can hear. Peak, per sample, with the
 * gain computed once per block from the block's peak and ramped across the
 * next one — the compressor's structure exactly, including the reason for
 * fx.nr.attack's 1 ms floor (below the block period it would be a promise the
 * structure cannot keep).
 *
 * Stereo, one detector: the two channels share a gain because a microphone
 * that ducks one side and not the other is a microphone with a hole in the
 * middle of it. That is also why the detector takes the larger of the two
 * rather than their sum.
 */

constexpr int kNrHumHarmonics = 3;
constexpr float kNrHumK = 0.05f;        /* Q = 20 */
constexpr float kNrHpfOff = 20.0f;      /* the registered minimum is the bypass */
constexpr float kNrHpK = 1.41421356f;   /* Butterworth */
constexpr float kNrKnee = 6.0f;         /* dB, fixed, as on the compressor */
constexpr float kNrFloorEps = 1e-6f;

struct NrFx {
    osynth::dsp::Svf hp_l, hp_r;
    osynth::dsp::SvfCoef hp_c;
    osynth::dsp::Svf hum_l[kNrHumHarmonics], hum_r[kNrHumHarmonics];
    osynth::dsp::SvfCoef hum_c[kNrHumHarmonics];
    int c_hum = -1;    /* the hum coefficients cost three tanf to build */
    float env = 0.0f;  /* detector */
    float gain = 1.0f; /* smoothed, block boundary */
    uint32_t hold = 0; /* blocks of hold left */
    osynth::dsp::Smooth s_hpf, s_thresh, s_ratio, s_floor, s_atk, s_hold, s_rel;
    UnitState u;
};

NrFx s_nr;

void SYNTH_RENDER_IRAM nr_process(float* __restrict__ bl,
                                  float* __restrict__ br, size_t frames) {
    NrFx& c = s_nr;
    const float m = unit_gate(c.u, pv(NR_ON), nullptr, 0);
    if (m < 0.0f) {
        /* Off: drop the filter states and reopen the expander, so switching
         * back on cannot ring or arrive mid-duck. */
        c.hp_l = c.hp_r = osynth::dsp::Svf{};
        for (int h = 0; h < kNrHumHarmonics; ++h) {
            c.hum_l[h] = osynth::dsp::Svf{};
            c.hum_r[h] = osynth::dsp::Svf{};
        }
        c.env = 0.0f;
        c.gain = 1.0f;
        c.hold = 0;
        return;
    }

    /* What this unit is looking at (S39b); the note in anr_process() covers
     * why these are not __restrict__. */
    const bool from_input = ((int)pv(NR_SRC) == 1);
    const float* sl = bl;
    const float* sr = br;
    if (from_input) {
        if (frames > SYNTH_BLOCK_SIZE ||
            !audio_io_in_fx_block(s_nr_src_l, s_nr_src_r, frames)) {
            /* Nothing arriving, so nothing to correct. The filters and the
             * detector are dropped rather than left charged: an input that
             * comes back should meet an open expander and clean integrators,
             * not the tail of the last thing it said. */
            c.hp_l = c.hp_r = osynth::dsp::Svf{};
            for (int h = 0; h < kNrHumHarmonics; ++h) {
                c.hum_l[h] = osynth::dsp::Svf{};
                c.hum_r[h] = osynth::dsp::Svf{};
            }
            c.env = 0.0f;
            c.gain = 1.0f;
            c.hold = 0;
            return;
        }
        sl = s_nr_src_l;
        sr = s_nr_src_r;
    }

    const float hpf = osynth::dsp::smooth_exp(c.s_hpf, pvm(NR_HPF));
    const bool use_hp = hpf > kNrHpfOff * 1.02f;
    if (use_hp) {
        c.hp_c = osynth::dsp::svf_coef_k(hpf, kNrHpK, kSr);
    } else {
        /* Bypassed, and held clean rather than merely unread: a sweep back up
         * off the minimum would otherwise re-engage the filter on top of
         * whatever its integrators were holding when it went out. */
        c.hp_l = c.hp_r = osynth::dsp::Svf{};
    }

    const int hum = (int)pv(NR_HUM);
    if (hum != c.c_hum) {
        c.c_hum = hum;
        /* New coefficients describe a different filter, so the state charged
         * at the old frequency is not this one's — carried across, it rings
         * out as a beat instead of settling into a null. */
        for (int h = 0; h < kNrHumHarmonics; ++h) {
            c.hum_l[h] = osynth::dsp::Svf{};
            c.hum_r[h] = osynth::dsp::Svf{};
        }
        if (hum > 0) {
            const float f0 = (hum == 1) ? 50.0f : 60.0f;
            for (int h = 0; h < kNrHumHarmonics; ++h) {
                c.hum_c[h] =
                    osynth::dsp::svf_coef_k(f0 * (float)(h + 1), kNrHumK, kSr);
            }
        }
    }

    const float thresh = osynth::dsp::smooth_lin(c.s_thresh, pvm(NR_THRESH));
    const float ratio = osynth::dsp::smooth_exp(c.s_ratio, pvm(NR_RATIO));
    const float floor_db = osynth::dsp::smooth_lin(c.s_floor, pvm(NR_FLOOR));
    const float atk_ms = osynth::dsp::smooth_exp(c.s_atk, pvm(NR_ATTACK));
    const float rel_ms = osynth::dsp::smooth_exp(c.s_rel, pvm(NR_RELEASE));
    const float hold_ms = osynth::dsp::smooth_lin(c.s_hold, pvm(NR_HOLD));

    const float ka = 1.0f - expf(-1.0f / (atk_ms * 0.001f * kSr));
    const float kr = 1.0f - expf(-1.0f / (rel_ms * 0.001f * kSr));

    /* Pass 1: the filters, and the detector on what leaves them. What goes on
     * the bus is the filters' *difference*: in `bus` mode that is the familiar
     * dry/wet crossfade, and in `input` mode it replaces the input's
     * contribution and touches nothing else. */
    float peak = 0.0f;
    for (size_t i = 0; i < frames; ++i) {
        const float xl = sl[i], xr = sr[i];
        float l = xl, r = xr;
        if (use_hp) {
            l = osynth::dsp::svf_next(c.hp_l, c.hp_c, osynth::dsp::SvfMode::Hp,
                                      l);
            r = osynth::dsp::svf_next(c.hp_r, c.hp_c, osynth::dsp::SvfMode::Hp,
                                      r);
        }
        if (hum > 0) {
            for (int h = 0; h < kNrHumHarmonics; ++h) {
                l = osynth::dsp::svf_next(c.hum_l[h], c.hum_c[h],
                                          osynth::dsp::SvfMode::Notch, l);
                r = osynth::dsp::svf_next(c.hum_r[h], c.hum_c[h],
                                          osynth::dsp::SvfMode::Notch, r);
            }
        }
        /* Kept only where pass 2 needs it back: there the expander's share of
         * the correction is a multiple of the filtered *input*, not of the
         * bus. Overwriting the scratch is safe — xl was read out of it. */
        if (from_input) {
            s_nr_src_l[i] = l;
            s_nr_src_r[i] = r;
        }
        bl[i] += m * (l - xl);
        br[i] += m * (r - xr);
        /* Detected on what the unit is *looking at* rather than on the bus: in
         * `input` mode a synth beside it must not be able to hold the expander
         * open, and in `bus` mode the two are the same signal once the
         * crossfade is through. Worth knowing which level the threshold is in
         * terms of — the input as it arrives on the bus, i.e. after in.gain,
         * which is the one the master meter shows. */
        const float al = fabsf(l), ar = fabsf(r);
        const float t = (al > ar) ? al : ar;
        c.env += ((t > c.env) ? ka : kr) * (t - c.env);
        if (c.env > peak) peak = c.env;
    }

    /* Gain computer, in dB, with the same fixed soft knee the compressor
     * uses — mirrored, because this one acts on what is *under* the
     * threshold rather than over it. */
    const float db = 20.0f * log10f(fmaxf(peak, kNrFloorEps));
    const float over = db - thresh;
    const uint32_t hold_blocks =
        (uint32_t)(hold_ms * 0.001f * kSr / (float)frames);
    if (over > 0.0f) {
        c.hold = hold_blocks; /* something is being said: re-arm */
    } else if (c.hold > 0) {
        c.hold--;
    }

    float gr = 0.0f;
    if (c.hold == 0) {
        const float slope = fmaxf(ratio, 1.0f) - 1.0f;
        if (over >= 0.5f * kNrKnee) {
            gr = 0.0f;
        } else if (over <= -0.5f * kNrKnee) {
            gr = slope * over;
        } else {
            const float t = over - 0.5f * kNrKnee;
            gr = -slope * t * t / (2.0f * kNrKnee);
        }
        if (gr < floor_db) gr = floor_db;
    }
    const float target = powf(10.0f, gr * (1.0f / 20.0f));

    /* Pass 2, and `m` folded into the gain exactly as the compressor folds its
     * mix: at m = 0 both this and the crossfade above are the identity. */
    if (from_input) {
        /* The expander acts on the filtered input alone, so what belongs on
         * the bus is (g - 1) times it — added, not multiplied, since a
         * multiply here would duck the synth along with it. Together with
         * pass 1 the bus receives m * (g * filt(x) - x), which is the input's
         * contribution replaced by the cleaned one and nothing besides. */
        float d = m * (c.gain - 1.0f);
        const float dstep = (m * (target - 1.0f) - d) / (float)frames;
        for (size_t i = 0; i < frames; ++i) {
            d += dstep;
            bl[i] += d * s_nr_src_l[i];
            br[i] += d * s_nr_src_r[i];
        }
    } else {
        const float g0 = 1.0f + m * (c.gain - 1.0f);
        const float g1 = 1.0f + m * (target - 1.0f);
        const float gstep = (g1 - g0) / (float)frames;
        float g = g0;
        for (size_t i = 0; i < frames; ++i) {
            g += gstep;
            bl[i] *= g;
            br[i] *= g;
        }
    }
    c.gain = target;
}

/* ---- mic noise reduction (S42): per bin, not per band ----------------------
 *
 * The third and last of the noise-reduction units, and the one to reach for
 * when the complaint is "it is quiet between words but the hiss is still there
 * underneath my voice". Neither of the other two can fix that:
 *
 *   fx.nr   is a downward expander. One detector, one gain, applied to every
 *           frequency at once. The moment the voice crosses the threshold that
 *           gain is 1 and every bit of noise in the band comes back with it.
 *           Measured on a real recording it improves signal-to-noise by
 *           0.0 dB — it cleans the gaps, which is all a gate has ever done.
 *   fx.anr  is the right family, at a dozen bands that overlap heavily by
 *           construction (the difference bank sums to unity, and that is what
 *           it costs). A voice in one region opens the gain across a wide
 *           swathe of neighbours, so it manages about 4.5 dB underneath.
 *
 * The idea both are missing is that *speech is sparse in frequency*. At any
 * instant a voice occupies a fraction of the spectrum; every bin it is not in
 * is noise-only and can still be pushed down, mid-syllable. Resolving that
 * needs bins, not bands, and bins need a transform.
 *
 * ---- the shape ----
 *
 * 256-point frames, 50% overlap, sqrt-Hann on the way in and again on the way
 * out (WOLA, which sums to unity at this hop). 129 bins, 187 Hz apart, one
 * frame per two audio blocks. 512 was measurably better in the gaps and worse
 * everywhere that matters, at twice the latency; 128 gave up the resolution
 * that is the entire point. The 50% hop was within 0.5 dB of 75% on every
 * measure and costs half the transforms, which on this target is the number
 * that decides whether a unit ships.
 *
 * Per bin, per frame:
 *
 *   lambda   the noise power, tracked as a minimum over `adapt` in
 *            kMnrBuckets buckets — Martin's minimum statistics, and the same
 *            structure fx.anr uses, including the one-pole ahead of the
 *            minimum. That smoother is not optional: a raw periodogram bin
 *            wanders by tens of dB frame to frame on stationary noise, and the
 *            minimum of the wander is not the level of anything. kMnrBias
 *            corrects minimum back to mean.
 *   gamma    P / lambda, the a-posteriori SNR: what is here now against what
 *            is usually here.
 *   xi       the a-priori SNR, by the decision-directed estimator (Ephraim &
 *            Malah 1984): mostly last frame's *result* — g^2 * gamma — and a
 *            little of this frame's raw excess. This one line is what
 *            separates a spectral subtractor that sounds like a denoiser from
 *            one that sounds like wind chimes. Estimating xi from the current
 *            frame alone makes it swing wildly on the noise's own fluctuation,
 *            each bin opening and closing independently; leaning on the
 *            previous *decision* is a feedback path that holds a bin steady
 *            unless something really arrived in it. kMnrDdTauS is how long it
 *            leans, and it trades musical noise against transient response.
 *   g        xi / (1 + xi), the Wiener gain, floored at `fx.mnr.floor`.
 *
 * Floored for the same reason fx.nr.floor exists: a bin taken to nothing is
 * what a listener hears as an artefact, where a bin left 24 dB down is heard
 * as a quiet room.
 *
 * ---- mono, and the delay ----
 *
 * The source is folded to mono, cleaned, and written to both channels. The
 * vocoder's reasoning applies unchanged — this exists to clean a microphone
 * and osynth's is mono — and it halves the transform count, which is what
 * makes the unit affordable at all. `bus` mode will fold a stereo patch; that
 * is why `input` is the default.
 *
 * A frame cannot be analysed until it has arrived, so the cleaned signal comes
 * out kFftN samples behind. In `input` mode that is handled the way the other
 * two handle their correction — the input's contribution is removed from the
 * bus and the cleaned version added in its place:
 *
 *     bus += m * (cleaned(t - N) - input(t))
 *
 * which leaves the bus carrying synth(t) + cleaned(t - N). The input is 5.3 ms
 * late against the synth beside it and nothing is comb-filtered, because the
 * undelayed copy is removed rather than mixed with. Adding the *difference*
 * delayed instead — the obvious-looking alternative — would leave x(t) and
 * -x(t-N) both on the bus, which is a comb filter with teeth every 187 Hz.
 */

/* The transform's geometry belongs to fx_fft.h; everything below is named in
 * terms of it, and this file's units live in an anonymous namespace of their
 * own rather than in osynth::fx. */
using osynth::fx::kFftBins;
using osynth::fx::kFftN;

constexpr int kMnrHop = kFftN / 2;      /* 50% overlap */
constexpr int kMnrBuckets = 4;          /* 8 measured identical; 2 lost 3 dB */
constexpr float kMnrOversub = 3.0f;     /* what `amount` = 1 subtracts */
constexpr float kMnrBias = 1.6f;        /* minimum -> mean, per bin */
constexpr float kMnrDdTauS = 0.06f;     /* decision-directed memory */
constexpr float kMnrSmTauS = 0.12f;     /* smoother ahead of the minimum */
constexpr float kMnrHuge = 1e30f;       /* "this bucket took nothing" */
constexpr float kMnrEps = 1e-20f;       /* power, not amplitude */
constexpr float kMnrGamMax = 1e6f;      /* keeps a silent lambda finite */

struct MnrFx {
    /* Built once by mnr_build(); sqrt-Hann, so analysis and synthesis windows
     * multiply to a Hann that sums to unity at this hop. */
    float win[kFftN];
    float in[kFftN];        /* the last kFftN source samples, oldest first */
    float ola[kFftN];       /* overlap-add accumulator */
    float out[kMnrHop];     /* finished samples waiting to be handed back */
    int in_fill = 0;        /* samples written since the last frame */
    int out_rd = 0;
    int out_have = 0;
    /* Per bin. buck is [bin][bucket] rather than [bucket][bin] so that one
     * bin's whole working set is contiguous — the loop below touches every
     * field of a bin together and then never returns to it. */
    float sm[kFftBins];
    float lam[kFftBins];
    float buck[kFftBins][kMnrBuckets];
    float gp[kFftBins];     /* last frame's gain, for the decision-directed xi */
    float gamp[kFftBins];   /* ...and last frame's a-posteriori SNR */
    uint32_t cnt = 0;       /* frames into the current bucket */
    bool primed = false;
    bool dirty = false;     /* state worth clearing on the way down */
    UnitState u;
};

MnrFx s_mnr;

void mnr_build() {
    for (int i = 0; i < kFftN; ++i) {
        const float h =
            0.5f * (1.0f - cosf(2.0f * 3.14159265f * (float)i / (float)kFftN));
        s_mnr.win[i] = sqrtf(h);
    }
}

void mnr_reset(MnrFx& c) {
    if (!c.dirty) return;
    for (int i = 0; i < kFftN; ++i) {
        c.in[i] = 0.0f;
        c.ola[i] = 0.0f;
    }
    for (int i = 0; i < kMnrHop; ++i) c.out[i] = 0.0f;
    for (int k = 0; k < kFftBins; ++k) {
        c.sm[k] = 0.0f;
        c.lam[k] = 0.0f;
        c.gp[k] = 1.0f;
        c.gamp[k] = 1.0f;
        for (int w = 0; w < kMnrBuckets; ++w) c.buck[k][w] = kMnrHuge;
    }
    c.in_fill = 0;
    c.out_rd = 0;
    c.out_have = 0;
    c.cnt = 0;
    c.primed = false;
    c.dirty = false;
}

/* One frame: window, transform, scale every bin, transform back, overlap-add.
 * Everything that is not per-bin has been hoisted into the caller. */
/* Deliberately NOT SYNTH_RENDER_IRAM (S42); S43 drew the same line through
 * the vocoder and the limiter, so see the microphone-chain note above
 * VocBand for the rule the three now share. IRAM is full: the FX bus alone holds 41 KB of it, and this unit's
 * 2.6 KB was 2150 bytes more than the image had left. Something had to give,
 * and this is the right thing to give for three reasons — it is the newest
 * code, so nothing else was working before it and stopped; it is opt-in and
 * off by default, where the units it would have displaced run on every patch;
 * and it is the only unit here that already carries 5.3 ms of latency, so it
 * is the one with somewhere to absorb a cache miss.
 *
 * What that costs is the jitter immunity the note above SYNTH_RENDER_IRAM in
 * synth_config.h describes: with `fx.mnr` on, BLE or LittleFS traffic can put
 * a flash-cache miss in the middle of a frame. The frame is one per two
 * blocks rather than per block, and 2.7 ms of budget is a lot of misses, so
 * this should be inaudible — but it is a real difference from the rest of the
 * bus and the first thing to suspect if this unit alone crackles under app
 * traffic. Marking it back up means finding the IRAM somewhere else; turning
 * OSYNTH_RENDER_IN_IRAM off entirely is the blunt instrument that frees all
 * of it, at the cost of every other unit's immunity. */
void mnr_frame(MnrFx& c, float over, float gmin, float k_sm,
                                 float a_dd, bool learn, bool boundary) {
    /* Off the stack for the reason fx_fft.cpp's scratch is: 2 KB here plus
     * 1 KB there does not fit in a 6 KB audio task that also runs a reverb. */
    static float buf[kFftN];
    static float re[kFftBins], im[kFftBins];
    for (int i = 0; i < kFftN; ++i) buf[i] = c.in[i] * c.win[i];
    osynth::fx::fft_real(buf, re, im);

    for (int k = 0; k < kFftBins; ++k) {
        const float p = re[k] * re[k] + im[k] * im[k];
        if (!c.primed) {
            c.sm[k] = p;
            /* lam holds the raw minimum; kMnrBias is applied once, below. */
            c.lam[k] = p;
            for (int w = 0; w < kMnrBuckets; ++w) c.buck[k][w] = p;
        } else {
            c.sm[k] += k_sm * (p - c.sm[k]);
            if (learn) {
                /* Held: the profile is what is arriving, not a minimum of it.
                 * The buckets are filled too, so releasing `learn` resumes
                 * tracking from here instead of from a window of nothing. */
                c.lam[k] = c.sm[k];
                for (int w = 0; w < kMnrBuckets; ++w) c.buck[k][w] = c.sm[k];
            } else {
                if (c.sm[k] < c.buck[k][0]) c.buck[k][0] = c.sm[k];
                if (boundary) {
                    float best = kMnrHuge;
                    for (int w = 0; w < kMnrBuckets; ++w) {
                        if (c.buck[k][w] < best) best = c.buck[k][w];
                    }
                    if (best < kMnrHuge) c.lam[k] = best;
                    for (int w = kMnrBuckets - 1; w > 0; --w) {
                        c.buck[k][w] = c.buck[k][w - 1];
                    }
                    c.buck[k][0] = kMnrHuge;
                }
            }
        }

        /* `amount` is over-subtraction, exactly as it is on fx.anr: it inflates
         * the estimate rather than scaling the gain, so turning it up buys a
         * deeper cut where there is noise and still leaves a loud bin alone. */
        float lam_e = c.lam[k] * kMnrBias * over;
        if (lam_e < kMnrEps) lam_e = kMnrEps;
        float gam = p / lam_e;
        if (gam > kMnrGamMax) gam = kMnrGamMax;
        const float ex = (gam > 1.0f) ? (gam - 1.0f) : 0.0f;
        float xi = a_dd * c.gp[k] * c.gp[k] * c.gamp[k] + (1.0f - a_dd) * ex;
        if (xi < 1e-10f) xi = 1e-10f;
        float g = xi / (1.0f + xi);
        if (g < gmin) g = gmin;
        c.gp[k] = g;
        c.gamp[k] = gam;
        re[k] *= g;
        im[k] *= g;
    }

    osynth::fx::fft_real_inv(re, im, buf);
    for (int i = 0; i < kFftN; ++i) c.ola[i] += buf[i] * c.win[i];
}

/* Out of IRAM with mnr_frame() above — the note there covers why. */
void mnr_process(float* __restrict__ bl, float* __restrict__ br,
                 size_t frames) {
    MnrFx& c = s_mnr;
    const float m = unit_gate(c.u, pv(MNR_ON), nullptr, 0);
    if (m < 0.0f) {
        /* Off: the frame in flight describes a room that will have moved by
         * the time this is switched back on, and the overlap-add tail would
         * arrive 5 ms into the next thing said. */
        mnr_reset(c);
        return;
    }

    /* Same source contract as the other two (S39b) — the note in
     * anr_process() covers why these are not __restrict__. */
    const bool from_input = ((int)pv(MNR_SRC) == 1);
    const float* sl = bl;
    const float* sr = br;
    if (from_input) {
        if (frames > SYNTH_BLOCK_SIZE ||
            !audio_io_in_fx_block(s_nr_src_l, s_nr_src_r, frames)) {
            mnr_reset(c);
            return;
        }
        sl = s_nr_src_l;
        sr = s_nr_src_r;
    }
    c.dirty = true;

    const float over = fmaxf(pvm(MNR_AMOUNT) * kMnrOversub, 1e-3f);
    const float gmin = powf(10.0f, pvm(MNR_FLOOR) * (1.0f / 20.0f));
    const bool learn = pv(MNR_LEARN) >= 0.5f;

    /* A frame is the clock for everything above, exactly as a block is for the
     * other two units. */
    const float fr_s = (float)kMnrHop / kSr;
    const float k_sm = 1.0f - expf(-fr_s / kMnrSmTauS);
    const float a_dd = expf(-fr_s / kMnrDdTauS);
    uint32_t bucket = (uint32_t)(pvm(MNR_ADAPT) / (float)kMnrBuckets / fr_s);
    if (bucket < 1) bucket = 1;

    for (size_t i = 0; i < frames; ++i) {
        /* Read the source before writing the bus: in `bus` mode these alias. */
        const float xl = sl[i], xr = sr[i];
        c.in[kMnrHop + c.in_fill] = 0.5f * (xl + xr);
        ++c.in_fill;
        if (c.in_fill >= kMnrHop) {
            c.in_fill = 0;
            const bool boundary = (++c.cnt >= bucket);
            if (boundary) c.cnt = 0;
            mnr_frame(c, over, gmin, k_sm, a_dd, learn, boundary);
            c.primed = true;
            /* The first half of the accumulator is finished — no later frame
             * reaches it — so it becomes the output, and both buffers slide.
             * The hop is exactly half the frame, which is what makes this two
             * copies rather than a pair of ring cursors. */
            for (int j = 0; j < kMnrHop; ++j) {
                c.out[j] = c.ola[j];
                c.ola[j] = c.ola[j + kMnrHop];
                c.ola[j + kMnrHop] = 0.0f;
                c.in[j] = c.in[j + kMnrHop];
            }
            c.out_rd = 0;
            c.out_have = kMnrHop;
        }
        /* Never underruns once primed: a frame lands on exactly the sample
         * that would have emptied this. Before the first one it is silent,
         * which is the unit's latency and not a fault. */
        float y = 0.0f;
        if (c.out_have > 0) {
            y = c.out[c.out_rd++];
            --c.out_have;
        }
        /* The input's contribution replaced by the cleaned one, or the bus
         * crossfaded to it — settled above, by where xl came from. */
        bl[i] += m * (y - xl);
        br[i] += m * (y - xr);
    }
}

/* ---- vocoder (S38): the input's spectrum imposed on the synth bus ----
 *
 * A classic analysis/synthesis vocoder, and the first unit on this bus whose
 * *modulator* is not the bus. Two matched constant-Q bandpass banks: one
 * analyses the audio input (`audio_io_in_mono()`, so whichever device
 * `in.source` names), one splits the carrier; each analysis band drives an
 * envelope follower that multiplies its carrier partner, and the products are
 * summed. Say a vowel and the synth says it.
 *
 * Ahead of every effect, deliberately — only the two noise-reduction units
 * (S39) come before it, and those clean the carrier rather than shape it.
 * The vocoder decides what the sound *is*, so everything after it — drive,
 * chorus, delay, reverb — colours the spoken result rather than the raw
 * carrier, which is what a reverb on a vocoder is supposed to do. `fx.voc.mix` therefore crossfades against the untouched
 * synth, which is also the clearest thing for it to mean.
 *
 * The carrier is the bus summed to mono, and the wet goes out to both
 * channels. That halves the filter count against a stereo carrier bank and
 * costs nothing that matters: a vocoder's output is a single voice, and every
 * hardware one worth copying is mono. The dry half of the crossfade keeps its
 * stereo, so a partly-wet setting still has the synth's image in it.
 *
 * Independent of `in.route` and `in.gain`, exactly like the granular engine's
 * capture ring: those name the *monitor* path, and a modulator is a control
 * signal, not something to hear. Speaking into a vocoder while monitoring your
 * own voice dry would be the wrong default and is one route setting away.
 *
 * `fx.voc.freeze` records a phrase and replays it under every note (S43).
 * Held down it is live and recording; released it becomes the modulator, and
 * each note-on restarts it from the beginning. Play a chord and the synth
 * says the phrase; play another and it says it again. Past the end of the
 * recording there is nothing to analyse, so a note held longer than the
 * phrase simply stops speaking — the vocoder goes quiet over a carrier that
 * is still sounding, which is what silence into a vocoder has always meant.
 * `fx.voc.loop` is the other reading of the same gesture: the phrase repeats
 * for as long as the note is held, which is what a rhythmic capture wants and
 * what a spoken one does not.
 *
 * It replaced a spectral freeze, and the reason is worth keeping. That
 * version held the band envelopes where they stood, which is one frame of
 * spectrum: a single vowel. It failed twice over. Frozen live, the envelopes
 * follow fx.voc.release (25-40 ms) and nobody releases a button while still
 * making the sound, so it captured the silence after the word — measured, 12
 * bands above the gate at the instant speech stopped and none 100 ms later.
 * Given a peak-hold to reach back through that gap, each band then took its
 * maximum from a *different* moment, which is the union of every vowel said
 * and therefore flat — and a flat set of band gains is a flat filter, so the
 * carrier passed through untouched. Both failures are one fact seen twice: a
 * vowel is a shape across the bank at a single instant, and holding one is
 * not what "capture a sample" means to anyone holding the button.
 *
 * Sibilance survives here where it could not survive a freeze: it is live HF
 * from whatever the modulator currently is, and in sample mode that is the
 * recording, so the consonants come back with it.
 *
 * Silence in, silence out, and that is correct rather than a failure: a
 * vocoder with nothing said into it has nothing to say. A build with no audio
 * input at all warns once and behaves the same way.
 *
 * ---------------------------------------------------------------------------
 * Gain staging (S43), and why this unit used to be so hard to set up
 *
 * Both banks read SvfMode::Bp, which in this SVF is the *un-normalized*
 * bandpass: its gain at the band centre is Q, not 1. Their outputs are then
 * multiplied together — an envelope from the analysis bank times a sample
 * from the synthesis bank — so the wet path carried a Q^2 term. Q here is not
 * a control of its own either; voc_rebuild() derives it from the band count
 * and the span as well, so *four* knobs moved the output level:
 *
 *   fx.voc.q     0 -> 1        ~ +28 dB   (Q_eff spans 5x, squared)
 *   fx.voc.bands 10 -> 16      ~ +13 dB   (q_nat rises as the bands narrow)
 *   a narrowed low/high span   ~ +5 dB
 *   speaking louder            linear and unbounded — no modulator AGC
 *
 * Every one of those reads as a *tone* control, so the unit had to be
 * re-gained after each of them, and a phrase leant into arrived at the sink's
 * soft_clip() as distortion. Two changes, and neither is a compensation
 * curve bolted on top:
 *
 *  - BpN, not Bp. The normalized bandpass is unity at the centre whatever Q
 *    is, so a bank whose skirts meet sums back to something flat *whatever
 *    the band count and span are*. fx.voc.q is now a timbre control and only
 *    that. The magnitudes it used to carry are restored by kVocQRef, a
 *    frozen Q standing where the derived one was — see the note there, and
 *    note especially that getting this wrong once made the unit quiet and
 *    unintelligible rather than merely mis-levelled, because `fx.voc.gate`
 *    is an absolute threshold that has to stay in the same domain.
 *
 *  - fx.voc.norm scales the band envelopes so the bank always sees a
 *    reference speaking loudness (kVocNormRef, and it has to be a value a
 *    voice really reaches), by dividing out the broadband
 *    follower the sibilance path already maintained. That is what makes a
 *    setting portable between two people, two mics and two input gains — and
 *    it is what makes a stored preset mean anything at all, since without it
 *    the "right" fx.voc.level depends on how far away you are sitting. The
 *    boost is capped at kVocNormRef/kVocNormFloor so a silent room is not
 *    amplified into the patch; the gate still runs first, in the modulator's
 *    own domain, so what gets normalized is what survived it.
 *
 * The wet path ends in soft_clip(). That is a backstop and not a sound: with
 * the two changes above a hot phrase lands near unity rather than 20 dB over
 * it, and anything left is the master limiter's job. It is here rather than
 * downstream so that an overload never reaches the delay and reverb lines,
 * which store int16 and would keep replaying it.
 */

/* The bank's frozen Q (S43). SvfMode::BpN is exactly SvfMode::Bp divided by
 * the band's Q, so every magnitude in this unit moved by that factor when the
 * banks changed -- including, fatally, its position relative to `fx.voc.gate`,
 * which is an absolute threshold and did not move with it. At the registered
 * defaults the envelopes shrank 4.67x while the gate stayed at 0.012, so most
 * bands sat below it and were skipped outright: quiet, and unintelligible
 * with it.
 *
 * The repair is not a new set of staging constants -- that was the first
 * attempt and it was two guesses wearing a comment. Because BpN = Bp / Q_eff
 * is an identity, putting a *constant* where Q_eff used to be reproduces the
 * old arithmetic term for term:
 *
 *     wet = Q * SUM(BpN_car * (Q*b.env - gate)),  then sibilance, then level
 *
 * with Q pinned here instead of derived per rebuild. fx.voc.gate and
 * fx.voc.level therefore keep the meanings, defaults and ranges they had
 * before S43 -- a stored patch is unaffected -- and the output no longer
 * tracks fx.voc.q, fx.voc.bands or the low/high span, which was the entire
 * point of the change.
 *
 * The value is Q_eff at the registered defaults: 16 bands over 150-7000 Hz at
 * q = 0.5. A 10-band build (no PSRAM) ran at 2.79 before and now runs here
 * too, so it is ~4.5 dB louder than it used to be. That is the dependence
 * being removed rather than a regression: the same patch now sounds the same
 * on both parts, which it never did. */
constexpr float kVocQRef = 4.671f;

/* Normalization (fx.voc.norm). kVocNormRef is the broadband follower value
 * the modulator is pinned to, so a speaker sitting at it gets nrm = 1 and
 * behaves exactly as the unit does with normalization off -- that is the
 * calibration point, and it is why turning the switch on is not also a level
 * change -- 0.04 is where env_bb actually sits for an ordinary mic signal,
 * measured, and getting that wrong is what made this switch a +10 dB button
 * instead of a normaliser. kVocNormFloor caps the boost at ref/floor, which
 * is 2x: enough to bring a quiet talker up, nowhere near enough to lift a
 * room between phrases into the patch. It was 20x, and it reached it.
 *
 * The gate is subtracted before this, in the modulator's own domain, because
 * it describes the room rather than the voice. The trade-off is real and
 * worth knowing: a very quiet source has band envelopes near the gate, so it
 * is gated before it can be normalized up. fx.voc.gate is the control for
 * that, and lowering it is the answer to "the vocoder ignores me". */
constexpr float kVocNormRef = 0.04f;
constexpr float kVocNormFloor = 0.02f;

/* How fast the normaliser is allowed to turn UP, in ms. Down is instantaneous
 * (the look-ahead below sees the block before it is used), which is what stops
 * a syllable opening with a burst; up has to be slow, and originally was not
 * slow at all -- nrm simply tracked 1/env_bb, so it rose as fast as env_bb
 * decayed. That is an AGC with no release: when a word ends env_bb collapses
 * while the high-Q band envelopes are still ringing, and the ring-out gets
 * multiplied by the rising gain. A swell after every word, which is what
 * "fx.voc.norm clips" turned out to be. 300 ms is slower than any band
 * envelope's release (fx.voc.release tops out at 500 ms, but the useful range
 * is tens of ms), so the gain can no longer outrun the decay it is dividing. */
constexpr float kVocNormRiseMs = 300.0f;

/* fx.voc.clarity (S43): the consonant path, and what is wrong with it off.
 *
 * The sibilance tap is gated by a presence test, `open`, which asks whether
 * the modulator is saying anything and whether there is a carrier to say it
 * on. Two things about the first half are wrong, and both only became visible
 * once anyone tried to be understood through the unit:
 *
 *   - it saturates at env_bb = 0.125 (`bb * 8.0f`), and an ordinary mic signal
 *     puts env_bb at 0.02-0.04. So it never came close to opening: measured
 *     over a speech-like modulator its mean value is 0.07, which is -23 dB on
 *     the consonants;
 *   - it reads RAW env_bb, so it is the one quantity in this unit that
 *     fx.voc.norm does not reach. A quiet speaker got the bank normalised up
 *     to reference while their consonants stayed exactly as buried.
 *
 * With clarity on the test is taken in the normalised domain and scaled to
 * saturate at the reference loudness rather than three times above it, and
 * the tap is lifted by kVocClarityGain. Off is the pre-S43 behaviour exactly,
 * so the two can be A/B'd, which is the point: this is a taste fork. More
 * consonant is more intelligible and also more raw microphone -- at some
 * point you stop hearing a vocoder and start hearing a voice with a synth
 * behind it, and where that point sits is not something a default can know.
 *
 * kVocClarityGain is sized so consonants land 6-10 dB under the vowels during
 * a burst, measured; it is not a taste value and moving it wants voc_sim. */
constexpr float kVocClarityGain = 0.35f;

/* ---- the microphone chain is not IRAM-resident (S43) ----
 *
 * vocoder_process() and limiter_process() below carry NOINLINE_ATTR and no
 * SYNTH_RENDER_IRAM, joining mnr_process()/mnr_frame() out in flash. Those
 * three are the units whose input is the microphone rather than the
 * instrument, and drawing the line there is what makes this a rule rather
 * than a series of individual retreats:
 *
 *   the instrument path - engines, drums, looper, and every effect that
 *                         colours them - keeps its flash-cache immunity;
 *   the microphone path - mic NR, vocoder, limiter - trades it for the IRAM
 *                         the instrument path needs.
 *
 * The measurements behind it. `sram_low` is 175 KB and holds all IRAM code
 * plus all *initialised* data - .bss lives in sram_high, and none of this is
 * guessable from the linker's errors; tools/iram_budget.py has the layout. A
 * linked image with none of this session's work leaves 5 408 bytes of it
 * free, and the mic NR alone is at or over that line, so this is not one unit
 * being slightly too big. These two free about 4.2 KB against a shortfall of
 * at most 960 bytes, which is the margin this should have had from the start.
 *
 * NOINLINE_ATTR is load-bearing, not decoration. Both are anonymous-namespace
 * functions with one call site, so GCC inlines them into fx_process() - which
 * *is* SYNTH_RENDER_IRAM - and the callee's own placement then means nothing.
 * Dropping SYNTH_RENDER_IRAM without noinline moves zero bytes and reports
 * the identical linker error, which is a mistake worth making only once.
 *
 * What it costs: with the vocoder or the limiter on, BLE or LittleFS traffic
 * can put a flash-cache miss inside the block. The budget is 1.33 ms and a
 * miss is microseconds - the same arithmetic the mic NR's note works through
 * to reach "should be inaudible" - but it is a real difference from the rest
 * of the bus, and these are the first units to suspect if something crackles
 * only while the app is talking. */

/* The capture buffer (S43). Mono int16 at the render rate, PSRAM-preferred
 * through line_alloc() like every other buffer on this bus.
 *
 * Sized in seconds rather than samples because what it holds is a spoken
 * phrase, and gated on the same PSRAM proxy the band count uses: 2.5 s is
 * 240 KB, which is nothing next to the looper on a P4 or S3 and impossible on
 * a classic ESP32, where 0.75 s still holds a word or two.
 *
 * Recording is linear from the press, not circular: a circular buffer would
 * always hold the last N seconds, which sounds like the same thing but makes
 * the release edge decide where the phrase *starts*, and that is exactly the
 * timing nobody can hit — the mistake the spectral freeze made twice. Here
 * the press starts the recording and the release only ends it, so both edges
 * are ones a person can place. */
#if CONFIG_SPIRAM
constexpr float kVocSampS = 2.5f;
#else
constexpr float kVocSampS = 0.75f;
#endif
constexpr uint32_t kVocSampLen = (uint32_t)(kVocSampS * (float)kSr);

struct VocBand {
    osynth::dsp::Svf mod; /* analysis bandpass, on the modulator */
    osynth::dsp::Svf car; /* synthesis bandpass, on the carrier */
    float env = 0.0f;     /* follower output */
};

struct VocoderFx {
    VocBand b[kVocBandsMax];
    osynth::dsp::SvfCoef mc[kVocBandsMax];
    osynth::dsp::SvfCoef cc[kVocBandsMax];
    osynth::dsp::Svf sib_hp; /* sibilance: the modulator above the top band */
    osynth::dsp::SvfCoef sib_c;
    osynth::dsp::Noise rng;
    float env_bb = 0.0f;  /* broadband modulator follower, opens sibilance */
    float env_car = 0.0f; /* carrier follower — see the sibilance block */
    float nrm = 1.0f;     /* fx.voc.norm scale, block rate */
    /* Capture (S43). `rec` is linear: `rec_w` is the write cursor while
     * recording and `rec_len` freezes it on release. `play` runs past
     * `rec_len` and stays there, which is what makes the tail silent. */
    osynth::dsp::Line rec;
    uint32_t rec_w = 0;
    uint32_t rec_len = 0;
    uint32_t play = 0;
    bool rec_ok = false;
    bool was_sampling = false; /* fx.voc.freeze edge, both directions */
    int n = 0;           /* bands actually built */
    /* Coefficient cache. Rebuilding is 2N tanf, which is affordable per block
     * but pointless: these five change when a knob moves and not otherwise. */
    int c_bands = -1;
    float c_low = 0.0f, c_high = 0.0f, c_q = -1.0f, c_shift = 1e9f;
    osynth::dsp::Smooth s_level, s_sib, s_gate;
    UnitState u;
    bool warned = false;
};

VocoderFx s_voc;

/* One block of modulator, mono. Audio task only, live only inside
 * vocoder_process(). */
float s_voc_mod[SYNTH_BLOCK_SIZE];

void voc_rebuild(VocoderFx& v, int bands, float low, float high, float q01,
                 float shift_st) {
    if (bands < kVocBandsMin) bands = kVocBandsMin;
    if (bands > kVocBandsMax) bands = kVocBandsMax;
    /* A span narrower than an octave would put every band on top of its
     * neighbour and the ratio maths below into a divide by ~0. */
    if (high < low * 2.0f) high = low * 2.0f;

    const float ratio = powf(high / low, 1.0f / (float)(bands - 1));
    /* Constant-Q, sized so adjacent skirts meet: at Q = sqrt(r)/(r-1) a band's
     * -3 dB edges land on its neighbours' centres. That is the setting where
     * the bank sums back to something flat, so it is what `fx.voc.q` = 0.5
     * means; below it the bands overlap into a smear, above it they separate
     * into the hollow, ringing, unmistakably-a-vocoder end. */
    const float q_nat = sqrtf(ratio) / (ratio - 1.0f);
    const float k = 1.0f / fmaxf(q_nat * (0.4f + 1.6f * q01), 0.05f);

    /* Formant shift: the carrier is split at moved centres while the
     * modulator is analysed where the voice actually is, so the spectral
     * envelope is transposed without touching the pitch. Up is chipmunk, down
     * is the classic robot-giant. svf_coef_k() clamps to [20, 0.45*sr], so a
     * shifted top band cannot run past Nyquist. */
    const float cmul = exp2f(shift_st * (1.0f / 12.0f));

    float f = low;
    for (int i = 0; i < bands; ++i) {
        v.mc[i] = osynth::dsp::svf_coef_k(f, k, kSr);
        v.cc[i] = osynth::dsp::svf_coef_k(f * cmul, k, kSr);
        f *= ratio;
    }
    /* Sibilance tap: everything above the top band's centre. Consonants live
     * up there and carry most of the intelligibility, and no band bank can
     * reproduce them — they are noise, not a resonance. */
    v.sib_c = osynth::dsp::svf_coef_k(high, 0.9f, kSr);
    v.n = bands;
}

NOINLINE_ATTR void vocoder_process(float* __restrict__ bl,
                                   float* __restrict__ br, size_t frames) {
    VocoderFx& v = s_voc;
    const float m =
        unit_gate(v.u, gated(pv(VOC_ON), pvm(VOC_MIX)), nullptr, 0);
    if (m < 0.0f) {
        /* Fully bypassed: drop the followers so re-enabling starts from
         * silence rather than from a vowel someone said a minute ago. */
        for (int i = 0; i < kVocBandsMax; ++i) v.b[i].env = 0.0f;
        v.env_bb = 0.0f;
        v.env_car = 0.0f;
        v.nrm = 1.0f;
        /* The recording survives a bypass, and so must was_sampling: the edges
         * belong to the parameter, not to the switch. Clearing either would
         * throw a phrase away, or re-arm a recording over it, because someone
         * toggled the unit off and on. */
        return;
    }

    const int bands = (int)pv(VOC_BANDS);
    const float low = pvm(VOC_LOW);
    const float high = pvm(VOC_HIGH);
    const float q01 = pvm(VOC_Q);
    const float shift = pvm(VOC_SHIFT);
    if (bands != v.c_bands || low != v.c_low || high != v.c_high ||
        q01 != v.c_q || shift != v.c_shift) {
        voc_rebuild(v, bands, low, high, q01, shift);
        v.c_bands = bands;
        v.c_low = low;
        v.c_high = high;
        v.c_q = q01;
        v.c_shift = shift;
    }
    const int n = v.n;

    /* Where the modulator comes from this block: the microphone, or the
     * recorded phrase. Either way the analysis below is identical, which is
     * the whole point of storing audio rather than a spectral frame.
     *
     * The frames <= guard is the scratch buffer's contract, not defensiveness
     * about a value that varies: the render callback always passes exactly
     * SYNTH_BLOCK_SIZE. It is here so that if that ever stops being true the
     * unit goes quiet instead of reading past s_voc_mod. */
    const bool sampling = pv(VOC_FREEZE) >= 0.5f && v.rec_ok;
    const bool fits = frames <= SYNTH_BLOCK_SIZE;
    bool live = false;

    if (sampling != v.was_sampling) {
        if (sampling) {
            /* Release: the phrase ends wherever the recording got to, and the
             * playhead is left at that end so nothing sounds until a note
             * asks for it. */
            v.rec_len = v.rec_w;
            v.play = v.rec_len;
        } else {
            /* Press: a new phrase starts here. */
            v.rec_w = 0;
            v.rec_len = 0;
        }
        v.was_sampling = sampling;
    }

    if (sampling && fits) {
        /* A note that started this block restarts the phrase. Read once per
         * block: the tap reports the loudest note-on of the block, so a chord
         * retriggers once rather than once per key. */
        if (voice_manager_block_note() != 0) v.play = 0;
        /* Past the end: silence, or back to the start. The rec_len test is
         * what keeps an empty recording from wrapping onto itself and reading
         * a buffer nothing was ever written into.
         *
         * The splice is not crossfaded, and does not need to be: the
         * modulator is a control signal rather than something anyone hears,
         * so a discontinuity here costs a fast step in the band followers --
         * which have attack and release smoothing of their own -- instead of
         * the click it would cost in an audio loop. */
        const bool loop = pv(VOC_LOOP) >= 0.5f && v.rec_len > 0;
        for (size_t i = 0; i < frames; ++i) {
            if (v.play >= v.rec_len) {
                if (!loop) {
                    s_voc_mod[i] = 0.0f;
                    continue;
                }
                v.play = 0;
            }
            s_voc_mod[i] = (float)v.rec.buf[v.play++] * (1.0f / 32768.0f);
        }
        /* Live even past the end of the phrase: the analysis has to keep
         * running on the silence so the band envelopes decay through their
         * release instead of stopping wherever the last sample left them. */
        live = true;
    } else if (!sampling && fits && audio_io_in_mono(s_voc_mod, frames)) {
        live = true;
        /* Record while the button is down, linearly, until the buffer is
         * full. Full is not an error: it is the longest phrase this build can
         * hold, and the release still ends it wherever it got to. */
        if (v.rec_ok) {
            for (size_t i = 0; i < frames && v.rec_w < v.rec.len; ++i) {
                int32_t s = (int32_t)(s_voc_mod[i] * 32767.0f);
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                v.rec.buf[v.rec_w++] = (int16_t)s;
            }
        }
    }
    if (!live && !v.warned) {
        v.warned = true;
        /* Queued, not printed: this runs on the audio task, where the console
         * write would itself be a dropout (synth_warn.h). */
        osynth::dsp::render_warn(
            TAG,
            "vocoder: no audio input on this build — it has nothing to "
            "analyse and will stay silent");
    }

    /* One-pole follower coefficients. Attack and release are separate because
     * a vocoder lives on that asymmetry: fast enough to catch a consonant,
     * slow enough that a vowel does not chatter between syllables. */
    const float ka =
        1.0f - expf(-1.0f / fmaxf(pvm(VOC_ATTACK) * 0.001f * kSr, 1.0f));
    const float kr =
        1.0f - expf(-1.0f / fmaxf(pvm(VOC_RELEASE) * 0.001f * kSr, 1.0f));

    const float level = osynth::dsp::smooth_lin(v.s_level, pvm(VOC_LEVEL));
    const float sib = osynth::dsp::smooth_lin(v.s_sib, pvm(VOC_SIB));
    /* Downward expansion rather than a hard threshold: subtracting the floor
     * is smooth, cannot chatter on a band hovering at it, and takes the DC
     * pedestal of room noise out of the envelope along with the noise. */
    const float gate = osynth::dsp::smooth_lin(v.s_gate, pvm(VOC_GATE)) * 0.2f;

    const bool clarity = pv(VOC_CLARITY) >= 0.5f;
    const int carrier = (int)pv(VOC_CARRIER);
    const bool use_bus = (carrier != 1);   /* bus, or bus+noise */
    const bool use_noise = (carrier != 0); /* noise, or bus+noise */

    /* Modulator normalization, block rate. One number for the whole bank, so
     * the *relative* shape of the spectrum — which is all a vocoder is —
     * survives untouched and only its overall size is pinned. Once per block
     * rather than per sample because a divide is the most expensive thing
     * that could go in the sample loop and the quantity it computes moves at
     * the follower's rate, which is far slower than 1.33 ms.
     *
     * The divisor is the larger of the broadband follower and *this* block's
     * mean-rectified level, which is the whole reason the second term is
     * there: env_bb carries the previous block, so at the first block of a
     * word it is still sitting near the floor and the reciprocal is at its
     * ceiling — every syllable would open with a 1.33 ms burst 26 dB over the
     * rest of it. The block is already in s_voc_mod before the loop runs, so
     * taking its level first is a look-ahead that costs one pass of adds and
     * makes an under-estimate impossible. Falling back to env_bb on the way
     * down is what keeps the release slow instead of snapping the gain up
     * between words.
     *
     * Frozen blocks hold the last value along with the envelopes it belongs
     * to; a build with no audio input has nothing to normalize. */
    const bool norm = pv(VOC_NORM) >= 0.5f;
    if (live && norm && frames > 0) {
        float sum = 0.0f;
        for (size_t i = 0; i < frames; ++i) sum += fabsf(s_voc_mod[i]);
        const float blk = sum / (float)frames;
        const float ref = (blk > v.env_bb) ? blk : v.env_bb;
        const float target = kVocNormRef / fmaxf(ref, kVocNormFloor);
        /* Instant down, slow up -- the limiter idiom, and here for the same
         * reason: a gain that is allowed to rise as fast as its divisor falls
         * will amplify every decay it is supposed to be levelling. */
        if (target < v.nrm) {
            v.nrm = target;
        } else {
            const float k = 1.0f - expf(-((float)frames / kSr) /
                                        (kVocNormRiseMs * 0.001f));
            v.nrm += k * (target - v.nrm);
        }
    } else {
        /* Switched off, or nothing arriving to measure. */
        v.nrm = 1.0f;
    }
    const float nrm = v.nrm;

    for (size_t i = 0; i < frames; ++i) {
        /* Analysis. `live` is false only when there is no modulator at all —
         * no audio input on this build — in which case the followers decay
         * and the unit falls silent, which is the correct thing for a vocoder
         * with nothing said into it. */
        if (live) {
            const float x = s_voc_mod[i];
            const float a = fabsf(x);
            v.env_bb += (a > v.env_bb ? ka : kr) * (a - v.env_bb);
            for (int k = 0; k < n; ++k) {
                VocBand& b = v.b[k];
                const float y = fabsf(osynth::dsp::svf_next(
                    b.mod, v.mc[k], osynth::dsp::SvfMode::BpN, x));
                b.env += (y > b.env ? ka : kr) * (y - b.env);
            }
        }

        /* Synthesis. The carrier is the bus in mono, optionally with noise —
         * a band bank can only shape what it is given, so a thin or silent
         * synth has nothing for the vowels to land on, and the noise source
         * is what makes whispers and unpitched consonants work. */
        float c = use_bus ? 0.5f * (bl[i] + br[i]) : 0.0f;
        if (use_noise) c += kVocNoiseGain * osynth::dsp::noise_next(v.rng);

        /* How much carrier there is to shape. The band products carry this
         * for free — a band multiplied by a silent carrier is silent — but
         * the sibilance path does not, and without this it is a direct line
         * from the microphone to the output: with no note held and the route
         * off, speaking is heard as itself, which reads as "the vocoder is
         * just passing the mic through". A vocoder with no carrier has
         * nothing to say, consonants included. */
        const float ac = fabsf(c);
        v.env_car += (ac > v.env_car ? ka : kr) * (ac - v.env_car);

        float wet = 0.0f;
        for (int k = 0; k < n; ++k) {
            VocBand& b = v.b[k];
            /* Gate first, in the modulator's own domain: it is a description
             * of the room's noise floor, so normalizing before subtracting it
             * would make the same room read as a different floor at every
             * distance from the mic. */
            const float e = (b.env * kVocQRef - gate) * nrm;
            if (e <= 0.0f) {
                /* Still run the filter: its state has to stay current or the
                 * band rings when the gate reopens. */
                (void)osynth::dsp::svf_next(b.car, v.cc[k],
                                            osynth::dsp::SvfMode::BpN, c);
                continue;
            }
            wet += osynth::dsp::svf_next(b.car, v.cc[k],
                                         osynth::dsp::SvfMode::BpN, c) * e;
        }
        /* The synthesis bank's half of the frozen Q. Applied to the sum
         * rather than per band: identical result, one multiply. */
        wet *= kVocQRef;

        /* Sibilance rides over the bank, gated by the broadband envelope so
         * room hiss does not sit on top of the patch between phrases. It is
         * live HF from whatever the modulator currently is, so unlike the old
         * spectral freeze it survives a capture: the consonants come back
         * with the replayed phrase. */
        if (live) {
            /* Filtered unconditionally, gain applied after: skipping the call
             * while shut leaves the filter's state stale, and it then rings on
             * the first sample after it reopens. */
            const float hp = osynth::dsp::svf_next(
                v.sib_hp, v.sib_c, osynth::dsp::SvfMode::Hp, s_voc_mod[i]);
            const float bb = v.env_bb - gate;
            if (sib > 0.0f && bb > 0.0f) {
                /* Two openings, and both have to be true: the modulator is
                 * saying something (bb) *and* there is a carrier for it to sit
                 * on (env_car). The scale factors just decide how quickly each
                 * reaches full — a carrier at any ordinary playing level
                 * saturates its term, so this is a presence test, not a
                 * loudness one. */
                /* Normalised and scaled to saturate at the reference
                 * loudness when clarity is on; the raw, far-too-high original
                 * when it is off. See the kVocClarityGain note. */
                const float open =
                    (clarity ? fminf(bb * nrm * (1.0f / kVocNormRef), 1.0f)
                             : fminf(bb * 8.0f, 1.0f)) *
                    fminf(v.env_car * 20.0f, 1.0f);
                /* Normalized with the bank, for the same reason and by the
                 * same number: a consonant that grew louder as you leant in
                 * while the vowels did not would be worse than either. No
                 * staging gain of its own -- it rides `level` with the bank,
                 * exactly as it did before S43, and the bank's frozen Q has
                 * already been applied above so the balance is preserved. */
                wet += hp * sib * open * nrm *
                       (clarity ? kVocClarityGain : 1.0f);
            }
        }

        /* Backstop, not a sound — see the staging note in the header. It is
         * the identity below 0.8, so a properly staged patch never reaches
         * it, and an improperly staged one saturates here instead of writing
         * a spike into the delay and reverb lines downstream. */
        wet = osynth::dsp::soft_clip(wet * level);
        bl[i] += m * (wet - bl[i]);
        br[i] += m * (wet - br[i]);
    }
}

/* ---- drive: waveshaper on the finished mix (S34) ----
 *
 * The graph's Shaper node, moved to where it can reach the drums and the
 * looper. First of the effects proper — the noise reduction and the vocoder
 * ahead of it are a cleanup and a source, not effects — for the reason at
 * the top of this file.
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
    const float m = unit_gate(d.u, gated(pv(DRV_ON), pvm(DRV_MIX)), nullptr, 0);
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
    const float m = unit_gate(c.u, gated(pv(CHO_ON), pvm(CHO_MIX)), lines, 2);
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
    const float m = unit_gate(f.u, gated(pv(FLG_ON), pvm(FLG_MIX)), lines, 2);
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
    const float m = unit_gate(p.u, gated(pv(PHS_ON), pvm(PHS_MIX)), nullptr, 0);
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
    const float m = unit_gate(d.u, gated(pv(DLY_ON), pvm(DLY_MIX)), lines, 2);
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
    const float m = unit_gate(g.u, gated(pv(GRN_ON), pvm(GRN_MIX)), lines, 1);
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

    /* ---- S36: the algorithm selector and the stages shared across it ---- */

    /* 0xFF so the first block always takes the switch path and populates
     * s_rev_lines; there is no valid algorithm this can be mistaken for. */
    uint8_t algo = 0xFF;
    uint8_t nlines = 0;
    /* A second scrub cursor, used only while changing algorithm. It is
     * separate from `u` because the two scrubs happen for different reasons
     * and can be in flight at once: `u` clears a unit the player switched
     * off, this clears a topology nobody is listening to yet. Sharing one
     * cursor would have the later reason silently cancel the earlier one. */
    UnitState sw;

    /* ---- algorithm change, as a fade rather than a cut ----
     *
     * Dropping straight into the scrub was a step on the *dry* path, which is
     * the half nobody notices when writing this. The unit renders
     * `(1-m)*dry + wet`; a block that returns early renders `dry` at unity.
     * At fx.rev.mix = 0.5 that is +6 dB on the dry signal in one sample, and
     * -6 dB again ~40 ms later when the scrub finishes. Two clicks, on a
     * control whose whole point is that the wet tail changes character.
     *
     * `sw_f` rides on top of the unit gate and takes `m` to zero before the
     * topology changes and back afterwards. At sw_f = 0 the mix law already
     * gives dry = 1, wet = 0 — exactly what the scrub phase emits — so the
     * three phases join continuously and nothing steps. Coming back needs no
     * fade of its own on the wet side (the lines are zero, so the new
     * algorithm builds up from silence); what sw_f is restoring is the dry
     * gain, which does have to ramp.
     *
     * The old algorithm keeps rendering all the way through the fade-out,
     * which is what makes its tail decay rather than vanish: `run` below is
     * `algo`, the committed selection, never the requested one. */
    enum : uint8_t { kSwIdle, kSwOut, kSwScrub, kSwIn };
    uint8_t sw_phase = kSwIdle;
    uint8_t want = 0xFF; /* the selection sw_phase is working toward */
    float sw_f = 1.0f;   /* 1 = the committed algorithm is fully present */

    Line pre_l, pre_r;                 /* shared pre-delay, in front of all */
    float tone_l = 0.0f, tone_r = 0.0f;
    osynth::dsp::Smooth s_pre, s_tone, s_width;
};

ReverbFx s_rev;

/* Whether each optional algorithm got its buffers at boot. Checked by
 * rev_impl() so a selection that cannot render falls back rather than
 * going silent — the same sink-fallback rule the rest of the bus follows. */
bool s_rev_wet_ok = false;
#if SYNTH_ENABLE_FX_GPL
bool s_rev_mverb_ok = false;
bool s_rev_dusk_ok = false;
#endif

/* The shared pre-delay plus every line of whichever algorithm is selected,
 * for the bypass scrub. Worst case is DuskVerb at 2 + 38 = 40; the array is
 * 192 bytes, and being one entry short of an algorithm's line count would
 * leave a tail unscrubbed — a stale-audio bug that only shows up the second
 * time you enable the effect, which is the worst kind to go looking for. */
constexpr size_t kRevLineMax = 48;
Line* s_rev_lines[kRevLineMax];

/* Algorithm-change fade, per block. 1/8 gives ~11 ms each way — the same
 * ramp the voice manager's engine-switch mute uses, and for the same reason:
 * it is long enough to bury a full-scale gain step and short enough that the
 * gap either side of the scrub does not grow noticeably. The scrub between
 * them is 10-40 blocks on its own, so this adds about a third to a change
 * nobody is playing through. */
constexpr float kSwStep = 0.125f;

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

/* Freeverb's own render, wet only, so the dispatch below has one shape for
 * all four algorithms. Byte-for-byte the arithmetic it has always run — the
 * only change is that the dry/wet mix moved out to the caller, which does it
 * identically for every algorithm. */
void SYNTH_RENDER_IRAM freeverb_render(ReverbFx& v, const float* il,
                                       const float* ir, float* wl, float* wr,
                                       size_t frames, float fb, float damp) {
    for (size_t i = 0; i < frames; ++i) {
        const float in = (il[i] + ir[i]) * kRevInGain;
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
        wl[i] = sl;
        wr[i] = sr;
    }
}

/* The implementation behind an algorithm index, or nullptr for freeverb,
 * which lives in this file and has no RevAlgorithm to hand back. Also
 * nullptr for an algorithm whose init() failed, which is how a board short
 * of PSRAM ends up refusing a selection instead of rendering silence. */
osynth::fx::RevAlgorithm* rev_impl(int algo) {
    switch (algo) {
        case kAlgoWet:
            return s_rev_wet_ok ? osynth::fx::wetreverb_instance() : nullptr;
#if SYNTH_ENABLE_FX_GPL
        case kAlgoMVerb:
            return s_rev_mverb_ok ? osynth::fx::mverb_instance() : nullptr;
        case kAlgoDusk:
            return s_rev_dusk_ok ? osynth::fx::duskverb_instance() : nullptr;
#endif
        default:
            return nullptr;
    }
}

/* Rebuilds s_rev_lines for `algo` and returns how many entries it holds.
 *
 * The shared pre-delay goes in whatever the algorithm is. It is not part of
 * any of them, but it is still a delay line holding audio, and leaving it out
 * would mean a unit switched off and back on replays up to 120 ms of whatever
 * was playing before — the exact thing the scrub exists to prevent, hidden
 * behind a control most patches leave at zero. */
size_t rev_collect_lines(int algo) {
    size_t n = 0;
    s_rev_lines[n++] = &s_rev.pre_l;
    s_rev_lines[n++] = &s_rev.pre_r;
    if (algo == kAlgoFreeverb) {
        for (int i = 0; i < 8; ++i) {
            s_rev_lines[n++] = &s_rev.cl[i].line;
            s_rev_lines[n++] = &s_rev.cr[i].line;
        }
        for (int i = 0; i < 4; ++i) {
            s_rev_lines[n++] = &s_rev.al[i];
            s_rev_lines[n++] = &s_rev.ar[i];
        }
        return n;
    }
    osynth::fx::RevAlgorithm* a = rev_impl(algo);
    if (a == nullptr) return n;
    return n + a->lines(s_rev_lines + n, kRevLineMax - n);
}

/* Wet scratch. One block, stereo — the algorithms write here and the shared
 * post stages and the dry/wet mix read it back, so no algorithm has to know
 * what the dry signal was. */
float s_rev_wl[SYNTH_BLOCK_SIZE];
float s_rev_wr[SYNTH_BLOCK_SIZE];

void SYNTH_RENDER_IRAM reverb_process(float* __restrict__ bl,
                                      float* __restrict__ br, size_t frames) {
    ReverbFx& v = s_rev;
    if (!v.ok) return;
    /* The scratch below is one block wide, and audio_io never asks for
     * more; bailing beats overrunning it if that ever changes. */
    if (frames > (size_t)SYNTH_BLOCK_SIZE) return;

    /* An out-of-range or unavailable selection falls back to freeverb rather
     * than to silence: on a build without CONFIG_OSYNTH_FX_GPL the store
     * clamps 2 and 3 down to 1 already, so reaching here means the algorithm
     * exists but could not allocate, and a reverb the player can hear beats
     * a reverb that is correct about being absent. */
    int sel = (int)pv(REV_ALGO);
    if (sel < 0 || sel >= kRevAlgoCount) sel = kAlgoFreeverb;
    if (sel != kAlgoFreeverb && rev_impl(sel) == nullptr) sel = kAlgoFreeverb;

    float m =
        unit_gate(v.u, gated(pv(REV_ON), pvm(REV_MIX)), s_rev_lines, v.nlines);
    if (m < 0.0f) {
        for (int i = 0; i < 8; ++i) v.cl[i].store = v.cr[i].store = 0.0f;
        v.tone_l = v.tone_r = 0.0f;
        /* Switched off, so an algorithm change costs nothing to commit: there
         * is no tail to fade and no dry gain to step. Doing it here rather
         * than deferring also keeps `u`'s bypass scrub honest — that scrub
         * walks s_rev_lines, and leaving a change pending would have it
         * finish clearing a line set the array no longer describes.
         *
         * The new set still has to be cleared before anything is heard of it,
         * and the phase machine is how: parking in kSwScrub with the fade
         * already down means re-enabling scrubs first and rises in second,
         * which is the same path a change made while playing takes. */
        if (v.algo != (uint8_t)sel) {
            v.algo = (uint8_t)sel;
            v.nlines = (uint8_t)rev_collect_lines(v.algo);
            osynth::fx::RevAlgorithm* a = rev_impl(v.algo);
            if (a != nullptr) a->reset();
            v.sw.sl = 0;
            v.sw.sp = 0;
            v.sw_f = 0.0f;
            v.sw_phase = ReverbFx::kSwScrub;
        }
        v.want = v.algo;
        return;
    }

    /* Changing topology mid-tail. The new algorithm's lines hold whatever the
     * last selection left in them, and there is no arrangement of a few
     * hundred KB of memset that fits in a 1.33 ms block, so the unit fades
     * out, scrubs a chunk per block — about 40 ms at the largest algorithm —
     * and fades back in. A brief gap on an algorithm change is what the
     * plugins these came from do too; a burst of somebody else's reverb tail
     * is not, and nor is a step on the dry path either side of the gap (see
     * the sw_f note on ReverbFx).
     *
     * The requested selection is latched every block, so changing your mind
     * mid-switch lands on the last thing you picked rather than queuing. */
    v.want = (uint8_t)sel;
    if (v.algo == 0xFF) {
        /* First block ever: no old algorithm to fade out of, and the lines
         * are calloc'd silent. Commit straight into the scrub with the fade
         * already down, so the unit rises in over kSwStep like every other
         * change instead of arriving at full mix. */
        v.algo = v.want;
        v.nlines = (uint8_t)rev_collect_lines(v.algo);
        v.sw.sl = 0;
        v.sw.sp = 0;
        v.sw_f = 0.0f;
        v.sw_phase = ReverbFx::kSwScrub;
    } else if (v.sw_phase == ReverbFx::kSwIdle && v.want != v.algo) {
        v.sw_phase = ReverbFx::kSwOut;
    }

    switch (v.sw_phase) {
        case ReverbFx::kSwOut:
            v.sw_f -= kSwStep;
            if (v.sw_f <= 0.0f) {
                v.sw_f = 0.0f;
                /* Commit. Nothing is audible at sw_f = 0, so this is the one
                 * point where the topology may change without a step. */
                v.algo = v.want;
                v.nlines = (uint8_t)rev_collect_lines(v.algo);
                osynth::fx::RevAlgorithm* a = rev_impl(v.algo);
                if (a != nullptr) {
                    a->reset();
                } else {
                    for (int i = 0; i < 8; ++i) {
                        v.cl[i].store = v.cr[i].store = 0.0f;
                    }
                }
                v.tone_l = v.tone_r = 0.0f;
                v.sw.sl = 0;
                v.sw.sp = 0;
                v.sw_phase = ReverbFx::kSwScrub;
                /* Not `break`. Falling through would run the algorithm just
                 * committed over lines the scrub has not reached yet, and
                 * then mix the result at sw_f = 0 — the same silence this
                 * returns, for the cost of a block of the most expensive
                 * stage on the bus, spent priming state that is about to be
                 * zeroed underneath it. */
                return;
            }
            break;
        case ReverbFx::kSwScrub:
            if (scrub_step(v.sw, s_rev_lines, v.nlines)) {
                /* Changed selection again while the lines were being cleared:
                 * go back out rather than in. sw_f is already 0, so kSwOut
                 * commits on its very next block and nothing is heard of the
                 * algorithm nobody ended up choosing. */
                v.sw_phase = (v.want != v.algo) ? ReverbFx::kSwOut
                                                : ReverbFx::kSwIn;
            }
            return; /* dry through — sw_f is 0, so this is continuous */
        case ReverbFx::kSwIn:
            v.sw_f += kSwStep;
            if (v.sw_f >= 1.0f) {
                v.sw_f = 1.0f;
                v.sw_phase = ReverbFx::kSwIdle;
            }
            break;
        default:
            break;
    }
    m *= v.sw_f;

    /* What actually renders below is the committed algorithm, not the
     * requested one: during kSwOut they differ, and the old one has a tail
     * that has to keep decaying while it fades.
     *
     * v.algo only ever takes a value `sel` was validated into, so the null
     * check cannot fire — it is kept because the dispatch below dereferences
     * `impl` for every algorithm but freeverb, and that invariant lives three
     * assignments away rather than on this line. */
    int algo = v.algo;
    osynth::fx::RevAlgorithm* impl = rev_impl(algo);
    if (algo != kAlgoFreeverb && impl == nullptr) algo = kAlgoFreeverb;

    /* ---- shared front: pre-delay ---- */
    const float pre_ms = osynth::dsp::smooth_lin(v.s_pre, pvm(REV_PRE));
    const float pre_d = pre_ms * 0.001f * (float)kSr;
    const bool do_pre = pre_d >= 1.0f;
    for (size_t i = 0; i < frames; ++i) {
        const float dl = bl[i], dr = br[i];
        if (do_pre) {
            s_rev_wl[i] = line_read_frac(v.pre_l, pre_d);
            s_rev_wr[i] = line_read_frac(v.pre_r, pre_d);
        } else {
            s_rev_wl[i] = dl;
            s_rev_wr[i] = dr;
        }
        /* Kept primed even while bypassed, so dialling pre-delay up from zero
         * fades in real signal instead of a hole the length of the delay. */
        line_push(v.pre_l, dl);
        line_push(v.pre_r, dr);
    }

    /* ---- the algorithm ---- */
    float makeup = 1.0f;
    bool comp = false;
    if (algo == kAlgoFreeverb) {
        /* Both feed the comb loop per sample: a raw jump steps the running
         * tail (a size change is audible as a click on a long decay).
         * Smoothed S21. */
        const float fb =
            0.70f + 0.28f * osynth::dsp::smooth_lin(v.s_fb, pvm(REV_SIZE));
        const float damp =
            0.95f * osynth::dsp::smooth_lin(v.s_damp, pvm(REV_DAMP));

        /* kRevWet is folded into the wet gain here rather than multiplied per
         * sample below, which is where the make-up joins it. */
        makeup = kRevWet;
        comp = pv(REV_COMP) >= 0.5f;
        if (comp) {
            const float g = sqrtf(fmaxf(1.0f - fb * fb, 1e-4f)) /
                            (kRevRefGain * kRevCombSum);
            makeup = kRevWet * fminf(fmaxf(g, kRevCompMin), kRevCompMax);
        }
        freeverb_render(v, s_rev_wl, s_rev_wr, s_rev_wl, s_rev_wr, frames, fb,
                        damp);
    } else {
        /* The other three are level-matched by construction and take the
         * knobs raw: each smooths internally what it needs to, and their
         * `size` moves delay lengths rather than a feedback coefficient, so
         * the S21 argument for smoothing here does not apply to them.
         *
         * fx.rev.comp stays freeverb-only. It undoes one specific staging
         * decision in *that* algorithm — see the derivation above — and has
         * nothing to undo in the others. */
        const osynth::fx::RevParams rp = {pvm(REV_SIZE), pvm(REV_DAMP),
                                          pvm(REV_DIFF), pvm(REV_EARLY)};
        impl->render(s_rev_wl, s_rev_wr, s_rev_wl, s_rev_wr, frames, rp);
    }

    /* ---- shared back: tone, then width ---- */
    const float tone_hz = osynth::dsp::smooth_exp(v.s_tone, pvm(REV_TONE));
    if (tone_hz < kRevToneOpen * 0.999f) {
        /* One-pole, wet only. A tone control on the dry path would be an EQ,
         * and the bus already has one of those. */
        const float c = 1.0f - expf(-kTwoPi * tone_hz / kSr);
        for (size_t i = 0; i < frames; ++i) {
            v.tone_l += c * (s_rev_wl[i] - v.tone_l);
            v.tone_r += c * (s_rev_wr[i] - v.tone_r);
            s_rev_wl[i] = v.tone_l;
            s_rev_wr[i] = v.tone_r;
        }
    }

    const float width = osynth::dsp::smooth_lin(v.s_width, pvm(REV_WIDTH));
    if (fabsf(width - 1.0f) > 1e-4f) {
        /* Skipped rather than run at unity: a mid/side round trip at width 1
         * is only *almost* the identity in float, and "almost" is the
         * difference between a pre-S36 patch rendering identically and
         * rendering nearly identically. */
        for (size_t i = 0; i < frames; ++i) {
            const float mid = (s_rev_wl[i] + s_rev_wr[i]) * 0.5f;
            const float side = (s_rev_wl[i] - s_rev_wr[i]) * 0.5f * width;
            s_rev_wl[i] = mid + side;
            s_rev_wr[i] = mid - side;
        }
    }

    const MixGains mg = mix_gains(m, comp, makeup);
    for (size_t i = 0; i < frames; ++i) {
        bl[i] = mg.dry * bl[i] + mg.wet * s_rev_wl[i];
        br[i] = mg.dry * br[i] + mg.wet * s_rev_wr[i];
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
    const float m = unit_gate(c.u, gated(pv(CRUSH_ON), pvm(CRUSH_MIX)), nullptr, 0);
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

/* ---- master limiter (S43) ----
 *
 * Last on the bus, after the stereo stage, so it is the only unit that sees
 * the mix exactly as audio_io will. That position is the point: `width` and
 * `pan` are the last things that can push one channel past the other, and a
 * ceiling applied before them is a ceiling on a signal that no longer exists.
 *
 * Why it is not the compressor two stages up. That one is a *glue* unit — a
 * ratio, a soft knee, and a gain computed once per block from the block's
 * peak and ramped across it, which is the right shape for something whose job
 * is to move by a few dB over tens of milliseconds. A ceiling cannot be
 * approximate: the sample that matters is the one that would have clipped,
 * and a gain that arrives up to a block late is a gain that missed it. So
 * this one computes its reduction per sample with *zero* attack, which makes
 * |out| <= ceil exact rather than a target, and spends its entire tuning
 * budget on the release.
 *
 * Why it is not the sink's soft_clip() either. That is a saturator: past its
 * knee it bends the waveform, which is the right failure mode for a last-
 * resort output stage and the wrong one for a ceiling anything is relying on.
 * It stays where it is, behind this — and note that its knee sits at -1.94
 * dBFS, so a `ceil` set above that is still handing it something to shape.
 * The default is just under the knee for exactly that reason; above it the
 * two stages are working on the same peaks and only the softer one is
 * audible.
 *
 * Zero attack has one cost and it is worth naming: a gain that steps within a
 * sample of a transient puts a small amount of intermodulation on the
 * transient itself, which on a bass-heavy mix reads as a click at the very
 * top of the reduction range. There is no lookahead buffer to trade for it —
 * one would cost latency on every path through the instrument, including the
 * keyboard — and at the depths this is meant for (a few dB, catching a
 * vocoder phrase or a drum-plus-reverb pile-up) it is inaudible. Reach for
 * the compressor if you want the mix squeezed rather than capped.
 *
 * `gain` sits in front of the ceiling, which is what makes it a loudness
 * control instead of a volume one: turn it up and the quiet parts rise while
 * the peaks stay where they are. Two knobs rather than one because those are
 * genuinely two decisions — how loud, and how hard.
 */

struct LimiterFx {
    float gain = 1.0f; /* current reduction, 1 = none */
    osynth::dsp::Smooth s_ceil, s_gain;
    UnitState u;
};

LimiterFx s_lim;

NOINLINE_ATTR void limiter_process(float* __restrict__ bl,
                                   float* __restrict__ br, size_t frames) {
    LimiterFx& l = s_lim;
    const float m = unit_gate(l.u, pv(LIM_ON), nullptr, 0);
    if (m < 0.0f) {
        l.gain = 1.0f;
        return;
    }

    /* Smoothed in dB, then converted: a ceiling dragged across its range in
     * the linear domain would move in jumps at the top and crawl at the
     * bottom. Two powf per block, outside the sample loop. */
    const float ceil_db = osynth::dsp::smooth_lin(l.s_ceil, pvm(LIM_CEIL));
    const float pre_db = osynth::dsp::smooth_lin(l.s_gain, pvm(LIM_GAIN));
    const float ceil_lin = powf(10.0f, ceil_db * (1.0f / 20.0f));
    const float pre = powf(10.0f, pre_db * (1.0f / 20.0f));
    const float kr =
        1.0f - expf(-1.0f / fmaxf(pvm(LIM_RELEASE) * 0.001f * kSr, 1.0f));

    for (size_t i = 0; i < frames; ++i) {
        const float xl = bl[i] * pre;
        const float xr = br[i] * pre;
        /* Linked, not per channel: reducing one side of a stereo image and
         * not the other is a pan move, and a limiter that pans on transients
         * is a limiter nobody can mix through. */
        const float al = fabsf(xl), ar = fabsf(xr);
        const float peak = (al > ar) ? al : ar;

        const float need = (peak > ceil_lin) ? (ceil_lin / peak) : 1.0f;
        /* Zero attack down, one-pole up. The branch is the whole limiter:
         * a reduction is taken on the sample that needs it, and given back
         * over `release`. */
        if (need < l.gain) {
            l.gain = need;
        } else {
            l.gain += kr * (need - l.gain);
        }

        /* `m` covers the *whole* unit, input gain included, so switching it
         * on ramps in over the same ~90 ms every other unit here does rather
         * than stepping the mix up by whatever `gain` is set to. The ceiling
         * is only exact once that has settled — during the crossfade the unit
         * is deliberately doing a fraction of its job, and soft_clip() at the
         * sink is still behind it. */
        const float g = 1.0f + m * (pre * l.gain - 1.0f);
        bl[i] *= g;
        br[i] *= g;
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
        /* ESP_OK with the bus left down, not ESP_FAIL. main.cpp
         * ESP_ERROR_CHECKs this call, so failing would turn a full parameter
         * store into a bootloop — on an instrument whose voices, drums,
         * sequencer and looper are all fine and only its effects are not.
         *
         * Safe to do here specifically, and not in the engines next door:
         * fx.h exposes exactly two functions, and fx_process() returns on
         * `s_up` before anything reads the s_p[] pointers that a partial
         * registration would have left null. Leaving it false is a bus that
         * is bypassed, which is the same answer this file already gives an
         * effect whose delay line could not be allocated. */
        ESP_LOGE(TAG,
                 "registered %u/%u params — FX bus disabled for this boot "
                 "(the parameter store is full; see the registry headroom "
                 "line above)",
                 (unsigned)added, (unsigned)P_COUNT);
        ps.removeRange(osynth::PID_FX_BASE, osynth::PID_SEQARP_BASE);
        return ESP_OK;
    }
    for (size_t i = 0; i < P_COUNT; ++i) {
        s_p[i] = ps.valuePtr(kParams[i].id);
    }

    /* Twiddles and the analysis window, both once (S42). Neither can fail and
     * neither allocates — the buffers behind them are static — so this needs
     * none of the `ok` flags the delay lines below carry. */
    osynth::fx::fft_init();
    mnr_build();

    s_cho.ok = line_alloc(s_cho.l, kChoLen) && line_alloc(s_cho.r, kChoLen);
    if (!s_cho.ok) ESP_LOGW(TAG, "chorus disabled: line alloc failed");

    const uint32_t dlen = (uint32_t)(kDelayMaxS * kSr) + 8;
    s_dly.ok = line_alloc(s_dly.l, dlen) && line_alloc(s_dly.r, dlen);
    if (!s_dly.ok) ESP_LOGW(TAG, "delay disabled: line alloc failed");

    s_grn.ok = line_alloc(s_grn.line, kGrnLen);
    if (!s_grn.ok) ESP_LOGW(TAG, "granular disabled: line alloc failed");

    /* Freeverb's own lines. s_rev_lines is no longer filled here: which lines
     * the bypass scrub walks depends on the selected algorithm, so
     * rev_collect_lines() rebuilds it on the first block and on every
     * algorithm change. */
    s_rev.ok = true;
    for (int i = 0; i < 8; ++i) {
        s_rev.ok = s_rev.ok && line_alloc(s_rev.cl[i].line, rv(kCombTune[i]));
        s_rev.ok =
            s_rev.ok && line_alloc(s_rev.cr[i].line, rv(kCombTune[i] + kSpread));
    }
    for (int i = 0; i < 4; ++i) {
        s_rev.ok = s_rev.ok && line_alloc(s_rev.al[i], rv(kApTune[i]));
        s_rev.ok = s_rev.ok && line_alloc(s_rev.ar[i], rv(kApTune[i] + kSpread));
    }
    /* The shared pre-delay sits in front of every algorithm, so it belongs to
     * the unit rather than to any of them, and its failure takes the whole
     * unit down the same way a comb's would. */
    const uint32_t prelen = (uint32_t)(kRevPreMaxMs * 0.001f * (float)kSr) + 8;
    s_rev.ok = s_rev.ok && line_alloc(s_rev.pre_l, prelen);
    s_rev.ok = s_rev.ok && line_alloc(s_rev.pre_r, prelen);
    if (!s_rev.ok) ESP_LOGW(TAG, "reverb disabled: line alloc failed");

    /* The other algorithms allocate their own lines, and each is allowed to
     * fail on its own: a board that cannot fit WetReverb's comb bank still
     * gets freeverb, and says so, rather than losing the reverb entirely. */
    constexpr uint32_t kSrHz = (uint32_t)SYNTH_SAMPLE_RATE;
    s_rev_wet_ok = osynth::fx::wetreverb_instance()->init(kSrHz);
    if (!s_rev_wet_ok) ESP_LOGW(TAG, "reverb: wetreverb unavailable (alloc)");
#if SYNTH_ENABLE_FX_GPL
    s_rev_mverb_ok = osynth::fx::mverb_instance()->init(kSrHz);
    if (!s_rev_mverb_ok) ESP_LOGW(TAG, "reverb: mverb unavailable (alloc)");
    s_rev_dusk_ok = osynth::fx::duskverb_instance()->init(kSrHz);
    if (!s_rev_dusk_ok) ESP_LOGW(TAG, "reverb: duskverb unavailable (alloc)");
#endif

    s_voc.rec_ok = line_alloc(s_voc.rec, kVocSampLen);
    if (!s_voc.rec_ok) {
        ESP_LOGW(TAG, "vocoder: capture buffer alloc failed — fx.voc.freeze "
                      "stays live instead of replaying a sample");
    }

    s_flg.ok = line_alloc(s_flg.l, kFlgLen) && line_alloc(s_flg.r, kFlgLen);
    if (!s_flg.ok) ESP_LOGW(TAG, "flanger disabled: line alloc failed");

    s_up = true;
    ESP_LOGI(TAG,
             "fx bus up: anr -> nr -> vocoder -> drive -> chorus -> flanger -> "
             "phaser -> delay -> granular -> reverb -> crush -> filter -> eq "
             "-> comp -> stereo -> limiter, "
             "%u params, "
             "delay max %.2f s, %d grains / %.2f s window, buffers %u KB "
             "PSRAM + %u KB internal",
             (unsigned)P_COUNT, (double)kDelayMaxS, kGrainMax,
             (double)kGrnLen / (double)kSr, (unsigned)(osynth::dsp::g_line_bytes_spiram / 1024),
             (unsigned)(osynth::dsp::g_line_bytes_internal / 1024));
    return ESP_OK;
}

void SYNTH_RENDER_IRAM fx_process(float* l, float* r, size_t frames) {
    if (!s_up || l == nullptr || r == nullptr) return;
    /* Three units on this bus stage through block-sized statics — the two
     * noise-reduction sources (s_nr_src_l/r) and the reverb's pre-delay tap
     * (s_rev_wl/wr). The vocoder and nr_process each tested `frames` for
     * themselves; anr_process did too, and reverb_process did not, which made
     * the contract something you had to reconstruct by reading five units.
     * Testing it once here is what the contract actually is — the render
     * callback passes exactly SYNTH_BLOCK_SIZE — and it covers whatever gets
     * added to the chain next.
     *
     * Clamped rather than refused, so an over-long block still comes out
     * effected for as much of itself as the scratch can hold; logged once,
     * because if this ever fires it is a caller bug and the tail of every
     * block is silently going past the FX bus untouched. */
    if (frames > SYNTH_BLOCK_SIZE) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            /* Deferred to a control task (synth_warn.h), which also costs the
             * message its two numbers: SYNTH_BLOCK_SIZE is a build constant
             * and the caller's frame count is whatever it is, so naming the
             * constant is enough to find the caller that disagreed with it. */
            osynth::dsp::render_warn(
                TAG,
                "fx_process called with more than SYNTH_BLOCK_SIZE frames — "
                "clamping; the rest of every block is bypassing the bus");
        }
        frames = SYNTH_BLOCK_SIZE;
    }
    /* One bypass-scrub allowance for the whole chain, spent in call order by
     * whichever units are dry this block — see kScrubChunk. */
    s_scrub_budget = kScrubChunk;

    /* First: every unit below reads its parameters through pvm(), which
     * consumes what this writes. */
    lfo_update(frames);

    /* Mic NR first: it is the one that exists to hand the rest of the bus a
     * clean input. Note that running more than one of the three with
     * `src` = input does not cascade — each computes its correction against
     * the *same* untouched input block and adds it, so two of them remove the
     * input's contribution twice. One at a time is the intended use. */
    mnr_process(l, r, frames);
    anr_process(l, r, frames);
    nr_process(l, r, frames);
    vocoder_process(l, r, frames);
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
    /* After the stereo stage and last of all: `width` and `pan` are the final
     * things that can put one channel past the other, so this is the only
     * point at which a ceiling is a ceiling on what the sink receives. */
    limiter_process(l, r, frames);
}
