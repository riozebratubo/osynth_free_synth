/*
 * osynth — master FX bus: drive -> chorus -> flanger -> phaser -> delay ->
 * granular delay -> reverb -> bitcrush -> filter -> EQ -> compressor ->
 * stereo/output (Sessions 10 + 11; bitcrush S17; filter S33; drive, flanger,
 * phaser, EQ, compressor, stereo and the FX LFOs S34).
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
 * ranges and defaults in docs/PARAM_MAP.md.
 *
 * Each unit owns a 16-id block and appends inside it. That is not tidiness:
 * ids are the on-wire form for presets and NRPN, so a unit that outgrows its
 * block takes the next free one rather than pushing its neighbour along. */
#define FX_PID_CHO_MIX   0x0300
#define FX_PID_CHO_RATE  0x0301
#define FX_PID_CHO_DEPTH 0x0302
#define FX_PID_DLY_MIX   0x0310
#define FX_PID_DLY_TIME  0x0311
#define FX_PID_DLY_FB    0x0312
#define FX_PID_DLY_TONE  0x0313
#define FX_PID_DLY_PP    0x0314
/* S34: note-division sync. Entry 0 is "free", which is what makes this one
 * parameter instead of a switch plus a division — and keeps fx.dly.time
 * meaningful (and modulatable) exactly when it is in charge. */
#define FX_PID_DLY_DIV   0x0315
/* Level compensation (S35) — see the block comment above fx.cpp's mix_gains().
 * Three units only, because they are the three whose wet path is *not* a
 * level-preserving copy of the dry one. Off by default everywhere: it changes
 * the sound of every patch that uses the unit, and a preset saved before S35
 * has to load and sound exactly as it did. */
#define FX_PID_DLY_COMP  0x0316
#define FX_PID_REV_MIX   0x0320
#define FX_PID_REV_SIZE  0x0321
#define FX_PID_REV_DAMP  0x0322
#define FX_PID_REV_COMP  0x0323
#define FX_PID_GRN_MIX   0x0330
#define FX_PID_GRN_SIZE  0x0331
#define FX_PID_GRN_DENS  0x0332
#define FX_PID_GRN_PITCH 0x0333
#define FX_PID_GRN_FB    0x0334
#define FX_PID_GRN_SPRAY 0x0335
#define FX_PID_GRN_COMP  0x0336
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

/* drive / saturation (S34) — the graph's Shaper, run on the finished mix.
 * First in the chain: dirt belongs before the time effects, for the same
 * reason the master filter is last (a distorted reverb tail reads as a
 * mistake, a reverb on a distorted source reads as an effect). */
#define FX_PID_DRV_MIX   0x0360
#define FX_PID_DRV_MODE  0x0361
#define FX_PID_DRV_DRIVE 0x0362
#define FX_PID_DRV_TONE  0x0363
#define FX_PID_DRV_LEVEL 0x0364

/* phaser (S34) — a chain of first-order allpasses swept by an LFO. No delay
 * line at all, which is what makes it a different effect from the chorus and
 * not a preset of it. */
#define FX_PID_PHS_MIX    0x0370
#define FX_PID_PHS_STAGES 0x0371
#define FX_PID_PHS_RATE   0x0372
#define FX_PID_PHS_DEPTH  0x0373
#define FX_PID_PHS_CENTER 0x0374
#define FX_PID_PHS_FB     0x0375
#define FX_PID_PHS_SPREAD 0x0376

/* flanger (S34) — a short modulated delay with feedback. Signed feedback:
 * negative inverts the comb, which is the hollow half of the sound. */
#define FX_PID_FLG_MIX    0x0380
#define FX_PID_FLG_RATE   0x0381
#define FX_PID_FLG_DEPTH  0x0382
#define FX_PID_FLG_DELAY  0x0383
#define FX_PID_FLG_FB     0x0384
#define FX_PID_FLG_SPREAD 0x0385

