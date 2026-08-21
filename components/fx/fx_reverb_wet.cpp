/*
 * osynth — WetReverb (S36), algorithm 1 of the master reverb bus.
 *
 * Ported from WET Reverb by Ronald Klarenbeek (Yonie), MIT-licensed:
 * https://github.com/yonie/WetReverb — see opensource/WetReverb/LICENSE.
 * MIT, so unlike MVerb and DuskVerb this one ships in the default build and
 * costs the firmware image nothing in licence terms.
 *
 * The original is a Schroeder bank — parallel lowpass-feedback combs into a
 * series allpass chain — with two things that make it sound like 1983 rather
 * than like the freeverb already on this bus:
 *
 *   1. It runs at HALF the host rate. That is not an optimisation in the
 *      original either; it is the voicing. A 24 kHz tank cannot carry
 *      anything above 12 kHz, which is exactly the ceiling the machines it
 *      is modelled on (Yamaha R1000, Roland DEP-5) had. Keeping it means
 *      keeping the sound, and it happens to halve the cost of the comb bank
 *      on the way, which is why this is the cheapest of the four algorithms
 *      despite having the most delay lines.
 *
 *   2. A tapped early-reflection field in front of the tank, with a per-tap
 *      lowpass that darkens later taps. Five tap patterns, from a tight
 *      6-tap room to a sparse 6-tap 92 ms wash, selected here by `size`.
 *
 * What changed in the port, and why:
 *
 *   - Buffers are the bus's int16 lines instead of std::vector<float>, like
 *      every other delay line in this instrument (synth_line.h). The lines
 *      are padded down on the way in and made up on the way out, exactly as
 *      freeverb does, because a comb running at fb 0.93 has a steady-state
 *      gain near 14 and would otherwise clip against int16 full scale.
 *
 *   - Resampling is a fixed 2:1 decimate/interpolate rather than the
 *      original's arbitrary-ratio LinearResampler. The internal rate is
 *      defined as half the host rate, so the general resampler had one job
 *      and a phase accumulator to do it with; at 2:1 the same result is a
 *      shift and a lerp, and the block maths stops depending on how many
 *      samples the resampler happened to emit.
 *
 *   - The 12-bit dither stage is kept. It looks like a bug on a 16-bit bus
 *      and is not: gain-ranging into a coarse quantizer is the Lexicon 224
 *      trick the original cites, and removing it makes the tail smoother
 *      than the machine being modelled. Its mt19937 is replaced with a
 *      xorshift — a 2.5 KB Mersenne twister per channel is not something an
 *      audio task should carry, and the dither only needs to be white.
 *
 *   - The pre-delay lines are dropped. The bus has its own pre-delay in
 *      front of every algorithm (fx.rev.pre), and the original's sat in the
 *      same place in the chain, so keeping both would have been two controls
 *      for one delay and ~14 KB spent twice.
 *
 * Cost: 12 combs x 2 channels at up to ~420 ms of 24 kHz int16, plus the
 * allpasses and the early field — about 230 KB, all of it PSRAM on a board
 * that has any. That lands before looper_init(), whose cap is sized from
 * whatever PSRAM is left, so it costs recording time rather than failing.
 */
#include "fx_reverb_wet.h"

#include <math.h>

#include "synth_config.h"
#include "synth_line.h"

