/*
 * osynth — master FX bus: adaptive NR -> NR -> vocoder -> drive -> chorus ->
 * flanger -> phaser -> delay -> granular delay -> reverb -> bitcrush ->
 * filter -> EQ -> compressor -> stereo/output (Sessions 10 + 11; bitcrush
 * S17; filter S33; drive, flanger, phaser, EQ, compressor, stereo and the FX
 * LFOs S34; vocoder S38; the two noise-reduction units S39).
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
/* Per-unit enable switches (S36).
 *
 * Every unit already skipped itself when fully dry, so this is not a CPU
 * feature — it is the missing *bypass*. Before it, auditioning a patch
 * without its reverb meant winding `mix` to zero and losing the setting you
 * were comparing against; the switch is the A/B that mix cannot be.
 *
 * The switch gates the mix rather than replacing it: the unit runs only when
 * `on` is set AND `mix` is above zero, so a unit left at mix 0 still costs
 * nothing whatever the switch says. Both directions ramp through the same
 * unit_gate() smoother, so toggling crossfades instead of stepping.
 *
 * Off by default, which is why S36 also bumps the preset file version: a
 * sparse .osp stores only non-defaults, so "no `on` pair" cannot otherwise be
 * told apart from "deliberately bypassed". Files written before the bump are
 * migrated on load — see presets.cpp's legacy_fx_enable().
 *
 * The three units that already had a switch (filter, EQ, compressor) keep it
 * and gain nothing here: theirs has always been the gate, and a second one
 * would be two controls for one decision.
 *
 * `fx.st` is deliberately absent. It is the master output stage, not an
 * effect — `fx.st.amp` and `fx.st.pan` live there — so a bypass would mute
 * the instrument rather than remove an effect. Its own neutral-value check
 * already skips the stereo work when nothing is asked of it. */
#define FX_PID_CHO_ON    0x0303
#define FX_PID_CHO_MIX   0x0300
#define FX_PID_CHO_RATE  0x0301
#define FX_PID_CHO_DEPTH 0x0302
#define FX_PID_DLY_ON    0x0317
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
#define FX_PID_REV_ON    0x0324
#define FX_PID_REV_MIX   0x0320
#define FX_PID_REV_SIZE  0x0321
#define FX_PID_REV_DAMP  0x0322
#define FX_PID_REV_COMP  0x0323

/* Reverb algorithm (S36). Four topologies behind one unit, because they are
 * four answers to the same question and nobody wants two of them at once:
 * one set of lines, one CPU cost, whichever is selected.
 *
 * 0 `freeverb`  — the original 8-comb/4-allpass Schroeder bus. Its path is
 *                 byte-identical to pre-S36, which is what lets every saved
 *                 patch keep its sound: entry 0 is not a favourite, it is the
 *                 backward-compatibility contract, so this enum is
 *                 append-only and 0 never moves.
 * 1 `wetreverb` — Yonie's WET Reverb, a half-rate Schroeder bank with a
 *                 tapped early-reflection field and 80s digital character.
 * 2 `mverb`     — Martin Eastwood's MVerb, a Dattorro figure-of-eight plate.
 * 3 `duskverb`  — Dusk Audio's DuskVerb "Plate": a Dattorro tank with a
 *                 density cascade in each loop.
 *
 * 2 and 3 are GPL-3 and are compiled in only under CONFIG_OSYNTH_FX_GPL
 * (see components/fx_gpl); 0 and 1 are MIT and always present. That is why
 * the two GPL entries are last and not in licence-blind alphabetical order:
 * an enum's index *is* its stored value, so compiling a middle entry out
 * would renumber the tail and quietly change what every saved patch means.
 * Dropping them off the end instead leaves 0 and 1 meaning what they always
 * did, and a patch asking for 2 or 3 on an MIT build clamps to 1 — a reverb
 * that is at least still a reverb.
 *
 * The five parameters below are the shared front and back of the unit, run
 * outside whichever algorithm is selected. All five are exact no-ops at
 * their defaults, so they cost nothing and change nothing until moved —
 * again, for the sake of patches saved before S36. */
#define FX_PID_REV_ALGO  0x0325
#define FX_PID_REV_PRE   0x0326
#define FX_PID_REV_TONE  0x0327
#define FX_PID_REV_WIDTH 0x0328
/* Algorithm-specific; freeverb has no diffusion or early field to control
 * and ignores both. */
