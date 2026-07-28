/*
 * osynth — IMA ADPCM primitives shared by the looper render path and the
 * loop_store backends (S20: tracks live ADPCM-encoded in PSRAM, so the
 * codec moved out of loop_store.cpp into this header).
 *
 * 4 bits per sample per channel. Stereo packs one frame per byte (L in the
 * high nibble); mono packs two frames per byte (the earlier frame in the
 * high nibble). Access is strictly sequential from frame 0 — the looper
 * transport only ever starts or wraps at the loop start — so a zeroed Ch
 * per channel is the entire seek story: no block-state tables anywhere.
 *
 * The step tables are DRAM_ATTR: the decoders run inside the IRAM render
 * path (looper_process), which must not fault on a flash-cache miss. The
 * tables are `static`, so each including TU carries its own ~200 B copy —
 * two TUs today, a non-cost.
 */
#pragma once

#include <stdint.h>

#include "esp_attr.h"

namespace osynth::adpcm {

struct Ch {
    int32_t pred = 0;
    int32_t index = 0;
};

namespace detail {

static DRAM_ATTR const int16_t kStep[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static DRAM_ATTR const int8_t kIndexAdj[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                                               -1, -1, -1, -1, 2, 4, 6, 8};

} // namespace detail

/* The encoder reconstructs exactly like the decoder so both predictors
 * stay in lock-step (the standard IMA scheme). */
inline uint8_t encode(Ch& c, int16_t sample) {
    const int32_t step = detail::kStep[c.index];
    int32_t diff = sample - c.pred;
    uint8_t nib = 0;
    if (diff < 0) {
        nib = 8;
        diff = -diff;
    }
    if (diff >= step) {
        nib |= 4;
        diff -= step;
    }
    if (diff >= (step >> 1)) {
        nib |= 2;
        diff -= step >> 1;
    }
    if (diff >= (step >> 2)) nib |= 1;
    int32_t delta = step >> 3;
    if (nib & 4) delta += step;
    if (nib & 2) delta += step >> 1;
    if (nib & 1) delta += step >> 2;
    c.pred += (nib & 8) ? -delta : delta;
    if (c.pred > 32767) c.pred = 32767;
    if (c.pred < -32768) c.pred = -32768;
    c.index += detail::kIndexAdj[nib];
    if (c.index < 0) c.index = 0;
    if (c.index > 88) c.index = 88;
    return nib;
}

inline int16_t decode(Ch& c, uint8_t nib) {
    const int32_t step = detail::kStep[c.index];
    int32_t delta = step >> 3;
    if (nib & 4) delta += step;
    if (nib & 2) delta += step >> 1;
    if (nib & 1) delta += step >> 2;
    c.pred += (nib & 8) ? -delta : delta;
    if (c.pred > 32767) c.pred = 32767;
    if (c.pred < -32768) c.pred = -32768;
    c.index += detail::kIndexAdj[nib];
    if (c.index < 0) c.index = 0;
    if (c.index > 88) c.index = 88;
    return (int16_t)c.pred;
}

} // namespace osynth::adpcm
