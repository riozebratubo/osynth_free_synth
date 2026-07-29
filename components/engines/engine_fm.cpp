/*
 * osynth — FM engine (Session 6).
 *
 * 2-op x 2 phase modulation on the shared sine LUT: two parallel
 * modulator -> carrier pairs per voice, mixed by their pair levels. Each
 * pair has a carrier amp ADSR (per sample) and a modulation-index ADSR
 * (block rate, like the subtractive filter env), plus single-sample
 * feedback on the modulator (fb near 1 goes DX-style noisy — a feature).
 * Pair B is detunable in cents for shimmer. Velocity scales amplitude and,
 * via fm.vel.index, the modulation index (harder = brighter). lfo -> pitch
 * is the vibrato hook (mod wheel); there is no filter — caps declare LFO1
 * only, the first engine to exercise module gating for real.
 *
 * Phase convention: everything is in cycles [0, 1). The modulation index
 * parameter is the classic I in radians (y = sin(wc t + I sin(wm t))), so
 * the carrier phase offset is I / 2pi cycles. Phase + offset is re-wrapped
 * with a positive-bias truncation (offsets are bounded, see kWrapBias).
 *
 * Gain staging: output = (cA * envA * levelA + cB * envB * levelB) * vel.
 * Default levels sum to 1.0, so full polyphony cannot clip under the voice
 * manager's 1/SYNTH_VOICES headroom; cranking both levels can.
 *
 * Mod matrix (S9): indexes, feedbacks, pair levels and the vibrato depth
 * are routed through synth_mod_apply() per voice in render(). env2/lfo2
 * sources read 0 on this engine (gated out — caps declare LFO1 only).
 */
#include "engine_fm.h"

#include <atomic>
#include <cmath>

#include "esp_log.h"

#include "synth_config.h"
#include "synth_dsp.h"
#include "synth_mod.h"
#include "synth_params.h"
#include "synth_smooth.h"

static const char* TAG = "eng_fm";

namespace dsp = osynth::dsp;
using osynth::ParamCurve;
using osynth::ParamDesc;
using osynth::ParamStore;
using osynth::ParamType;

