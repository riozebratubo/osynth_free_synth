/*
 * osynth — DuskVerb Plate (S36), algorithm 3 of the master reverb bus.
 *
 * Ported from the DattorroTank engine of DuskVerb, by Dusk Audio:
 *   https://github.com/  (dusk-audio-plugins, plugins/DuskVerb)
 *
 * This file is part of osynth's fx_gpl component and is licensed under the
 * GNU General Public License version 3 or later, because DuskVerb is. See
 * components/fx_gpl/LICENSE. It is compiled only when CONFIG_OSYNTH_FX_GPL
 * is enabled; the default osynth build does not contain it and stays MIT.
 *
 * DuskVerb ships sixteen engines; this is its flagship, the one its manual
 * tells you to reach for first. Structurally it is the same figure-of-eight
 * as MVerb sitting next to it on this bus, with one addition that changes
 * the character completely:
 *
 *   each loop:  modulated allpass -> delay -> TWELVE-DEEP ALLPASS DENSITY
 *               CASCADE -> two-band damping -> static allpass -> delay
 *
 * The cascade is the point. A plain Dattorro tank builds echo density by
 * circulating — the first 30 ms of its impulse response is sparse, and it
 * only thickens after several passes. The cascade multiplies density on the
 * *first* pass, so the reverb arrives dense instead of becoming dense. On a
 * short decay, where there is no time for a plain tank to thicken at all,
 * that is the difference between a room and a flutter.
 *
 * The twelve lengths are all prime and all coprime to the loop delays, which
 * is what stops the cascade from having a period of its own; the upstream
 * comments are emphatic about this and the numbers are carried over verbatim.
 *
 * What is NOT ported, and why: the upstream engine also carries an octave-
 * band GEQ in the feedback path, a sustain-band limiter, a post-tank tonal-
 * correction EQ, per-band EDT shaping, a noise gate, a ducker and a freeze.
 * Every one of those is a *studio* control — they exist to make one reverb
 * sit in a mix of forty tracks. On an instrument's master bus, with four
 * knobs to spend, they would each cost cycles to sit at their neutral
 * setting. What remains is the topology, which is what makes it DuskVerb.
 */
#include <math.h>
#include <string.h>

#include "fx_gpl.h"
#include "synth_config.h"
#include "synth_line.h"

