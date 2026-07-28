/*
 * osynth — additive engine (Session 8): public handle + parameter IDs.
 *
 * 16 sine partials per voice with drawbar levels, spectral tilt, even/odd
 * balance and inharmonicity; brightness (base + env2 + lfo2 − velocity)
 * drives an exponential per-partial rolloff — a filter-sweep feel with no
 * filter. Partials above Nyquist or below −60 dB are culled per voice-block.
 * The IDs live in the engine-specific 0x02xx range: registered by init(),
 * removed by deinit() on engine switch. Names, ranges and defaults:
 * docs/PARAM_MAP.md.
 */
#pragma once

#include "synth_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const synth_engine_t g_engine_additive;

#define ADD_PARTIALS 16

/* drawbar levels: add.p1.level .. add.p16.level, one contiguous block */
#define ADD_PID_P1_LEVEL     0x0200
#define ADD_PID_P16_LEVEL    0x020F
/* spectral shaping */
#define ADD_PID_TILT         0x0210
#define ADD_PID_EVENODD      0x0211
#define ADD_PID_INHARM       0x0212
/* brightness: base + env2 / lfo2 / velocity modulation */
#define ADD_PID_BRIGHT       0x0213
#define ADD_PID_ENV_BRIGHT   0x0214
#define ADD_PID_VEL_BRIGHT   0x0215
/* envelopes: env1 = amplitude (per sample), env2 = brightness (block rate) */
#define ADD_PID_ENV1_ATTACK  0x0216
#define ADD_PID_ENV1_DECAY   0x0217
#define ADD_PID_ENV1_SUSTAIN 0x0218
#define ADD_PID_ENV1_RELEASE 0x0219
#define ADD_PID_ENV2_ATTACK  0x021A
#define ADD_PID_ENV2_DECAY   0x021B
#define ADD_PID_ENV2_SUSTAIN 0x021C
#define ADD_PID_ENV2_RELEASE 0x021D
/* LFOs — default routing lfo1 -> pitch, lfo2 -> brightness; the S9 mod
 * matrix can retarget the depths — docs/PARAM_MAP.md */
#define ADD_PID_LFO1_RATE    0x021E
#define ADD_PID_LFO1_WAVE    0x021F
#define ADD_PID_LFO1_PITCH   0x0220
#define ADD_PID_LFO2_RATE    0x0221
#define ADD_PID_LFO2_WAVE    0x0222
#define ADD_PID_LFO2_BRIGHT  0x0223

#ifdef __cplusplus
}
#endif
