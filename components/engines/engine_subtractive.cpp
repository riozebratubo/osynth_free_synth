/*
 * osynth — subtractive engine (Session 5; mod matrix Session 9; filter
 * family Session 33).
 *
 * Per voice: osc1 + osc2 + noise -> mixer -> filter -> amp env, composed from
 * the shared blocks in synth_dsp.h. env2 sweeps the filter and lfo2 wobbles
 * it, both at block rate (1.33 ms steps — inaudible as zipper at these
 * modulation speeds); lfo1 is vibrato. All parameters are read once per
 * block in begin_block() into a cache shared by every voice (the envelope
 * and mix settings are global); only note-dependent values (keyboard
 * tracking, env2 level, vibrato) are computed per voice in render().
 *
 * The filter is one of five types (flt.type) behind one bypass switch
 * (flt.on); both resolve to a dsp::FltType in begin_block(), so the sample
 * loop sees a single filt_next() call whatever is selected — see the
 * dispatch note in synth_dsp.h.
 *
 * Mod matrix (S9): the per-voice consumable params — pulse widths, mix
 * levels, cutoff/reso/drive/vowel, flt.env and the LFO depths — are routed
 * through synth_mod_apply() at the top of render(), so any matrix slot can
 * retarget them per voice at block rate (docs/PARAM_MAP.md lists the dests).
 *
 * Gain staging: output = mixer * filter * env1 * velocity. The voice
 * manager applies the 1/SYNTH_VOICES headroom and unison pan on top, so the
 * default mix (levels summing to <= 1) cannot clip at full polyphony;
 * cranking all three mix levels or the resonance can.
 */
#include "engine_subtractive.h"

#include <atomic>
#include <cmath>

#include "esp_log.h"

#include "synth_config.h"
#include "synth_dsp.h"
#include "synth_mod.h"
#include "synth_params.h"
#include "synth_smooth.h"

static const char* TAG = "eng_sub";

namespace dsp = osynth::dsp;
using osynth::ParamCurve;
using osynth::ParamDesc;
using osynth::ParamStore;
using osynth::ParamType;