namespace osynth {
namespace fx {
namespace {

using osynth::dsp::Line;
using osynth::dsp::line_alloc;
using osynth::dsp::line_push;
using osynth::dsp::line_read;

constexpr float kPi = 3.14159265359f;
constexpr float kTwoPi = 6.28318530718f;

constexpr int kCombs = 12;
constexpr int kAllpasses = 4;
constexpr int kMaxTaps = 8;

/* The original's comb and allpass lengths are picked off this prime table so
 * that no two lines share a period; keeping the table verbatim is what keeps
 * the modal density — and therefore the sound — the same. */
const int kPrimes[] = {31,  37,  41,  43,  47,  53,  59,  61,  67,  71,
                       73,  79,  83,  89,  97,  101, 103, 107, 109, 113,
                       127, 131, 137, 139, 149, 151, 157, 163, 167, 173};
constexpr int kNumPrimes = (int)(sizeof(kPrimes) / sizeof(kPrimes[0]));

/* Parameter ranges, spanning the five programs the original shipped as
 * buttons (Room, Plate, Hall, Cathedral, Cosmos) as one continuum instead.
 * A button per program does not fit an instrument whose every other control
 * is a knob, and the programs differ along exactly these axes anyway. */
constexpr float kBaseDelayMinMs = 7.0f;
constexpr float kBaseDelayMaxMs = 32.0f;
constexpr int kCombsMin = 4;
constexpr float kFbMin = 0.62f;
constexpr float kFbMax = 0.93f;
constexpr float kDampHzMax = 9500.0f;
constexpr float kDampHzMin = 1200.0f;
constexpr float kApFbMin = 0.50f;
constexpr float kApFbMax = 0.82f;

/* Input padding for the int16 lines, and its exact inverse on the way out.
 *
 * Same trick and the same value as the freeverb above it: a comb at the top
 * of the feedback range has a steady-state gain near 1/(1-0.93), so unpadded
 * input would clip against int16 full scale long before the tail decayed.
 *
 * The cost is that the make-up lifts the lines' quantization floor by the
 * same 24 dB. That would be a real objection on another algorithm; here it
 * lands almost exactly on the 12-bit floor the dither stage below imposes
 * deliberately, so the line noise disappears under the voicing rather than
 * adding to it. */
constexpr float kInGain = 0.06f;
constexpr float kOutGain = 1.0f / kInGain;

/* The original's output staging, unchanged: the comb sum is normalised by
 * the number of combs (times 1.5, which is empirical) and the whole wet path
 * then multiplied back up. */
constexpr float kCombNorm = 1.5f;
constexpr float kOutputGain = 5.0f;

struct Tap {
    float ms;
    float gl;
    float gr;
};
struct TapPattern {
    int n;
    Tap taps[kMaxTaps];
};

/* Verbatim from the original's EARLY_REFLECTION_PATTERNS: tap times in ms
 * with per-side gains, decaying with distance. The L/R gains differ slightly
 * on purpose — that asymmetry is the whole stereo image of the early field,
 * which is otherwise fed from a mono sum. */
const TapPattern kPatterns[5] = {
    /* Room: small space, tight reflections 3-25 ms */
    {6,
     {{3.1f, 0.40f, 0.34f},
      {7.3f, 0.33f, 0.38f},
      {11.7f, 0.28f, 0.25f},
      {16.1f, 0.21f, 0.24f},
      {19.8f, 0.15f, 0.13f},
      {24.3f, 0.09f, 0.11f}}},
    /* Plate: dense, 5 taps at 2-17 ms */
    {5,
     {{1.7f, 0.36f, 0.33f},
      {4.3f, 0.34f, 0.36f},
      {7.9f, 0.31f, 0.29f},
      {11.3f, 0.28f, 0.31f},
      {16.7f, 0.24f, 0.22f}}},
    /* Hall: wider spacing, 7 taps at 8-53 ms */
    {7,
     {{8.3f, 0.36f, 0.30f},
      {15.7f, 0.30f, 0.34f},
      {22.1f, 0.25f, 0.22f},
      {29.9f, 0.20f, 0.24f},
      {37.3f, 0.16f, 0.14f},
      {44.7f, 0.11f, 0.13f},
      {53.1f, 0.07f, 0.06f}}},
    /* Cathedral: wide spacing, 8 taps at 12-79 ms */
    {8,
     {{12.1f, 0.32f, 0.26f},
      {21.7f, 0.28f, 0.31f},
      {31.3f, 0.23f, 0.20f},
      {41.9f, 0.19f, 0.22f},
      {52.7f, 0.15f, 0.13f},
      {61.3f, 0.11f, 0.13f},
      {71.9f, 0.07f, 0.06f},
      {79.3f, 0.04f, 0.05f}}},
    /* Cosmos: sparse, ethereal, 6 taps at 15-92 ms */
    {6,
     {{15.3f, 0.28f, 0.22f},
      {31.7f, 0.24f, 0.27f},
      {49.1f, 0.19f, 0.16f},
      {63.9f, 0.14f, 0.17f},
      {78.3f, 0.09f, 0.07f},
      {91.7f, 0.05f, 0.06f}}},
};
constexpr float kTapMaxMs = 92.0f;

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* One-pole, in the original's parameterisation (coefficient is the fraction
 * of the input that gets through per sample). */
struct OnePole {
    float z = 0.0f;
    inline float lp(float x, float c) {
        z = c * x + (1.0f - c) * z;
        return z;
    }
    inline float hp(float x, float c) {
        z = c * x + (1.0f - c) * z;
        return x - z;
    }
};

inline float pole_coef(float hz, float sr) {
    return 1.0f - expf(-kTwoPi * hz / sr);
}

class WetReverb final : public RevAlgorithm {
   public:
    bool init(uint32_t sr) override;
    void reset() override;
    /* The attribute belongs on the declaration, not the out-of-line
     * definition. Note this only pins the code: the vtable itself is
     * .rodata, so the one indirect call per block still touches flash —
     * which is fine at 750 calls/s and would not be inside the sample loop. */
    SYNTH_RENDER_IRAM void render(const float* in_l, const float* in_r,
                                  float* out_l, float* out_r, size_t n,
                                  const RevParams& p) override;
    size_t lines(Line** out, size_t max) override;

