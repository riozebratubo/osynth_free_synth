/*
 * osynth — MVerb (S36), algorithm 2 of the master reverb bus.
 *
 * Ported from MVerb by Martin Eastwood:
 *   Copyright (c) 2010 Martin Eastwood
 *   https://github.com/martineastwood/mverb
 *
 * This file is part of osynth's fx_gpl component and is licensed under the
 * GNU General Public License version 3 or later, because MVerb is. See
 * components/fx_gpl/LICENSE. It is compiled only when CONFIG_OSYNTH_FX_GPL
 * is enabled; the default osynth build does not contain it and stays MIT.
 *
 * MVerb is Dattorro's figure-of-eight, written out plainly:
 *
 *   in -> bandwidth LP -> pre-delay -> 4 smearing allpasses -> tank
 *   tank: two loops, each  [4-tap allpass -> delay -> damping LP ->
 *                           4-tap allpass -> delay],  cross-fed so each
 *                           loop's output is the other loop's input
 *   out: 7 signed taps per side, gathered from inside both loops
 *        + an 8-tap early-reflection field, balanced against the tank
 *
 * What changed in the port, and why:
 *
 *   - The templated fixed-size float arrays (fifteen of them, each declared
 *     `[96000]`, about 5.8 MB of .bss) become the bus's int16 lines, sized
 *     from the actual delay each one needs at maximum size. That is the only
 *     change that matters for whether this runs at all: 5.8 MB is more than
 *     the S3 has, and the original only ever uses a fraction of each array.
 *     Total here is ~87 KB.
 *
 *   - `Size` sweeps the tank's delay *lengths*. The original recomputes them
 *     in reset(), which also clears every buffer — fine when the host calls
 *     it on a preset change, not fine on a knob the player is turning. Here
 *     the lengths are recomputed per block and the lines are read at a
 *     variable delay instead, so `size` sweeps continuously the way the rest
 *     of this instrument's knobs do.
 *
 *   - The pre-delay line is dropped; the bus has its own (fx.rev.pre) in the
 *     same place in the chain.
 *
 *   - The 4x-oversampled state-variable filters are kept oversampled. They
 *     are the bandwidth and damping controls, and at 1x an SVF's cutoff
 *     warps badly enough near the top of the range that `damp` stops being
 *     monotonic. Four iterations of two SVFs per channel per sample is the
 *     bulk of this algorithm's CPU and it is buying something.
 *
 *   - The original's `ControlRate` decimation of the filter coefficient
 *     updates (once per millisecond) becomes once per block, which at 64
 *     frames / 48 kHz is 1.33 ms — the same idea, on the grid this firmware
 *     already has.
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

constexpr float kPi = 3.14159265359f;

/* Delay lengths in seconds at Size = 1, straight from MVerb::reset(). Every
 * one of these is a tuned constant of the original; changing any of them
 * changes what the algorithm is. */
constexpr float kApS[4] = {0.0048f, 0.0036f, 0.0127f, 0.0093f};
constexpr float kApFb[4] = {0.75f, 0.75f, 0.625f, 0.625f};
constexpr float kTankApS[4] = {0.020f, 0.060f, 0.030f, 0.089f};
constexpr float kTankDelS[4] = {0.15f, 0.12f, 0.14f, 0.11f};
/* Extra read positions inside the tank allpasses and delays, as offsets from
 * the write head. The original stores them as independent indices that
 * advance in lockstep with the head, so an offset of `off` reads the sample
 * written (Length - off) ago — which makes an offset of ZERO a real tap at
 * the line's full length, not an unused slot. Two of the entries below are 0
 * for exactly that reason. */
constexpr float kTankApTap[4][2] = {
    {0.0f, 0.0f}, {0.006f, 0.041f}, {0.0f, 0.0f}, {0.031f, 0.011f}};
constexpr float kTankDelTap[4][3] = {{0.067f, 0.011f, 0.121f},
                                     {0.036f, 0.089f, 0.0f},
                                     {0.0089f, 0.099f, 0.0f},
                                     {0.067f, 0.0041f, 0.0f}};
