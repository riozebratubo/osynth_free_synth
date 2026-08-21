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
    int32_t s = (int32_t)(v * 32767.0f);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    l.buf[l.w] = (int16_t)s;
    if (++l.w == l.len) l.w = 0;
}

/* Oldest sample — a delay of exactly len; read before line_push overwrites. */
inline float line_tap(const Line& l) {
    return (float)l.buf[l.w] * (1.0f / 32768.0f);
}

/* x[n - d] with fractional d in [1, len-3], linear interpolation; call
 * before pushing sample n. */
inline float line_read_frac(const Line& l, float d) {
    const uint32_t di = (uint32_t)d;
    const float frac = d - (float)di;
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
    uint32_t i = l.w + l.len - d;
    if (i >= l.len) i -= l.len;
    return (float)l.buf[i] * (1.0f / 32768.0f);
}

}  // namespace dsp
}  // namespace osynth