   private:
    /* Comb i's length in ms at `base`, verbatim from the original's
     * setupCombs(): a prime stride so the twelve lines stay mutually
     * incommensurate however `base` moves. */
    static float comb_ms(int i, float base) {
        const int pi = (i * 7 + 3) % kNumPrimes;
        return base + (float)kPrimes[pi] * 0.25f * (float)(i + 1);
    }
    static float allpass_ms(int i) {
        const int pi = (i * 11 + 7) % kNumPrimes;
        return (float)kPrimes[pi] * 0.05f + 0.3f;
    }

    uint32_t sr_ = 48000;   /* host rate */
    float isr_ = 24000.0f;  /* internal rate = host / 2 */
    bool ok_ = false;

    Line comb_l_[kCombs], comb_r_[kCombs];
    float comb_damp_l_[kCombs] = {}, comb_damp_r_[kCombs] = {};
    Line ap_l_[kAllpasses], ap_r_[kAllpasses];
    uint32_t ap_d_l_[kAllpasses] = {}, ap_d_r_[kAllpasses] = {};
    Line early_;

    OnePole tap_lp_l_[kMaxTaps], tap_lp_r_[kMaxTaps];
    OnePole aa_l_, aa_r_;       /* anti-alias, host rate */
    OnePole recon_l_, recon_r_; /* reconstruction, host rate */
    OnePole late_lp_l_, late_lp_r_;
    OnePole hp_l_, hp_r_;
    float up_z_l_ = 0.0f, up_z_r_ = 0.0f; /* last internal sample, for the lerp */