constexpr float kErS[2] = {0.089f, 0.069f};
/* The SIX taps the original actually sums, which are its GetIndex(2)..(7) —
 * i.e. index3..index8 of its eight, NOT index2..index7. The first offset in
 * each of its SetIndex() calls (0.0199 / 0.0099) is index2 and is never read;
 * the trailing 0 is index8 and IS read, at gain 0.1. Getting this window
 * wrong shifts every early reflection by one tap and quietly changes the
 * room. */
constexpr float kErTap[2][6] = {
    {0.0219f, 0.0354f, 0.0389f, 0.0414f, 0.0692f, 0.0f},
    {0.0110f, 0.0182f, 0.0189f, 0.0213f, 0.0431f, 0.0f}};
constexpr float kErGain[6] = {0.6f, 0.4f, 0.3f, 0.3f, 0.1f, 0.1f};

/* `Size` never goes below this: the tap offsets above are absolute seconds,
 * and a tank shorter than its own longest tap would read past its head. The
 * original has the same floor implicitly, by only ever being driven from a
 * normalized Size the host clamps to [0, 1] and then never scaling below the
 * tap it reads. Stating it here makes the read bounds provable. */
constexpr float kSizeMin = 0.35f;

/* Padding into the int16 lines and its inverse out, for the same reason the
 * freeverb and WetReverb have one: the tank runs at a decay coefficient up to
 * 0.805 through two allpasses at 0.625, and the circulating signal sits well
 * above the input. */
constexpr float kInGain = 0.25f;
constexpr float kOutGain = 1.0f / kInGain;

/* MVerb's own 4x-oversampled state-variable lowpass. Two per channel: one on
 * the input (bandwidth) and one inside each tank loop (damping). */
struct Svf {
    float low = 0.0f, band = 0.0f, f = 0.1f;
    inline void set(float hz, float sr) {
        /* sr is multiplied by the oversample count, as in the original. */
        f = 2.0f * sinf(kPi * hz / (sr * 4.0f));
    }
    inline float lp(float x) {
        for (int i = 0; i < 4; ++i) {
            low += f * band + 1e-25f;
            const float high = x - low - 2.0f * band; /* q = 2 - 2*0 */
            band += f * high;
        }
        return low;
    }
    inline void clear() { low = band = 0.0f; }
};

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

class MVerb final : public RevAlgorithm {
   public:
    bool init(uint32_t sr) override;
    void reset() override;
    SYNTH_RENDER_IRAM void render(const float* in_l, const float* in_r,
                                  float* out_l, float* out_r, size_t n,
                                  const RevParams& p) override;
    size_t lines(Line** out, size_t max) override;

   private:
    float sr_ = 48000.0f;
    bool ok_ = false;

    Line ap_[4];        /* input smearing allpasses, fixed length */
    uint32_t ap_d_[4] = {};
    Line tank_ap_[4];   /* two per loop */
    Line tank_del_[4];
    Line er_[2];

    Svf bw_[2], damp_[2];
    float prev_l_ = 0.0f, prev_r_ = 0.0f; /* the cross-feed between loops */
};

bool MVerb::init(uint32_t sr) {
    sr_ = (float)sr;
    bool ok = true;
    for (int i = 0; i < 4; ++i) {
        ap_d_[i] = (uint32_t)(kApS[i] * sr_);
        ok = ok && line_alloc(ap_[i], ap_d_[i] + 4);
        /* Tank lines are allocated at Size = 1 and read shorter as `size`
         * comes down, so the allocation is the ceiling and the read is the
         * control. */
        ok = ok && line_alloc(tank_ap_[i], (uint32_t)(kTankApS[i] * sr_) + 4);
        ok = ok && line_alloc(tank_del_[i], (uint32_t)(kTankDelS[i] * sr_) + 4);
    }
    for (int i = 0; i < 2; ++i)
        ok = ok && line_alloc(er_[i], (uint32_t)(kErS[i] * sr_) + 4);
    ok_ = ok;
    if (ok_) reset();
    return ok_;
}

