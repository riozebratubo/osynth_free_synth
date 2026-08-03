/*
 * osynth — subtractive engine (Session 5): public handle + parameter IDs.
 *
 * The IDs live in the engine-specific 0x02xx range: they are registered by
 * the engine's init() and exist only while this engine is active (the S6
 * switch machinery re-registers the range per engine). Names, ranges and
 * defaults: docs/PARAM_MAP.md.
 */
#pragma once

#include "synth_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const synth_engine_t g_engine_subtractive;

/* oscillators + mixer */
#define SUB_PID_OSC1_WAVE    0x0200
#define SUB_PID_OSC1_PW      0x0201
#define SUB_PID_OSC2_WAVE    0x0202
#define SUB_PID_OSC2_PW      0x0203
#define SUB_PID_OSC2_SEMI    0x0204
#define SUB_PID_OSC2_FINE    0x0205
#define SUB_PID_MIX_OSC1     0x0206
#define SUB_PID_MIX_OSC2     0x0207
#define SUB_PID_MIX_NOISE    0x0208
/* filter */
#define SUB_PID_FLT_MODE     0x0209
#define SUB_PID_FLT_CUTOFF   0x020A
#define SUB_PID_FLT_RESO     0x020B
#define SUB_PID_FLT_ENV      0x020C
#define SUB_PID_FLT_KBD      0x020D
/* envelopes: env1 = amplitude, env2 = filter */
#define SUB_PID_ENV1_ATTACK  0x020E
#define SUB_PID_ENV1_DECAY   0x020F
#define SUB_PID_ENV1_SUSTAIN 0x0210
#define SUB_PID_ENV1_RELEASE 0x0211
#define SUB_PID_ENV2_ATTACK  0x0212
#define SUB_PID_ENV2_DECAY   0x0213
#define SUB_PID_ENV2_SUSTAIN 0x0214
#define SUB_PID_ENV2_RELEASE 0x0215
/* LFOs — default routing lfo1 -> pitch, lfo2 -> cutoff; the S9 mod matrix
 * can retarget the depths (and everything per-voice) — docs/PARAM_MAP.md */
#define SUB_PID_LFO1_RATE    0x0216
#define SUB_PID_LFO1_WAVE    0x0217
#define SUB_PID_LFO1_PITCH   0x0218
#define SUB_PID_LFO2_RATE    0x0219
#define SUB_PID_LFO2_WAVE    0x021A
#define SUB_PID_LFO2_CUTOFF  0x021B
/* filter, part 2 (S33). Appended here rather than next to the 0x0209 block
 * because ids are the on-wire preset format: 0x020E onwards was already
 * spoken for, and moving a parameter is the one thing that breaks a saved
 * patch. The app groups the Filter card by name prefix, not by id, so these
 * still land in it — at the end of the card, since it lists in id order. */
#define SUB_PID_FLT_ON       0x021C
#define SUB_PID_FLT_TYPE     0x021D
#define SUB_PID_FLT_DRIVE    0x021E
#define SUB_PID_FLT_SPREAD   0x021F
#define SUB_PID_FLT_VOWEL    0x0220

#ifdef __cplusplus
}
#endif
