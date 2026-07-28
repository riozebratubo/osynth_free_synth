/*
 * osynth — shared DSP blocks: sine table + per-block coefficient builders
 * (Session 5). The per-sample paths are inline in synth_dsp.h.
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

SvfCoef svf_coef(float cutoff_hz, float res01, float sample_rate) {
    const float fmax = 0.45f * sample_rate;
    if (cutoff_hz < 20.0f) cutoff_hz = 20.0f;
    if (cutoff_hz > fmax) cutoff_hz = fmax;
    if (res01 < 0.0f) res01 = 0.0f;
    if (res01 > 0.98f) res01 = 0.98f;
    const float g = tanf(3.14159265f * cutoff_hz / sample_rate);
    SvfCoef c;
    c.k = 2.0f - 2.0f * res01;
    c.a1 = 1.0f / (1.0f + g * (g + c.k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
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
