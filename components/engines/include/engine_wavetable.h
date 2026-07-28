/*
 * osynth — wavetable engine (Session 7): public handle + parameter IDs.
 *
 * 2 wavetable oscillators scanning the factory table sets (generated at
 * build time by tools/gen_wavetables.py), position morph modulated by env2
 * and lfo2, SVF filter + amp ADSR from the shared blocks. The IDs live in
 * the engine-specific 0x02xx range: registered by init(), removed by
 * deinit() on engine switch. Names, ranges and defaults: docs/PARAM_MAP.md.
 */
#pragma once

#include "synth_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const synth_engine_t g_engine_wavetable;

/* oscillators + mixer */
#define WT_PID_OSC1_TABLE   0x0200
#define WT_PID_OSC1_POS     0x0201
#define WT_PID_OSC2_TABLE   0x0202
#define WT_PID_OSC2_POS     0x0203
#define WT_PID_OSC2_SEMI    0x0204
#define WT_PID_OSC2_FINE    0x0205
#define WT_PID_MIX_OSC1     0x0206
#define WT_PID_MIX_OSC2     0x0207
/* env2 -> position amount (both oscs share the modulation offset) */
#define WT_PID_ENV_POS      0x0208
/* filter (env2 can also sweep cutoff via flt.env) */
#define WT_PID_FLT_MODE     0x0209
#define WT_PID_FLT_CUTOFF   0x020A
#define WT_PID_FLT_RESO     0x020B
#define WT_PID_FLT_ENV      0x020C
#define WT_PID_FLT_KBD      0x020D
/* envelopes: env1 = amplitude, env2 = mod (position + filter) */
#define WT_PID_ENV1_ATTACK  0x020E
#define WT_PID_ENV1_DECAY   0x020F
#define WT_PID_ENV1_SUSTAIN 0x0210
#define WT_PID_ENV1_RELEASE 0x0211
#define WT_PID_ENV2_ATTACK  0x0212
#define WT_PID_ENV2_DECAY   0x0213
#define WT_PID_ENV2_SUSTAIN 0x0214
#define WT_PID_ENV2_RELEASE 0x0215
/* LFOs — default routing lfo1 -> pitch, lfo2 -> position; the S9 mod matrix
 * can retarget the depths — docs/PARAM_MAP.md */
#define WT_PID_LFO1_RATE    0x0216
#define WT_PID_LFO1_WAVE    0x0217
#define WT_PID_LFO1_PITCH   0x0218
#define WT_PID_LFO2_RATE    0x0219
#define WT_PID_LFO2_WAVE    0x021A
#define WT_PID_LFO2_POS     0x021B

#ifdef __cplusplus
}
#endif