    uint32_t rng_ = 0x1234567u;
    inline float noise() { /* xorshift32, scaled to [-1, 1) */
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return (float)(int32_t)rng_ * (1.0f / 2147483648.0f);
    }
};

bool WetReverb::init(uint32_t sr) {
    sr_ = sr;
    isr_ = (float)sr * 0.5f;

    bool ok = true;
    for (int i = 0; i < kCombs; ++i) {
        /* Sized for the longest this comb can ever be asked for: the top of
         * the base-delay range, and the right channel's extra 1 %. */
        const float ms = comb_ms(i, kBaseDelayMaxMs);
        const uint32_t nl = (uint32_t)(ms * isr_ / 1000.0f) + 4;
        const uint32_t nr = (uint32_t)(ms * 1.01f * isr_ / 1000.0f) + 4;
        ok = ok && line_alloc(comb_l_[i], nl);
        ok = ok && line_alloc(comb_r_[i], nr);
    }
    for (int i = 0; i < kAllpasses; ++i) {
        const float ms = allpass_ms(i);
        ap_d_l_[i] = (uint32_t)(ms * isr_ / 1000.0f);
        if (ap_d_l_[i] < 3) ap_d_l_[i] = 3;
        ap_d_r_[i] = ap_d_l_[i] + 1;
        ok = ok && line_alloc(ap_l_[i], ap_d_l_[i] + 4);
        ok = ok && line_alloc(ap_r_[i], ap_d_r_[i] + 4);
    }
    ok = ok && line_alloc(early_, (uint32_t)(kTapMaxMs * isr_ / 1000.0f) + 4);

    ok_ = ok;
    if (ok_) reset();
    return ok_;
}

void WetReverb::reset() {
    for (int i = 0; i < kCombs; ++i) comb_damp_l_[i] = comb_damp_r_[i] = 0.0f;
    for (int i = 0; i < kMaxTaps; ++i) {
        tap_lp_l_[i] = OnePole{};
        tap_lp_r_[i] = OnePole{};
    }
    aa_l_ = aa_r_ = recon_l_ = recon_r_ = OnePole{};
    late_lp_l_ = late_lp_r_ = hp_l_ = hp_r_ = OnePole{};
    up_z_l_ = up_z_r_ = 0.0f;
    rng_ = 0x1234567u;
}

size_t WetReverb::lines(Line** out, size_t max) {
    size_t n = 0;
    for (int i = 0; i < kCombs && n + 1 < max; ++i) {
        out[n++] = &comb_l_[i];
        out[n++] = &comb_r_[i];
    }
    for (int i = 0; i < kAllpasses && n + 1 < max; ++i) {
        out[n++] = &ap_l_[i];
        out[n++] = &ap_r_[i];
    }
    if (n < max) out[n++] = &early_;
    return n;
}

void WetReverb::render(const float* in_l, const float* in_r, float* out_l,
                       float* out_r, size_t n, const RevParams& p) {
    if (!ok_ || n == 0) return;

    /* --- the five programs, spread over the four knobs --- */
    const int combs =
        kCombsMin + (int)(clampf(p.size, 0.0f, 1.0f) * (kCombs - kCombsMin) + 0.5f);
    const float fb = kFbMin + p.size * (kFbMax - kFbMin);
    const float base_ms = kBaseDelayMinMs + p.size * (kBaseDelayMaxMs - kBaseDelayMinMs);
    int pat = (int)(p.size * 5.0f);
    if (pat > 4) pat = 4;
    if (pat < 0) pat = 0;

    /* Damping is swept on a log scale: the audible half of "darker" is all in
     * the bottom octaves of the range, and a linear sweep spends most of the
     * knob above 5 kHz where nothing happens. */
    const float damp_hz =
        kDampHzMax * powf(kDampHzMin / kDampHzMax, clampf(p.damp, 0.0f, 1.0f));
    const float damp_c = clampf(pole_coef(damp_hz, isr_), 0.02f, 0.98f);
    const float ap_fb = kApFbMin + clampf(p.diff, 0.0f, 1.0f) * (kApFbMax - kApFbMin);
    const float early_lvl = 0.05f + clampf(p.early, 0.0f, 1.0f) * 0.35f;
    const float late_lvl = 0.70f - clampf(p.early, 0.0f, 1.0f) * 0.45f;

    const float aa_c = pole_coef(10000.0f, (float)sr_);
    const float lp_c = pole_coef(6000.0f, isr_);
    const float hp_c = pole_coef(80.0f, isr_);

    const TapPattern& tp = kPatterns[pat];
    uint32_t tap_d[kMaxTaps];
    float tap_c[kMaxTaps];
    for (int t = 0; t < tp.n; ++t) {
        uint32_t d = (uint32_t)(tp.taps[t].ms * isr_ / 1000.0f);
        if (d < 1) d = 1;
        if (d >= early_.len) d = early_.len - 1;
        tap_d[t] = d;
        /* First tap nearly transparent, later taps progressively darker —
         * the original's absorption model, and the reason the early field
         * reads as a room rather than as a chorus. */
        tap_c[t] = 0.95f - 0.05f * (float)t;
    }

    uint32_t comb_d_l[kCombs], comb_d_r[kCombs];
    for (int c = 0; c < combs; ++c) {
        const float ms = comb_ms(c, base_ms);
        uint32_t dl = (uint32_t)(ms * isr_ / 1000.0f);
        uint32_t dr = (uint32_t)(ms * 1.01f * isr_ / 1000.0f);
        if (dl < 10) dl = 10;
        if (dr < 10) dr = 10;
        if (dl >= comb_l_[c].len) dl = comb_l_[c].len - 1;
        if (dr >= comb_r_[c].len) dr = comb_r_[c].len - 1;
        comb_d_l[c] = dl;
        comb_d_r[c] = dr;
    }

    const float comb_norm =
        (combs > 0) ? 1.0f / ((float)combs * kCombNorm) : 0.0f;

    /* --- half-rate loop ---
     *
     * Two host samples in, one internal sample out, one internal sample back
     * out interpolated against the previous one. The host block is always
     * even (SYNTH_BLOCK_SIZE is 64), so this never has to carry a half pair
     * across a block boundary; the odd tail is handled anyway rather than
     * asserting, because OSYNTH_BLOCK_SIZE is a Kconfig integer and someone
     * will eventually set it to 33. */
    size_t i = 0;
    while (i < n) {
        /* Decimate: anti-alias both host samples, keep the second. */
        const float a_l = aa_l_.lp(in_l[i], aa_c);
        const float a_r = aa_r_.lp(in_r[i], aa_c);
        float b_l = a_l, b_r = a_r;
        const bool pair = (i + 1) < n;
        if (pair) {
            b_l = aa_l_.lp(in_l[i + 1], aa_c);
            b_r = aa_r_.lp(in_r[i + 1], aa_c);
        }

        const float mono = (b_l + b_r) * 0.5f * kInGain;

        /* Early field: one write, `n` taps out, each with its own lowpass. */
        line_push(early_, mono);
        float er_l = 0.0f, er_r = 0.0f;
        for (int t = 0; t < tp.n; ++t) {
            const float s = line_read(early_, tap_d[t]);
            er_l += tap_lp_l_[t].lp(s, tap_c[t]) * tp.taps[t].gl;
            er_r += tap_lp_r_[t].lp(s, tap_c[t]) * tp.taps[t].gr;
        }

        /* Parallel lowpass-feedback combs. */
        float late_l = 0.0f, late_r = 0.0f;
        for (int c = 0; c < combs; ++c) {
            const float dl = line_read(comb_l_[c], comb_d_l[c]);
            comb_damp_l_[c] = damp_c * dl + (1.0f - damp_c) * comb_damp_l_[c];
            line_push(comb_l_[c], mono + comb_damp_l_[c] * fb);
            late_l += dl;

            const float dr = line_read(comb_r_[c], comb_d_r[c]);
            comb_damp_r_[c] = damp_c * dr + (1.0f - damp_c) * comb_damp_r_[c];
            line_push(comb_r_[c], mono + comb_damp_r_[c] * fb);
            late_r += dr;
        }
        late_l *= comb_norm;
        late_r *= comb_norm;

        /* Series allpass diffusion. */
        for (int a = 0; a < kAllpasses; ++a) {
            const float dl = line_read(ap_l_[a], ap_d_l_[a]);
            const float ol = -ap_fb * late_l + dl;
            line_push(ap_l_[a], late_l + ap_fb * ol);
            late_l = ol;

            const float dr = line_read(ap_r_[a], ap_d_r_[a]);
            const float orr = -ap_fb * late_r + dr;
            line_push(ap_r_[a], late_r + ap_fb * orr);
            late_r = orr;
        }

        late_l = late_lp_l_.lp(late_l, lp_c);
        late_r = late_lp_r_.lp(late_r, lp_c);

        float wl = early_lvl * er_l + late_lvl * late_l;
        float wr = early_lvl * er_r + late_lvl * late_r;
        wl = hp_l_.hp(wl, hp_c);
        wr = hp_r_.hp(wr, hp_c);
        wl *= kOutputGain * kOutGain;
        wr *= kOutputGain * kOutGain;

        /* 12-bit gain-ranged quantizer with TPDF dither — the Lexicon 224
         * DAC80 trick the original cites, and a load-bearing part of how this
         * algorithm sounds. Removing it makes the tail smoother than the
         * hardware being modelled, which is the opposite of the point. */
        constexpr float kQGain = 2.0f;
        constexpr float kLevels = 4096.0f;
        constexpr float kDither = 0.5f / kLevels;
        wl = floorf((wl * kQGain + (noise() + noise()) * kDither) * kLevels) /
             (kLevels * kQGain);
        wr = floorf((wr * kQGain + (noise() + noise()) * kDither) * kLevels) /
             (kLevels * kQGain);

        /* Interpolate back to host rate: the previous internal sample and
         * this one, half way for the first of the pair. */
        if (pair) {
            out_l[i] = 0.5f * (up_z_l_ + wl);
            out_r[i] = 0.5f * (up_z_r_ + wr);
            out_l[i + 1] = wl;
            out_r[i + 1] = wr;
        } else {
            out_l[i] = wl;
            out_r[i] = wr;
        }
        up_z_l_ = wl;
        up_z_r_ = wr;
        i += pair ? 2 : 1;
    }

    /* Reconstruction filter, host rate — removes the images the 2x lerp
     * leaves behind. */
    for (size_t k = 0; k < n; ++k) {
        out_l[k] = recon_l_.lp(out_l[k], aa_c);
        out_r[k] = recon_r_.lp(out_r[k], aa_c);
    }
}

WetReverb s_wet;

}  // namespace

RevAlgorithm* wetreverb_instance() { return &s_wet; }

}  // namespace fx
}  // namespace osynth