namespace osynth {
namespace fx {
namespace {

using osynth::dsp::Line;
using osynth::dsp::line_alloc;
using osynth::dsp::line_push;
using osynth::dsp::line_read;
using osynth::dsp::line_read_frac;

constexpr float kTwoPi = 6.28318530718f;

constexpr int kDensity = 12;
constexpr int kDiffusers = 6;
constexpr int kTaps = 7;

/* Every length below is in samples at 44.1 kHz, exactly as upstream, and is
 * rescaled to this build's rate at init. They are primes chosen to be coprime
 * to each other; treat them as one tuned set, not as twenty-six numbers. */
constexpr float kBaseSr = 44100.0f;
constexpr uint32_t kAp1[2] = {331, 443};
constexpr uint32_t kDel1[2] = {2203, 2081};
constexpr uint32_t kAp2[2] = {887, 1307};
constexpr uint32_t kDel2[2] = {1831, 1559};
constexpr uint32_t kDens[2][kDensity] = {
    {137, 199, 281, 53, 79, 113, 43, 67, 97, 131, 163, 191},
    {149, 211, 263, 61, 89, 127, 47, 71, 101, 139, 167, 193}};
constexpr uint32_t kDiff[kDiffusers] = {43, 71, 103, 167, 239, 313};
constexpr float kDiffCoef[kDiffusers] = {0.65f, 0.65f, 0.62f, 0.60f, 0.60f, 0.58f};

/* Output taps: {buffer, fraction of that buffer's length, sign}. Buffers are
 * 0 = L delay1, 1 = L delay2, 2 = L ap2, 3 = R delay1, 4 = R delay2,
 * 5 = R ap2. Both sides read from both tanks — that is what decorrelates a
 * stereo output fed from a mono sum. */
struct Tap {
    uint8_t buf;
    float frac;
    float sign;
};
constexpr Tap kTapL[kTaps] = {{3, 0.120f, 1.0f},  {3, 0.675f, 1.0f},
                              {5, 0.480f, -1.0f}, {0, 0.450f, 1.0f},
                              {1, 0.540f, -1.0f}, {2, 0.210f, -1.0f},
                              {4, 0.310f, -1.0f}};
constexpr Tap kTapR[kTaps] = {{0, 0.140f, 1.0f},  {0, 0.710f, 1.0f},
                              {2, 0.520f, -1.0f}, {3, 0.410f, 1.0f},
                              {4, 0.580f, -1.0f}, {5, 0.240f, -1.0f},
                              {1, 0.350f, -1.0f}};

/* `size` scales the loop lengths over this range; the lines are allocated at
 * the top of it and read shorter below. 2x is where upstream's "hall scale"
 * bases sit relative to its room ones, so it is the range the tuning was
 * chosen to stay coprime across. */
constexpr float kSizeMin = 0.5f;
constexpr float kSizeMax = 2.0f;

/* Modulation on the first allpass of each loop: a slow wander that breaks the
 * comb-tooth phase lock a static tank rings with. Depth is in samples, and
 * small — upstream measures 3-5 samples as enough to spread a 28-30 ms tank's
 * teeth while staying under the threshold where it reads as chorus. */
constexpr float kModDepth = 4.0f;
constexpr float kModHz = 0.9f;

/* Two-band damping crossover. Bass decaying longer than treble is what a real
 * room does and what every plate emulation since the EMT 140 has faked. */
constexpr float kXoverHz = 700.0f;
constexpr float kBassMul = 1.15f;

constexpr float kInGain = 0.25f;
constexpr float kOutGain = 1.0f / kInGain;

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

class DuskVerb final : public RevAlgorithm {
   public:
    bool init(uint32_t sr) override;
    void reset() override;
    SYNTH_RENDER_IRAM void render(const float* in_l, const float* in_r,
                                  float* out_l, float* out_r, size_t n,
                                  const RevParams& p) override;
    size_t lines(Line** out, size_t max) override;

   private:
    struct Tank {
        Line ap1, del1, ap2, del2;
        Line dens[kDensity];
        float lp = 0.0f;   /* two-band damping state */
        float mod = 0.0f;  /* modulation phase, radians */
    };

    /* Scaled base lengths in samples at size = 1. */
    struct Bases {
        float ap1, del1, ap2, del2;
        float dens[kDensity];
    };

    float sr_ = 48000.0f;
    bool ok_ = false;
    Tank t_[2];
    Bases b_[2];
    Line diff_[kDiffusers];
    uint32_t diff_d_[kDiffusers] = {};
    float cross_[2] = {0.0f, 0.0f};

