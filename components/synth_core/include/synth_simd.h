/*
 * osynth — 4-wide render kernels via GCC vector extensions (S17).
 *
 * The types below are plain GCC generic vectors (`vector_size`), the same
 * device ESP32Synth uses for its S3 paths. Honest expectations: today's
 * Xtensa GCC has no auto-codegen for the S3's PIE 128-bit SIMD, so these
 * lower to well-scheduled scalar code — the measurable win is the
 * guaranteed 4× unroll, hoisted loads and `__restrict__` aliasing freedom.
 * If a toolchain that vectorizes generic vectors for PIE lands, these
 * kernels upgrade for free without touching the call sites.
 *
 * Element access (v[0]..v[3]) is used for memory traffic on purpose: it
 * needs no alignment guarantees (the callers hand in arbitrary offsets into
 * float buses), and a PIE-capable compiler still vectorizes the arithmetic
 * between the accesses.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace osynth::dsp {

typedef float v4f32 __attribute__((vector_size(16)));
typedef int32_t v4i32 __attribute__((vector_size(16)));

/* Mix interleaved int16 L/R frames into the float buses:
 * l[i] += p[2i] * g;  r[i] += p[2i+1] * g.  Any n, any offsets. */
inline void simd_mix_i16lr_f32(const int16_t* __restrict__ p, float g,
                               float* __restrict__ l, float* __restrict__ r,
                               size_t n) {
    size_t i = 0;
    const v4f32 gv = {g, g, g, g};
    for (; i + 4 <= n; i += 4, p += 8) {
        const v4i32 sl = {p[0], p[2], p[4], p[6]};
        const v4i32 sr = {p[1], p[3], p[5], p[7]};
        const v4f32 fl = __builtin_convertvector(sl, v4f32) * gv;
        const v4f32 fr = __builtin_convertvector(sr, v4f32) * gv;
        l[i + 0] += fl[0]; l[i + 1] += fl[1];
        l[i + 2] += fl[2]; l[i + 3] += fl[3];
        r[i + 0] += fr[0]; r[i + 1] += fr[1];
        r[i + 2] += fr[2]; r[i + 3] += fr[3];
    }
    for (; i < n; ++i) {
        l[i] += (float)*p++ * g;
        r[i] += (float)*p++ * g;
    }
}

/* Mix mono int16 frames into both float buses (S19 mono looper tracks):
 * l[i] += p[i] * g;  r[i] += p[i] * g.  Any n, any offsets. */
inline void simd_mix_i16m_f32(const int16_t* __restrict__ p, float g,
                              float* __restrict__ l, float* __restrict__ r,
                              size_t n) {
    size_t i = 0;
    const v4f32 gv = {g, g, g, g};
    for (; i + 4 <= n; i += 4, p += 4) {
        const v4i32 s = {p[0], p[1], p[2], p[3]};
        const v4f32 f = __builtin_convertvector(s, v4f32) * gv;
        l[i + 0] += f[0]; l[i + 1] += f[1];
        l[i + 2] += f[2]; l[i + 3] += f[3];
        r[i + 0] += f[0]; r[i + 1] += f[1];
        r[i + 2] += f[2]; r[i + 3] += f[3];
    }
    for (; i < n; ++i) {
        const float f = (float)*p++ * g;
        l[i] += f;
        r[i] += f;
    }
}

} // namespace osynth::dsp
