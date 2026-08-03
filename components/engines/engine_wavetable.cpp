/*
 * osynth — wavetable engine (Session 7; filter family Session 33).
 *
 * Per voice: wt1 + wt2 (two wavetable scanners) -> mixer -> filter -> amp env,
 * composed from the shared blocks in synth_dsp.h. The tables come from
 * factory_wavetables.h, generated at build time by tools/gen_wavetables.py:
 * 4 sets x 8 frames x 8 band-limited mips (int16 in flash). A scanner
 * linearly interpolates within the frame cycle and crossfades between the
 * two frames around the table position; the mip is chosen per block from
 * the voice's phase step so wt_mip_harm[m] harmonics never cross Nyquist —
 * high notes stay clean without oversampling.
 *
 * Position is the engine's signature control: pos = base + env.pos * env2
 * + lfo2.pos * lfo2, clamped to [0,1], shared by both oscillators (each on
 * top of its own base). env2 doubles as the filter envelope via flt.env
 * (default 0 — the morph is the star; the SVF defaults mostly open).
 * lfo1 is vibrato. Same block-rate modulation scheme as the subtractive
 * engine; parameters are read once per block in begin_block().
 *
 * The filter is one of five types (flt.type) behind a bypass switch
 * (flt.on), both folded into a dsp::FltType per block — see synth_dsp.h.
 * The formant type pairs particularly well with this engine: a wavetable
 * morph under a vowel morph is two spectra moving at once.
 *
 * Mod matrix (S9): positions, mix levels, cutoff/reso/drive/vowel, flt.env
 * and the env2/lfo2 position depths go through synth_mod_apply() per voice —
 * any matrix source can drive the morph (see docs/PARAM_MAP.md).
 *
 * Gain staging: frames are RMS-normalized by the generator (~0.45) with a
 * per-set peak cap, so the default mix (levels summing to <= 1) cannot clip
 * at full polyphony under the voice manager's 1/SYNTH_VOICES headroom.
 */
#include "engine_wavetable.h"

#include <atomic>
#include <cmath>

#include "esp_log.h"

#include "factory_wavetables.h"
#include "synth_config.h"
#include "synth_dsp.h"
#include "synth_mod.h"
#include "synth_params.h"
#include "synth_smooth.h"

static const char* TAG = "eng_wt";

namespace dsp = osynth::dsp;
using osynth::ParamCurve;
using osynth::ParamDesc;
using osynth::ParamStore;
using osynth::ParamType;

