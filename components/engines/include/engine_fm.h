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

#ifdef __cplusplus
}
#endif
