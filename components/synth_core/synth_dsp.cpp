/*
 * osynth — shared DSP blocks: sine table + per-block coefficient builders
 * (Session 5; the filter family added in S33). The per-sample paths are
 * inline in synth_dsp.h — everything here is trig-heavy and runs once per
 * block, which is the whole reason the split exists.
 */
#include "synth_dsp.h"

namespace osynth::dsp {

namespace detail {
float g_sine[kSineN + 1];
} // namespace detail

void tables_init() {
    static bool ready = false;
    if (ready) return;
    for (uint32_t i = 0; i <= detail::kSineN; ++i) {
        detail::g_sine[i] =
            sinf(6.28318530718f * (float)i / (float)detail::kSineN);
    }
    ready = true;
}

namespace {

inline float clamp01(float x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

inline float clamp_cutoff(float hz, float sample_rate) {
    const float fmax = 0.45f * sample_rate;
    if (hz < 20.0f) return 20.0f;
    return hz > fmax ? fmax : hz;
}

/* First three formants and their relative levels, measured male values.
 * Ordered a-e-i-o-u because that is the order a morph knob should sweep. */
struct VowelPoint {
    float f[3];
    float g[3];
};

const VowelPoint kVowels[5] = {
    /* a */ {{730.0f, 1090.0f, 2440.0f}, {1.00f, 0.50f, 0.25f}},
    /* e */ {{530.0f, 1840.0f, 2480.0f}, {1.00f, 0.32f, 0.20f}},
    /* i */ {{270.0f, 2290.0f, 3010.0f}, {1.00f, 0.25f, 0.16f}},
    /* o */ {{570.0f,  840.0f, 2410.0f}, {1.00f, 0.35f, 0.10f}},
    /* u */ {{300.0f,  870.0f, 2240.0f}, {1.00f, 0.10f, 0.06f}},
};

/* Drive across a two-stage cascade: boost into the first, trim out of the
 * last, each stage still clipping its own resonant integrator. */
void drive_cascade(float drive01, SvfCoef* first, SvfCoef* last) {
    float pre, post;
    svf_drive_gains(drive01, &pre, &post);
    first->pre = pre;
    last->post = post;
}

} // namespace

SvfCoef svf_coef_k(float cutoff_hz, float k, float sample_rate) {
    cutoff_hz = clamp_cutoff(cutoff_hz, sample_rate);
    if (k < kSvfMinK) k = kSvfMinK;
    const float g = tanf(3.14159265f * cutoff_hz / sample_rate);
    SvfCoef c;
    c.k = k;
    c.a1 = 1.0f / (1.0f + g * (g + c.k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    return c;
}

SvfCoef svf_coef(float cutoff_hz, float res01, float sample_rate) {
    /* res01 0.98 -> k 0.04, which is where kSvfMinK came from. */
    if (res01 < 0.0f) res01 = 0.0f;
    if (res01 > 0.98f) res01 = 0.98f;
    return svf_coef_k(cutoff_hz, 2.0f - 2.0f * res01, sample_rate);
}

/* Drive is a gain staging trick, not a waveshaper: push harder into a
 * saturator that was already there and pull the result back down. The trim
 * is 1/sqrt(pre) rather than 1/pre because a drive control that is perfectly
 * level-compensated does not feel like drive — half the gain stays in. */
void svf_drive_gains(float drive01, float* pre, float* post) {
    const float d = clamp01(drive01);
    const float p = 1.0f + 7.0f * d; /* up to +18 dB into the clipper */
    *pre = p;
    *post = 1.0f / sqrtf(p);
}

SvfCoef svf_coef_drive(float cutoff_hz, float res01, float drive01,
                       float sample_rate) {
    SvfCoef c = svf_coef(cutoff_hz, res01, sample_rate);
    svf_drive_gains(drive01, &c.pre, &c.post);
    return c;
}

Svf2Coef svf2_coef(float cutoff_hz, float res01, float drive01,
                   float sample_rate) {
    /* 4th-order Butterworth Q pair: 0.5412 and 1.3065, as damping. */
    constexpr float kK1 = 1.8477590f;
    constexpr float kK2 = 0.7653669f;
    Svf2Coef c;
    c.s1 = svf_coef_k(cutoff_hz, kK1, sample_rate);
    c.s2 = svf_coef_k(cutoff_hz, kK2 - (kK2 - kSvfMinK) * clamp01(res01),
                      sample_rate);
    drive_cascade(drive01, &c.s1, &c.s2);
    return c;
}

Svf2Coef dual_coef(float cutoff_hz, float res01, float spread_oct,
                   float drive01, float sample_rate) {
    const float half = 0.5f * spread_oct;
    Svf2Coef c;
    c.s1 = svf_coef(cutoff_hz * exp2f(half), res01, sample_rate);   /* lp */
    c.s2 = svf_coef(cutoff_hz * exp2f(-half), res01, sample_rate);  /* hp */
    drive_cascade(drive01, &c.s1, &c.s2);
    return c;
}

LadderCoef ladder_coef(float cutoff_hz, float res01, float drive01,
                       float sample_rate) {
    cutoff_hz = clamp_cutoff(cutoff_hz, sample_rate);
    const float g = tanf(3.14159265f * cutoff_hz / sample_rate);
    LadderCoef c;
    c.g = g / (1.0f + g);
    /* 4 is the textbook oscillation threshold; the delayed feedback reaches
     * it slightly early, and soft_clip() inside the loop is what keeps that
     * from being a problem. */
    c.k = 4.0f * clamp01(res01);
    svf_drive_gains(drive01, &c.pre, &c.post);
    return c;
}

VowelCoef vowel_coef(float morph01, float shift_hz, float res01, float drive01,
                     float sample_rate) {
    const float t = clamp01(morph01) * 4.0f;
    int i = (int)t;
    if (i > 3) i = 3;
    const float fr = t - (float)i;
    const VowelPoint& a = kVowels[i];
    const VowelPoint& b = kVowels[i + 1];

    /* Neutral at 1 kHz so the cutoff parameter keeps a sane default and every
     * existing cutoff modulation becomes a vocal-tract shift. */
    const float shift = clamp_cutoff(shift_hz, sample_rate) * 0.001f;
    /* Formant bandwidths: Q 4 (breathy) to 24 (a talking robot). */
    const float k = 1.0f / (4.0f + 20.0f * clamp01(res01));

    VowelCoef c;
    float f[3], gn[3];
    for (int n = 0; n < 3; ++n) {
        /* log-domain frequency interpolation — see the header note */
        f[n] = a.f[n] * exp2f(fr * log2f(b.f[n] / a.f[n]));
        gn[n] = a.g[n] + fr * (b.g[n] - a.g[n]);
    }
    c.c1 = svf_coef_k(f[0] * shift, k, sample_rate);
    c.c2 = svf_coef_k(f[1] * shift, k, sample_rate);
    c.c3 = svf_coef_k(f[2] * shift, k, sample_rate);
    c.g1 = gn[0];
    c.g2 = gn[1];
    c.g3 = gn[2];
    /* Normalized against the gain sum so the morph does not change loudness,
     * with a fixed makeup on top. The 1.5 is a by-ear figure and wants a
     * listen on hardware before it is treated as final. */
    const float sum = gn[0] + gn[1] + gn[2];
    float drive_post;
    svf_drive_gains(drive01, &c.pre, &drive_post);
    /* The two trims multiply: the drive's own compensation on top of the
     * morph normalization, not instead of it. */
    c.post = ((sum > 0.0f) ? 1.5f / sum : 1.0f) * drive_post;
    return c;
}

FiltCoef filt_coef(FltType type, SvfMode mode, float cutoff_hz, float res01,
                   float drive01, float spread_oct, float vowel01,
                   float sample_rate) {
    FiltCoef c;
    c.type = type;
    c.mode = mode;
    c.drive = drive01 > 0.0f;
    switch (type) {
        case FltType::Svf12:
            c.svf.s1 = c.drive
                           ? svf_coef_drive(cutoff_hz, res01, drive01, sample_rate)
                           : svf_coef(cutoff_hz, res01, sample_rate);
            break;
        case FltType::Svf24:
            c.svf = svf2_coef(cutoff_hz, res01, drive01, sample_rate);
            break;
        case FltType::Ladder:
            c.ladder = ladder_coef(cutoff_hz, res01, drive01, sample_rate);
            break;
        case FltType::Dual:
            c.svf = dual_coef(cutoff_hz, res01, spread_oct, drive01, sample_rate);
            break;
        case FltType::Vowel:
            c.vowel = vowel_coef(vowel01, cutoff_hz, res01, drive01, sample_rate);
            break;
        default: /* Bypass: filt_next() returns its input, nothing to build */
            break;
    }
    return c;
}

AdsrCoef adsr_coef(float attack_s, float decay_s, float sustain01,
                   float release_s, float rate) {
    constexpr float kMinT = 0.0005f; /* 0.5 ms floor keeps the steps finite */
    AdsrCoef c;
    c.attack_step = 1.0f / (fmaxf(attack_s, kMinT) * rate);
    c.decay_k = expf(-1.0f / (fmaxf(decay_s, kMinT) * rate));
    c.sustain = sustain01;
    c.release_k = expf(-1.0f / (fmaxf(release_s, kMinT) * rate));
    return c;
}

AdsrCoef adsr_coef_block(float attack_s, float decay_s, float sustain01,
                         float release_s, float rate, uint32_t frames) {
    constexpr float kMinT = 0.0005f;
    AdsrCoef c = adsr_coef(attack_s, decay_s, sustain01, release_s, rate);
    /* k^frames computed directly (two expf per block, not per voice) */
    const float n = (float)frames;
    c.decay_k_blk = expf(-n / (fmaxf(decay_s, kMinT) * rate));
    c.release_k_blk = expf(-n / (fmaxf(release_s, kMinT) * rate));
    return c;
}

} // namespace osynth::dsp