void MVerb::reset() {
    for (int i = 0; i < 2; ++i) {
        bw_[i].clear();
        damp_[i].clear();
    }
    prev_l_ = prev_r_ = 0.0f;
}

size_t MVerb::lines(Line** out, size_t max) {
    size_t n = 0;
    for (int i = 0; i < 4 && n + 2 < max; ++i) {
        out[n++] = &ap_[i];
        out[n++] = &tank_ap_[i];
        out[n++] = &tank_del_[i];
    }
    for (int i = 0; i < 2 && n < max; ++i) out[n++] = &er_[i];
    return n;
}

void MVerb::render(const float* in_l, const float* in_r, float* out_l,
                   float* out_r, size_t n, const RevParams& p) {
    if (!ok_ || n == 0) return;

    const float size = kSizeMin + clampf(p.size, 0.0f, 1.0f) * (1.0f - kSizeMin);
    /* The original's mappings, verbatim: decay and density are both
     * compressed into the top of their nominal range because the tank goes
     * unstable above it. */
    const float decay = 0.005f + 0.7995f * clampf(p.size, 0.0f, 1.0f);
    const float dens1 = 0.005f + 0.7995f * clampf(p.diff, 0.0f, 1.0f);
    float dens2 = decay + 0.15f;
    if (dens2 > 0.5f) dens2 = 0.5f;
    if (dens2 < 0.25f) dens2 = 0.25f;
    const float early = clampf(p.early, 0.0f, 1.0f);

    /* Coefficients once per block — the original's ControlRate decimation,
     * on this firmware's existing grid. `damp` is inverted: the knob reads
     * "how dark", the filter takes "how open". */
    const float bw_hz = 100.0f + 18400.0f * 0.9f;
    const float damp_hz = 100.0f + 18400.0f * (1.0f - clampf(p.damp, 0.0f, 1.0f));
    for (int i = 0; i < 2; ++i) {
        bw_[i].set(bw_hz, sr_);
        damp_[i].set(damp_hz, sr_);
    }

    /* Delays for this block, in samples, scaled by `size`. */
    uint32_t tap_d[4], del_d[4];
    uint32_t ap_tap[4][2], del_tap[4][3];
    for (int i = 0; i < 4; ++i) {
        tap_d[i] = (uint32_t)(kTankApS[i] * sr_ * size);
        del_d[i] = (uint32_t)(kTankDelS[i] * sr_ * size);
        if (tap_d[i] < 4) tap_d[i] = 4;
        if (del_d[i] < 4) del_d[i] = 4;
        for (int t = 0; t < 2; ++t) {
            const uint32_t off = (uint32_t)(kTankApTap[i][t] * sr_ * size);
            ap_tap[i][t] = off < tap_d[i] ? tap_d[i] - off : 1;
        }
        for (int t = 0; t < 3; ++t) {
            const uint32_t off = (uint32_t)(kTankDelTap[i][t] * sr_ * size);
            del_tap[i][t] = off < del_d[i] ? del_d[i] - off : 1;
        }
    }
    uint32_t er_d[2], er_tap[2][6];
    for (int i = 0; i < 2; ++i) {
        er_d[i] = (uint32_t)(kErS[i] * sr_);
        for (int t = 0; t < 6; ++t) {
            const uint32_t off = (uint32_t)(kErTap[i][t] * sr_);
            er_tap[i][t] = off < er_d[i] ? er_d[i] - off : 1;
        }
    }

    for (size_t k = 0; k < n; ++k) {
        const float l = in_l[k] * kInGain;
        const float r = in_r[k] * kInGain;
        const float bl = bw_[0].lp(l);
        const float br = bw_[1].lp(r);

        /* Early reflections: one write per side, six extra taps each, plus
         * the direct arrival. The cross-weighted feeds are the original's. */
        float er_out[2];
        const float er_in[2] = {bl * 0.5f + br * 0.3f, bl * 0.3f + br * 0.5f};
        const float er_dir[2] = {(bl * 0.4f + br * 0.2f) * 0.5f,
                                 (bl * 0.2f + br * 0.4f) * 0.5f};
        for (int i = 0; i < 2; ++i) {
            /* Every read before the write, or the taps come back one sample
             * early: line_read() is relative to the write head, and pushing
             * first moves it. The original gets this for free by advancing
             * all eight of its indices together after reading. */
            float s = line_read(er_[i], er_d[i]);
            for (int t = 0; t < 6; ++t)
                s += line_read(er_[i], er_tap[i][t]) * kErGain[t];
            line_push(er_[i], er_in[i]);
            er_out[i] = s + er_dir[i];
        }

        /* Four smearing allpasses on the mono sum. */
        float smear = (bl + br) * 0.5f;
        for (int i = 0; i < 4; ++i) {
            const float buf = line_read(ap_[i], ap_d_[i]);
            const float tmp = smear * -kApFb[i];
            const float o = buf + tmp;
            line_push(ap_[i], smear + (buf + tmp) * kApFb[i]);
            smear = o;
        }

        /* The figure of eight. Loop 0 is fed the previous block's loop-1
         * output and vice versa — that cross is the whole structure. */
        float tank[2];
        for (int loop = 0; loop < 2; ++loop) {
            const int a0 = loop * 2, a1 = loop * 2 + 1;
            float v = smear + (loop == 0 ? prev_r_ : prev_l_);

            /* allpassFourTap 0 and 2 always take Density1, 1 and 3 always
             * take Density2 — the first of each loop's pair is the fixed
             * one, the second tracks decay. */
            const float fb0 = dens1;
            float buf = line_read(tank_ap_[a0], tap_d[a0]);
            float tmp = v * -fb0;
            float o = buf + tmp;
            line_push(tank_ap_[a0], v + (buf + tmp) * fb0);
            v = o;

            const float d0 = line_read(tank_del_[a0], del_d[a0]);
            line_push(tank_del_[a0], v);
            v = damp_[loop].lp(d0);

            const float fb1 = dens2;
            buf = line_read(tank_ap_[a1], tap_d[a1]);
            tmp = v * -fb1;
            o = buf + tmp;
            line_push(tank_ap_[a1], v + (buf + tmp) * fb1);
            v = o;

            const float d1 = line_read(tank_del_[a1], del_d[a1]);
            line_push(tank_del_[a1], v);
            tank[loop] = d1;
        }
        prev_l_ = tank[0] * decay;
        prev_r_ = tank[1] * decay;

        /* Seven signed taps a side, all 0.6 in the original. They are pulled
         * from *both* loops on each side, which is what decorrelates the two
         * outputs of a tank fed from a mono sum. */
        auto tapd = [&](int i, int t) {
            return line_read(tank_del_[i], del_tap[i][t]);
        };
        auto tapa = [&](int i, int t) {
            return line_read(tank_ap_[i], ap_tap[i][t]);
        };
        const float accl =
            0.6f * (tapd(2, 0) + tapd(2, 1) - tapa(3, 0) + tapd(3, 0) -
                    tapd(0, 0) - tapa(1, 0) - tapd(1, 0));
        const float accr =
            0.6f * (tapd(0, 1) + tapd(0, 2) - tapa(1, 1) + tapd(1, 1) -
                    tapd(2, 2) - tapa(3, 1) - tapd(3, 1));

        /* early = 1 is all early field, early = 0 is all tank — the inverse
         * of the original's EarlyMix, which reads backwards on a knob. */
        out_l[k] = ((accl * (1.0f - early)) + early * er_out[0]) * kOutGain;
        out_r[k] = ((accr * (1.0f - early)) + early * er_out[1]) * kOutGain;
    }
}

MVerb s_mverb;

}  // namespace

RevAlgorithm* mverb_instance() { return &s_mverb; }

}  // namespace fx
}  // namespace osynth
