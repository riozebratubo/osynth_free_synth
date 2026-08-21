/*
 * osynth — granular engine (Session 38): public handle + parameter IDs.
 *
 * A per-voice grain cloud. Each voice emits a stream of short windowed
 * bursts; what is inside a burst is `grn.src`:
 *
 *   synth  an oscillator burst at its own frequency — FOF / pulsar
 *          synthesis. The grain *rate* carries the pitch and the grain
 *          *content* frequency (grn.form) is a formant peak, which is the
 *          one thing none of the other four engines can do.
 *   in     a window onto the engine's capture ring, fed from the audio
 *          input — granulation of whatever is plugged in, transposed by
 *          the key against buf.root, with position, spray and reverse.
 *
 * Both sources are registered on every build. `in` renders silence where
 * there is no audio input (or where its ring could not be allocated), the
 * same contract the modular graph's LineIn node has: a saved patch stores
 * the source it was written with, and a parameter that exists on one build
 * and not the next is a preset that loads differently depending on which
 * firmware reads it.
 *
 * The IDs live in the engine-specific 0x02xx range: registered by init(),
 * removed by deinit() on engine switch. Names, ranges and defaults:
 * docs/PARAM_MAP.md.
 */
#pragma once

#include "synth_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const synth_engine_t g_engine_granular;

/* the cloud itself */
#define GRAN_PID_SRC        0x0200
#define GRAN_PID_WAVE       0x0201
#define GRAN_PID_MODE       0x0202
#define GRAN_PID_DENS       0x0203
#define GRAN_PID_SIZE       0x0204
#define GRAN_PID_FORM       0x0205
#define GRAN_PID_SHAPE      0x0206
#define GRAN_PID_JIT        0x0207
#define GRAN_PID_SCAT       0x0208
#define GRAN_PID_SPREAD     0x0209
#define GRAN_PID_PW         0x020A
/* the capture ring (src = in). Named apart from the cloud controls so the
 * app draws them as their own card: with src = synth the whole group is
 * inert, and a panel is the honest place to say so. */
#define GRAN_PID_BUF_POS    0x020B
#define GRAN_PID_BUF_SPRAY  0x020C
#define GRAN_PID_BUF_REV    0x020D
#define GRAN_PID_BUF_FREEZE 0x020E
#define GRAN_PID_BUF_ROOT   0x020F
#define GRAN_PID_BUF_GAIN   0x0210
/* env2 -> formant, in octaves. The lfo2 depth is lfo2.form below; position
 * is reached through the S9 mod matrix instead — the formant is what this
 * engine is for, and it gets the dedicated pair the wavetable engine gives
 * to its table position. */
#define GRAN_PID_ENV_FORM   0x0211
/* filter (the S33 family) — env2 doubles as the filter envelope via flt.env */
#define GRAN_PID_FLT_ON     0x0212
#define GRAN_PID_FLT_TYPE   0x0213
#define GRAN_PID_FLT_MODE   0x0214
#define GRAN_PID_FLT_CUTOFF 0x0215
#define GRAN_PID_FLT_RESO   0x0216
#define GRAN_PID_FLT_ENV    0x0217
#define GRAN_PID_FLT_KBD    0x0218
#define GRAN_PID_FLT_DRIVE  0x0219
#define GRAN_PID_FLT_SPREAD 0x021A
#define GRAN_PID_FLT_VOWEL  0x021B
/* envelopes: env1 = amplitude, env2 = mod (formant + filter) */
#define GRAN_PID_ENV1_ATTACK  0x021C
#define GRAN_PID_ENV1_DECAY   0x021D
#define GRAN_PID_ENV1_SUSTAIN 0x021E
#define GRAN_PID_ENV1_RELEASE 0x021F
#define GRAN_PID_ENV2_ATTACK  0x0220
#define GRAN_PID_ENV2_DECAY   0x0221
#define GRAN_PID_ENV2_SUSTAIN 0x0222
#define GRAN_PID_ENV2_RELEASE 0x0223
/* LFOs — default routing lfo1 -> pitch, lfo2 -> formant; the S9 mod matrix
 * can retarget the depths — docs/PARAM_MAP.md */
#define GRAN_PID_LFO1_RATE  0x0224
#define GRAN_PID_LFO1_WAVE  0x0225
#define GRAN_PID_LFO1_PITCH 0x0226
#define GRAN_PID_LFO2_RATE  0x0227
#define GRAN_PID_LFO2_WAVE  0x0228
#define GRAN_PID_LFO2_FORM  0x0229

#ifdef __cplusplus
}
#endif
