/*
 * osynth — shared DSP blocks (Session 5).
 *
 * Small, allocation-free building blocks composed by the engines: band-limited
 * oscillator (PolyBLEP saw/pulse), a family of filters built on a trapezoidal
 * state-variable core (S33: 12/24 dB, seven responses, drive, Moog ladder,
 * dual/spread, formant), ADSR with linear attack + one-pole exponential
 * decay/release, block-rate LFO, xorshift white noise.
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

/* ---- waveshapers (S28, shared with the FX bus in S34) ----
 *
 * Distinct from soft_clip() above, which is the *output stage* saturator and
 * is the identity below its knee. These are the ones a patch reaches for on
 * purpose. They live here rather than in one caller so that "fold" means the
 * same curve in a graph patch and on the master bus — the two are routinely
 * A/B'd against each other and a private copy that drifted would be a
 * genuinely confusing bug to chase.
 *
 * Rational tanh (Padé): within ~0.3% over |x| < 3 and monotone beyond it,
 * for four flops and no libm call in the sample loop. */
inline float fast_tanh(float x) {
    if (x < -3.0f) return -1.0f;
    if (x > 3.0f) return 1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Triangle wavefolder: reflects at ±1 instead of clipping, so overdrive
 * adds harmonics rather than removing them. */
inline float fold(float x) {
    while (x > 1.0f || x < -1.0f) {
        if (x > 1.0f) x = 2.0f - x;
        if (x < -1.0f) x = -2.0f - x;
    }
    return x;
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

/* Every response below is a linear combination of the three values the core
 * already computes (v0 = input, v1 = bandpass, v2 = lowpass), so the four
 * modes added in S33 cost one more switch arm and no arithmetic at all.
 * Appended, never reordered: the value is what presets store. */
enum class SvfMode : uint8_t {
    Lp = 0,
    Bp,
    Hp,
    Notch, /* lp + hp */
    Peak,  /* lp - hp */
    Ap,    /* flat magnitude, swept phase */
    BpN,   /* bandpass at constant peak gain (plain Bp rises with resonance) */
};

struct SvfCoef {
    float k, a1, a2, a3;
    /* Drive (S33): input pre-gain into a soft-clipped resonant state, then an
     * output trim. 1/1 is the linear filter, and svf_next() ignores both —
     * only svf_next_drive() reads them, so the clean path is untouched. */
    float pre = 1.0f, post = 1.0f;
};

struct Svf {
    float ic1 = 0.0f, ic2 = 0.0f;
};

/* Damping floor. k = 1/Q, so this is Q = 25: high enough to ring for a long
 * time, low enough that the integrators can never lose their grip. */
inline constexpr float kSvfMinK = 0.04f;

/* Per block (one tanf). cutoff_hz is clamped to [20, 0.45*sr].
 *
 * svf_coef_k() takes damping directly (k = 1/Q) because the cascades below
 * need per-stage Q values that no 0..1 "resonance" mapping can express;
 * svf_coef() is the musician-facing wrapper, where res01 0..1 approaches
 * self-oscillation near 1 (clamped so damping never reaches 0). */
SvfCoef svf_coef_k(float cutoff_hz, float k, float sample_rate);
SvfCoef svf_coef(float cutoff_hz, float res01, float sample_rate);
SvfCoef svf_coef_drive(float cutoff_hz, float res01, float drive01,
                       float sample_rate);

/* Pre/post gains for a 0..1 drive amount. Split out because every filter
 * type below drives its input the same way. */
void svf_drive_gains(float drive01, float* pre, float* post);

/* Response pick, shared by the clean and driven paths. v0 is the filter's
 * input *after* any drive pre-gain — the Hp/Notch/Peak/Ap sums are only
 * correct against the same signal the integrators saw. */
inline float svf_out(SvfMode m, const SvfCoef& c, float v0, float v1,
                     float v2) {
    switch (m) {
        case SvfMode::Lp:    return v2;
        case SvfMode::Bp:    return v1;
        case SvfMode::Hp:    return v0 - c.k * v1 - v2;
        case SvfMode::Notch: return v0 - c.k * v1;
        case SvfMode::Peak:  return 2.0f * v2 - v0 + c.k * v1;
        case SvfMode::Ap:    return v0 - 2.0f * c.k * v1;
        default:             return c.k * v1; /* BpN */
    }
}

inline float svf_next(Svf& f, const SvfCoef& c, SvfMode m, float x) {
    const float v3 = x - f.ic2;
    const float v1 = c.a1 * f.ic1 + c.a2 * v3;
    const float v2 = f.ic2 + c.a2 * f.ic1 + c.a3 * v3;
    f.ic1 = 2.0f * v1 - f.ic1;
    f.ic2 = 2.0f * v2 - f.ic2;
    return svf_out(m, c, x, v1, v2);
}

/* Saturating variant: the input stage clips, and so does the bandpass
 * integrator — which is the resonant one, so this is what turns a screaming
 * digital self-oscillation into something that compresses and growls instead.
 * soft_clip() is the identity below 0.8, so at low levels the only difference
 * from svf_next() is the pre/post gain pair. */
inline float svf_next_drive(Svf& f, const SvfCoef& c, SvfMode m, float x) {
    const float v0 = soft_clip(x * c.pre);
    const float v3 = v0 - f.ic2;
    const float v1 = c.a1 * f.ic1 + c.a2 * v3;
    const float v2 = f.ic2 + c.a2 * f.ic1 + c.a3 * v3;
    f.ic1 = soft_clip(2.0f * v1 - f.ic1);
    f.ic2 = 2.0f * v2 - f.ic2;
    return svf_out(m, c, v0, v1, v2) * c.post;
}

/* ---- 24 dB/oct: two SVFs in series (S33) ----
 *
 * Also the state for the dual/spread filter, which is the same two SVFs at
 * different cutoffs — a pair of state-variable filters is a pair of state-
 * variable filters, so there is one struct for it. */

struct Svf2 {
    Svf a, b;
};

struct Svf2Coef {
    SvfCoef s1, s2;
};

/* Butterworth-split cascade: stage 1 is pinned at Q = 0.5412 and stage 2
 * carries the user's resonance from Q = 1.3065 up to self-oscillation. Two
 * *identical* stages would square the passband response and put a 6 dB bump
 * where flat is wanted; splitting the Q pair keeps reso 0 genuinely flat. */
Svf2Coef svf2_coef(float cutoff_hz, float res01, float drive01,
                   float sample_rate);

/* Both stages run the same response, so "lp 24" is two lowpasses, "notch 24"
 * is two notches, and so on. */
inline float svf2_next(Svf2& f, const Svf2Coef& c, SvfMode m, float x) {
    return svf_next(f.b, c.s2, m, svf_next(f.a, c.s1, m, x));
}

inline float svf2_next_drive(Svf2& f, const Svf2Coef& c, SvfMode m, float x) {
    return svf_next_drive(f.b, c.s2, m, svf_next_drive(f.a, c.s1, m, x));
}

/* ---- dual / spread: lowpass and highpass in series (S33) ----
 *
 * A bandpass whose width is a parameter instead of a side effect of Q: the
 * lowpass sits half a `spread` above the cutoff and the highpass half below,
 * so spread is the passband width in octaves and resonance still peaks both
 * edges independently. Reuses Svf2 for state. */

Svf2Coef dual_coef(float cutoff_hz, float res01, float spread_oct,
                   float drive01, float sample_rate);

inline float dual_next(Svf2& f, const Svf2Coef& c, float x) {
    return svf_next(f.b, c.s2, SvfMode::Hp, svf_next(f.a, c.s1, SvfMode::Lp, x));
}

inline float dual_next_drive(Svf2& f, const Svf2Coef& c, float x) {
    return svf_next_drive(f.b, c.s2, SvfMode::Hp,
                          svf_next_drive(f.a, c.s1, SvfMode::Lp, x));
}

/* ---- Moog ladder, 4-pole lowpass (S33) ----
 *
 * Four TPT one-poles with a saturated resonance feedback. Deliberately *not*
 * the zero-delay-feedback solve: a one-sample-delayed feedback costs one
 * multiply where the implicit solve costs a division per sample, and the
 * price is some cutoff/resonance detuning up near Nyquist that nobody plays.
 * No tanh either — soft_clip() is the saturator, which is what bounds the
 * self-oscillation instead of letting it run away.
 *
 * The 0.5 * x term inside the feedback is the classic passband compensation:
 * without it the ladder's low end drains away as resonance rises, which is
 * authentic and also unusable. */

struct Ladder {
    float s[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float fb = 0.0f;
};

struct LadderCoef {
    float g;    /* one-pole TPT coefficient */
    float k;    /* resonance feedback, 0..4 (self-oscillates near 4) */
    float pre, post;
};

LadderCoef ladder_coef(float cutoff_hz, float res01, float drive01,
                       float sample_rate);

inline float ladder_next(Ladder& f, const LadderCoef& c, float x) {
    float u = soft_clip((x - c.k * (f.fb - 0.5f * x)) * c.pre);
    for (int i = 0; i < 4; ++i) {
        const float v = (u - f.s[i]) * c.g;
        const float y = v + f.s[i];
        f.s[i] = y + v;
        u = y;
    }
    f.fb = u;
    return u * c.post;
}

/* ---- formant / vowel filter (S33) ----
 *
 * Three constant-gain bandpasses on the first three formants, morphing
 * a -> e -> i -> o -> u. Frequencies interpolate in the log domain (a linear
 * morph between 270 Hz and 2290 Hz sweeps through everything in between and
 * sounds like a siren, not a vowel); gains interpolate linearly.
 *
 * The cutoff parameter becomes a formant shift, neutral at 1 kHz — so filter
 * envelope, keyboard tracking and every cutoff modulation route already in
 * the patch move the whole vocal tract, which is the useful thing to do
 * with them here. */

struct Vowel {
    Svf f1, f2, f3;
};

struct VowelCoef {
    SvfCoef c1, c2, c3;
    float g1, g2, g3;
    float pre, post;
};

VowelCoef vowel_coef(float morph01, float shift_hz, float res01, float drive01,
                     float sample_rate);

inline float vowel_next(Vowel& f, const VowelCoef& c, float x) {
    return (c.g1 * svf_next(f.f1, c.c1, SvfMode::BpN, x) +
            c.g2 * svf_next(f.f2, c.c2, SvfMode::BpN, x) +
            c.g3 * svf_next(f.f3, c.c3, SvfMode::BpN, x)) *
           c.post;
}

inline float vowel_next_drive(Vowel& f, const VowelCoef& c, float x) {
    const float xd = soft_clip(x * c.pre);
    return (c.g1 * svf_next(f.f1, c.c1, SvfMode::BpN, xd) +
            c.g2 * svf_next(f.f2, c.c2, SvfMode::BpN, xd) +
            c.g3 * svf_next(f.f3, c.c3, SvfMode::BpN, xd)) *
           c.post;
}

/* ---- the filter family behind one dispatch (S33) ----
 *
 * The fixed engines each own one filter whose *type* is a parameter, so they
 * need a state blob wide enough for whichever type is selected and a single
 * call that routes to it. (The modular graph does not use this: there, each
 * heavy type is its own node kind, so the compiler can cost it honestly —
 * see the kind table in graph_model.cpp.)
 *
 * Bypass is a type rather than a branch in the render loop. `flt.on` is a
 * switch in the app, but by the time the sample loop sees it, "off" is just
 * another arm of a switch it was already going to execute — no second loop,
 * no per-sample test that every other patch has to pay for.
 *
 * The type switch is per sample. It is loop-invariant, so it predicts
 * perfectly and costs an indirect jump; the alternative — templating the
 * whole render loop on the type — was rejected because it multiplies the
 * IRAM-resident render path by five, and IRAM is the scarcer resource here.
 * Values are stored in presets: append only. */
enum class FltType : uint8_t {
    Svf12 = 0,
    Svf24,
    Ladder,
    Dual,
    Vowel,
    /* Not a selectable value — `flt.on == 0` substitutes it per block. Keep
     * it last so the five above keep matching the parameter's enum names. */
    Bypass,
};

struct Filt {
    Svf2 svf;      /* Svf12 uses .a alone; Svf24 and Dual use both */
    Ladder ladder;
    Vowel vowel;
};

struct FiltCoef {
    FltType type = FltType::Svf12;
    SvfMode mode = SvfMode::Lp; /* Svf12/Svf24 only; the rest fix their own */
    bool drive = false;         /* selects the saturating path */
    Svf2Coef svf;
    LadderCoef ladder;
    VowelCoef vowel;
};

/* Builds only the member the type needs — a voice's cutoff differs from its
 * neighbour's (keyboard tracking, envelope), so this runs per voice per
 * block and there is no sense paying for coefficients nothing will read. */
FiltCoef filt_coef(FltType type, SvfMode mode, float cutoff_hz, float res01,
                   float drive01, float spread_oct, float vowel01,
                   float sample_rate);

inline float filt_next(Filt& f, const FiltCoef& c, float x) {
    if (c.drive) {
        switch (c.type) {
            case FltType::Svf12:  return svf_next_drive(f.svf.a, c.svf.s1, c.mode, x);
            case FltType::Svf24:  return svf2_next_drive(f.svf, c.svf, c.mode, x);
            case FltType::Ladder: return ladder_next(f.ladder, c.ladder, x);
            case FltType::Dual:   return dual_next_drive(f.svf, c.svf, x);
            case FltType::Vowel:  return vowel_next_drive(f.vowel, c.vowel, x);
            default:              return x; /* Bypass */
        }
    }
    switch (c.type) {
        case FltType::Svf12:  return svf_next(f.svf.a, c.svf.s1, c.mode, x);
        case FltType::Svf24:  return svf2_next(f.svf, c.svf, c.mode, x);
        case FltType::Ladder: return ladder_next(f.ladder, c.ladder, x);
        case FltType::Dual:   return dual_next(f.svf, c.svf, x);
        case FltType::Vowel:  return vowel_next(f.vowel, c.vowel, x);
        default:              return x; /* Bypass */
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
