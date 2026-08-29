/*
 * osynth — int16 circular delay line, shared by every unit that needs one.
 *
 * Lifted out of fx.cpp in S36, unchanged, because the reverb algorithms added
 * there live in two different components: the MIT ones in `fx`, the GPL-3
 * ones in `fx_gpl`. Both need this primitive and neither may include the
 * other's private sources, so it sits here in synth_core where both already
 * depend on it. Nothing about the representation changed in the move — a
 * preset saved before S36 renders sample-for-sample as it did.
 *
 * Why int16 and not float: the whole output chain is 16-bit, so the wet paths
 * lose nothing audible, and the footprint (plus PSRAM cache pressure on the
 * S3) halves. The cost is that a make-up gain applied downstream lifts a
 * line's quantization floor along with its signal, which is why the reverb
 * buys its headroom in its input staging instead.
 */
#pragma once

#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

namespace osynth {
namespace dsp {

/* ---- the float -> int16 boundary, in one place ----
 *
 * Every buffer in this instrument that stores audio stores it as int16, and
 * until S46 each of them converted with its own two lines.
 *
 * The clamp happens on the int32, not in the float domain, and that is a
 * budget decision that was measured rather than assumed.
 *
 * Clamping first — `if (y > -32767.0f && y < 32767.0f)` — is the form that is
 * defined by the C standard for every input. It also needs two float
 * constants, and RISC-V has no float immediates: each one costs a `lui`+`flw`
 * pair. GCC hoists them per loop rather than per call, but this inlines into
 * every line_push() on the FX bus, which is a dozen-odd distinct sample loops,
 * and the measured cost landed within noise of the exact shortfall that was
 * overflowing sram_low (tools/iram_budget.py, and the disassembly). The
 * integer clamp below needs immediates, which are free.
 *
 * So what is guaranteed here is target behaviour rather than C semantics:
 * both back ends lower this to a saturating convert — RISC-V `fcvt.w.s` and
 * Xtensa `trunc.s` return INT32_MAX/INT32_MIN for out-of-range and NaN
 * without trapping — so the clamp below always sees a sane int32 and a NaN
 * arrives as full-scale positive rather than as a wild value.
 *
 * NaN is NOT fenced here, and S46b is where that stopped being an oversight
 * and became a measurement. The fence is one instruction — `v != v` compares
 * a register against itself and loads no constant — but this function inlines
 * into 29 sites across libfx, libengines and liblooper, and adding it there
 * overflowed sram_low by 794 bytes at link. So it lives at the *sink*
 * instead, in to_i16_dith() (audio_io.cpp), which is one site inside one loop
 * and where the consequence is not a click but a permanent one: soft_clip()
 * passes a NaN through unchanged, so a NaN settled in a recursive filter's
 * state would otherwise leave the box as full-scale DC, every sample, for as
 * long as the patch is loaded.
 *
 * What is given up by not fencing here is bounded and much smaller: a NaN
 * written into a delay line lands as a single +full-scale sample, which that
 * unit replays once per circulation until its tail decays. Audible, not
 * catastrophic, and it can only happen downstream of a bug that is worth
 * finding at its source anyway.
 *
 * This is the first thing to restore if sram_low ever has 800 bytes spare —
 * `if (v != v) return 0;` as the first line, and the corresponding paragraph
 * above soft_clip() in synth_dsp.h.
 *
 * The rails are symmetric at +-32767 rather than reaching INT16_MIN: two of
 * the three call sites this replaced already clamped that way, and it costs
 * one LSB on a sample that was over full scale to begin with. */
inline int16_t f2i16(float v) {
    int32_t s = (int32_t)(v * 32767.0f);
    if (s > 32767) s = 32767;
    if (s < -32767) s = -32767;
    return (int16_t)s;
}

struct Line {
    int16_t* buf = nullptr;
    uint32_t len = 0;
    uint32_t w = 0; /* next write index */
};

/* Running totals of everything line_alloc() has handed out, split by where it
 * landed. Diagnostic only — fx_init() logs them at boot, which is how a board
 * whose PSRAM did not come up announces itself as "buffers 0 KB PSRAM" rather
 * than as a mysterious shortage later. */
inline size_t g_line_bytes_spiram = 0;
inline size_t g_line_bytes_internal = 0;

/* PSRAM first, internal RAM as the fallback, calloc so a line starts silent.
 * Returns false if neither pool could satisfy it; every caller treats that as
 * "this effect is disabled" rather than as a fatal error. */
inline bool line_alloc(Line& l, uint32_t len) {
    int16_t* p = nullptr;
#if CONFIG_SPIRAM
    p = (int16_t*)heap_caps_calloc(len, sizeof(int16_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (p == nullptr) {
        p = (int16_t*)heap_caps_calloc(len, sizeof(int16_t),
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p == nullptr) return false;
    }
    (esp_ptr_external_ram(p) ? g_line_bytes_spiram : g_line_bytes_internal) +=
        (size_t)len * sizeof(int16_t);
    l.buf = p;
    l.len = len;
    l.w = 0;
    return true;
}

inline void line_push(Line& l, float v) {
    l.buf[l.w] = f2i16(v);
    if (++l.w == l.len) l.w = 0;
}

/* Oldest sample — a delay of exactly len; read before line_push overwrites. */
inline float line_tap(const Line& l) {
    return (float)l.buf[l.w] * (1.0f / 32768.0f);
}

/* x[n - d] with fractional d in [1, len-3], linear interpolation; call
 * before pushing sample n. */
inline float line_read_frac(const Line& l, float d) {
    uint32_t di = (uint32_t)d;
    float frac = d - (float)di;
    /* The wrap below corrects by exactly one length, so it only recovers a
     * `di` that is already inside the line: past `l.w + l.len` the unsigned
     * subtraction underflows and one correction leaves an index far outside
     * the buffer. Every caller in the tree bounds `d` correctly today, but
     * this primitive is shared by fx, fx_gpl and graph and the invariant
     * lived only in the comment above — one compare is cheaper than that
     * staying true by inspection.
     *
     * `frac` is reset with it (S46b), because the two have to describe the
     * same sample: clamping the index alone leaves `frac` measuring the delay
     * that was *asked* for, which past the rail is not a fraction at all and
     * turns the interpolation below into an extrapolation. Free — it is a
     * store inside a branch that is never taken in a correct build.
     *
     * The other direction is NOT guarded, and that is a budget decision
     * rather than an oversight. A negative `d` converts to `di` = 0 on both
     * back ends, so the rail below never fires and `frac` is left at `d` —
     * the same extrapolation, reached from underneath. `if (!(frac >= 0.0f))
     * frac = 0.0f;` closes it for about eight bytes a site, but this inlines
     * into every modulated tap on the bus and those bytes were part of a
     * 227-byte sram_low overflow at link (S46b; same budget as the NaN fence
     * over f2i16 above).
     *
     * What holds it up meanwhile is a caller contract, checked once: every
     * caller in the tree floors `d` at 1 or more before calling — the chorus
     * at its 12 ms base, the flanger and the reverb pre-delay explicitly, the
     * delay at 2, both granulars at 3. A new caller that does not is the way
     * this comes back, so floor it there. */
    if (di >= l.len) {
        di = l.len - 1;
        frac = 0.0f;
    }
    uint32_t i0 = l.w + l.len - di;
    if (i0 >= l.len) i0 -= l.len;
    const uint32_t i1 = (i0 == 0) ? l.len - 1 : i0 - 1;
    const float a = (float)l.buf[i0];
    return (a + frac * ((float)l.buf[i1] - a)) * (1.0f / 32768.0f);
}

/* x[n - d] at an integer delay, no interpolation — for the fixed taps a
 * Schroeder bank and an early-reflection field are built from, where the
 * delay never moves and the interpolator would only cost cycles. */
inline float line_read(const Line& l, uint32_t d) {
    /* Same one-length wrap and therefore the same bound as line_read_frac():
     * the taps here are scaled by a `size` parameter at block rate rather
     * than being compile-time constants, so "the read never exceeds the
     * allocation" is a property of five separate reverb topologies rather
     * than of this function. */
    if (d >= l.len) d = l.len - 1;
    uint32_t i = l.w + l.len - d;
    if (i >= l.len) i -= l.len;
    return (float)l.buf[i] * (1.0f / 32768.0f);
}

}  // namespace dsp
}  // namespace osynth