namespace {

struct FmPair {
    float cph = 0.0f; /* carrier phase, cycles */
    float mph = 0.0f; /* modulator phase */
    float fbz = 0.0f; /* previous modulator sample (feedback) */
    dsp::Adsr aenv;   /* carrier amplitude, per sample */
    dsp::Adsr menv;   /* modulation index, block rate */
};

struct FmVoice {
    FmPair a, b;
    dsp::Lfo lfo;
    uint8_t note = 60; /* mod-matrix note source */
    float vel = 0.0f;
};

/* ---- parameter set (order matches PIdx) ---- */

enum PIdx {
    A_CRATIO, A_MRATIO, A_INDEX, A_FB, A_LEVEL,
    A_ENV_A, A_ENV_D, A_ENV_S, A_ENV_R,
    A_MENV_A, A_MENV_D, A_MENV_S, A_MENV_R,
    B_CRATIO, B_MRATIO, B_INDEX, B_FB, B_LEVEL,
    B_ENV_A, B_ENV_D, B_ENV_S, B_ENV_R,
    B_MENV_A, B_MENV_D, B_MENV_S, B_MENV_R,
    B_DETUNE,
    VEL_INDEX, LFO_RATE, LFO_WAVE, LFO_PITCH,
    P_COUNT
};

const char* const kLfoWaves[] = {"sine", "triangle", "saw", "square", "s&h"};

/* Default patch: DX-style e-piano — pair A is the 1:1 body (index softens
 * after the strike), pair B a fast-decaying overtone ping two octaves up. */
const ParamDesc kParams[P_COUNT] = {
    {FM_PID_A_CRATIO, "fm.a.cratio", ParamType::Float, ParamCurve::Exp,
     0.25f, 16.0f, 1.0f, nullptr, 0},
    {FM_PID_A_MRATIO, "fm.a.mratio", ParamType::Float, ParamCurve::Exp,
     0.25f, 16.0f, 1.0f, nullptr, 0},
    {FM_PID_A_INDEX, "fm.a.index", ParamType::Float, ParamCurve::Linear,
     0.0f, 13.0f, 2.0f, nullptr, 0}, /* radians */
    {FM_PID_A_FB, "fm.a.fb", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FM_PID_A_LEVEL, "fm.a.level", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.8f, nullptr, 0},
    {FM_PID_A_ENV_A, "fm.a.env.a", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.002f, nullptr, 0},
    {FM_PID_A_ENV_D, "fm.a.env.d", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 1.2f, nullptr, 0},
    {FM_PID_A_ENV_S, "fm.a.env.s", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* 0: piano-like fade while held */
    {FM_PID_A_ENV_R, "fm.a.env.r", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.35f, nullptr, 0},
    {FM_PID_A_MENV_A, "fm.a.menv.a", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.001f, nullptr, 0},
    {FM_PID_A_MENV_D, "fm.a.menv.d", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.6f, nullptr, 0},
    {FM_PID_A_MENV_S, "fm.a.menv.s", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.25f, nullptr, 0},
    {FM_PID_A_MENV_R, "fm.a.menv.r", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.35f, nullptr, 0},
    {FM_PID_B_CRATIO, "fm.b.cratio", ParamType::Float, ParamCurve::Exp,
     0.25f, 16.0f, 4.0f, nullptr, 0},
    {FM_PID_B_MRATIO, "fm.b.mratio", ParamType::Float, ParamCurve::Exp,
     0.25f, 16.0f, 4.0f, nullptr, 0},
    {FM_PID_B_INDEX, "fm.b.index", ParamType::Float, ParamCurve::Linear,
     0.0f, 13.0f, 1.0f, nullptr, 0},
    {FM_PID_B_FB, "fm.b.fb", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FM_PID_B_LEVEL, "fm.b.level", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.2f, nullptr, 0},
    {FM_PID_B_ENV_A, "fm.b.env.a", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.001f, nullptr, 0},
    {FM_PID_B_ENV_D, "fm.b.env.d", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.25f, nullptr, 0},
    {FM_PID_B_ENV_S, "fm.b.env.s", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FM_PID_B_ENV_R, "fm.b.env.r", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.2f, nullptr, 0},
    {FM_PID_B_MENV_A, "fm.b.menv.a", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.001f, nullptr, 0},
    {FM_PID_B_MENV_D, "fm.b.menv.d", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.15f, nullptr, 0},
    {FM_PID_B_MENV_S, "fm.b.menv.s", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FM_PID_B_MENV_R, "fm.b.menv.r", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.15f, nullptr, 0},
    {FM_PID_B_DETUNE, "fm.b.detune", ParamType::Float, ParamCurve::Linear,
     -50.0f, 50.0f, 3.0f, nullptr, 0}, /* cents */
    {FM_PID_VEL_INDEX, "fm.vel.index", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.6f, nullptr, 0},
    {FM_PID_LFO_RATE, "fm.lfo.rate", ParamType::Float, ParamCurve::Exp,
     0.02f, 20.0f, 5.5f, nullptr, 0},
    {FM_PID_LFO_WAVE, "fm.lfo.wave", ParamType::Enum, ParamCurve::Linear,
     0.0f, 4.0f, 0.0f /* sine */, kLfoWaves, 5},
    {FM_PID_LFO_PITCH, "fm.lfo.pitch", ParamType::Float, ParamCurve::Linear,
     0.0f, 2.0f, 0.0f, nullptr, 0}, /* semitones (a matrix wheel slot raises it) */
};

const std::atomic<float>* s_p[P_COUNT];

inline float pv(PIdx i) { return s_p[i]->load(std::memory_order_relaxed); }

/* ---- block-shared cache, rebuilt in begin_block() ---- */

struct PairCache {
    float cratio, mratio;
    float index; /* peak modulation index, radians (raw param — the matrix
                  * modulates it per voice; cycles conversion in render) */
    float fb;    /* feedback amount, raw 0..1 param (kFbMax in render) */
    float level;
    dsp::AdsrCoef amp; /* per-sample rates */
    dsp::AdsrCoef mod; /* per-block rates */
};

struct BlockCache {
    PairCache a, b;
    float bdet;      /* pair-B frequency multiplier from fm.b.detune */
    float vel_index;
    float lfo_inc;
    dsp::LfoWave lw;
    float lfo_pitch;
};

BlockCache s_bc;

/* Block-rate parameter smoothers (S21) — see synth_smooth.h. The operator
 * levels are straight amplitude and the indices/feedback are the timbre
 * axis: both step the signal if written raw, so both are smoothed. The
 * envelope times only set rates and the LFO wave is a switch — neither is. */
struct PairSmooth {
    dsp::Smooth cratio, mratio, index, fb, level;
};

struct Smoothers {
    PairSmooth a, b;
    dsp::Smooth bdet, vel_index, lfo_pitch;
};

Smoothers s_sm;

constexpr float kSr = (float)SYNTH_SAMPLE_RATE;
constexpr float kInvSr = 1.0f / (float)SYNTH_SAMPLE_RATE;
constexpr float kMaxStep = 0.49f;  /* keep phase increments below Nyquist */
constexpr float kInv2Pi = 0.15915494f;
constexpr float kFbMax = 0.35f;    /* fb = 1: ~2.2 rad, well into DX noise */
/* Largest carrier offset: 13 rad * kInv2Pi ~ 2.07 cycles; modulator offset
 * stays under kFbMax. Adding the bias keeps phase+offset positive so the
 * wrap can be a plain truncation. */
constexpr float kWrapBias = 3.0f;

inline float wrap01(float x) { return x - (float)(int)x; /* x >= 0 */ }

void pair_cache(PairCache& p, PairSmooth& sm, PIdx base, size_t frames) {
    p.cratio = dsp::smooth_exp(sm.cratio, pv((PIdx)(base + 0)));
    p.mratio = dsp::smooth_exp(sm.mratio, pv((PIdx)(base + 1)));
    p.index = dsp::smooth_lin(sm.index, pv((PIdx)(base + 2)));
    p.fb = dsp::smooth_lin(sm.fb, pv((PIdx)(base + 3)));
    p.level = dsp::smooth_lin(sm.level, pv((PIdx)(base + 4)));
    p.amp = dsp::adsr_coef_block(pv((PIdx)(base + 5)), pv((PIdx)(base + 6)),
                                 pv((PIdx)(base + 7)), pv((PIdx)(base + 8)),
                                 kSr, (uint32_t)frames);
    p.mod = dsp::adsr_coef(pv((PIdx)(base + 9)), pv((PIdx)(base + 10)),
                           pv((PIdx)(base + 11)), pv((PIdx)(base + 12)),
                           kSr / (float)frames);
}

/* Advance a silent pair's phases by a whole block (stays phase-coherent). */
inline void pair_advance(FmPair& p, float cstep, float mstep, size_t frames) {
    float c = p.cph + cstep * (float)frames;
    float m = p.mph + mstep * (float)frames;
    p.cph = c - floorf(c);
    p.mph = m - floorf(m);
}

/* ---- vtable entries ---- */

esp_err_t fm_init(void) {
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
    ESP_LOGI(TAG, "fm engine up: %u params, caps 0x%02x (2-op x 2)",
             (unsigned)P_COUNT, (unsigned)g_engine_fm.caps);
    return ESP_OK;
}

void fm_deinit(void) {
    ParamStore::instance().removeRange(osynth::PID_ENGINE_BASE,
                                       osynth::PID_FX_BASE);
}

void SYNTH_RENDER_IRAM fm_begin_block(size_t frames) {
    BlockCache& b = s_bc;
    pair_cache(b.a, s_sm.a, A_CRATIO, frames);
    pair_cache(b.b, s_sm.b, B_CRATIO, frames);
    b.bdet = dsp::smooth_exp(s_sm.bdet,
                             exp2f(pv(B_DETUNE) * (1.0f / 1200.0f)));
    b.vel_index = dsp::smooth_lin(s_sm.vel_index, pv(VEL_INDEX));
    b.lfo_inc = pv(LFO_RATE) * (float)frames * kInvSr;
    b.lw = (dsp::LfoWave)(int)pv(LFO_WAVE);
    b.lfo_pitch = dsp::smooth_lin(s_sm.lfo_pitch, pv(LFO_PITCH));
}

void fm_voice_reset(void* vs) {
    FmVoice& v = *(FmVoice*)vs;
    v = FmVoice{};
    dsp::noise_seed(v.lfo.rng, 0xF10000A5u ^ (uint32_t)(uintptr_t)vs);
}

void fm_note_on(void* vs, uint8_t note, float vel01, bool was_sounding) {
    FmVoice& v = *(FmVoice*)vs;
    const float vel = fmaxf(vel01, 1.0f / 127.0f);
    if (!was_sounding) {
        /* deterministic attack transient: all phases from zero */
        v.a.cph = v.a.mph = v.a.fbz = 0.0f;
        v.b.cph = v.b.mph = v.b.fbz = 0.0f;
        dsp::lfo_retrig(v.lfo);
    } else if (v.vel > 0.0f) {
        /* retrigger/steal: rescale so vel * env is continuous (no step) */
        if (v.a.aenv.level > 0.0f) {
            v.a.aenv.level = fminf(1.0f, v.a.aenv.level * v.vel / vel);
        }
        if (v.b.aenv.level > 0.0f) {
            v.b.aenv.level = fminf(1.0f, v.b.aenv.level * v.vel / vel);
        }
    }
    v.note = note;
    v.vel = vel;
    dsp::adsr_gate_on(v.a.aenv);
    dsp::adsr_gate_on(v.a.menv);
    dsp::adsr_gate_on(v.b.aenv);
    dsp::adsr_gate_on(v.b.menv);
}

void fm_note_off(void* vs) {
    FmVoice& v = *(FmVoice*)vs;
    dsp::adsr_gate_off(v.a.aenv);
    dsp::adsr_gate_off(v.a.menv);
    dsp::adsr_gate_off(v.b.aenv);
    dsp::adsr_gate_off(v.b.menv);
}

void SYNTH_RENDER_IRAM fm_render(void* vs, const synth_voice_frame_t* f,
                                 float* __restrict__ out_l,
                                 float* __restrict__ out_r, size_t frames) {
    FmVoice& v = *(FmVoice*)vs;
    const BlockCache& b = s_bc;

    /* block-rate modulators: vibrato + the two index envelopes */
    const float l = dsp::lfo_next(v.lfo, b.lw, b.lfo_inc);

    /* mod matrix: per-voice sources (no env2/lfo2 on this engine) */
    const synth_mod_voice_src_t ms = {0.0f, l, 0.0f, v.vel, (float)v.note};
    const float lfo_pitch = synth_mod_apply(FM_PID_LFO_PITCH, b.lfo_pitch, &ms);
    const float ia_rad = synth_mod_apply(FM_PID_A_INDEX, b.a.index, &ms);
    const float ib_rad = synth_mod_apply(FM_PID_B_INDEX, b.b.index, &ms);
    const float fb_a = synth_mod_apply(FM_PID_A_FB, b.a.fb, &ms) * kFbMax;
    const float fb_b = synth_mod_apply(FM_PID_B_FB, b.b.fb, &ms) * kFbMax;
    const float lvl_a = synth_mod_apply(FM_PID_A_LEVEL, b.a.level, &ms);
    const float lvl_b = synth_mod_apply(FM_PID_B_LEVEL, b.b.level, &ms);

    const float pitch_mul =
        (lfo_pitch != 0.0f) ? exp2f(l * lfo_pitch * (1.0f / 12.0f)) : 1.0f;
    const float step0 = f->freq_hz * pitch_mul * kInvSr;
    const float step_ca = fminf(step0 * b.a.cratio, kMaxStep);
    const float step_ma = fminf(step0 * b.a.mratio, kMaxStep);
    const float step_cb = fminf(step0 * b.bdet * b.b.cratio, kMaxStep);
    const float step_mb = fminf(step0 * b.bdet * b.b.mratio, kMaxStep);

    const float vscale = 1.0f - b.vel_index * (1.0f - v.vel);
    const float ia =
        ia_rad * kInv2Pi * dsp::adsr_next(v.a.menv, b.a.mod) * vscale;
    const float ib =
        ib_rad * kInv2Pi * dsp::adsr_next(v.b.menv, b.b.mod) * vscale;

    const float gl = f->gain_l * v.vel;
    const float gr = f->gain_r * v.vel;

    /* amp envelopes: one state-machine pass each, branch-free ramps after */
    const dsp::AdsrRamp ra = dsp::adsr_block(v.a.aenv, b.a.amp, (uint32_t)frames);
    const dsp::AdsrRamp rb = dsp::adsr_block(v.b.aenv, b.b.amp, (uint32_t)frames);
    const bool on_a = !dsp::adsr_ramp_silent(ra);
    const bool on_b = !dsp::adsr_ramp_silent(rb);
    if (SYNTH_UNLIKELY(!on_a && !on_b)) {
        pair_advance(v.a, step_ca, step_ma, frames);
        pair_advance(v.b, step_cb, step_mb, frames);
        return;
    }
    /* a silent pair (say a short B release already over while A rings) is
     * skipped inside the loop — its per-block phase advance keeps it
     * coherent, and the block-constant branch predicts perfectly */
    float ea = ra.base, eb = rb.base;
    for (size_t i = 0; i < frames; ++i) {
        ea += ra.step;
        eb += rb.step;
        float y = 0.0f;
        if (on_a) {
            const float ma =
                dsp::sine01(wrap01(v.a.mph + fb_a * v.a.fbz + 1.0f));
            v.a.fbz = ma;
            const float ca = dsp::sine01(wrap01(v.a.cph + ia * ma + kWrapBias));
            v.a.mph += step_ma; if (v.a.mph >= 1.0f) v.a.mph -= 1.0f;
            v.a.cph += step_ca; if (v.a.cph >= 1.0f) v.a.cph -= 1.0f;
            y += ca * ea * lvl_a;
        }
        if (on_b) {
            const float mb =
                dsp::sine01(wrap01(v.b.mph + fb_b * v.b.fbz + 1.0f));
            v.b.fbz = mb;
            const float cb = dsp::sine01(wrap01(v.b.cph + ib * mb + kWrapBias));
            v.b.mph += step_mb; if (v.b.mph >= 1.0f) v.b.mph -= 1.0f;
            v.b.cph += step_cb; if (v.b.cph >= 1.0f) v.b.cph -= 1.0f;
            y += cb * eb * lvl_b;
        }
        out_l[i] += y * gl;
        out_r[i] += y * gr;
    }
    if (!on_a) pair_advance(v.a, step_ca, step_ma, frames);
    if (!on_b) pair_advance(v.b, step_cb, step_mb, frames);
}

bool fm_busy(const void* vs) {
    const FmVoice& v = *(const FmVoice*)vs;
    return dsp::adsr_active(v.a.aenv) || dsp::adsr_active(v.b.aenv);
}

float fm_level(const void* vs) {
    const FmVoice& v = *(const FmVoice*)vs;
    /* approximate loudness for steal ranking (levels read off the cache) */
    return v.a.aenv.level * s_bc.a.level + v.b.aenv.level * s_bc.b.level;
}

} // namespace

extern "C" const synth_engine_t g_engine_fm = {
    "fm",
    /* no filter/env2/lfo2/mixer: gated out for this engine */
    SYNTH_CAP_LFO1 | SYNTH_CAP_MODMATRIX,
    sizeof(FmVoice),
    fm_init,
    fm_deinit,
    fm_begin_block,
    fm_voice_reset,
    fm_note_on,
    fm_note_off,
    fm_render,
    fm_busy,
    fm_level,
    nullptr, /* render_block (S28): fixed engines render per voice */
};
