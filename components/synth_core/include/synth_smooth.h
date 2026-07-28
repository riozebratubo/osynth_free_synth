/*
 * osynth — block-rate parameter smoothing (Session 21).
 *
 * Why this exists: the engines and the FX bus read ParamStore once per block
 * and hold each value constant for the whole block (synth_dsp.h's "blocks
 * hold state, never parameters" rule). That is the right shape for the
 * render loop, but it means a control change lands as a *step*: BLE slider
 * drags arrive at the connection interval (tens of ms apart, each a large
 * jump), MIDI CCs at 1/128 of the range, preset loads as one hard jump of
 * everything at once. On an amplitude-domain parameter — a mix level, an FM
 * operator level, a drawbar — that step is a discontinuity in the signal,
 * i.e. a click; on a timbre parameter — cutoff, modulation index, wavetable
 * position — it is the classic zipper.
 *
 * A Smooth is a one-pole filter clocked once per audio block. The engine
 * keeps one next to its block cache, feeds it the raw parameter in
 * begin_block(), and renders with what it returns. Per-sample interpolation
 * is deliberately *not* used: at 750 blocks/s a one-pole at kParamSlew
 * moves at most ~8% of the remaining distance per block, so the residual
 * staircase is far below audibility, and the per-sample render cost stays
 * exactly zero. This is the same scheme (and the same coefficient) as the
 * FX bus mix gate, which has been click-free since S10.
 *
 * Two flavours:
 *  - smooth_lin(): linear domain — levels, mix, resonance, mod depths.
 *  - smooth_exp(): geometric domain — anything registered ParamCurve::Exp
 *    (cutoff, delay tone, times). Smoothing 20 Hz -> 18 kHz linearly would
 *    spend nearly the whole ramp inside the top octave; in the log domain
 *    the sweep is perceptually even. Requires target > 0, which every
 *    Exp-curve param satisfies by construction (positive min).
 *
 * Both snap on the first call — a smoother has no value until it sees one,
 * so boot, engine binds and preset loads at startup begin *at* the
 * parameter rather than sliding up from zero — and snap again once the
 * remaining distance falls under an epsilon, so a control parked at exactly
 * 0, 1 or 18 kHz really lands there. When a parameter is not moving (the
 * overwhelmingly common case) the whole call is one float compare.
 *
 * Note for callers: this only works if begin_block() actually runs every
 * block. The voice manager calls it unconditionally for that reason — see
 * the comment in voice_manager_render().
 */
#pragma once

#include <cmath>

#include "synth_config.h"

namespace osynth::dsp {

/* One-pole coefficient per audio block (~1.33 ms at 48 kHz / 64 frames):
 * ~16 ms time constant, a full swing in ~90 ms. Fast enough that a filter
 * sweep still feels attached to the finger, slow enough that a full-range
 * jump never steps audibly. */
inline constexpr float kParamSlew = 0.08f;

/* Below this the smoother snaps: 1e-6 of a unit-range param is ~120 dB down,
 * and it guarantees exact arrival (a one-pole never reaches its target). */
inline constexpr float kSmoothEps = 1e-6f;

/* Ratio band for the geometric flavour: within ±0.01 cent, snap. */
inline constexpr float kSmoothRatioLo = 0.999994f;
inline constexpr float kSmoothRatioHi = 1.000006f;

struct Smooth {
    float cur = 0.0f;
    bool primed = false;
};

inline void smooth_reset(Smooth& s) {
    s.cur = 0.0f;
    s.primed = false;
}

/* Linear one-pole toward `target`. */
inline float smooth_lin(Smooth& s, float target, float k = kParamSlew) {
    if (SYNTH_UNLIKELY(!s.primed)) {
        s.primed = true;
        s.cur = target;
        return target;
    }
    if (SYNTH_LIKELY(s.cur == target)) return target; /* settled: one compare */
    const float d = target - s.cur;
    if (fabsf(d) <= kSmoothEps) {
        s.cur = target;
        return target;
    }
    s.cur += k * d;
    return s.cur;
}

/* Geometric one-pole: equal ratio per block rather than equal difference.
 * `target` and the running value must both be > 0. Costs a powf only while
 * the parameter is actually moving. */
inline float smooth_exp(Smooth& s, float target, float k = kParamSlew) {
    if (SYNTH_UNLIKELY(!s.primed)) {
        s.primed = true;
        s.cur = target;
        return target;
    }
    if (SYNTH_LIKELY(s.cur == target)) return target;
    const float r = target / s.cur;
    if (r > kSmoothRatioLo && r < kSmoothRatioHi) {
        s.cur = target;
        return target;
    }
    s.cur *= powf(r, k);
    return s.cur;
}

} // namespace osynth::dsp