#define FX_PID_REV_DIFF  0x0329
#define FX_PID_REV_EARLY 0x032A
#define FX_PID_GRN_ON    0x0337
#define FX_PID_GRN_MIX   0x0330
#define FX_PID_GRN_SIZE  0x0331
#define FX_PID_GRN_DENS  0x0332
#define FX_PID_GRN_PITCH 0x0333
#define FX_PID_GRN_FB    0x0334
#define FX_PID_GRN_SPRAY 0x0335
#define FX_PID_GRN_COMP  0x0336
#define FX_PID_CRUSH_ON   0x0343
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
 * First of the effects proper: dirt belongs before the time effects, for the
 * same reason the master filter is last (a distorted reverb tail reads as a
 * mistake, a reverb on a distorted source reads as an effect). */
#define FX_PID_DRV_ON    0x0365
#define FX_PID_DRV_MIX   0x0360
#define FX_PID_DRV_MODE  0x0361
#define FX_PID_DRV_DRIVE 0x0362
#define FX_PID_DRV_TONE  0x0363
#define FX_PID_DRV_LEVEL 0x0364

/* phaser (S34) — a chain of first-order allpasses swept by an LFO. No delay
 * line at all, which is what makes it a different effect from the chorus and
 * not a preset of it. */
#define FX_PID_PHS_ON     0x0377
#define FX_PID_PHS_MIX    0x0370
#define FX_PID_PHS_STAGES 0x0371
#define FX_PID_PHS_RATE   0x0372
#define FX_PID_PHS_DEPTH  0x0373
#define FX_PID_PHS_CENTER 0x0374
#define FX_PID_PHS_FB     0x0375
#define FX_PID_PHS_SPREAD 0x0376

/* flanger (S34) — a short modulated delay with feedback. Signed feedback:
 * negative inverts the comb, which is the hollow half of the sound. */
#define FX_PID_FLG_ON     0x0386
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
/* Vocoder (S38): the audio input's spectral envelope imposed on the synth
 * bus. Ahead of every effect — it decides what the sound is, so everything
 * after it colours the spoken result; the two noise-reduction units (S39) are
 * the only stages before it. The modulator is whichever device `in.source`
 * names, independent of `in.route`. fx.voc.freeze holds the band envelopes,
 * which is what the app's Hold-to-sample button drives (inverted: recording
 * while held, frozen on release). */
#define FX_PID_VOC_ON      0x03D0
#define FX_PID_VOC_MIX     0x03D1
#define FX_PID_VOC_BANDS   0x03D2
#define FX_PID_VOC_LOW     0x03D3
#define FX_PID_VOC_HIGH    0x03D4
#define FX_PID_VOC_Q       0x03D5
#define FX_PID_VOC_ATTACK  0x03D6
#define FX_PID_VOC_RELEASE 0x03D7
#define FX_PID_VOC_SHIFT   0x03D8
#define FX_PID_VOC_SIB     0x03D9
#define FX_PID_VOC_GATE    0x03DA
#define FX_PID_VOC_LEVEL   0x03DB
#define FX_PID_VOC_CARRIER 0x03DC
#define FX_PID_VOC_FREEZE  0x03DD

/* Noise reduction (S39) — two units at the head of the chain, and the reason
 * they exist is a use for this instrument that is not musical at all: a P4
 * build already enumerates on a computer as a UAC2 capture device (the USB
 * tap, S29) and already has a microphone on it (S37), so it is one cleanup
 * stage short of being a usable USB microphone. These are that stage.
 *
 * The path is the one that was already there and needs no new plumbing: an
 * input device chosen by `in.source`, mixed into the bus by `in.route` = fx,
 * through this bus, out over USB. What was missing is everything between "the
 * microphone works" and "the microphone is worth listening to" — a room's air
 * conditioning, a desk's rumble, mains hum off an unbalanced lead, and the
 * hiss a MEMS capsule has at the gain a conversational voice needs.
 *
 * Two units rather than one mode switch, because they answer different halves
 * of that question and fail in different ways:
 *
 *   fx.anr  adaptive. A filterbank that learns the *steady* part of whatever
 *           is arriving and subtracts it, continuously, with nothing to set
 *           up. It is what takes out fan noise and hiss. It cannot take out a
 *           slammed door, because a slammed door is not steady.
 *   fx.nr   fixed. High-pass, mains-hum notches, and a downward expander with
 *           a hold. Nothing is learned and every number is one the player
 *           chose — which is exactly why it is the one to reach for when the
 *           adaptive unit has guessed wrong.
 *
 * Their order is anr -> nr and it does not commute. The expander's whole job
 * is to duck the gaps between phrases, and a ducked gap is a gap with no noise
 * floor left in it; run the other way round, the estimator would learn that
 * ducked floor — up to `fx.nr.floor` too low — and then under-subtract by
 * exactly that much for the whole of the next phrase. An estimator has to see
 * the floor it is estimating.
 *
 * Both sit ahead of the vocoder, which is where a source cleanup belongs but
 * changes nothing *for* the vocoder: its modulator comes from
 * audio_io_in_mono(), not from this bus, so it is deaf to everything here.
 * Cleaning that path too would mean a second instance of the analysis and is
 * not what these are for.
 *
 * Neither is microphone-specific. They are ordinary bus units, so a noisy
 * sampled loop or a hissy line input gets the same treatment; the microphone
 * is only the case that made them worth building.
 *
 * These two blocks fill 0x03xx. A tenth unit needs a page of its own. */