namespace {

struct SubVoice {
    dsp::Osc osc1, osc2;
    dsp::Noise noise;
    dsp::Filt filt;
    dsp::Adsr env1; /* amplitude, per sample */
    dsp::Adsr env2; /* filter, block rate */
    dsp::Lfo lfo1, lfo2;
    uint8_t note = 60;
    float vel = 0.0f;
};

/* ---- parameter set (order matches PIdx) ---- */

enum PIdx {
    OSC1_WAVE, OSC1_PW, OSC2_WAVE, OSC2_PW, OSC2_SEMI, OSC2_FINE,
    MIX_OSC1, MIX_OSC2, MIX_NOISE,
    FLT_MODE, FLT_CUTOFF, FLT_RESO, FLT_ENV, FLT_KBD,
    FLT_ON, FLT_TYPE, FLT_DRIVE, FLT_SPREAD, FLT_VOWEL,
    ENV1_A, ENV1_D, ENV1_S, ENV1_R,
    ENV2_A, ENV2_D, ENV2_S, ENV2_R,
    LFO1_RATE, LFO1_WAVE, LFO1_PITCH,
    LFO2_RATE, LFO2_WAVE, LFO2_CUTOFF,
    P_COUNT
};

const char* const kOscWaves[] = {"sine", "triangle", "saw", "pulse"};
/* Both lists are append-only: presets store the index, not the name. */
const char* const kFltModes[] = {"lp",   "bp",   "hp", "notch",
                                 "peak", "ap",   "bp norm"};
const char* const kFltTypes[] = {"svf 12", "svf 24", "ladder", "dual", "vowel"};
const char* const kLfoWaves[] = {"sine", "triangle", "saw", "square", "s&h"};

const ParamDesc kParams[P_COUNT] = {
    {SUB_PID_OSC1_WAVE, "osc1.wave", ParamType::Enum, ParamCurve::Linear,
     0.0f, 3.0f, 2.0f /* saw */, kOscWaves, 4},
    {SUB_PID_OSC1_PW, "osc1.pw", ParamType::Float, ParamCurve::Linear,
     0.05f, 0.95f, 0.5f, nullptr, 0},
    {SUB_PID_OSC2_WAVE, "osc2.wave", ParamType::Enum, ParamCurve::Linear,
     0.0f, 3.0f, 2.0f /* saw */, kOscWaves, 4},
    {SUB_PID_OSC2_PW, "osc2.pw", ParamType::Float, ParamCurve::Linear,
     0.05f, 0.95f, 0.5f, nullptr, 0},
    {SUB_PID_OSC2_SEMI, "osc2.semi", ParamType::Int, ParamCurve::Linear,
     -24.0f, 24.0f, 0.0f, nullptr, 0},
    {SUB_PID_OSC2_FINE, "osc2.fine", ParamType::Float, ParamCurve::Linear,
     -100.0f, 100.0f, 4.0f, nullptr, 0}, /* cents */
    {SUB_PID_MIX_OSC1, "mix.osc1", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.8f, nullptr, 0},
    {SUB_PID_MIX_OSC2, "mix.osc2", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {SUB_PID_MIX_NOISE, "mix.noise", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {SUB_PID_FLT_MODE, "flt.mode", ParamType::Enum, ParamCurve::Linear,
     0.0f, 6.0f, 0.0f /* lp */, kFltModes, 7},
    {SUB_PID_FLT_CUTOFF, "flt.cutoff", ParamType::Float, ParamCurve::Exp,
     20.0f, 18000.0f, 1200.0f, nullptr, 0},
    {SUB_PID_FLT_RESO, "flt.reso", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.15f, nullptr, 0},
    {SUB_PID_FLT_ENV, "flt.env", ParamType::Float, ParamCurve::Linear,
     -4.0f, 4.0f, 2.5f, nullptr, 0}, /* octaves */
    {SUB_PID_FLT_KBD, "flt.kbd", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.5f, nullptr, 0},
    /* Bypass. Defaults on, so every patch saved before S33 keeps its
     * filter; off costs nothing, it just selects FltType::Bypass. */
    {SUB_PID_FLT_ON, "flt.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0},
    /* Defaults to svf 12 with no drive: an untouched patch renders exactly
     * what it rendered before these five parameters existed. */
    {SUB_PID_FLT_TYPE, "flt.type", ParamType::Enum, ParamCurve::Linear,
     0.0f, 4.0f, 0.0f /* svf 12 */, kFltTypes, 5},
    {SUB_PID_FLT_DRIVE, "flt.drive", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {SUB_PID_FLT_SPREAD, "flt.spread", ParamType::Float, ParamCurve::Linear,
     0.0f, 6.0f, 2.0f, nullptr, 0}, /* dual: passband width in octaves */
    {SUB_PID_FLT_VOWEL, "flt.vowel", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* vowel: morph a-e-i-o-u */
    {SUB_PID_ENV1_ATTACK, "env1.attack", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.005f, nullptr, 0},
    {SUB_PID_ENV1_DECAY, "env1.decay", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.25f, nullptr, 0},
    {SUB_PID_ENV1_SUSTAIN, "env1.sustain", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.7f, nullptr, 0},
    {SUB_PID_ENV1_RELEASE, "env1.release", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.25f, nullptr, 0},
    {SUB_PID_ENV2_ATTACK, "env2.attack", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.002f, nullptr, 0},
    {SUB_PID_ENV2_DECAY, "env2.decay", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.3f, nullptr, 0},
    {SUB_PID_ENV2_SUSTAIN, "env2.sustain", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.2f, nullptr, 0},
    {SUB_PID_ENV2_RELEASE, "env2.release", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.3f, nullptr, 0},
    {SUB_PID_LFO1_RATE, "lfo1.rate", ParamType::Float, ParamCurve::Exp,
     0.02f, 20.0f, 5.0f, nullptr, 0},
    {SUB_PID_LFO1_WAVE, "lfo1.wave", ParamType::Enum, ParamCurve::Linear,
     0.0f, 4.0f, 0.0f /* sine */, kLfoWaves, 5},
    {SUB_PID_LFO1_PITCH, "lfo1.pitch", ParamType::Float, ParamCurve::Linear,
     0.0f, 2.0f, 0.0f, nullptr, 0}, /* semitones (a matrix wheel slot raises it) */
    {SUB_PID_LFO2_RATE, "lfo2.rate", ParamType::Float, ParamCurve::Exp,
     0.02f, 20.0f, 1.5f, nullptr, 0},
    {SUB_PID_LFO2_WAVE, "lfo2.wave", ParamType::Enum, ParamCurve::Linear,
     0.0f, 4.0f, 1.0f /* triangle */, kLfoWaves, 5},
    {SUB_PID_LFO2_CUTOFF, "lfo2.cutoff", ParamType::Float, ParamCurve::Linear,
     0.0f, 4.0f, 0.0f, nullptr, 0}, /* octaves */
};

const std::atomic<float>* s_p[P_COUNT];

inline float pv(PIdx i) { return s_p[i]->load(std::memory_order_relaxed); }

/* ---- block-shared cache, rebuilt in begin_block() ---- */

struct BlockCache {
    dsp::OscWave w1, w2;
    float pw1, pw2;
    float mul2; /* osc2 frequency multiplier (semi + fine) */
    float m1, m2, mn;
    dsp::SvfMode fmode;
    dsp::FltType ftype;
    float cutoff, reso, fenv_oct, fkbd, fdrive, fspread, fvowel;
    dsp::AdsrCoef amp; /* per-sample rates */
    dsp::AdsrCoef flt; /* per-block rates */
    float lfo1_inc, lfo2_inc;
    dsp::LfoWave lw1, lw2;
    float l1_pitch, l2_oct;
};

BlockCache s_bc;

/* Block-rate smoothers for the continuous parameters (S21). Enums (waves,
 * filter mode) are switches, not ramps, and the ADSR times only set rates —
 * neither can step the signal, so neither is smoothed. */
struct Smoothers {
    dsp::Smooth pw1, pw2, mul2, m1, m2, mn;
    dsp::Smooth cutoff, reso, fenv, fkbd, fdrive, fspread, fvowel;
    dsp::Smooth l1_pitch, l2_oct;
};

Smoothers s_sm;

constexpr float kSr = (float)SYNTH_SAMPLE_RATE;
constexpr float kInvSr = 1.0f / (float)SYNTH_SAMPLE_RATE;
constexpr float kMaxStep = 0.49f; /* keep phase increments below Nyquist */

/* ---- vtable entries ---- */

esp_err_t sub_init(void) {
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
    ESP_LOGI(TAG, "subtractive engine up: %u params, caps 0x%02x",
             (unsigned)P_COUNT, (unsigned)g_engine_subtractive.caps);
    return ESP_OK;
}

void sub_deinit(void) {
    ParamStore::instance().removeRange(osynth::PID_ENGINE_BASE,
                                       osynth::PID_FX_BASE);
}

void SYNTH_RENDER_IRAM sub_begin_block(size_t frames) {
    BlockCache& b = s_bc;
    b.w1 = (dsp::OscWave)(int)pv(OSC1_WAVE);
    b.pw1 = dsp::smooth_lin(s_sm.pw1, pv(OSC1_PW));
    b.w2 = (dsp::OscWave)(int)pv(OSC2_WAVE);
    b.pw2 = dsp::smooth_lin(s_sm.pw2, pv(OSC2_PW));
    b.mul2 = dsp::smooth_exp(
        s_sm.mul2,
        exp2f((pv(OSC2_SEMI) + pv(OSC2_FINE) * 0.01f) * (1.0f / 12.0f)));
    b.m1 = dsp::smooth_lin(s_sm.m1, pv(MIX_OSC1));
    b.m2 = dsp::smooth_lin(s_sm.m2, pv(MIX_OSC2));
    b.mn = dsp::smooth_lin(s_sm.mn, pv(MIX_NOISE));
    b.fmode = (dsp::SvfMode)(int)pv(FLT_MODE);
    /* The bypass switch resolves to a filter type here, once per block, so
     * the render loop never learns it exists. */
    b.ftype = (pv(FLT_ON) < 0.5f) ? dsp::FltType::Bypass
                                  : (dsp::FltType)(int)pv(FLT_TYPE);
    b.cutoff = dsp::smooth_exp(s_sm.cutoff, pv(FLT_CUTOFF));
    b.reso = dsp::smooth_lin(s_sm.reso, pv(FLT_RESO));
    b.fenv_oct = dsp::smooth_lin(s_sm.fenv, pv(FLT_ENV));
    b.fkbd = dsp::smooth_lin(s_sm.fkbd, pv(FLT_KBD));
    b.fdrive = dsp::smooth_lin(s_sm.fdrive, pv(FLT_DRIVE));
    b.fspread = dsp::smooth_lin(s_sm.fspread, pv(FLT_SPREAD));
    b.fvowel = dsp::smooth_lin(s_sm.fvowel, pv(FLT_VOWEL));
    b.amp = dsp::adsr_coef_block(pv(ENV1_A), pv(ENV1_D), pv(ENV1_S),
                                 pv(ENV1_R), kSr, (uint32_t)frames);
    b.flt = dsp::adsr_coef(pv(ENV2_A), pv(ENV2_D), pv(ENV2_S), pv(ENV2_R),
                           kSr / (float)frames);
    b.lfo1_inc = pv(LFO1_RATE) * (float)frames * kInvSr;
    b.lw1 = (dsp::LfoWave)(int)pv(LFO1_WAVE);
    b.l1_pitch = dsp::smooth_lin(s_sm.l1_pitch, pv(LFO1_PITCH));
    b.lfo2_inc = pv(LFO2_RATE) * (float)frames * kInvSr;
    b.lw2 = (dsp::LfoWave)(int)pv(LFO2_WAVE);
    b.l2_oct = dsp::smooth_lin(s_sm.l2_oct, pv(LFO2_CUTOFF));
}

void sub_voice_reset(void* vs) {
    SubVoice& v = *(SubVoice*)vs;
    v = SubVoice{};
    dsp::noise_seed(v.noise, 0xA5A50000u ^ (uint32_t)(uintptr_t)vs);
}

void sub_note_on(void* vs, uint8_t note, float vel01, bool was_sounding) {
    SubVoice& v = *(SubVoice*)vs;
    const float vel = fmaxf(vel01, 1.0f / 127.0f);
    if (!was_sounding) {
        v.osc1.phase = 0.0f;
        v.osc2.phase = 0.25f; /* offset: equal-tuned oscs neither null nor double */
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

void sub_note_off(void* vs) {
    SubVoice& v = *(SubVoice*)vs;
    dsp::adsr_gate_off(v.env1);
    dsp::adsr_gate_off(v.env2);
}

void SYNTH_RENDER_IRAM sub_render(void* vs, const synth_voice_frame_t* f,
                                  float* __restrict__ out_l,
                                  float* __restrict__ out_r, size_t frames) {
    SubVoice& v = *(SubVoice*)vs;
    const BlockCache& b = s_bc;

    /* block-rate modulators */
    const float l1 = dsp::lfo_next(v.lfo1, b.lw1, b.lfo1_inc);
    const float l2 = dsp::lfo_next(v.lfo2, b.lw2, b.lfo2_inc);
    const float fenv = dsp::adsr_next(v.env2, b.flt);

    /* mod matrix: per-voice sources, then the modulatable destinations */
    const synth_mod_voice_src_t ms = {fenv, l1, l2, v.vel, (float)v.note};
    const float l1_pitch = synth_mod_apply(SUB_PID_LFO1_PITCH, b.l1_pitch, &ms);
    const float l2_oct = synth_mod_apply(SUB_PID_LFO2_CUTOFF, b.l2_oct, &ms);
    const float pw1 = synth_mod_apply(SUB_PID_OSC1_PW, b.pw1, &ms);
    const float pw2 = synth_mod_apply(SUB_PID_OSC2_PW, b.pw2, &ms);
    const float m1 = synth_mod_apply(SUB_PID_MIX_OSC1, b.m1, &ms);
    const float m2 = synth_mod_apply(SUB_PID_MIX_OSC2, b.m2, &ms);
    const float mn = synth_mod_apply(SUB_PID_MIX_NOISE, b.mn, &ms);
    const float cutoff = synth_mod_apply(SUB_PID_FLT_CUTOFF, b.cutoff, &ms);
    const float reso = synth_mod_apply(SUB_PID_FLT_RESO, b.reso, &ms);
    const float fenv_oct = synth_mod_apply(SUB_PID_FLT_ENV, b.fenv_oct, &ms);
    const float fdrive = synth_mod_apply(SUB_PID_FLT_DRIVE, b.fdrive, &ms);
    const float fvowel = synth_mod_apply(SUB_PID_FLT_VOWEL, b.fvowel, &ms);

    const float pitch_mul =
        (l1_pitch != 0.0f) ? exp2f(l1 * l1_pitch * (1.0f / 12.0f)) : 1.0f;
    const float step1 = fminf(f->freq_hz * pitch_mul * kInvSr, kMaxStep);
    const float step2 = fminf(step1 * b.mul2, kMaxStep);

    const float oct = fenv_oct * fenv + l2_oct * l2 +
                      b.fkbd * ((float)v.note - 60.0f) * (1.0f / 12.0f);
    const dsp::FiltCoef fc =
        dsp::filt_coef(b.ftype, b.fmode, cutoff * exp2f(oct), reso, fdrive,
                       b.fspread, fvowel, kSr);

    const float gl = f->gain_l * v.vel;
    const float gr = f->gain_r * v.vel;

    /* amp envelope: one state-machine pass, then a branch-free linear ramp */
    const dsp::AdsrRamp ar = dsp::adsr_block(v.env1, b.amp, (uint32_t)frames);
    if (SYNTH_UNLIKELY(dsp::adsr_ramp_silent(ar))) {
        /* provably silent block: keep the oscillators phase-coherent, skip
         * the rest (the SVF holds state — its output was gated to 0 anyway) */
        dsp::osc_advance(v.osc1, step1, (uint32_t)frames);
        dsp::osc_advance(v.osc2, step2, (uint32_t)frames);
        return;
    }

    float a = ar.base;
    for (size_t i = 0; i < frames; ++i) {
        a += ar.step;
        const float x = m1 * dsp::osc_next(v.osc1, b.w1, step1, pw1) +
                        m2 * dsp::osc_next(v.osc2, b.w2, step2, pw2) +
                        mn * dsp::noise_next(v.noise);
        const float y = dsp::filt_next(v.filt, fc, x) * a;
        out_l[i] += y * gl;
        out_r[i] += y * gr;
    }
}

bool sub_busy(const void* vs) {
    return dsp::adsr_active(((const SubVoice*)vs)->env1);
}

float sub_level(const void* vs) {
    return ((const SubVoice*)vs)->env1.level;
}

} // namespace

extern "C" const synth_engine_t g_engine_subtractive = {
    "subtractive",
    SYNTH_CAP_FILTER | SYNTH_CAP_ENV2 | SYNTH_CAP_LFO1 | SYNTH_CAP_LFO2 |
        SYNTH_CAP_MIXER | SYNTH_CAP_MODMATRIX,
    sizeof(SubVoice),
    sub_init,
    sub_deinit,
    sub_begin_block,
    sub_voice_reset,
    sub_note_on,
    sub_note_off,
    sub_render,
    sub_busy,
    sub_level,
    /* render_block (S28): the batched contract exists for the modular graph,
     * whose per-voice loop is the wrong nesting. A fixed engine has nothing
     * to amortize — this whole chain is already one fused loop. */
    nullptr,
};