namespace {

struct WtVoice {
    dsp::Osc osc1, osc2; /* phase accumulators */
    dsp::Filt filt;
    dsp::Adsr env1; /* amplitude, per sample */
    dsp::Adsr env2; /* mod (position + filter), block rate */
    dsp::Lfo lfo1, lfo2;
    uint8_t note = 60;
    float vel = 0.0f;
};

/* ---- parameter set (order matches PIdx) ---- */

enum PIdx {
    OSC1_TABLE, OSC1_POS, OSC2_TABLE, OSC2_POS, OSC2_SEMI, OSC2_FINE,
    MIX_OSC1, MIX_OSC2, ENV_POS,
    FLT_MODE, FLT_CUTOFF, FLT_RESO, FLT_ENV, FLT_KBD,
    FLT_ON, FLT_TYPE, FLT_DRIVE, FLT_SPREAD, FLT_VOWEL,
    ENV1_A, ENV1_D, ENV1_S, ENV1_R,
    ENV2_A, ENV2_D, ENV2_S, ENV2_R,
    LFO1_RATE, LFO1_WAVE, LFO1_PITCH,
    LFO2_RATE, LFO2_WAVE, LFO2_POS,
    P_COUNT
};

/* Append-only, both of them: presets store the index, not the name. */
const char* const kFltModes[] = {"lp",   "bp", "hp", "notch",
                                 "peak", "ap", "bp norm"};
const char* const kFltTypes[] = {"svf 12", "svf 24", "ladder", "dual", "vowel"};
const char* const kLfoWaves[] = {"sine", "triangle", "saw", "square", "s&h"};

const ParamDesc kParams[P_COUNT] = {
    {WT_PID_OSC1_TABLE, "wt1.table", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(WT_TABLE_COUNT - 1), 0.0f /* basic */, wt_table_names,
     WT_TABLE_COUNT},
    {WT_PID_OSC1_POS, "wt1.pos", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {WT_PID_OSC2_TABLE, "wt2.table", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(WT_TABLE_COUNT - 1), 1.0f /* sync */, wt_table_names,
     WT_TABLE_COUNT},
    {WT_PID_OSC2_POS, "wt2.pos", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {WT_PID_OSC2_SEMI, "wt2.semi", ParamType::Int, ParamCurve::Linear,
     -24.0f, 24.0f, 0.0f, nullptr, 0},
    {WT_PID_OSC2_FINE, "wt2.fine", ParamType::Float, ParamCurve::Linear,
     -100.0f, 100.0f, 4.0f, nullptr, 0}, /* cents */
    {WT_PID_MIX_OSC1, "mix.wt1", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.8f, nullptr, 0},
    {WT_PID_MIX_OSC2, "mix.wt2", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {WT_PID_ENV_POS, "env.pos", ParamType::Float, ParamCurve::Linear,
     -1.0f, 1.0f, 0.55f, nullptr, 0}, /* env2 -> position, frames-fraction */
    {WT_PID_FLT_MODE, "flt.mode", ParamType::Enum, ParamCurve::Linear,
     0.0f, 6.0f, 0.0f /* lp */, kFltModes, 7},
    {WT_PID_FLT_CUTOFF, "flt.cutoff", ParamType::Float, ParamCurve::Exp,
     20.0f, 18000.0f, 9000.0f, nullptr, 0}, /* mostly open by default */
    {WT_PID_FLT_RESO, "flt.reso", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.1f, nullptr, 0},
    {WT_PID_FLT_ENV, "flt.env", ParamType::Float, ParamCurve::Linear,
     -4.0f, 4.0f, 0.0f, nullptr, 0}, /* octaves; env2 doubles as filter env */
    {WT_PID_FLT_KBD, "flt.kbd", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.5f, nullptr, 0},
    /* Bypass, defaulting on so pre-S33 patches keep their filter. */
    {WT_PID_FLT_ON, "flt.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0},
    /* svf 12 + no drive == what this engine rendered before S33. */
    {WT_PID_FLT_TYPE, "flt.type", ParamType::Enum, ParamCurve::Linear,
     0.0f, 4.0f, 0.0f /* svf 12 */, kFltTypes, 5},
    {WT_PID_FLT_DRIVE, "flt.drive", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {WT_PID_FLT_SPREAD, "flt.spread", ParamType::Float, ParamCurve::Linear,
     0.0f, 6.0f, 2.0f, nullptr, 0}, /* dual: passband width in octaves */
    {WT_PID_FLT_VOWEL, "flt.vowel", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* vowel: morph a-e-i-o-u */
    {WT_PID_ENV1_ATTACK, "env1.attack", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.005f, nullptr, 0},
    {WT_PID_ENV1_DECAY, "env1.decay", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.25f, nullptr, 0},
    {WT_PID_ENV1_SUSTAIN, "env1.sustain", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.7f, nullptr, 0},
    {WT_PID_ENV1_RELEASE, "env1.release", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.25f, nullptr, 0},
    {WT_PID_ENV2_ATTACK, "env2.attack", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.002f, nullptr, 0},
    {WT_PID_ENV2_DECAY, "env2.decay", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.35f, nullptr, 0},
    {WT_PID_ENV2_SUSTAIN, "env2.sustain", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.15f, nullptr, 0},
    {WT_PID_ENV2_RELEASE, "env2.release", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.3f, nullptr, 0},
    {WT_PID_LFO1_RATE, "lfo1.rate", ParamType::Float, ParamCurve::Exp,
     0.02f, 20.0f, 5.0f, nullptr, 0},
    {WT_PID_LFO1_WAVE, "lfo1.wave", ParamType::Enum, ParamCurve::Linear,
     0.0f, 4.0f, 0.0f /* sine */, kLfoWaves, 5},
    {WT_PID_LFO1_PITCH, "lfo1.pitch", ParamType::Float, ParamCurve::Linear,
     0.0f, 2.0f, 0.0f, nullptr, 0}, /* semitones (a matrix wheel slot raises it) */
    {WT_PID_LFO2_RATE, "lfo2.rate", ParamType::Float, ParamCurve::Exp,
     0.02f, 20.0f, 0.8f, nullptr, 0},
    {WT_PID_LFO2_WAVE, "lfo2.wave", ParamType::Enum, ParamCurve::Linear,
     0.0f, 4.0f, 1.0f /* triangle */, kLfoWaves, 5},
    {WT_PID_LFO2_POS, "lfo2.pos", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* position wobble depth */
};

const std::atomic<float>* s_p[P_COUNT];

inline float pv(PIdx i) { return s_p[i]->load(std::memory_order_relaxed); }

/* ---- block-shared cache, rebuilt in begin_block() ---- */

struct BlockCache {
    int t1, t2;
    float pos1, pos2;
    float mul2; /* wt2 frequency multiplier (semi + fine) */
    float m1, m2;
    float env_pos;
    dsp::SvfMode fmode;
    dsp::FltType ftype;
    float cutoff, reso, fenv_oct, fkbd, fdrive, fspread, fvowel;
    dsp::AdsrCoef amp; /* per-sample rates */
    dsp::AdsrCoef mod; /* per-block rates */
    float lfo1_inc, lfo2_inc;
    dsp::LfoWave lw1, lw2;
    float l1_pitch, l2_pos;
};

BlockCache s_bc;

/* Block-rate parameter smoothers (S21) — see synth_smooth.h. The table-set
 * enums, the LFO waves and the filter mode are switches; the ADSR times only
 * set rates. Everything else here can step the signal, so it is smoothed. */
struct Smoothers {
    dsp::Smooth pos1, pos2, mul2, m1, m2, env_pos;
    dsp::Smooth cutoff, reso, fenv, fkbd, fdrive, fspread, fvowel;
    dsp::Smooth l1_pitch, l2_pos;
};

Smoothers s_sm;

constexpr float kSr = (float)SYNTH_SAMPLE_RATE;
constexpr float kInvSr = 1.0f / (float)SYNTH_SAMPLE_RATE;
constexpr float kMaxStep = 0.49f; /* keep phase increments below Nyquist */

inline float clamp01(float x) { return fminf(fmaxf(x, 0.0f), 1.0f); }

/* ---- wavetable scanner ----
 *
 * Per voice-block: pick the mip whose harmonic cap fits the phase step
 * (step * harm <= 0.5 keeps the top harmonic below Nyquist; the last mip
 * holds 2 harmonics, so only f0 > 12 kHz can marginally exceed it) and
 * resolve the frame pair around the position. Per sample: two linear
 * interpolations (the +1 guard sample makes [i+1] always valid) and the
 * frame crossfade. */

struct WtScan {
    const int16_t* a0; /* frame floor(pos), at the mip's offset */
    const int16_t* a1; /* frame floor(pos)+1 */
    float ff;          /* frame crossfade fraction */
    float len;         /* mip length */
};

inline void wt_scan_setup(WtScan& s, int table, float pos01, float step) {
    int m = 0;
    while (m < WT_MIP_COUNT - 1 && step * (float)wt_mip_harm[m] > 0.5f) ++m;
    float fpos = pos01 * (float)(WT_FRAME_COUNT - 1);
    int fi = (int)fpos;
    if (fi > WT_FRAME_COUNT - 2) fi = WT_FRAME_COUNT - 2;
    s.ff = fpos - (float)fi;
    const uint16_t off = wt_mip_off[m];
    s.a0 = wt_tables[table][fi] + off;
    s.a1 = wt_tables[table][fi + 1] + off;
    s.len = (float)wt_mip_len[m];
}

inline float wt_scan_next(dsp::Osc& o, const WtScan& s, float step) {
    const float x = o.phase * s.len;
    const uint32_t i = (uint32_t)x;
    const float fr = x - (float)i;
    const float v0 = (float)s.a0[i] + fr * (float)(s.a0[i + 1] - s.a0[i]);
    const float v1 = (float)s.a1[i] + fr * (float)(s.a1[i + 1] - s.a1[i]);
    float next = o.phase + step;
    if (next >= 1.0f) next -= 1.0f;
    o.phase = next;
    return (v0 + s.ff * (v1 - v0)) * (1.0f / 32768.0f);
}

/* ---- vtable entries ---- */

esp_err_t wt_init(void) {
    dsp::tables_init(); /* LFO sine */
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
             "wavetable engine up: %u params, caps 0x%02x (%d tables x %d "
             "frames, %d mips, ~%u KB flash)",
             (unsigned)P_COUNT, (unsigned)g_engine_wavetable.caps,
             WT_TABLE_COUNT, WT_FRAME_COUNT, WT_MIP_COUNT,
             (unsigned)(sizeof(wt_tables) / 1024));
    return ESP_OK;
}

void wt_deinit(void) {
    ParamStore::instance().removeRange(osynth::PID_ENGINE_BASE,
                                       osynth::PID_FX_BASE);
}

void SYNTH_RENDER_IRAM wt_begin_block(size_t frames) {
    BlockCache& b = s_bc;
    b.t1 = (int)pv(OSC1_TABLE);
    b.pos1 = dsp::smooth_lin(s_sm.pos1, pv(OSC1_POS));
    b.t2 = (int)pv(OSC2_TABLE);
    b.pos2 = dsp::smooth_lin(s_sm.pos2, pv(OSC2_POS));
    b.mul2 = dsp::smooth_exp(
        s_sm.mul2,
        exp2f((pv(OSC2_SEMI) + pv(OSC2_FINE) * 0.01f) * (1.0f / 12.0f)));
    b.m1 = dsp::smooth_lin(s_sm.m1, pv(MIX_OSC1));
    b.m2 = dsp::smooth_lin(s_sm.m2, pv(MIX_OSC2));
    b.env_pos = dsp::smooth_lin(s_sm.env_pos, pv(ENV_POS));
    b.fmode = (dsp::SvfMode)(int)pv(FLT_MODE);
    /* Bypass folds into the type here, so render() stays one call. */
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
    b.mod = dsp::adsr_coef(pv(ENV2_A), pv(ENV2_D), pv(ENV2_S), pv(ENV2_R),
                           kSr / (float)frames);
    b.lfo1_inc = pv(LFO1_RATE) * (float)frames * kInvSr;
    b.lw1 = (dsp::LfoWave)(int)pv(LFO1_WAVE);
    b.l1_pitch = dsp::smooth_lin(s_sm.l1_pitch, pv(LFO1_PITCH));
    b.lfo2_inc = pv(LFO2_RATE) * (float)frames * kInvSr;
    b.lw2 = (dsp::LfoWave)(int)pv(LFO2_WAVE);
    b.l2_pos = dsp::smooth_lin(s_sm.l2_pos, pv(LFO2_POS));
}

void wt_voice_reset(void* vs) {
    WtVoice& v = *(WtVoice*)vs;
    v = WtVoice{};
}

void wt_note_on(void* vs, uint8_t note, float vel01, bool was_sounding) {
    WtVoice& v = *(WtVoice*)vs;
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

void wt_note_off(void* vs) {
    WtVoice& v = *(WtVoice*)vs;
    dsp::adsr_gate_off(v.env1);
    dsp::adsr_gate_off(v.env2);
}

void SYNTH_RENDER_IRAM wt_render(void* vs, const synth_voice_frame_t* f,
                                 float* __restrict__ out_l,
                                 float* __restrict__ out_r, size_t frames) {
    WtVoice& v = *(WtVoice*)vs;
    const BlockCache& b = s_bc;

    /* block-rate modulators */
    const float l1 = dsp::lfo_next(v.lfo1, b.lw1, b.lfo1_inc);
    const float l2 = dsp::lfo_next(v.lfo2, b.lw2, b.lfo2_inc);
    const float menv = dsp::adsr_next(v.env2, b.mod);

    /* mod matrix: per-voice sources, then the modulatable destinations */
    const synth_mod_voice_src_t ms = {menv, l1, l2, v.vel, (float)v.note};
    const float l1_pitch = synth_mod_apply(WT_PID_LFO1_PITCH, b.l1_pitch, &ms);
    const float env_pos = synth_mod_apply(WT_PID_ENV_POS, b.env_pos, &ms);
    const float l2_pos = synth_mod_apply(WT_PID_LFO2_POS, b.l2_pos, &ms);
    const float pos1 = synth_mod_apply(WT_PID_OSC1_POS, b.pos1, &ms);
    const float pos2 = synth_mod_apply(WT_PID_OSC2_POS, b.pos2, &ms);
    const float m1 = synth_mod_apply(WT_PID_MIX_OSC1, b.m1, &ms);
    const float m2 = synth_mod_apply(WT_PID_MIX_OSC2, b.m2, &ms);
    const float cutoff = synth_mod_apply(WT_PID_FLT_CUTOFF, b.cutoff, &ms);
    const float reso = synth_mod_apply(WT_PID_FLT_RESO, b.reso, &ms);
    const float fenv_oct = synth_mod_apply(WT_PID_FLT_ENV, b.fenv_oct, &ms);
    const float fdrive = synth_mod_apply(WT_PID_FLT_DRIVE, b.fdrive, &ms);
    const float fvowel = synth_mod_apply(WT_PID_FLT_VOWEL, b.fvowel, &ms);

    const float pitch_mul =
        (l1_pitch != 0.0f) ? exp2f(l1 * l1_pitch * (1.0f / 12.0f)) : 1.0f;
    const float step1 = fminf(f->freq_hz * pitch_mul * kInvSr, kMaxStep);
    const float step2 = fminf(step1 * b.mul2, kMaxStep);

    const float posmod = env_pos * menv + l2_pos * l2;
    WtScan s1, s2;
    wt_scan_setup(s1, b.t1, clamp01(pos1 + posmod), step1);
    wt_scan_setup(s2, b.t2, clamp01(pos2 + posmod), step2);

    const float oct = fenv_oct * menv +
                      b.fkbd * ((float)v.note - 60.0f) * (1.0f / 12.0f);
    const dsp::FiltCoef fc =
        dsp::filt_coef(b.ftype, b.fmode, cutoff * exp2f(oct), reso, fdrive,
                       b.fspread, fvowel, kSr);

    const float gl = f->gain_l * v.vel;
    const float gr = f->gain_r * v.vel;

    /* amp envelope: one state-machine pass, then a branch-free linear ramp */
    const dsp::AdsrRamp ar = dsp::adsr_block(v.env1, b.amp, (uint32_t)frames);
    if (SYNTH_UNLIKELY(dsp::adsr_ramp_silent(ar))) {
        /* provably silent block: keep the scanners phase-coherent, skip
         * the rest (the SVF holds state — its output was gated to 0 anyway) */
        dsp::osc_advance(v.osc1, step1, (uint32_t)frames);
        dsp::osc_advance(v.osc2, step2, (uint32_t)frames);
        return;
    }

    float a = ar.base;
    for (size_t i = 0; i < frames; ++i) {
        a += ar.step;
        const float x = m1 * wt_scan_next(v.osc1, s1, step1) +
                        m2 * wt_scan_next(v.osc2, s2, step2);
        const float y = dsp::filt_next(v.filt, fc, x) * a;
        out_l[i] += y * gl;
        out_r[i] += y * gr;
    }
}

bool wt_busy(const void* vs) {
    return dsp::adsr_active(((const WtVoice*)vs)->env1);
}

float wt_level(const void* vs) {
    return ((const WtVoice*)vs)->env1.level;
}

} // namespace

extern "C" const synth_engine_t g_engine_wavetable = {
    "wavetable",
    SYNTH_CAP_FILTER | SYNTH_CAP_ENV2 | SYNTH_CAP_LFO1 | SYNTH_CAP_LFO2 |
        SYNTH_CAP_MIXER | SYNTH_CAP_MODMATRIX,
    sizeof(WtVoice),
    wt_init,
    wt_deinit,
    wt_begin_block,
    wt_voice_reset,
    wt_note_on,
    wt_note_off,
    wt_render,
    wt_busy,
    wt_level,
    nullptr, /* render_block (S28): fixed engines render per voice */
};
