/*
 * osynth — additive engine (Session 8; filter added Session 33).
 *
 * 16 sine partials per voice, summed straight from the shared LUT: the
 * spectrum is the patch. Per block (global): drawbar levels ×
 * spectral tilt (dB/oct, referenced so it only attenuates) × even/odd
 * balance give the base spectrum; inharmonicity stretches the partial
 * ratios (n·sqrt(1 + B·n²), renormalized so the fundamental stays put).
 * Per voice-block: brightness b = base + env.bright·env2 + lfo2.bright·lfo2
 * − vel.bright·(1 − vel), clamped to [0,1], maps to an exponential rolloff
 * r^(n−1) with r = exp(−1.8·(1 − b)²) — the brightness envelope behaves
 * like a filter sweep without a filter. Partials whose effective gain falls
 * below −60 dB or whose frequency crosses Nyquist are culled from the
 * render list: the DSP load breathes with the spectrum instead of always
 * paying for 16 partials (watch the heartbeat's dsp/pk while sweeping
 * brightness).
 *
 * That cull is not a budget, though, and S33b is where the difference
 * mattered. It is a *threshold*, so it says nothing about the worst case —
 * and at the default patch there is no worst case to speak of: brightness
 * sits at 0.85 through every attack, the rolloff is 0.96 per partial, and
 * all sixteen clear −60 dB comfortably. Eight voices of that is 128 partial
 * oscillators, which measured 83 % of the block on an S3 against a 52 %
 * total for subtractive — continuous underruns and, eventually, a task
 * watchdog on the audio core. kPartialBudget below is the missing bound: a
 * fixed number of partial oscillators shared over the sounding voices, so
 * polyphony trades against harmonics instead of against the deadline.
 *
 * Render shape: a partial-outer loop accumulates into a mono scratch buffer
 * (each partial's phase stays in a register), then one pass applies the
 * filter, the per-sample amp env and the voice gains. Culled partials keep
 * their phase; they rejoin below −60 dB, so the step is inaudible.
 *
 * The S33 filter sits in that final pass, off by default. It does something
 * the brightness rolloff cannot: the rolloff attenuates partial by partial
 * and can only ever get darker, where a resonant peak, a notch or a vowel
 * imposes a shape the drawbars never contained. env2 drives both.
 *
 * Gain staging: tilt / even-odd / brightness only attenuate, so the
 * worst-case sum is the drawbar sum. The default 1/n drawbars sum to 1.0 —
 * full polyphony cannot clip under the voice manager's 1/SYNTH_VOICES
 * headroom; cranking all 16 drawbars can.
 *
 * Mod matrix (S9): the brightness terms (add.bright, env.bright,
 * lfo2.bright) and the vibrato depth go through synth_mod_apply() per
 * voice. The global spectrum shaping (drawbars, tilt, even/odd, inharm) is
 * built once per block for all voices, so it is not a matrix destination.
 */
#include "engine_additive.h"

#include <atomic>
#include <cmath>
#include <cstring>

#include "esp_log.h"

#include "synth_config.h"
#include "synth_dsp.h"
#include "synth_mod.h"
#include "synth_params.h"
#include "synth_smooth.h"
#include "synth_voice.h"

static const char* TAG = "eng_add";

namespace dsp = osynth::dsp;
using osynth::ParamCurve;
using osynth::ParamDesc;
using osynth::ParamStore;
using osynth::ParamType;

