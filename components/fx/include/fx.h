/*
 * osynth — master FX bus: chorus -> delay -> granular delay -> reverb
 * -> bitcrush -> filter (Sessions 10 + 11; bitcrush S17; filter S33).
 *
 * The bus is stereo and global (not per voice) and runs on the audio task:
 * main.cpp chains fx_process() after voice_manager_render() in the render
 * callback, before audio_io applies master volume. Parameters live in
 * 0x03xx (docs/PARAM_MAP.md) and are registered once at boot by fx_init(),
 * which must run before audio_io_start(): since S9 the audio task reads the
 * registry every block (mod-matrix plan), so a later add() would race.
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FX parameter IDs (0x03xx) — C defines like the engine headers; names,
 * ranges and defaults in docs/PARAM_MAP.md. */
#define FX_PID_CHO_MIX   0x0300
#define FX_PID_CHO_RATE  0x0301
#define FX_PID_CHO_DEPTH 0x0302
#define FX_PID_DLY_MIX   0x0310
#define FX_PID_DLY_TIME  0x0311
#define FX_PID_DLY_FB    0x0312
#define FX_PID_DLY_TONE  0x0313
#define FX_PID_DLY_PP    0x0314
#define FX_PID_REV_MIX   0x0320
#define FX_PID_REV_SIZE  0x0321
#define FX_PID_REV_DAMP  0x0322
#define FX_PID_GRN_MIX   0x0330
#define FX_PID_GRN_SIZE  0x0331
#define FX_PID_GRN_DENS  0x0332
#define FX_PID_GRN_PITCH 0x0333
#define FX_PID_GRN_FB    0x0334
#define FX_PID_GRN_SPRAY 0x0335
#define FX_PID_CRUSH_MIX  0x0340
#define FX_PID_CRUSH_BITS 0x0341
#define FX_PID_CRUSH_DOWN 0x0342
/* master filter (S33) — the same filter family the voices use, stereo and
 * once per block instead of once per voice, so it costs about a quarter of
 * one voice's filter. fx.flt.on is the dry/wet gate: like every other unit
 * here, off means the stage returns immediately. */
#define FX_PID_FLT_ON     0x0350
#define FX_PID_FLT_TYPE   0x0351
#define FX_PID_FLT_MODE   0x0352
#define FX_PID_FLT_CUTOFF 0x0353
#define FX_PID_FLT_RESO   0x0354
#define FX_PID_FLT_DRIVE  0x0355
#define FX_PID_FLT_SPREAD 0x0356
#define FX_PID_FLT_VOWEL  0x0357

/* Registers the 0x03xx params and allocates the delay lines (PSRAM first,
 * internal RAM fallback; an effect whose lines cannot be allocated is
 * disabled with a warning). Call before audio_io_start(). */
esp_err_t fx_init(void);

/* Processes one block in place: chorus -> delay -> granular -> reverb ->
 * bitcrush, each behind its own dry/wet mix (a fully-dry effect costs
 * ~nothing). Audio task only — no locks, no allocation. */
void fx_process(float* l, float* r, size_t frames);

#ifdef __cplusplus
}
#endif
