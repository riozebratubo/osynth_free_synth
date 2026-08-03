/*
 * osynth — FM engine (Session 6): public handle + parameter IDs.
 *
 * 2-op x 2 phase-modulation FM: two parallel modulator -> carrier pairs per
 * voice (A = body, B = tine/attack by default). The IDs live in the
 * engine-specific 0x02xx range: registered by init(), removed by deinit()
 * on engine switch. Names, ranges and defaults: docs/PARAM_MAP.md.
 */
#pragma once

#include "synth_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const synth_engine_t g_engine_fm;

/* pair A (body): modulator -> carrier */
#define FM_PID_A_CRATIO  0x0200
#define FM_PID_A_MRATIO  0x0201
#define FM_PID_A_INDEX   0x0202
#define FM_PID_A_FB      0x0203
#define FM_PID_A_LEVEL   0x0204
#define FM_PID_A_ENV_A   0x0205 /* carrier amp envelope */
#define FM_PID_A_ENV_D   0x0206
#define FM_PID_A_ENV_S   0x0207
#define FM_PID_A_ENV_R   0x0208
#define FM_PID_A_MENV_A  0x0209 /* modulation-index envelope */
#define FM_PID_A_MENV_D  0x020A
#define FM_PID_A_MENV_S  0x020B
#define FM_PID_A_MENV_R  0x020C
/* pair B (tine/attack) */
#define FM_PID_B_CRATIO  0x020D
#define FM_PID_B_MRATIO  0x020E
#define FM_PID_B_INDEX   0x020F
#define FM_PID_B_FB      0x0210
#define FM_PID_B_LEVEL   0x0211
#define FM_PID_B_ENV_A   0x0212
#define FM_PID_B_ENV_D   0x0213
#define FM_PID_B_ENV_S   0x0214
#define FM_PID_B_ENV_R   0x0215
#define FM_PID_B_MENV_A  0x0216
#define FM_PID_B_MENV_D  0x0217
#define FM_PID_B_MENV_S  0x0218
#define FM_PID_B_MENV_R  0x0219
#define FM_PID_B_DETUNE  0x021A
/* global */
#define FM_PID_VEL_INDEX 0x021B
#define FM_PID_LFO_RATE  0x021C
#define FM_PID_LFO_WAVE  0x021D
#define FM_PID_LFO_PITCH 0x021E
/* filter (S33) — this engine had none until the shared filter family made
 * one cheap enough to be worth the caps bit. There is no flt.env: FM has no
 * shared mod envelope to drive it (the per-pair menvs are the index
 * envelopes and belong to their operators), so cutoff modulation comes from
 * the mod matrix, lfo1 and velocity. Defaults off — an existing FM preset
 * sounds exactly as it did. */
#define FM_PID_FLT_ON     0x021F
#define FM_PID_FLT_TYPE   0x0220
#define FM_PID_FLT_MODE   0x0221
#define FM_PID_FLT_CUTOFF 0x0222
#define FM_PID_FLT_RESO   0x0223
#define FM_PID_FLT_KBD    0x0224
#define FM_PID_FLT_DRIVE  0x0225
#define FM_PID_FLT_SPREAD 0x0226
#define FM_PID_FLT_VOWEL  0x0227

#ifdef __cplusplus
}
#endif