/* 3-band EQ (S34) — RBJ shelves plus a sweepable bell. Placed after the
 * filter and before the compressor, so the compressor reacts to the tone
 * you actually chose. A band parked at 0 dB is skipped, so a flat EQ with
 * fx.eq.on set still costs nothing. */
#define FX_PID_EQ_ON      0x0390
#define FX_PID_EQ_LOW     0x0391
#define FX_PID_EQ_LOFREQ  0x0392
#define FX_PID_EQ_MID     0x0393
#define FX_PID_EQ_MIDFREQ 0x0394
#define FX_PID_EQ_MIDQ    0x0395
#define FX_PID_EQ_HIGH    0x0396
#define FX_PID_EQ_HIFREQ  0x0397

/* compressor (S34) — feed-forward, with a selectable key. fx.comp.key picks
 * between the bus itself (glue compression) and a drum slot's trigger
 * (sidechain ducking); everything else about the unit is the same in both
 * cases, which is why it is one unit and not two. */
#define FX_PID_COMP_ON      0x03A0
#define FX_PID_COMP_THRESH  0x03A1
#define FX_PID_COMP_RATIO   0x03A2
#define FX_PID_COMP_ATTACK  0x03A3
#define FX_PID_COMP_RELEASE 0x03A4
#define FX_PID_COMP_MAKEUP  0x03A5
#define FX_PID_COMP_MIX     0x03A6
#define FX_PID_COMP_KEY     0x03A7
#define FX_PID_COMP_SLOT    0x03A8

/* stereo + output (S34) — last in the chain, because it is the only place
 * where the total width of the mix is decided: the ping-pong delay and the
 * granular panner both throw energy wide upstream of here. `amp` and `pan`
 * live in this unit rather than in audio_io's master volume so that the FX
 * LFOs have somewhere to put tremolo and auto-pan. */
#define FX_PID_ST_WIDTH 0x03B0
#define FX_PID_ST_BASS  0x03B1
#define FX_PID_ST_MONO  0x03B2
#define FX_PID_ST_AMP   0x03B3
#define FX_PID_ST_PAN   0x03B4

/* FX LFOs (S34) — two block-rate LFOs, each free-running or locked to a note
 * division of the master clock, modulating one FX parameter apiece.
 *
 * They exist because the S9 mod matrix is evaluated *per voice* and its
 * destinations are per-voice parameters; nothing in the instrument could
 * reach the master bus. A tempo-locked filter wobble over a whole track, or
 * tremolo, or auto-pan, were not expressible anywhere. */
#define FX_PID_LFO1_DEST  0x03C0
#define FX_PID_LFO1_WAVE  0x03C1
#define FX_PID_LFO1_RATE  0x03C2
#define FX_PID_LFO1_SYNC  0x03C3
#define FX_PID_LFO1_DEPTH 0x03C4
#define FX_PID_LFO1_PHASE 0x03C5
#define FX_PID_LFO2_DEST  0x03C8
#define FX_PID_LFO2_WAVE  0x03C9
#define FX_PID_LFO2_RATE  0x03CA
#define FX_PID_LFO2_SYNC  0x03CB
#define FX_PID_LFO2_DEPTH 0x03CC
#define FX_PID_LFO2_PHASE 0x03CD

/* Registers the 0x03xx params and allocates the delay lines (PSRAM first,
 * internal RAM fallback; an effect whose lines cannot be allocated is
 * disabled with a warning). Call before audio_io_start(). */
esp_err_t fx_init(void);

/* Processes one block in place, in the order at the top of this file, each
 * unit behind its own dry/wet mix or on/off gate (a fully-dry effect costs
 * ~nothing). Audio task only — no locks, no allocation.
 *
 * Must run after drums_pre_fx() in the same render callback: that is what
 * fills in the drum hit the compressor's sidechain key reads. */
void fx_process(float* l, float* r, size_t frames);

#ifdef __cplusplus
}
#endif
