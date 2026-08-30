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

#ifndef _MSC_VER

typedef float v4f32 __attribute__((vector_size(16)));
typedef int32_t v4i32 __attribute__((vector_size(16)));

#else /* _MSC_VER */

/* MSVC has no generic vector extension and no __builtin_convertvector, and it
 * is the one toolchain in the set that does not (GCC builds all three ESP
 * targets; Clang builds the Apple and Android hosts). Rather than fork the two
 * kernels below, this supplies types with the same spelling and the same three
 * operations they use -- aggregate init, element read, and elementwise
 * multiply -- so both function bodies compile unchanged.
 *
 * These stay aggregates: no user-declared constructor, so `const v4f32 gv =
 * {g, g, g, g};` is still valid aggregate initialisation with brace elision,
 * exactly as it is for a GCC vector.
 *
 * The result is scalar code, and that is not a loss worth avoiding. The header
 * comment above already says these lower to scalar on Xtensa: the win being
 * bought is the guaranteed 4x unroll, the hoisted loads and the __restrict__
 * aliasing freedom, and all three survive here. MSVC autovectorises the
 * unrolled bodies on x64 in release builds anyway. */
struct v4f32 {
    float e[4];
    float operator[](size_t i) const { return e[i]; }
    float& operator[](size_t i) { return e[i]; }
};

struct v4i32 {
    int32_t e[4];
    int32_t operator[](size_t i) const { return e[i]; }
    int32_t& operator[](size_t i) { return e[i]; }
};

inline v4f32 operator*(const v4f32& a, const v4f32& b) {
    return v4f32{{a.e[0] * b.e[0], a.e[1] * b.e[1], a.e[2] * b.e[2],
                  a.e[3] * b.e[3]}};
}

inline v4f32 simd_convert_i32_f32(const v4i32& s) {
    return v4f32{{(float)s.e[0], (float)s.e[1], (float)s.e[2], (float)s.e[3]}};
}

/* Intercepts the call sites unchanged. The second argument is the destination
 * type and is only ever v4f32 in this file, so the macro ignores it rather
 * than pretending to a generality it does not have -- a second conversion
 * would need a second helper, and a static_assert would be the place to say
 * so if one ever appears. */
#define __builtin_convertvector(vec, type)     (::osynth::dsp::simd_convert_i32_f32(vec))

#endif /* _MSC_VER */

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