/* `fx.anr.src` / `fx.nr.src` (S39b) decide what each unit is looking at, and
 * are the difference between "clean up my microphone" and "put a denoiser
 * across my instrument". At `bus` — entry 0, so a patch saved before the
 * control existed still means what it did — the unit processes the finished
 * mix, which is the right thing for a noisy sampled loop and the wrong thing
 * for a synth that was never noisy.
 *
 * At `input` it processes only what audio_io mixed in. Nothing it does is a
 * function of the synth beside it and no gain of any kind is applied to that
 * — the only thing reaching the bus is a term derived from the input alone,
 * so muting the input leaves the output bit-identical to the unit being off.
 * That is exact rather than approximate, and it is worth knowing why: both
 * units are *corrections*. The
 * adaptive one's band sum is already (g-1) times each band, and the fixed
 * one's is g*filt(x) - x. Neither ever needed the bus in order to work — only
 * something to be a difference *from* — so pointing them at the block
 * audio_io_in_fx_block() hands back and adding the result to the bus replaces
 * the input's contribution and nothing else.
 *
 * `input` needs `in.route` = fx. That is the only position summed into the bus
 * by the time this stage runs; from `mon` or `dry` there is nothing here yet
 * to correct, and the unit stays inert rather than inventing a correction for
 * a signal that arrives later. */

/* Adaptive: a learned noise profile, subtracted per band. `fx.anr.learn` is
 * momentary: held, the estimator's window drops to 80 ms and its "that is
 * signal, not noise" test is waived, so a second of it in a quiet room is a
 * complete profile. That is what the app's Hold-to-learn button drives, and
 * it is not stored in presets, for the reason fx.voc.freeze is not. */
#define FX_PID_ANR_ON      0x03E0
#define FX_PID_ANR_AMOUNT  0x03E1
#define FX_PID_ANR_FLOOR   0x03E2
#define FX_PID_ANR_BANDS   0x03E3
#define FX_PID_ANR_LOW     0x03E4
#define FX_PID_ANR_HIGH    0x03E5
#define FX_PID_ANR_ADAPT   0x03E6
#define FX_PID_ANR_ATTACK  0x03E7
#define FX_PID_ANR_RELEASE 0x03E8
#define FX_PID_ANR_LEARN   0x03E9
#define FX_PID_ANR_SRC     0x03EA

/* Fixed: high-pass, hum notch, downward expander. `fx.nr.floor` is the most
 * important control here and the one a gate usually does not have — it caps
 * the attenuation, so the unit *ducks* the gaps instead of chopping them, and
 * a room that goes absolutely silent between words is the thing that makes a
 * cheap gate audible as a gate. */
#define FX_PID_NR_ON      0x03F0
#define FX_PID_NR_HPF     0x03F1
#define FX_PID_NR_HUM     0x03F2
#define FX_PID_NR_THRESH  0x03F3
#define FX_PID_NR_RATIO   0x03F4
#define FX_PID_NR_FLOOR   0x03F5
#define FX_PID_NR_ATTACK  0x03F6
#define FX_PID_NR_HOLD    0x03F7
#define FX_PID_NR_RELEASE 0x03F8
#define FX_PID_NR_SRC     0x03F9

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