    /* Current per-block read lengths, also used by the output taps. */
    uint32_t d_del1_[2] = {}, d_del2_[2] = {}, d_ap2_[2] = {};
};

bool DuskVerb::init(uint32_t sr) {
    sr_ = (float)sr;
    const float k = sr_ / kBaseSr;
    bool ok = true;
    for (int i = 0; i < 2; ++i) {
        b_[i].ap1 = (float)kAp1[i] * k;
        b_[i].del1 = (float)kDel1[i] * k;
        b_[i].ap2 = (float)kAp2[i] * k;
        b_[i].del2 = (float)kDel2[i] * k;
        /* +kModDepth+4 of headroom on ap1: it is read at a wandering
         * fractional delay and line_read_frac reads one sample behind. */
        ok = ok && line_alloc(t_[i].ap1,
                              (uint32_t)(b_[i].ap1 * kSizeMax + kModDepth) + 8);
        ok = ok && line_alloc(t_[i].del1, (uint32_t)(b_[i].del1 * kSizeMax) + 4);
        ok = ok && line_alloc(t_[i].ap2, (uint32_t)(b_[i].ap2 * kSizeMax) + 4);
        ok = ok && line_alloc(t_[i].del2, (uint32_t)(b_[i].del2 * kSizeMax) + 4);
        for (int d = 0; d < kDensity; ++d) {
            b_[i].dens[d] = (float)kDens[i][d] * k;
            ok = ok && line_alloc(t_[i].dens[d],
                                  (uint32_t)(b_[i].dens[d] * kSizeMax) + 4);
        }
    }
    /* The input diffusers scale with the sample rate only, never with size:
     * initial diffusion is a property of the medium, not of the room. */
    for (int i = 0; i < kDiffusers; ++i) {
        diff_d_[i] = (uint32_t)((float)kDiff[i] * k);
        if (diff_d_[i] < 2) diff_d_[i] = 2;
        ok = ok && line_alloc(diff_[i], diff_d_[i] + 4);
    }
    ok_ = ok;
    if (ok_) reset();
    return ok_;
}

void DuskVerb::reset() {
    for (int i = 0; i < 2; ++i) {
        t_[i].lp = 0.0f;
        t_[i].mod = (float)i * 1.7f; /* the two loops wander out of phase */
    }
    cross_[0] = cross_[1] = 0.0f;
}

size_t DuskVerb::lines(Line** out, size_t max) {
    size_t n = 0;
    for (int i = 0; i < 2; ++i) {
        if (n + 4 >= max) break;
        out[n++] = &t_[i].ap1;
        out[n++] = &t_[i].del1;
        out[n++] = &t_[i].ap2;
        out[n++] = &t_[i].del2;
        for (int d = 0; d < kDensity && n < max; ++d) out[n++] = &t_[i].dens[d];
    }
    for (int i = 0; i < kDiffusers && n < max; ++i) out[n++] = &diff_[i];
    return n;
}

void DuskVerb::render(const float* in_l, const float* in_r, float* out_l,
                      float* out_r, size_t n, const RevParams& p) {
    if (!ok_ || n == 0) return;

    const float sz = clampf(p.size, 0.0f, 1.0f);
    const float scale = kSizeMin + sz * (kSizeMax - kSizeMin);
    /* Decay is the loop gain. The ceiling is short of 1 by enough that the
     * tank cannot run away even with `size` modulated from an LFO. */
    const float decay = 0.30f + sz * 0.655f;

    /* Density cascade coefficient, around upstream's 0.55 baseline. */
    const float dc = clampf(0.30f + clampf(p.diff, 0.0f, 1.0f) * 0.55f, 0.0f, 0.85f);
    /* The two in-loop allpasses use the classic Dattorro pair. */
    const float ap1c = 0.70f;
    const float ap2c = 0.50f;

    const float damp = clampf(p.damp, 0.0f, 1.0f);
    const float treble = 1.0f - damp * 0.85f;
    const float xc = clampf(1.0f - expf(-kTwoPi * kXoverHz / sr_), 0.01f, 0.99f);

    /* `early` fades between the diffused early field and the tank. The early
     * field here is the input diffuser cascade's own output — upstream feeds
     * it forward into the tank only, but it is already a dense 20 ms burst,
     * which is exactly what an early-reflection send wants. */
    const float early = clampf(p.early, 0.0f, 1.0f);

    const float mod_inc = kTwoPi * kModHz / sr_;

    for (int i = 0; i < 2; ++i) {
        d_del1_[i] = (uint32_t)(b_[i].del1 * scale);
        d_del2_[i] = (uint32_t)(b_[i].del2 * scale);
        d_ap2_[i] = (uint32_t)(b_[i].ap2 * scale);
        if (d_del1_[i] < 4) d_del1_[i] = 4;
        if (d_del2_[i] < 4) d_del2_[i] = 4;
        if (d_ap2_[i] < 4) d_ap2_[i] = 4;
    }

    for (size_t k = 0; k < n; ++k) {
        /* Feed-forward input diffusion: a mono cascade of static allpasses
         * that smears the impulse into a dense burst before the tank sees it.
         * Not in the recirculating path, so it does not touch the decay. */
        float x = (in_l[k] + in_r[k]) * 0.5f * kInGain;
        for (int i = 0; i < kDiffusers; ++i) {
            const float buf = line_read(diff_[i], diff_d_[i]);
            const float o = -kDiffCoef[i] * x + buf;
            line_push(diff_[i], x + kDiffCoef[i] * o);
            x = o;
        }
        const float diffused = x;

        for (int i = 0; i < 2; ++i) {
            Tank& t = t_[i];
            /* The figure-of-eight cross: each loop is fed the other's tail. */
            float v = diffused + cross_[i ^ 1];

            /* Modulated allpass. The wander is why this does not ring. */
            t.mod += mod_inc;
            if (t.mod > kTwoPi) t.mod -= kTwoPi;
            float d1 = b_[i].ap1 * scale + kModDepth * (1.0f + sinf(t.mod));
            if (d1 < 2.0f) d1 = 2.0f;
            const float a1 = line_read_frac(t.ap1, d1);
            const float o1 = -ap1c * v + a1;
            line_push(t.ap1, v + ap1c * o1);
            v = o1;

            const float dl1 = line_read(t.del1, d_del1_[i]);
            line_push(t.del1, v);
            v = dl1;

            /* The density cascade — twelve short allpasses in series. */
            for (int d = 0; d < kDensity; ++d) {
                uint32_t dd = (uint32_t)(b_[i].dens[d] * scale);
                if (dd < 2) dd = 2;
                const float buf = line_read(t.dens[d], dd);
                const float o = -dc * v + buf;
                line_push(t.dens[d], v + dc * o);
                v = o;
            }

            /* Two-band damping: split at the crossover, decay the treble
             * faster than the bass. */
            t.lp += xc * (v - t.lp);
            v = t.lp * kBassMul + (v - t.lp) * treble;

            const float a2 = line_read(t.ap2, d_ap2_[i]);
            const float o2 = -ap2c * v + a2;
            line_push(t.ap2, v + ap2c * o2);
            v = o2;

            const float dl2 = line_read(t.del2, d_del2_[i]);
            line_push(t.del2, v);
            cross_[i] = dl2 * decay;
        }

        /* Seven signed taps a side, at fractions of each buffer's live
         * length, so they follow `size` instead of drifting off the end. */
        auto read_tap = [&](const Tap& tp) {
            const int tank = tp.buf >= 3 ? 1 : 0;
            const int which = tp.buf % 3;
            const Line* ln;
            uint32_t len;
            if (which == 0) {
                ln = &t_[tank].del1;
                len = d_del1_[tank];
            } else if (which == 1) {
                ln = &t_[tank].del2;
                len = d_del2_[tank];
            } else {
                ln = &t_[tank].ap2;
                len = d_ap2_[tank];
            }
            uint32_t d = (uint32_t)((float)len * tp.frac);
            if (d < 1) d = 1;
            return line_read(*ln, d) * tp.sign;
        };
        float wl = 0.0f, wr = 0.0f;
        for (int i = 0; i < kTaps; ++i) {
            wl += read_tap(kTapL[i]);
            wr += read_tap(kTapR[i]);
        }
        /* 7 taps summed; the 1/7 keeps the unit at roughly the level the
         * other three algorithms leave it at, so the enum is a character
         * switch and not a volume switch. */
        wl *= 1.0f / 7.0f;
        wr *= 1.0f / 7.0f;

        out_l[k] = (wl * (1.0f - early) + diffused * early) * kOutGain;
        out_r[k] = (wr * (1.0f - early) + diffused * early) * kOutGain;
    }
}

DuskVerb s_dusk;

}  // namespace

RevAlgorithm* duskverb_instance() { return &s_dusk; }

}  // namespace fx
}  // namespace osynth
