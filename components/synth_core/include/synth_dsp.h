/*
 * osynth — shared DSP blocks (Session 5).
 *
 * Small, allocation-free building blocks composed by the engines: band-limited
 * oscillator (PolyBLEP saw/pulse), trapezoidal state-variable filter, ADSR
 * with linear attack + one-pole exponential decay/release, block-rate LFO,
 * xorshift white noise.
 *
 * Conventions:
 *  - Blocks hold state only, never parameters: engines read ParamStore once
 *    per block and pass values/coefficients in. Module gating falls out of
 *    this design — an engine's voice state simply doesn't contain the blocks
 *    it doesn't declare in its caps mask (synth_engine.h).
 *  - "step"/"inc" are phase increments per update, in cycles; phase lives in
 *    [0, 1) and every process function keeps it there.
 *  - Everything here is audio-task safe: no locks, no allocation. The
 *    trig-heavy coefficient builders (tanf/expf) are per-block, in the .cpp.
 */
#pragma once

#include <cmath>
#include <cstdint>

namespace osynth::dsp {

/* ---- shared tables / helpers ---- */

void tables_init(); /* builds the sine LUT; idempotent, call before rendering */

namespace detail {
inline constexpr uint32_t kSineN = 2048;
extern float g_sine[kSineN + 1]; /* +1: wraparound sample for interpolation */
} // namespace detail

/* LUT sine; phase01 must be in [0, 1). */
inline float sine01(float phase01) {
    const float x = phase01 * (float)detail::kSineN;
    const uint32_t i = (uint32_t)x;
    const float frac = x - (float)i;
    return detail::g_sine[i] + frac * (detail::g_sine[i + 1] - detail::g_sine[i]);
}

inline float midi_to_freq(float note) {
    return 440.0f * exp2f((note - 69.0f) * (1.0f / 12.0f));
}

/* ---- soft clipper (Session 21) ----
 *
 * The bus is float and unbounded, but two places downstream are not: the FX
 * delay lines are int16 (fx.cpp line_push) and the sink conversion is int16.
 * Both used to hard-clamp, and a hard clamp on a dense chord is a
 * discontinuity per sample pair — that is the "clicks with no underruns"
 * signature: the render made its deadline, the waveform just got its corners
 * cut off.
 *
 * Below the knee this is the identity, so nothing that was already in range
 * changes at all. Above it, a rational curve with unit slope at the knee
 * (C1 — no kink to hear at the transition) that approaches ±1
 * asymptotically and never reaches it. An overloaded chord now saturates
 * like an analogue output stage instead of clicking.
 */
inline constexpr float kSoftKnee = 0.80f;

inline float soft_clip(float x) {
    const float a = fabsf(x);
    if (a <= kSoftKnee) return x;
    constexpr float d = 1.0f - kSoftKnee;
    const float over = a - kSoftKnee;
    const float y = kSoftKnee + d * over / (over + d);
    return (x < 0.0f) ? -y : y;
}

/* ---- white noise (xorshift32) ---- */

struct Noise {
    uint32_t s = 0x9e3779b9u;
};

inline void noise_seed(Noise& n, uint32_t seed) {
    n.s = seed != 0 ? seed : 0x9e3779b9u;
}

inline float noise_next(Noise& n) {
    uint32_t x = n.s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    n.s = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

/* ---- oscillator ---- */

enum class OscWave : uint8_t { Sine = 0, Triangle, Saw, Pulse };

struct Osc {
    float phase = 0.0f;
};

/* Two-sample PolyBLEP residual for a downward step at phase 0. */
inline float polyblep(float t, float dt) {
    if (t < dt) {
        const float x = t / dt;
        return x + x - x * x - 1.0f;
    }
    if (t > 1.0f - dt) {
        const float x = (t - 1.0f) / dt;
        return x * x + x + x + 1.0f;
    }
    return 0.0f;
}

/* The waveform at an explicit phase, without advancing anything. Split out
 * of osc_next() (S28) so the modular graph's phase-modulation path can read
 * at a modulated phase while the ordinary path stays exactly what it was:
 * osc_next() is this plus the advance, so nothing about the fixed engines'
 * output changed. `p` must be in [0, 1). */
inline float osc_at(OscWave w, float p, float step, float pw) {
    switch (w) {
        case OscWave::Sine:
            return sine01(p);
        case OscWave::Triangle:
            return 2.0f * fabsf(2.0f * p - 1.0f) - 1.0f;
        case OscWave::Saw:
            return 2.0f * p - 1.0f - polyblep(p, step);
        default: { /* Pulse */
            float t2 = p - pw;
            if (t2 < 0.0f) t2 += 1.0f;
            return (p < pw ? 1.0f : -1.0f) + polyblep(p, step) -
                   polyblep(t2, step);
        }
    }
}

/* One sample, phase advances by `step` (must stay < 0.5). `pw` is used by
 * Pulse only. Triangle is naive: its harmonics fall at 12 dB/oct, so the
 * aliasing is negligible without band-limiting. */
inline float osc_next(Osc& o, OscWave w, float step, float pw) {
    const float out = osc_at(w, o.phase, step, pw);
    float next = o.phase + step;
    if (next >= 1.0f) next -= 1.0f;
    o.phase = next;
    return out;
}

/* ---- state-variable filter (trapezoidal integrators, Cytomic form) ---- */

enum class SvfMode : uint8_t { Lp = 0, Bp, Hp };

struct SvfCoef {
    float k, a1, a2, a3;
};

struct Svf {
    float ic1 = 0.0f, ic2 = 0.0f;
};

/* Per block (one tanf). cutoff_hz is clamped to [20, 0.45*sr]; res01 0..1
 * approaches self-oscillation near 1 (clamped so damping never reaches 0). */
SvfCoef svf_coef(float cutoff_hz, float res01, float sample_rate);

inline float svf_next(Svf& f, const SvfCoef& c, SvfMode m, float x) {
    const float v3 = x - f.ic2;
    const float v1 = c.a1 * f.ic1 + c.a2 * v3;
    const float v2 = f.ic2 + c.a2 * f.ic1 + c.a3 * v3;
    f.ic1 = 2.0f * v1 - f.ic1;
    f.ic2 = 2.0f * v2 - f.ic2;
    switch (m) {
        case SvfMode::Lp: return v2;
        case SvfMode::Bp: return v1;
        default:          return x - c.k * v1 - v2; /* Hp */
    }
}

/* ---- ADSR: linear attack, one-pole exponential decay/release ----
 *
 * There is no separate Sustain stage: Decay converges on the (live) sustain
 * value forever, so sustain edits during a held note track for free.
 * gate_on restarts the attack from the current level ("analog" retrigger —
 * a retrigger or steal never clicks). Rate-agnostic: pass rate = sr for
 * per-sample use, or sr / frames for one update per block. */

struct AdsrCoef {
    float attack_step; /* linear rise per update */
    float decay_k;     /* one-pole multiplier toward sustain */
    float sustain;
    float release_k;   /* one-pole multiplier toward 0 */
    /* whole-block multipliers (= *_k ^ frames), filled by adsr_coef_block()
     * only — adsr_block() needs them, adsr_next() ignores them */
    float decay_k_blk = 0.0f;
    float release_k_blk = 0.0f;
};

AdsrCoef adsr_coef(float attack_s, float decay_s, float sustain01,
                   float release_s, float rate);

/* Per-sample rates plus the whole-block multipliers for adsr_block(). */
AdsrCoef adsr_coef_block(float attack_s, float decay_s, float sustain01,
                         float release_s, float rate, uint32_t frames);

enum class AdsrStage : uint8_t { Idle = 0, Attack, Decay, Release };

struct Adsr {
    AdsrStage stage = AdsrStage::Idle;
    float level = 0.0f;
};

constexpr float kAdsrSilence = 1e-4f; /* -80 dB: a release below this is over */

inline void adsr_gate_on(Adsr& e) { e.stage = AdsrStage::Attack; }

inline void adsr_gate_off(Adsr& e) {
    if (e.stage != AdsrStage::Idle) e.stage = AdsrStage::Release;
}

inline void adsr_reset(Adsr& e) {
    e.stage = AdsrStage::Idle;
    e.level = 0.0f;
}

inline bool adsr_active(const Adsr& e) { return e.stage != AdsrStage::Idle; }

inline float adsr_next(Adsr& e, const AdsrCoef& c) {
    switch (e.stage) {
        case AdsrStage::Attack:
            e.level += c.attack_step;
            if (e.level >= 1.0f) {
                e.level = 1.0f;
                e.stage = AdsrStage::Decay;
            }
            break;
        case AdsrStage::Decay:
            e.level = c.sustain + (e.level - c.sustain) * c.decay_k;
            break;
        case AdsrStage::Release:
            e.level *= c.release_k;
            if (e.level < kAdsrSilence) {
                e.level = 0.0f;
                e.stage = AdsrStage::Idle;
            }
            break;
        default:
            break;
    }
    return e.level;
}

/* ---- block-linearized ADSR (S17) ----
 *
 * adsr_block() advances the whole envelope state machine once per block and
 * hands the render loop a linear ramp: sample i uses base + step * (i + 1),
 * so the per-sample code is a single add — no stage branches, no compare.
 * The exponential decay/release segments are chord-approximated per block
 * (SYNTH_BLOCK_SIZE samples ≈ 1.33 ms — the deviation from the true curve
 * is far below audibility); a stage boundary inside the block lands exactly
 * on the block edge instead, skewing timing by less than one block.
 * Needs a coefficient set built by adsr_coef_block(). A ramp with
 * base == 0 && step == 0 means the block is provably silent: the caller
 * may skip its sample loop entirely (advance oscillator phases with
 * osc_advance() so the voice stays phase-coherent). */

struct AdsrRamp {
    float base; /* level on entry; first sample uses base + step */
    float step; /* per-sample increment */
};

inline bool adsr_ramp_silent(const AdsrRamp& r) {
    return r.base == 0.0f && r.step == 0.0f;
}

inline AdsrRamp adsr_block(Adsr& e, const AdsrCoef& c, uint32_t frames) {
    const float lv = e.level;
    float end;
    switch (e.stage) {
        case AdsrStage::Attack:
            end = lv + c.attack_step * (float)frames;
            if (end >= 1.0f) {
                end = 1.0f;
                e.stage = AdsrStage::Decay;
            }
            break;
        case AdsrStage::Decay:
            /* converges on the (live) sustain forever, like adsr_next() */
            end = c.sustain + (lv - c.sustain) * c.decay_k_blk;
            break;
        case AdsrStage::Release:
            end = lv * c.release_k_blk;
            if (end < kAdsrSilence) {
                end = 0.0f;
                e.stage = AdsrStage::Idle;
            }
            break;
        default: /* Idle */
            return {0.0f, 0.0f};
    }
    e.level = end;
    return {lv, (end - lv) / (float)frames};
}

/* Advance an oscillator by a whole silent block (phase stays coherent). */
inline void osc_advance(Osc& o, float step, uint32_t frames) {
    float p = o.phase + step * (float)frames;
    o.phase = p - floorf(p);
}

/* ---- LFO (block rate: one value per audio block) ---- */

enum class LfoWave : uint8_t { Sine = 0, Triangle, Saw, Square, SampleHold };

struct Lfo {
    float phase = 0.0f;
    float sh = 0.0f;
    Noise rng;
};

inline void lfo_retrig(Lfo& l) {
    l.phase = 0.0f;
    l.sh = noise_next(l.rng);
}

/* Returns [-1, 1]; inc = rate_hz * frames / sample_rate. */
inline float lfo_next(Lfo& l, LfoWave w, float inc) {
    float p = l.phase + inc;
    if (p >= 1.0f) {
        p -= (float)(int)p;
        l.sh = noise_next(l.rng); /* SampleHold picks on each cycle wrap */
    }
    l.phase = p;
    switch (w) {
        case LfoWave::Sine:     return sine01(p);
        case LfoWave::Triangle: return 2.0f * fabsf(2.0f * p - 1.0f) - 1.0f;
        case LfoWave::Saw:      return 2.0f * p - 1.0f;
        case LfoWave::Square:   return p < 0.5f ? 1.0f : -1.0f;
        default:                return l.sh;
    }
}

} // namespace osynth::dsp