namespace {

constexpr int kPartials = ADD_PARTIALS;

struct AddVoice {
    /* Phase as a 32-bit fraction of a cycle rather than a float in [0, 1).
     *
     * The wrap is the point. A float phase needs `if (ph >= 1) ph -= 1` after
     * every advance — a compare and a branch inside the hottest loop in the
     * firmware, executed 16 partials x 64 samples x 8 voices = 8192 times a
     * block. An unsigned accumulator wraps by overflowing, which is free, and
     * the LUT index falls out of a shift instead of a float multiply and a
     * float-to-int conversion.
     *
     * Exactly as accurate: 32 bits of phase is far finer than the 2048-entry
     * table's 11 bits of index plus the 21 bits of interpolation fraction this
     * derives from the remainder, so nothing about the output changes. */
    uint32_t phase[kPartials];
    dsp::Filt filt;         /* S33; off unless flt.on */
    dsp::Adsr env1;         /* amplitude, per sample */
    dsp::Adsr env2;         /* brightness + filter, block rate */
    dsp::Lfo lfo1, lfo2;
    uint8_t note = 60; /* mod-matrix note source */
    float vel = 0.0f;
};

/* ---- parameter set (order matches PIdx) ---- */

enum PIdx {
    P1_LEVEL, /* .. P1_LEVEL + 15: the drawbar block is contiguous */
    TILT = P1_LEVEL + kPartials,
    EVENODD, INHARM, BRIGHT, ENV_BRIGHT, VEL_BRIGHT,
    ENV1_A, ENV1_D, ENV1_S, ENV1_R,
    ENV2_A, ENV2_D, ENV2_S, ENV2_R,
    LFO1_RATE, LFO1_WAVE, LFO1_PITCH,
    LFO2_RATE, LFO2_WAVE, LFO2_BRIGHT,
    FLT_ON, FLT_TYPE, FLT_MODE, FLT_CUTOFF, FLT_RESO, FLT_ENV, FLT_KBD,
    FLT_DRIVE, FLT_SPREAD, FLT_VOWEL,
    P_COUNT
};

const char* const kLfoWaves[] = {"sine", "triangle", "saw", "square", "s&h"};
/* Append-only, both: presets store the index, not the name. */
const char* const kFltModes[] = {"lp",   "bp", "hp", "notch",
                                 "peak", "ap", "bp norm"};
const char* const kFltTypes[] = {"svf 12", "svf 24", "ladder", "dual", "vowel"};

#define ADD_DRAWBAR(n, def)                                                \
    {(uint16_t)(ADD_PID_P1_LEVEL + (n) - 1), "add.p" #n ".level",          \
     ParamType::Float, ParamCurve::Linear, 0.0f, 1.0f, def, nullptr, 0}

/* Default patch: 1/n "saw" drawbars normalized to sum 1.0, brightness 0.35
 * + 0.5·env2 — a bright pluck mellowing into an organ-ish sustain. */
const ParamDesc kParams[P_COUNT] = {
    ADD_DRAWBAR(1, 0.2958f),
    ADD_DRAWBAR(2, 0.1479f),
    ADD_DRAWBAR(3, 0.0986f),
    ADD_DRAWBAR(4, 0.0739f),
    ADD_DRAWBAR(5, 0.0592f),
    ADD_DRAWBAR(6, 0.0493f),
    ADD_DRAWBAR(7, 0.0423f),
    ADD_DRAWBAR(8, 0.0370f),
    ADD_DRAWBAR(9, 0.0329f),
    ADD_DRAWBAR(10, 0.0296f),
    ADD_DRAWBAR(11, 0.0269f),
    ADD_DRAWBAR(12, 0.0247f),
    ADD_DRAWBAR(13, 0.0228f),
    ADD_DRAWBAR(14, 0.0211f),
    ADD_DRAWBAR(15, 0.0197f),
    ADD_DRAWBAR(16, 0.0185f),
    {ADD_PID_TILT, "add.tilt", ParamType::Float, ParamCurve::Linear,
     -12.0f, 12.0f, 0.0f, nullptr, 0}, /* dB/oct */
    {ADD_PID_EVENODD, "add.evenodd", ParamType::Float, ParamCurve::Linear,
     -1.0f, 1.0f, 0.0f, nullptr, 0}, /* -1: odd partials only, +1: even only */
    {ADD_PID_INHARM, "add.inharm", ParamType::Float, ParamCurve::Linear,
     0.0f, 0.05f, 0.0f, nullptr, 0}, /* B: piano ~0.0005, bells toward 0.05 */
    {ADD_PID_BRIGHT, "add.bright", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.35f, nullptr, 0},
    {ADD_PID_ENV_BRIGHT, "env.bright", ParamType::Float, ParamCurve::Linear,
     -1.0f, 1.0f, 0.5f, nullptr, 0}, /* env2 -> brightness */
    {ADD_PID_VEL_BRIGHT, "vel.bright", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.3f, nullptr, 0}, /* soft velocity darkens */
    {ADD_PID_ENV1_ATTACK, "env1.attack", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.005f, nullptr, 0},
    {ADD_PID_ENV1_DECAY, "env1.decay", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.25f, nullptr, 0},
    {ADD_PID_ENV1_SUSTAIN, "env1.sustain", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.8f, nullptr, 0},
    {ADD_PID_ENV1_RELEASE, "env1.release", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.2f, nullptr, 0},
    {ADD_PID_ENV2_ATTACK, "env2.attack", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.002f, nullptr, 0},
    {ADD_PID_ENV2_DECAY, "env2.decay", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.4f, nullptr, 0},
    {ADD_PID_ENV2_SUSTAIN, "env2.sustain", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.15f, nullptr, 0},
    {ADD_PID_ENV2_RELEASE, "env2.release", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.3f, nullptr, 0},
    {ADD_PID_LFO1_RATE, "lfo1.rate", ParamType::Float, ParamCurve::Exp,
     0.02f, 20.0f, 5.0f, nullptr, 0},
    {ADD_PID_LFO1_WAVE, "lfo1.wave", ParamType::Enum, ParamCurve::Linear,
     0.0f, 4.0f, 0.0f /* sine */, kLfoWaves, 5},
    {ADD_PID_LFO1_PITCH, "lfo1.pitch", ParamType::Float, ParamCurve::Linear,
     0.0f, 2.0f, 0.0f, nullptr, 0}, /* semitones (a matrix wheel slot raises it) */
    {ADD_PID_LFO2_RATE, "lfo2.rate", ParamType::Float, ParamCurve::Exp,
     0.02f, 20.0f, 0.7f, nullptr, 0},
    {ADD_PID_LFO2_WAVE, "lfo2.wave", ParamType::Enum, ParamCurve::Linear,
     0.0f, 4.0f, 1.0f /* triangle */, kLfoWaves, 5},
    {ADD_PID_LFO2_BRIGHT, "lfo2.bright", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* brightness wobble depth */
    /* Filter (S33), off by default — this engine shaped its spectrum with
     * brightness alone until now, and every preset assumes that. */
    {ADD_PID_FLT_ON, "flt.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {ADD_PID_FLT_TYPE, "flt.type", ParamType::Enum, ParamCurve::Linear,
     0.0f, 4.0f, 0.0f /* svf 12 */, kFltTypes, 5},
    {ADD_PID_FLT_MODE, "flt.mode", ParamType::Enum, ParamCurve::Linear,
     0.0f, 6.0f, 0.0f /* lp */, kFltModes, 7},
    {ADD_PID_FLT_CUTOFF, "flt.cutoff", ParamType::Float, ParamCurve::Exp,
     20.0f, 18000.0f, 8000.0f, nullptr, 0},
    {ADD_PID_FLT_RESO, "flt.reso", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.1f, nullptr, 0},
    {ADD_PID_FLT_ENV, "flt.env", ParamType::Float, ParamCurve::Linear,
     -4.0f, 4.0f, 0.0f, nullptr, 0}, /* octaves; env2 doubles as filter env */
    {ADD_PID_FLT_KBD, "flt.kbd", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.5f, nullptr, 0},
    {ADD_PID_FLT_DRIVE, "flt.drive", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {ADD_PID_FLT_SPREAD, "flt.spread", ParamType::Float, ParamCurve::Linear,
     0.0f, 6.0f, 2.0f, nullptr, 0}, /* dual: passband width in octaves */
    {ADD_PID_FLT_VOWEL, "flt.vowel", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* vowel: morph a-e-i-o-u */
};

const std::atomic<float>* s_p[P_COUNT];

inline float pv(PIdx i) { return s_p[i]->load(std::memory_order_relaxed); }

/* ---- block-shared cache, rebuilt in begin_block() ---- */

struct BlockCache {
    float ratio[kPartials]; /* partial freq ratios after inharmonicity */
    float base[kPartials];  /* drawbar x tilt x even/odd */
    float bright, env_bright, vel_bright;
    dsp::AdsrCoef amp; /* per-sample rates */
    dsp::AdsrCoef mod; /* per-block rates */
    float lfo1_inc, lfo2_inc;
    dsp::LfoWave lw1, lw2;
    float l1_pitch, l2_bright;
    dsp::SvfMode fmode;
    dsp::FltType ftype;
    float cutoff, reso, fenv_oct, fkbd, fdrive, fspread, fvowel;
    int max_partials; /* this block's share of kPartialBudget, per voice */
};

BlockCache s_bc;

/* Block-rate parameter smoothers (S21) — see synth_smooth.h. The 16 partial
 * gains are smoothed on the *output* side rather than per input control:
 * drawbars, tilt and even/odd all collapse into base[n], so one smoother per
 * partial covers all three (and their interactions) at a fixed cost. The
 * inharmonicity coefficient is smoothed on the input side because it lands
 * in a square root. Brightness is this engine's filter-sweep equivalent, so
 * it gets the same treatment as a cutoff. */
struct Smoothers {
    dsp::Smooth base[kPartials];
    dsp::Smooth inharm, bright, env_bright, vel_bright;
    dsp::Smooth l1_pitch, l2_bright;
    dsp::Smooth cutoff, reso, fenv, fkbd, fdrive, fspread, fvowel;
};

Smoothers s_sm;

constexpr float kSr = (float)SYNTH_SAMPLE_RATE;
constexpr float kInvSr = 1.0f / (float)SYNTH_SAMPLE_RATE;
constexpr float kMaxStep = 0.49f;    /* keep phase increments below Nyquist */
constexpr float kCullLevel = 1e-3f;  /* -60 dB: partials below drop out */
constexpr float kBrightSlope = 1.8f; /* rolloff strength at brightness 0 */

/* Cycles-to-fraction for the phase accumulator above, and the split of that
 * accumulator into the sine table's index and the interpolation fraction. */
constexpr float kPhaseScale = 4294967296.0f; /* 2^32 */
constexpr int kIdxShift = 32 - 11;           /* detail::kSineN is 2048 = 2^11 */
constexpr uint32_t kFracMask = (1u << kIdxShift) - 1u;
constexpr float kFracScale = 1.0f / (float)(1u << kIdxShift);

/* ---- polyphony budget (S33b) ----
 *
 * The one number that decides whether this engine fits. Everything else here
 * is per-partial arithmetic; the cost is that arithmetic times how many
 * partial oscillators are running, and nothing before this bounded that
 * product. Sixteen partials times eight voices is 128 of them, and 128 does
 * not fit — measured on an ESP32-S3 at 240 MHz:
 *
 *     subtractive, 8 voices:  dsp  52 %  [voi 30]   underruns flat
 *     additive,    8 voices:  dsp 105 %  [voi 83]   underruns climbing,
 *                                                   task watchdog on IDLE1
 *
 * So one partial oscillator costs 83/128 = 0.65 % of a block, on the
 * pre-S33b loop. The engine is not sharing the chip with nothing: the FX bus
 * measured 18.3 % and the looper 0.4 % in that same run, and the peak-to-EMA
 * ratio across those readings was about 1.15 (51.6/57.6, 46.8/53.8) — so a
 * peak ceiling of 90 % allows an EMA of 78 %, leaving the voices about 57 %,
 * which is 88 partial oscillators.
 *
 * 64, not 88, because the figure above is measured against a loop this same
 * session rewrote, and sizing a budget from a cost that is about to fall is
 * how you end up back at 105 %. 64 is safe under the *old* per-partial cost
 * and therefore safe under any improvement to it. Raising it is the intended
 * next step once there is a fresh `voi` reading to raise it against.
 *
 * Distributed across whatever is sounding, so the budget only ever binds under
 * polyphony: one to four voices still get all sixteen partials, five get
 * twelve, eight get eight. Partials are dropped from the top, which is what
 * the brightness rolloff already does continuously — a dense chord gets darker
 * rather than glitching, and that is the trade this engine has to make
 * somewhere. It used to make it by missing its deadline.
 *
 * Retune it against the heartbeat, not against taste: play the densest chord
 * the patch allows and read `voi`. Room to raise this is `(64 - voi) / 0.65`
 * more partials, less whatever margin the FX settings want. */
constexpr int kPartialBudget = 64;
/* Never below this, however many voices are sounding: a partial or two is not
 * an additive engine, and past this point the honest answer is fewer voices,
 * which the voice manager's stealing already provides. */
constexpr int kMinPartialsPerVoice = 4;

/* log2(n) for n = 1..16 — the tilt exponent in octaves; 4.0 = log2(16) */
const float kLog2N[kPartials] = {
    0.0f,    1.0f,    1.5850f, 2.0f,    2.3219f, 2.5850f, 2.8074f, 3.0f,
    3.1699f, 3.3219f, 3.4594f, 3.5850f, 3.7004f, 3.8074f, 3.9069f, 4.0f};

/* dB/oct -> exp2f octave factor: log2(10) / 20 */
constexpr float kDbOct2Exp = 0.1660964f;

inline float clamp01(float x) { return fminf(fmaxf(x, 0.0f), 1.0f); }

float s_acc[SYNTH_BLOCK_SIZE]; /* mono partial sum; audio task only */

/* ---- vtable entries ---- */

esp_err_t add_init(void) {
    dsp::tables_init();
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
    s_sm = Smoothers{}; /* unprimed: the first block snaps to the patch */
    ESP_LOGI(TAG,
             "additive engine up: %u params, caps 0x%02x (%d partials, cull "
             "< -60 dB / > Nyquist, budget %d partial osc = %d each at %d "
             "voices)",
             (unsigned)P_COUNT, (unsigned)g_engine_additive.caps, kPartials,
             kPartialBudget, kPartialBudget / SYNTH_VOICES, SYNTH_VOICES);
    return ESP_OK;
}

void add_deinit(void) {
    ParamStore::instance().removeRange(osynth::PID_ENGINE_BASE,
                                       osynth::PID_FX_BASE);
}

void SYNTH_RENDER_IRAM add_begin_block(size_t frames) {
    BlockCache& b = s_bc;
    const float tilt = pv(TILT) * kDbOct2Exp;
    /* reference partial 16 for positive tilt, 1 for negative: either way the
     * gains stay <= 1, so tilt never breaks the drawbar-sum headroom */
    const float tref = (tilt > 0.0f) ? 4.0f : 0.0f;
    const float eo = pv(EVENODD);
    const float g_even = (eo < 0.0f) ? 1.0f + eo : 1.0f;
    const float g_odd = (eo > 0.0f) ? 1.0f - eo : 1.0f;
    const float B = dsp::smooth_lin(s_sm.inharm, pv(INHARM));
    const float fnorm = 1.0f / sqrtf(1.0f + B); /* fundamental stays put */
    for (int n = 0; n < kPartials; ++n) {
        const float h = (float)(n + 1);
        b.ratio[n] = h * sqrtf(1.0f + B * h * h) * fnorm;
        b.base[n] = dsp::smooth_lin(s_sm.base[n],
                                    pv((PIdx)(P1_LEVEL + n)) *
                                        (((n + 1) & 1) ? g_odd : g_even) *
                                        exp2f(tilt * (kLog2N[n] - tref)));
    }
    b.bright = dsp::smooth_lin(s_sm.bright, pv(BRIGHT));
    b.env_bright = dsp::smooth_lin(s_sm.env_bright, pv(ENV_BRIGHT));
    b.vel_bright = dsp::smooth_lin(s_sm.vel_bright, pv(VEL_BRIGHT));
    b.amp = dsp::adsr_coef_block(pv(ENV1_A), pv(ENV1_D), pv(ENV1_S),
                                 pv(ENV1_R), kSr, (uint32_t)frames);
    b.mod = dsp::adsr_coef(pv(ENV2_A), pv(ENV2_D), pv(ENV2_S), pv(ENV2_R),
                           kSr / (float)frames);
    b.lfo1_inc = pv(LFO1_RATE) * (float)frames * kInvSr;
    b.lw1 = (dsp::LfoWave)(int)pv(LFO1_WAVE);
    b.l1_pitch = dsp::smooth_lin(s_sm.l1_pitch, pv(LFO1_PITCH));
    b.lfo2_inc = pv(LFO2_RATE) * (float)frames * kInvSr;
    b.lw2 = (dsp::LfoWave)(int)pv(LFO2_WAVE);
    b.l2_bright = dsp::smooth_lin(s_sm.l2_bright, pv(LFO2_BRIGHT));
    b.fmode = (dsp::SvfMode)(int)pv(FLT_MODE);
    b.ftype = (pv(FLT_ON) < 0.5f) ? dsp::FltType::Bypass
                                  : (dsp::FltType)(int)pv(FLT_TYPE);
    b.cutoff = dsp::smooth_exp(s_sm.cutoff, pv(FLT_CUTOFF));
    b.reso = dsp::smooth_lin(s_sm.reso, pv(FLT_RESO));
    b.fenv_oct = dsp::smooth_lin(s_sm.fenv, pv(FLT_ENV));
    b.fkbd = dsp::smooth_lin(s_sm.fkbd, pv(FLT_KBD));
    b.fdrive = dsp::smooth_lin(s_sm.fdrive, pv(FLT_DRIVE));
    b.fspread = dsp::smooth_lin(s_sm.fspread, pv(FLT_SPREAD));
    b.fvowel = dsp::smooth_lin(s_sm.fvowel, pv(FLT_VOWEL));

    /* Share the partial budget out over what is sounding (see kPartialBudget).
     * The count is last block's — voice_manager_render() publishes it after
     * the render — which is exactly right: it is the number of voices about to
     * be rendered in all but the block a note starts or ends on, and being one
     * block late on a limit that only bites at full polyphony costs nothing.
     * Guarded at 1 so a first note gets the whole budget rather than a
     * division by zero. */
    size_t active = voice_manager_active_voices();
    if (active < 1) active = 1;
    int per = kPartialBudget / (int)active;
    if (per < kMinPartialsPerVoice) per = kMinPartialsPerVoice;
    if (per > kPartials) per = kPartials;
    b.max_partials = per;
}

void add_voice_reset(void* vs) {
    AddVoice& v = *(AddVoice*)vs;
    v = AddVoice{};
    dsp::noise_seed(v.lfo1.rng, 0xADD10001u ^ (uint32_t)(uintptr_t)vs);
    dsp::noise_seed(v.lfo2.rng, 0xADD2F00Du ^ (uint32_t)(uintptr_t)vs);
}

void add_note_on(void* vs, uint8_t note, float vel01, bool was_sounding) {
    AddVoice& v = *(AddVoice*)vs;
    const float vel = fmaxf(vel01, 1.0f / 127.0f);
    if (!was_sounding) {
        /* deterministic attack transient: all partials from phase zero */
        for (int n = 0; n < kPartials; ++n) v.phase[n] = 0u;
        dsp::lfo_retrig(v.lfo1);
        dsp::lfo_retrig(v.lfo2);
    } else if (v.env1.level > 0.0f && v.vel > 0.0f) {
        /* retrigger/steal: rescale so vel * env is continuous (no step) */
        v.env1.level = fminf(1.0f, v.env1.level * v.vel / vel);
    }
    v.note = note;
    v.vel = vel;
    dsp::adsr_gate_on(v.env1);
    dsp::adsr_gate_on(v.env2);
}

void add_note_off(void* vs) {
    AddVoice& v = *(AddVoice*)vs;
    dsp::adsr_gate_off(v.env1);
    dsp::adsr_gate_off(v.env2);
}

void SYNTH_RENDER_IRAM add_render(void* vs, const synth_voice_frame_t* f,
                                  float* __restrict__ out_l,
                                  float* __restrict__ out_r, size_t frames) {
    AddVoice& v = *(AddVoice*)vs;
    const BlockCache& b = s_bc;
    if (frames > (size_t)SYNTH_BLOCK_SIZE) frames = SYNTH_BLOCK_SIZE; /* scratch bound */

    /* block-rate modulators: vibrato, brightness wobble, brightness env */
    const float l1 = dsp::lfo_next(v.lfo1, b.lw1, b.lfo1_inc);
    const float l2 = dsp::lfo_next(v.lfo2, b.lw2, b.lfo2_inc);
    const float menv = dsp::adsr_next(v.env2, b.mod);

    /* mod matrix: per-voice sources, then the modulatable destinations */
    const synth_mod_voice_src_t ms = {menv, l1, l2, v.vel, (float)v.note};
    const float l1_pitch = synth_mod_apply(ADD_PID_LFO1_PITCH, b.l1_pitch, &ms);
    const float m_bright = synth_mod_apply(ADD_PID_BRIGHT, b.bright, &ms);
    const float m_ebright =
        synth_mod_apply(ADD_PID_ENV_BRIGHT, b.env_bright, &ms);
    const float m_l2bright =
        synth_mod_apply(ADD_PID_LFO2_BRIGHT, b.l2_bright, &ms);
    const float fcut = synth_mod_apply(ADD_PID_FLT_CUTOFF, b.cutoff, &ms);
    const float freso = synth_mod_apply(ADD_PID_FLT_RESO, b.reso, &ms);
    const float fenv_oct = synth_mod_apply(ADD_PID_FLT_ENV, b.fenv_oct, &ms);
    const float fdrive = synth_mod_apply(ADD_PID_FLT_DRIVE, b.fdrive, &ms);
    const float fvowel = synth_mod_apply(ADD_PID_FLT_VOWEL, b.fvowel, &ms);

    const float pitch_mul =
        (l1_pitch != 0.0f) ? exp2f(l1 * l1_pitch * (1.0f / 12.0f)) : 1.0f;
    const float step0 = f->freq_hz * pitch_mul * kInvSr;

    const float bright = clamp01(m_bright + m_ebright * menv +
                                 m_l2bright * l2 -
                                 b.vel_bright * (1.0f - v.vel));
    const float dark = 1.0f - bright;
    const float r = expf(-kBrightSlope * dark * dark); /* per-partial rolloff */

    /* Cull: keep partials below Nyquist and above -60 dB, then stop at this
     * block's share of the partial budget (kPartialBudget). The list is built
     * in ascending n and the gains fall with n for any spectrum a drawbar set
     * describes, so cutting it short drops the quietest and highest — the same
     * end the brightness rolloff works from. */
    float gain[kPartials];
    uint32_t step[kPartials];
    uint8_t idx[kPartials];
    int count = 0;
    float rp = 1.0f; /* r^(n-1) */
    for (int n = 0; n < kPartials && count < b.max_partials; ++n) {
        const float g = b.base[n] * rp;
        rp *= r;
        const float st = step0 * b.ratio[n];
        if (g < kCullLevel || st > kMaxStep) continue;
        idx[count] = (uint8_t)n;
        step[count] = (uint32_t)(st * kPhaseScale);
        gain[count] = g;
        ++count;
    }

    /* amp envelope: one state-machine pass, then a branch-free linear ramp */
    const dsp::AdsrRamp ar = dsp::adsr_block(v.env1, b.amp, (uint32_t)frames);
    if (SYNTH_UNLIKELY(dsp::adsr_ramp_silent(ar))) {
        /* provably silent block: advance the live partials' phases in bulk
         * (culled partials keep their phase, same as the live behavior) */
        for (int k = 0; k < count; ++k) {
            v.phase[idx[k]] += step[k] * (uint32_t)frames;
        }
        return;
    }

    /* The hot loop. 32-bit phase, so the wrap is the accumulator overflowing
     * and the table index is a shift — see AddVoice::phase. */
    memset(s_acc, 0, frames * sizeof(float));
    const float* const tbl = dsp::detail::g_sine;
    for (int k = 0; k < count; ++k) {
        uint32_t ph = v.phase[idx[k]];
        const uint32_t st = step[k];
        const float g = gain[k];
        for (size_t i = 0; i < frames; ++i) {
            const float* const e = tbl + (ph >> kIdxShift);
            const float fr = (float)(ph & kFracMask) * kFracScale;
            s_acc[i] += g * (e[0] + fr * (e[1] - e[0]));
            ph += st;
        }
        v.phase[idx[k]] = ph;
    }

    /* Filter (S33) — env2 drives it as well as the brightness rolloff, so
     * one envelope opens the spectrum from both ends. */
    const float foct = fenv_oct * menv +
                       b.fkbd * ((float)v.note - 60.0f) * (1.0f / 12.0f);
    const dsp::FiltCoef fc =
        dsp::filt_coef(b.ftype, b.fmode, fcut * exp2f(foct), freso, fdrive,
                       b.fspread, fvowel, kSr);

    const float gl = f->gain_l * v.vel;
    const float gr = f->gain_r * v.vel;
    float a = ar.base;
    for (size_t i = 0; i < frames; ++i) {
        a += ar.step;
        const float y = dsp::filt_next(v.filt, fc, s_acc[i]) * a;
        out_l[i] += y * gl;
        out_r[i] += y * gr;
    }
}

bool add_busy(const void* vs) {
    return dsp::adsr_active(((const AddVoice*)vs)->env1);
}

float add_level(const void* vs) {
    return ((const AddVoice*)vs)->env1.level;
}

} // namespace

extern "C" const synth_engine_t g_engine_additive = {
    "additive",
    SYNTH_CAP_FILTER | SYNTH_CAP_ENV2 | SYNTH_CAP_LFO1 | SYNTH_CAP_LFO2 |
        SYNTH_CAP_MODMATRIX,
    sizeof(AddVoice),
    add_init,
    add_deinit,
    add_begin_block,
    add_voice_reset,
    add_note_on,
    add_note_off,
    add_render,
    add_busy,
    add_level,
    nullptr, /* render_block (S28): fixed engines render per voice */
};
