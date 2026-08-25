/*
 * osynth — factory presets (Session 13; widened to 48 in S33): 48 per
 * engine, const in flash. The whole table is ~20 KB of .rodata against 2 MB
 * of free app partition, so the bank size is a question of how many sounds
 * are worth writing, not of space.
 *
 * Each preset is a sparse list of {param id, value} overrides on the
 * engine-default patch: loading resets the patch ranges to their defaults
 * first (minus the skip list — presets.cpp), so anything not listed here
 * is the PARAM_MAP.md default. Slot 0 of every bank is "init", the pure
 * default patch. Enum values are numeric: osc waves 0 sine / 1 tri /
 * 2 saw / 3 pulse; filter modes 0 lp / 1 bp / 2 hp / 3 notch / 4 peak /
 * 5 ap / 6 bp-norm (S33 appended 3-6); filter types 0 svf12 / 1 svf24 /
 * 2 ladder / 3 dual / 4 vowel; lfo waves 0 sine / 1 tri / 2 saw /
 * 3 square / 4 s&h; wavetable sets 0 basic / 1 sync / 2 vocal / 3 fm;
 * arp modes 0 off / 1 up / 2 down / 3 updown / 4 random / 5 played;
 * seq divisions 0 1/4 … 5 1/32; granular sources 0 synth / 1 in, grain
 * waves 0 sine / 1 tri / 2 saw / 3 pulse / 4 noise, rate modes 0 sync /
 * 1 free (S38).
 *
 * Slots 0-15 of each bank are the original S13 sixteen, untouched: their
 * defaults were chosen so they render exactly as they did before S33
 * (flt.type svf12, flt.drive 0, flt.on 1 where a filter already existed,
 * 0 on fm/additive, which had none). Slots 16-47 are the S33 additions,
 * and those do use the new filters.
 *
 * Two range notes, both pre-S33 and both harmless: "glide lead" asks for a
 * 0.48 s delay and "shimmer pad" for a 0.3 s grain, which are legal on the
 * S3 and clamp to the classic ESP32's smaller ceilings. tools/
 * check_preset_ranges.py reports those as warnings and everything else as
 * an error.
 *
 * Mix/level/drawbar sums are kept near 1.0 so full 8-voice polyphony
 * cannot clip (the engines' gain-staging convention since S4).
 */
#include "presets_priv.h"

#include "engine_additive.h"
#include "engine_fm.h"
#include "engine_granular.h"
#include "engine_sampler.h"
#include "engine_subtractive.h"
#include "engine_wavetable.h"
#include "fx.h"
#include "seqarp.h"
#include "synth_config.h"
#include "synth_mod.h"
#include "synth_params_c.h"

#define P(id, v) {(uint16_t)(id), (float)(v)}
#define N(t) (uint16_t)(sizeof(t) / sizeof((t)[0]))

/* Mod-matrix slot k as three pairs. */
#define MOD(k, src, dest, amt)              \
    P(SYNTH_PID_MOD_SRC(k), (src)),         \
    P(SYNTH_PID_MOD_DEST(k), (dest)),       \
    P(SYNTH_PID_MOD_AMOUNT(k), (amt))

/* ---- subtractive (bank 0, linear slots 0-111) ------------------------------- */

/* two detuned saws + light unison, mostly-open filter, wheel = vibrato */
static const preset_pair_t kSubFatSaw[] = {
    P(SUB_PID_OSC2_WAVE, 2), P(SUB_PID_OSC2_FINE, -9.0f),
    P(SUB_PID_MIX_OSC1, 0.6f), P(SUB_PID_MIX_OSC2, 0.6f),
    P(SUB_PID_FLT_CUTOFF, 4000.0f), P(SUB_PID_FLT_ENV, 1.0f),
    P(SUB_PID_FLT_RESO, 0.1f),
    P(SUB_PID_ENV1_SUSTAIN, 0.85f), P(SUB_PID_ENV1_RELEASE, 0.35f),
    P(SYNTH_PID_COMMON_UNISON, 2), P(SYNTH_PID_COMMON_UNI_DETUNE, 16.0f),
    P(SYNTH_PID_COMMON_UNI_SPREAD, 0.8f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, SUB_PID_LFO1_PITCH, 0.35f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.18f),
};

/* 303-ish: closed resonant filter, snappy filter env, glide, ping-pong */
static const preset_pair_t kSubAcid[] = {
    P(SUB_PID_FLT_CUTOFF, 320.0f), P(SUB_PID_FLT_RESO, 0.82f),
    P(SUB_PID_FLT_ENV, 2.8f), P(SUB_PID_FLT_KBD, 0.3f),
    P(SUB_PID_ENV2_ATTACK, 0.001f), P(SUB_PID_ENV2_DECAY, 0.16f),
    P(SUB_PID_ENV2_SUSTAIN, 0.0f),
    P(SUB_PID_ENV1_DECAY, 0.3f), P(SUB_PID_ENV1_SUSTAIN, 0.35f),
    P(SUB_PID_ENV1_RELEASE, 0.08f),
    P(SYNTH_PID_COMMON_GLIDE, 0.07f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, SUB_PID_FLT_CUTOFF, 0.5f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.22f), P(FX_PID_DLY_TIME, 0.375f),
    P(FX_PID_DLY_FB, 0.45f), P(FX_PID_DLY_PP, 1),
};

/* slow attack, dark filter swelling open, chorus + big room */
static const preset_pair_t kSubWarmPad[] = {
    P(SUB_PID_OSC2_WAVE, 2), P(SUB_PID_OSC2_FINE, -7.0f),
    P(SUB_PID_MIX_OSC1, 0.5f), P(SUB_PID_MIX_OSC2, 0.5f),
    P(SUB_PID_FLT_CUTOFF, 900.0f), P(SUB_PID_FLT_ENV, 0.8f),
    P(SUB_PID_ENV2_ATTACK, 1.2f), P(SUB_PID_ENV2_DECAY, 1.0f),
    P(SUB_PID_ENV2_SUSTAIN, 0.6f),
    P(SUB_PID_ENV1_ATTACK, 0.7f), P(SUB_PID_ENV1_DECAY, 1.0f),
    P(SUB_PID_ENV1_SUSTAIN, 0.9f), P(SUB_PID_ENV1_RELEASE, 1.8f),
    P(SUB_PID_LFO2_RATE, 0.25f), P(SUB_PID_LFO2_CUTOFF, 0.3f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.4f), P(FX_PID_REV_MIX, 0.35f),
    P(FX_PID_REV_SIZE, 0.7f),
};

/* pulse with lfo2-driven width via the matrix, ensemble chorus */
static const preset_pair_t kSubPwmStrings[] = {
    P(SUB_PID_OSC1_WAVE, 3), P(SUB_PID_OSC1_PW, 0.5f),
    P(SUB_PID_FLT_CUTOFF, 2600.0f), P(SUB_PID_FLT_ENV, 0.5f),
    P(SUB_PID_ENV1_ATTACK, 0.28f), P(SUB_PID_ENV1_SUSTAIN, 0.85f),
    P(SUB_PID_ENV1_RELEASE, 0.9f),
    P(SUB_PID_LFO2_RATE, 0.6f),
    MOD(0, SYNTH_MOD_SRC_LFO2, SUB_PID_OSC1_PW, 0.35f),
    MOD(1, SYNTH_MOD_SRC_WHEEL, SUB_PID_LFO1_PITCH, 0.3f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.5f), P(FX_PID_REV_MIX, 0.25f),
};

/* percussive filter blip, velocity opens the sweep */
static const preset_pair_t kSubFunkPluck[] = {
    P(SUB_PID_FLT_CUTOFF, 620.0f), P(SUB_PID_FLT_RESO, 0.5f),
    P(SUB_PID_FLT_ENV, 3.2f),
    P(SUB_PID_ENV2_DECAY, 0.09f), P(SUB_PID_ENV2_SUSTAIN, 0.0f),
    P(SUB_PID_ENV1_DECAY, 0.35f), P(SUB_PID_ENV1_SUSTAIN, 0.0f),
    P(SUB_PID_ENV1_RELEASE, 0.18f),
    MOD(0, SYNTH_MOD_SRC_VEL, SUB_PID_FLT_CUTOFF, 0.4f),
};

/* sine + a whisper of triangle, fast clean envelope */
static const preset_pair_t kSubDeepSub[] = {
    P(SUB_PID_OSC1_WAVE, 0), P(SUB_PID_OSC2_WAVE, 1),
    P(SUB_PID_OSC2_FINE, 0.0f),
    P(SUB_PID_MIX_OSC1, 0.75f), P(SUB_PID_MIX_OSC2, 0.25f),
    P(SUB_PID_FLT_CUTOFF, 700.0f),
    P(SUB_PID_ENV1_ATTACK, 0.003f), P(SUB_PID_ENV1_DECAY, 0.4f),
    P(SUB_PID_ENV1_SUSTAIN, 0.9f), P(SUB_PID_ENV1_RELEASE, 0.12f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.05f),
};

/* band-pass noise sweep rising while held — an FX riser, not a "note" */
static const preset_pair_t kSubRiser[] = {
    P(SUB_PID_MIX_OSC1, 0.3f), P(SUB_PID_MIX_NOISE, 0.7f),
    P(SUB_PID_FLT_MODE, 1), P(SUB_PID_FLT_CUTOFF, 250.0f),
    P(SUB_PID_FLT_RESO, 0.6f), P(SUB_PID_FLT_ENV, 4.0f),
    P(SUB_PID_ENV2_ATTACK, 3.5f), P(SUB_PID_ENV2_SUSTAIN, 1.0f),
    P(SUB_PID_ENV1_ATTACK, 1.5f), P(SUB_PID_ENV1_SUSTAIN, 1.0f),
    P(SUB_PID_ENV1_RELEASE, 1.5f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.4f), P(FX_PID_REV_SIZE, 0.8f),
};

/* detuned octave stack, short filter bite, ping-pong echoes */
static const preset_pair_t kSubRaveStab[] = {
    P(SUB_PID_OSC2_WAVE, 2), P(SUB_PID_OSC2_SEMI, 12),
    P(SUB_PID_MIX_OSC1, 0.65f), P(SUB_PID_MIX_OSC2, 0.4f),
    P(SUB_PID_FLT_CUTOFF, 2000.0f), P(SUB_PID_FLT_RESO, 0.25f),
    P(SUB_PID_FLT_ENV, 1.2f),
    P(SUB_PID_ENV2_DECAY, 0.2f), P(SUB_PID_ENV2_SUSTAIN, 0.1f),
    P(SUB_PID_ENV1_DECAY, 0.5f), P(SUB_PID_ENV1_SUSTAIN, 0.0f),
    P(SUB_PID_ENV1_RELEASE, 0.3f),
    P(SYNTH_PID_COMMON_UNISON, 3), P(SYNTH_PID_COMMON_UNI_DETUNE, 30.0f),
    P(SYNTH_PID_COMMON_UNI_SPREAD, 1.0f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.25f), P(FX_PID_DLY_TIME, 0.25f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_DLY_PP, 1), P(FX_PID_REV_MIX, 0.2f),
};

/* portamento lead over a sub-octave pulse, tape-ish delay */
static const preset_pair_t kSubGlideLead[] = {
    P(SYNTH_PID_COMMON_GLIDE, 0.12f),
    P(SUB_PID_OSC2_WAVE, 3), P(SUB_PID_OSC2_PW, 0.35f),
    P(SUB_PID_OSC2_SEMI, -12), P(SUB_PID_MIX_OSC2, 0.35f),
    P(SUB_PID_MIX_OSC1, 0.65f),
    P(SUB_PID_FLT_CUTOFF, 2400.0f), P(SUB_PID_FLT_RESO, 0.3f),
    P(SUB_PID_ENV1_SUSTAIN, 0.8f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, SUB_PID_LFO1_PITCH, 0.4f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.3f), P(FX_PID_DLY_TIME, 0.48f),
    P(FX_PID_DLY_FB, 0.5f), P(FX_PID_DLY_TONE, 3000.0f),
};

/* slow-ish filter env into a high sustain — the classic analog brass rise */
static const preset_pair_t kSubBrass[] = {
    P(SUB_PID_FLT_CUTOFF, 800.0f), P(SUB_PID_FLT_RESO, 0.12f),
    P(SUB_PID_FLT_ENV, 1.6f),
    P(SUB_PID_ENV2_ATTACK, 0.06f), P(SUB_PID_ENV2_DECAY, 0.25f),
    P(SUB_PID_ENV2_SUSTAIN, 0.65f),
    P(SUB_PID_ENV1_ATTACK, 0.045f), P(SUB_PID_ENV1_SUSTAIN, 0.9f),
    P(SUB_PID_ENV1_RELEASE, 0.25f),
    P(SYNTH_PID_COMMON_UNISON, 2), P(SYNTH_PID_COMMON_UNI_DETUNE, 9.0f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.2f),
};

/* lfo2 grinding the cutoff over a sub-octave saw */
static const preset_pair_t kSubWobble[] = {
    P(SUB_PID_OSC2_WAVE, 2), P(SUB_PID_OSC2_SEMI, -12),
    P(SUB_PID_OSC2_FINE, 0.0f),
    P(SUB_PID_MIX_OSC1, 0.55f), P(SUB_PID_MIX_OSC2, 0.45f),
    P(SUB_PID_FLT_CUTOFF, 350.0f), P(SUB_PID_FLT_RESO, 0.45f),
    P(SUB_PID_FLT_ENV, 0.0f),
    P(SUB_PID_LFO2_RATE, 2.6f), P(SUB_PID_LFO2_CUTOFF, 2.6f),
    P(SUB_PID_ENV1_ATTACK, 0.005f), P(SUB_PID_ENV1_SUSTAIN, 0.95f),
    P(SUB_PID_ENV1_RELEASE, 0.1f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, SUB_PID_LFO2_CUTOFF, 0.3f),
};

/* triangle + thin pulse, mellow decaying keys */
static const preset_pair_t kSubHollowKeys[] = {
    P(SUB_PID_OSC1_WAVE, 1), P(SUB_PID_OSC2_WAVE, 3),
    P(SUB_PID_OSC2_PW, 0.2f), P(SUB_PID_OSC2_FINE, 0.0f),
    P(SUB_PID_MIX_OSC1, 0.6f), P(SUB_PID_MIX_OSC2, 0.35f),
    P(SUB_PID_FLT_CUTOFF, 3800.0f), P(SUB_PID_FLT_ENV, 0.8f),
    P(SUB_PID_ENV1_DECAY, 0.7f), P(SUB_PID_ENV1_SUSTAIN, 0.25f),
    P(SUB_PID_ENV1_RELEASE, 0.4f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.3f), P(FX_PID_REV_MIX, 0.3f),
};

/* wide detune + deep ensemble chorus — the string-machine recipe */
static const preset_pair_t kSubStringMachine[] = {
    P(SUB_PID_OSC2_WAVE, 2), P(SUB_PID_OSC2_FINE, -10.0f),
    P(SUB_PID_MIX_OSC1, 0.5f), P(SUB_PID_MIX_OSC2, 0.5f),
    P(SUB_PID_FLT_CUTOFF, 3200.0f),
    P(SUB_PID_ENV1_ATTACK, 0.4f), P(SUB_PID_ENV1_SUSTAIN, 0.9f),
    P(SUB_PID_ENV1_RELEASE, 1.1f),
    P(SYNTH_PID_COMMON_UNISON, 2), P(SYNTH_PID_COMMON_UNI_DETUNE, 20.0f),
    P(SYNTH_PID_COMMON_UNI_SPREAD, 1.0f),
    P(FX_PID_CHO_ON, 1),
    P(FX_PID_CHO_MIX, 0.55f), P(FX_PID_CHO_RATE, 0.7f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_DEPTH, 5.0f), P(FX_PID_REV_MIX, 0.3f),
};

/* resonant downward whistle: negative filter env ramped by a slow attack */
static const preset_pair_t kSubZapPerc[] = {
    P(SUB_PID_MIX_NOISE, 0.2f), P(SUB_PID_MIX_OSC1, 0.7f),
    P(SUB_PID_FLT_CUTOFF, 6000.0f), P(SUB_PID_FLT_RESO, 0.85f),
    P(SUB_PID_FLT_ENV, -4.0f),
    P(SUB_PID_ENV2_ATTACK, 0.12f), P(SUB_PID_ENV2_SUSTAIN, 1.0f),
    P(SUB_PID_ENV1_DECAY, 0.18f), P(SUB_PID_ENV1_SUSTAIN, 0.0f),
    P(SUB_PID_ENV1_RELEASE, 0.12f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.25f), P(FX_PID_DLY_PP, 1),
};

/* triangle through a gentle sweep, velocity brightens */
static const preset_pair_t kSubSoftKeys[] = {
    P(SUB_PID_OSC1_WAVE, 1),
    P(SUB_PID_FLT_CUTOFF, 1800.0f), P(SUB_PID_FLT_ENV, 1.2f),
    P(SUB_PID_ENV2_DECAY, 0.5f), P(SUB_PID_ENV2_SUSTAIN, 0.1f),
    P(SUB_PID_ENV1_ATTACK, 0.01f), P(SUB_PID_ENV1_DECAY, 0.9f),
    P(SUB_PID_ENV1_SUSTAIN, 0.4f), P(SUB_PID_ENV1_RELEASE, 0.6f),
    MOD(0, SYNTH_MOD_SRC_VEL, SUB_PID_FLT_CUTOFF, 0.35f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.2f), P(FX_PID_REV_MIX, 0.25f),
};

/* ---- subtractive, slots 16-47 (S33) ---------------------------------
 *
 * The first twelve are here to show what the S33 filter family does, one
 * idea each — the rest are the bread-and-butter sounds the original sixteen
 * had no room for. Filter types: 0 svf12, 1 svf24, 2 ladder, 3 dual,
 * 4 vowel. Modes: 0 lp, 1 bp, 2 hp, 3 notch, 4 peak, 5 ap, 6 bp-norm. */

/* the ladder's bass loss is compensated, so this stays fat as reso climbs */
static const preset_pair_t kSubLadderBass[] = {
    P(SUB_PID_FLT_TYPE, 2), P(SUB_PID_FLT_CUTOFF, 260.0f),
    P(SUB_PID_FLT_RESO, 0.55f), P(SUB_PID_FLT_ENV, 2.2f),
    P(SUB_PID_FLT_KBD, 0.35f), P(SUB_PID_FLT_DRIVE, 0.25f),
    P(SUB_PID_OSC2_SEMI, -12), P(SUB_PID_MIX_OSC2, 0.45f),
    P(SUB_PID_ENV2_DECAY, 0.22f), P(SUB_PID_ENV2_SUSTAIN, 0.0f),
    P(SUB_PID_ENV1_DECAY, 0.4f), P(SUB_PID_ENV1_SUSTAIN, 0.55f),
    P(SUB_PID_ENV1_RELEASE, 0.12f),
};

/* 24 dB of slope under the fingers: kbd tracking full, glide on */
static const preset_pair_t kSubLadderLead[] = {
    P(SUB_PID_FLT_TYPE, 2), P(SUB_PID_FLT_CUTOFF, 900.0f),
    P(SUB_PID_FLT_RESO, 0.62f), P(SUB_PID_FLT_ENV, 1.6f),
    P(SUB_PID_FLT_KBD, 1.0f), P(SUB_PID_FLT_DRIVE, 0.3f),
    P(SUB_PID_OSC2_FINE, -7.0f), P(SUB_PID_MIX_OSC2, 0.5f),
    P(SUB_PID_ENV1_SUSTAIN, 0.8f), P(SUB_PID_ENV1_RELEASE, 0.2f),
    P(SYNTH_PID_COMMON_GLIDE, 0.06f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, SUB_PID_LFO1_PITCH, 0.4f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.18f), P(FX_PID_DLY_TIME, 0.28f),
};

/* ladder + drive at the edge of self-oscillation — the classic squelch */
static const preset_pair_t kSubLadderAcid[] = {
    P(SUB_PID_FLT_TYPE, 2), P(SUB_PID_FLT_CUTOFF, 300.0f),
    P(SUB_PID_FLT_RESO, 0.9f), P(SUB_PID_FLT_ENV, 3.0f),
    P(SUB_PID_FLT_DRIVE, 0.55f), P(SUB_PID_FLT_KBD, 0.3f),
    P(SUB_PID_ENV2_ATTACK, 0.001f), P(SUB_PID_ENV2_DECAY, 0.13f),
    P(SUB_PID_ENV2_SUSTAIN, 0.0f),
    P(SUB_PID_ENV1_DECAY, 0.25f), P(SUB_PID_ENV1_SUSTAIN, 0.3f),
    P(SUB_PID_ENV1_RELEASE, 0.06f),
    P(SYNTH_PID_COMMON_GLIDE, 0.05f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.22f), P(FX_PID_DLY_FB, 0.4f),
};

/* the whole vocal tract on one envelope; wheel morphs a -> u */
static const preset_pair_t kSubVowelPad[] = {
    P(SUB_PID_FLT_TYPE, 4), P(SUB_PID_FLT_VOWEL, 0.15f),
    P(SUB_PID_FLT_CUTOFF, 1000.0f), P(SUB_PID_FLT_RESO, 0.45f),
    P(SUB_PID_FLT_ENV, 0.6f),
    P(SUB_PID_OSC2_FINE, -11.0f), P(SUB_PID_MIX_OSC2, 0.6f),
    P(SUB_PID_ENV1_ATTACK, 0.5f), P(SUB_PID_ENV1_SUSTAIN, 0.9f),
    P(SUB_PID_ENV1_RELEASE, 1.2f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, SUB_PID_FLT_VOWEL, 0.7f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.4f), P(FX_PID_REV_SIZE, 0.7f),
};

/* an LFO on the morph is the whole trick — it talks on its own */
static const preset_pair_t kSubTalkingLead[] = {
    P(SUB_PID_FLT_TYPE, 4), P(SUB_PID_FLT_VOWEL, 0.3f),
    P(SUB_PID_FLT_CUTOFF, 1100.0f), P(SUB_PID_FLT_RESO, 0.7f),
    P(SUB_PID_FLT_DRIVE, 0.2f),
    P(SUB_PID_OSC1_WAVE, 3), P(SUB_PID_OSC1_PW, 0.35f),
    P(SUB_PID_ENV1_SUSTAIN, 0.85f), P(SUB_PID_ENV1_RELEASE, 0.15f),
    P(SUB_PID_LFO2_RATE, 3.2f), P(SUB_PID_LFO2_WAVE, 1),
    MOD(0, SYNTH_MOD_SRC_LFO2, SUB_PID_FLT_VOWEL, 0.5f),
    MOD(1, SYNTH_MOD_SRC_VEL, SUB_PID_FLT_VOWEL, 0.3f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.2f),
};

/* short, hard formant hit — the vowel filter as a percussion voice */
static const preset_pair_t kSubFormantStab[] = {
    P(SUB_PID_FLT_TYPE, 4), P(SUB_PID_FLT_VOWEL, 0.0f),
    P(SUB_PID_FLT_CUTOFF, 1400.0f), P(SUB_PID_FLT_RESO, 0.8f),
    P(SUB_PID_FLT_ENV, 1.2f), P(SUB_PID_FLT_DRIVE, 0.35f),
    P(SUB_PID_MIX_NOISE, 0.15f),
    P(SUB_PID_ENV2_DECAY, 0.09f), P(SUB_PID_ENV2_SUSTAIN, 0.0f),
    P(SUB_PID_ENV1_ATTACK, 0.002f), P(SUB_PID_ENV1_DECAY, 0.18f),
    P(SUB_PID_ENV1_SUSTAIN, 0.0f), P(SUB_PID_ENV1_RELEASE, 0.1f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.22f),
};

/* notch on a slow LFO: a phaser that lives inside the filter */
static const preset_pair_t kSubNotchSweep[] = {
    P(SUB_PID_FLT_MODE, 3), P(SUB_PID_FLT_CUTOFF, 800.0f),
    P(SUB_PID_FLT_RESO, 0.5f), P(SUB_PID_FLT_ENV, 0.0f),
    P(SUB_PID_OSC2_FINE, 8.0f), P(SUB_PID_MIX_OSC2, 0.6f),
    P(SUB_PID_LFO2_RATE, 0.35f), P(SUB_PID_LFO2_CUTOFF, 2.2f),
    P(SUB_PID_ENV1_ATTACK, 0.15f), P(SUB_PID_ENV1_SUSTAIN, 0.9f),
    P(SUB_PID_ENV1_RELEASE, 0.5f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.25f),
};

/* allpass: flat magnitude, swept phase — motion with no tone change */
static const preset_pair_t kSubPhaseKeys[] = {
    P(SUB_PID_FLT_MODE, 5), P(SUB_PID_FLT_CUTOFF, 1100.0f),
    P(SUB_PID_FLT_RESO, 0.7f), P(SUB_PID_FLT_ENV, 0.0f),
    P(SUB_PID_OSC1_WAVE, 1), P(SUB_PID_OSC2_WAVE, 2),
    P(SUB_PID_MIX_OSC2, 0.35f),
    P(SUB_PID_LFO2_RATE, 0.6f), P(SUB_PID_LFO2_CUTOFF, 3.0f),
    P(SUB_PID_ENV1_DECAY, 1.2f), P(SUB_PID_ENV1_SUSTAIN, 0.45f),
    P(SUB_PID_ENV1_RELEASE, 0.7f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.25f), P(FX_PID_REV_MIX, 0.3f),
};

/* peak mode is lp - hp: a resonant hump you can sweep without losing ends */
static const preset_pair_t kSubPeakSweep[] = {
    P(SUB_PID_FLT_MODE, 4), P(SUB_PID_FLT_CUTOFF, 500.0f),
    P(SUB_PID_FLT_RESO, 0.75f), P(SUB_PID_FLT_ENV, 3.2f),
    P(SUB_PID_FLT_KBD, 0.2f),
    P(SUB_PID_ENV2_DECAY, 0.7f), P(SUB_PID_ENV2_SUSTAIN, 0.25f),
    P(SUB_PID_ENV1_SUSTAIN, 0.75f), P(SUB_PID_ENV1_RELEASE, 0.35f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, SUB_PID_FLT_CUTOFF, 0.5f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.15f),
};

/* dual: width is a knob, not a consequence of Q — a real bandpass stab */
static const preset_pair_t kSubDualStab[] = {
    P(SUB_PID_FLT_TYPE, 3), P(SUB_PID_FLT_SPREAD, 1.2f),
    P(SUB_PID_FLT_CUTOFF, 900.0f), P(SUB_PID_FLT_RESO, 0.4f),
    P(SUB_PID_FLT_ENV, 1.8f), P(SUB_PID_FLT_DRIVE, 0.2f),
    P(SUB_PID_OSC2_SEMI, 7), P(SUB_PID_MIX_OSC2, 0.5f),
    P(SUB_PID_ENV2_DECAY, 0.2f), P(SUB_PID_ENV2_SUSTAIN, 0.0f),
    P(SUB_PID_ENV1_DECAY, 0.3f), P(SUB_PID_ENV1_SUSTAIN, 0.25f),
    P(SUB_PID_ENV1_RELEASE, 0.18f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.2f),
};

/* the same filter closed to a slit — nasal, telephone-ish lead */
static const preset_pair_t kSubNarrowLead[] = {
    P(SUB_PID_FLT_TYPE, 3), P(SUB_PID_FLT_SPREAD, 0.35f),
    P(SUB_PID_FLT_CUTOFF, 1300.0f), P(SUB_PID_FLT_RESO, 0.55f),
    P(SUB_PID_FLT_KBD, 0.8f), P(SUB_PID_FLT_DRIVE, 0.4f),
    P(SUB_PID_OSC1_WAVE, 3), P(SUB_PID_OSC1_PW, 0.25f),
    P(SUB_PID_ENV1_SUSTAIN, 0.85f), P(SUB_PID_ENV1_RELEASE, 0.12f),
    P(SYNTH_PID_COMMON_GLIDE, 0.04f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.25f), P(FX_PID_DLY_FB, 0.35f),
};

/* drive into a 24 dB slope: saturates before it gets shrill */
static const preset_pair_t kSubDrivenSaw[] = {
    P(SUB_PID_FLT_TYPE, 1), P(SUB_PID_FLT_CUTOFF, 1500.0f),
    P(SUB_PID_FLT_RESO, 0.35f), P(SUB_PID_FLT_ENV, 2.0f),
    P(SUB_PID_FLT_DRIVE, 0.7f),
    P(SUB_PID_OSC2_FINE, -14.0f), P(SUB_PID_MIX_OSC1, 0.6f),
    P(SUB_PID_MIX_OSC2, 0.6f),
    P(SUB_PID_ENV1_SUSTAIN, 0.8f), P(SUB_PID_ENV1_RELEASE, 0.25f),
    P(SYNTH_PID_COMMON_UNISON, 2), P(SYNTH_PID_COMMON_UNI_DETUNE, 14.0f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.2f),
};

/* two saws a hair apart under a 24 dB lowpass — the jungle staple */
static const preset_pair_t kSubReeseBass[] = {
    P(SUB_PID_FLT_TYPE, 1), P(SUB_PID_FLT_CUTOFF, 420.0f),
    P(SUB_PID_FLT_RESO, 0.2f), P(SUB_PID_FLT_ENV, 0.8f),
    P(SUB_PID_FLT_KBD, 0.25f),
    P(SUB_PID_OSC2_FINE, -18.0f), P(SUB_PID_MIX_OSC1, 0.6f),
    P(SUB_PID_MIX_OSC2, 0.6f),
    P(SUB_PID_ENV1_ATTACK, 0.01f), P(SUB_PID_ENV1_SUSTAIN, 0.95f),
    P(SUB_PID_ENV1_RELEASE, 0.15f),
    P(SUB_PID_LFO2_RATE, 0.25f), P(SUB_PID_LFO2_CUTOFF, 0.7f),
};

/* four detuned voices, spread wide, filter mostly out of the way */
static const preset_pair_t kSubSuperSaw[] = {
    P(SUB_PID_FLT_TYPE, 1), P(SUB_PID_FLT_CUTOFF, 6000.0f),
    P(SUB_PID_FLT_RESO, 0.12f), P(SUB_PID_FLT_ENV, 1.0f),
    P(SUB_PID_OSC2_FINE, 12.0f), P(SUB_PID_MIX_OSC2, 0.55f),
    P(SUB_PID_ENV1_ATTACK, 0.02f), P(SUB_PID_ENV1_SUSTAIN, 0.9f),
    P(SUB_PID_ENV1_RELEASE, 0.6f),
    P(SYNTH_PID_COMMON_UNISON, 4), P(SYNTH_PID_COMMON_UNI_DETUNE, 22.0f),
    P(SYNTH_PID_COMMON_UNI_SPREAD, 1.0f),
    P(FX_PID_REV_ON, 1), P(FX_PID_DLY_ON, 1),
    P(FX_PID_REV_MIX, 0.3f), P(FX_PID_DLY_MIX, 0.15f),
};

/* pulse + saw, filter opening on a long sweep — the rave siren */
static const preset_pair_t kSubHoover[] = {
    P(SUB_PID_OSC1_WAVE, 3), P(SUB_PID_OSC1_PW, 0.2f),
    P(SUB_PID_OSC2_WAVE, 2), P(SUB_PID_OSC2_SEMI, -12),
    P(SUB_PID_MIX_OSC1, 0.55f), P(SUB_PID_MIX_OSC2, 0.55f),
    P(SUB_PID_FLT_TYPE, 1), P(SUB_PID_FLT_CUTOFF, 700.0f),
    P(SUB_PID_FLT_RESO, 0.45f), P(SUB_PID_FLT_ENV, 3.0f),
    P(SUB_PID_FLT_DRIVE, 0.3f),
    P(SUB_PID_ENV2_ATTACK, 0.25f), P(SUB_PID_ENV2_DECAY, 1.5f),
    P(SUB_PID_ENV2_SUSTAIN, 0.5f),
    P(SUB_PID_ENV1_SUSTAIN, 0.9f), P(SUB_PID_ENV1_RELEASE, 0.3f),
    P(SYNTH_PID_COMMON_GLIDE, 0.08f),
};

/* square with the filter parked open: drawbar-ish, no sweep at all */
static const preset_pair_t kSubOrganTone[] = {
    P(SUB_PID_OSC1_WAVE, 3), P(SUB_PID_OSC1_PW, 0.5f),
    P(SUB_PID_OSC2_WAVE, 3), P(SUB_PID_OSC2_SEMI, 12),
    P(SUB_PID_OSC2_FINE, 0.0f),
    P(SUB_PID_MIX_OSC1, 0.6f), P(SUB_PID_MIX_OSC2, 0.35f),
    P(SUB_PID_FLT_CUTOFF, 7000.0f), P(SUB_PID_FLT_ENV, 0.0f),
    P(SUB_PID_ENV1_ATTACK, 0.004f), P(SUB_PID_ENV1_DECAY, 0.05f),
    P(SUB_PID_ENV1_SUSTAIN, 1.0f), P(SUB_PID_ENV1_RELEASE, 0.05f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.3f), P(FX_PID_REV_MIX, 0.15f),
};

/* narrow pulse, fast decay, resonant bite */
static const preset_pair_t kSubClavPlink[] = {
    P(SUB_PID_OSC1_WAVE, 3), P(SUB_PID_OSC1_PW, 0.12f),
    P(SUB_PID_FLT_CUTOFF, 1600.0f), P(SUB_PID_FLT_RESO, 0.55f),
    P(SUB_PID_FLT_ENV, 2.4f), P(SUB_PID_FLT_KBD, 0.6f),
    P(SUB_PID_FLT_DRIVE, 0.25f),
    P(SUB_PID_ENV2_DECAY, 0.11f), P(SUB_PID_ENV2_SUSTAIN, 0.0f),
    P(SUB_PID_ENV1_ATTACK, 0.001f), P(SUB_PID_ENV1_DECAY, 0.35f),
    P(SUB_PID_ENV1_SUSTAIN, 0.1f), P(SUB_PID_ENV1_RELEASE, 0.1f),
    MOD(0, SYNTH_MOD_SRC_VEL, SUB_PID_FLT_CUTOFF, 0.45f),
};

/* fat and round: sine sub under a triangle, barely any harmonics left */
static const preset_pair_t kSubRubberBass[] = {
    P(SUB_PID_OSC1_WAVE, 1), P(SUB_PID_OSC2_WAVE, 0),
    P(SUB_PID_OSC2_SEMI, -12),
    P(SUB_PID_MIX_OSC1, 0.55f), P(SUB_PID_MIX_OSC2, 0.5f),
    P(SUB_PID_FLT_TYPE, 2), P(SUB_PID_FLT_CUTOFF, 240.0f),
    P(SUB_PID_FLT_RESO, 0.35f), P(SUB_PID_FLT_ENV, 1.6f),
    P(SUB_PID_ENV2_DECAY, 0.15f), P(SUB_PID_ENV2_SUSTAIN, 0.0f),
    P(SUB_PID_ENV1_DECAY, 0.5f), P(SUB_PID_ENV1_SUSTAIN, 0.5f),
    P(SUB_PID_ENV1_RELEASE, 0.1f),
};

/* one sine, one very fast filter+amp decay: a synth kick on the keyboard */
static const preset_pair_t kSubKickSynth[] = {
    P(SUB_PID_OSC1_WAVE, 0), P(SUB_PID_MIX_OSC1, 1.0f),
    P(SUB_PID_FLT_CUTOFF, 180.0f), P(SUB_PID_FLT_RESO, 0.3f),
    P(SUB_PID_FLT_ENV, 3.5f), P(SUB_PID_FLT_KBD, 0.0f),
    P(SUB_PID_ENV2_ATTACK, 0.001f), P(SUB_PID_ENV2_DECAY, 0.045f),
    P(SUB_PID_ENV2_SUSTAIN, 0.0f),
    P(SUB_PID_ENV1_ATTACK, 0.001f), P(SUB_PID_ENV1_DECAY, 0.22f),
    P(SUB_PID_ENV1_SUSTAIN, 0.0f), P(SUB_PID_ENV1_RELEASE, 0.12f),
};

/* noise through a bandpass, snapped shut */
static const preset_pair_t kSubSnareSynth[] = {
    P(SUB_PID_MIX_OSC1, 0.25f), P(SUB_PID_MIX_NOISE, 0.8f),
    P(SUB_PID_OSC1_WAVE, 1),
    P(SUB_PID_FLT_MODE, 6), P(SUB_PID_FLT_CUTOFF, 1900.0f),
    P(SUB_PID_FLT_RESO, 0.5f), P(SUB_PID_FLT_ENV, 1.0f),
    P(SUB_PID_FLT_KBD, 0.0f),
    P(SUB_PID_ENV2_DECAY, 0.06f), P(SUB_PID_ENV2_SUSTAIN, 0.0f),
    P(SUB_PID_ENV1_ATTACK, 0.001f), P(SUB_PID_ENV1_DECAY, 0.16f),
    P(SUB_PID_ENV1_SUSTAIN, 0.0f), P(SUB_PID_ENV1_RELEASE, 0.1f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.2f),
};

/* pitched membrane: triangle, no harmonics above the skin tone */
static const preset_pair_t kSubTomSynth[] = {
    P(SUB_PID_OSC1_WAVE, 1), P(SUB_PID_MIX_OSC1, 0.9f),
    P(SUB_PID_MIX_NOISE, 0.08f),
    P(SUB_PID_FLT_TYPE, 2), P(SUB_PID_FLT_CUTOFF, 420.0f),
    P(SUB_PID_FLT_RESO, 0.25f), P(SUB_PID_FLT_ENV, 2.0f),
    P(SUB_PID_FLT_KBD, 0.4f),
    P(SUB_PID_ENV2_DECAY, 0.08f), P(SUB_PID_ENV2_SUSTAIN, 0.0f),
    P(SUB_PID_ENV1_ATTACK, 0.001f), P(SUB_PID_ENV1_DECAY, 0.45f),
    P(SUB_PID_ENV1_SUSTAIN, 0.0f), P(SUB_PID_ENV1_RELEASE, 0.25f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.18f),
};

/* highpassed noise on a slow LFO — surf, breath, weather */
static const preset_pair_t kSubWindNoise[] = {
    P(SUB_PID_MIX_OSC1, 0.0f), P(SUB_PID_MIX_NOISE, 0.9f),
    P(SUB_PID_FLT_TYPE, 3), P(SUB_PID_FLT_SPREAD, 1.8f),
    P(SUB_PID_FLT_CUTOFF, 1200.0f), P(SUB_PID_FLT_RESO, 0.5f),
    P(SUB_PID_FLT_ENV, 0.0f), P(SUB_PID_FLT_KBD, 0.0f),
    P(SUB_PID_LFO2_RATE, 0.18f), P(SUB_PID_LFO2_CUTOFF, 2.5f),
    P(SUB_PID_ENV1_ATTACK, 1.2f), P(SUB_PID_ENV1_SUSTAIN, 1.0f),
    P(SUB_PID_ENV1_RELEASE, 1.5f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.45f), P(FX_PID_REV_SIZE, 0.8f),
};

/* slow, low, and barely moving — something to put underneath everything */
static const preset_pair_t kSubDarkDrone[] = {
    P(SUB_PID_OSC2_SEMI, -12), P(SUB_PID_OSC2_FINE, -6.0f),
    P(SUB_PID_MIX_OSC1, 0.5f), P(SUB_PID_MIX_OSC2, 0.5f),
    P(SUB_PID_FLT_TYPE, 1), P(SUB_PID_FLT_CUTOFF, 320.0f),
    P(SUB_PID_FLT_RESO, 0.3f), P(SUB_PID_FLT_ENV, 0.5f),
    P(SUB_PID_ENV1_ATTACK, 2.0f), P(SUB_PID_ENV1_SUSTAIN, 1.0f),
    P(SUB_PID_ENV1_RELEASE, 2.5f),
    P(SUB_PID_LFO2_RATE, 0.09f), P(SUB_PID_LFO2_CUTOFF, 1.2f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.5f), P(FX_PID_REV_SIZE, 0.85f),
};

/* highpass keeps the low end out of the way — sits over a bass line */
static const preset_pair_t kSubGlassPad[] = {
    P(SUB_PID_FLT_MODE, 2), P(SUB_PID_FLT_CUTOFF, 700.0f),
    P(SUB_PID_FLT_RESO, 0.4f), P(SUB_PID_FLT_ENV, -1.5f),
    P(SUB_PID_OSC1_WAVE, 1), P(SUB_PID_OSC2_WAVE, 2),
    P(SUB_PID_OSC2_FINE, 9.0f), P(SUB_PID_MIX_OSC2, 0.45f),
    P(SUB_PID_ENV1_ATTACK, 0.8f), P(SUB_PID_ENV1_SUSTAIN, 0.85f),
    P(SUB_PID_ENV1_RELEASE, 1.4f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.35f), P(FX_PID_REV_MIX, 0.45f),
};

/* triangle into a high-Q peak: struck-metal overtone, no FM needed */
static const preset_pair_t kSubBellPluck[] = {
    P(SUB_PID_OSC1_WAVE, 1), P(SUB_PID_OSC2_WAVE, 0),
    P(SUB_PID_OSC2_SEMI, 19), P(SUB_PID_MIX_OSC2, 0.3f),
    P(SUB_PID_FLT_MODE, 4), P(SUB_PID_FLT_CUTOFF, 2400.0f),
    P(SUB_PID_FLT_RESO, 0.85f), P(SUB_PID_FLT_ENV, 1.5f),
    P(SUB_PID_FLT_KBD, 0.9f),
    P(SUB_PID_ENV2_DECAY, 0.25f), P(SUB_PID_ENV2_SUSTAIN, 0.0f),
    P(SUB_PID_ENV1_ATTACK, 0.001f), P(SUB_PID_ENV1_DECAY, 1.6f),
    P(SUB_PID_ENV1_SUSTAIN, 0.0f), P(SUB_PID_ENV1_RELEASE, 1.2f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.35f),
};

/* octave-stacked saws, no detune — clean and wide */
static const preset_pair_t kSubOctaveStack[] = {
    P(SUB_PID_OSC2_SEMI, 12), P(SUB_PID_OSC2_FINE, 0.0f),
    P(SUB_PID_MIX_OSC1, 0.6f), P(SUB_PID_MIX_OSC2, 0.4f),
    P(SUB_PID_FLT_CUTOFF, 3200.0f), P(SUB_PID_FLT_ENV, 1.4f),
    P(SUB_PID_ENV1_SUSTAIN, 0.85f), P(SUB_PID_ENV1_RELEASE, 0.3f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.2f),
};

/* just-detuned keys, short filter blip on every note */
static const preset_pair_t kSubDetunedKeys[] = {
    P(SUB_PID_OSC1_WAVE, 2), P(SUB_PID_OSC2_WAVE, 3),
    P(SUB_PID_OSC2_FINE, -6.0f), P(SUB_PID_OSC2_PW, 0.4f),
    P(SUB_PID_MIX_OSC1, 0.55f), P(SUB_PID_MIX_OSC2, 0.45f),
    P(SUB_PID_FLT_CUTOFF, 1500.0f), P(SUB_PID_FLT_RESO, 0.3f),
    P(SUB_PID_FLT_ENV, 1.8f), P(SUB_PID_FLT_KBD, 0.55f),
    P(SUB_PID_ENV2_DECAY, 0.2f), P(SUB_PID_ENV2_SUSTAIN, 0.15f),
    P(SUB_PID_ENV1_DECAY, 0.8f), P(SUB_PID_ENV1_SUSTAIN, 0.45f),
    P(SUB_PID_ENV1_RELEASE, 0.4f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.2f), P(FX_PID_REV_MIX, 0.25f),
};

/* slow triangle chords, nothing sharp anywhere */
static const preset_pair_t kSubSoftChords[] = {
    P(SUB_PID_OSC1_WAVE, 1), P(SUB_PID_OSC2_WAVE, 1),
    P(SUB_PID_OSC2_FINE, 7.0f),
    P(SUB_PID_MIX_OSC1, 0.55f), P(SUB_PID_MIX_OSC2, 0.45f),
    P(SUB_PID_FLT_CUTOFF, 2200.0f), P(SUB_PID_FLT_ENV, 0.8f),
    P(SUB_PID_ENV1_ATTACK, 0.35f), P(SUB_PID_ENV1_SUSTAIN, 0.9f),
    P(SUB_PID_ENV1_RELEASE, 1.0f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.3f), P(FX_PID_REV_MIX, 0.35f),
};

/* velocity opens the filter and nothing else — expressive comping voice */
static const preset_pair_t kSubVelKeys[] = {
    P(SUB_PID_OSC1_WAVE, 3), P(SUB_PID_OSC1_PW, 0.45f),
    P(SUB_PID_FLT_TYPE, 2), P(SUB_PID_FLT_CUTOFF, 600.0f),
    P(SUB_PID_FLT_RESO, 0.3f), P(SUB_PID_FLT_ENV, 1.2f),
    P(SUB_PID_FLT_KBD, 0.5f),
    P(SUB_PID_ENV1_DECAY, 0.7f), P(SUB_PID_ENV1_SUSTAIN, 0.4f),
    P(SUB_PID_ENV1_RELEASE, 0.35f),
    MOD(0, SYNTH_MOD_SRC_VEL, SUB_PID_FLT_CUTOFF, 0.7f),
    MOD(1, SYNTH_MOD_SRC_VEL, SUB_PID_FLT_DRIVE, 0.4f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.25f),
};

/* sub-octave sine with a long fall — drops under a mix */
static const preset_pair_t kSubDropTail[] = {
    P(SUB_PID_OSC1_WAVE, 0), P(SUB_PID_OSC2_WAVE, 0),
    P(SUB_PID_OSC2_SEMI, -12), P(SUB_PID_OSC2_FINE, 0.0f),
    P(SUB_PID_MIX_OSC1, 0.5f), P(SUB_PID_MIX_OSC2, 0.6f),
    P(SUB_PID_FLT_CUTOFF, 400.0f), P(SUB_PID_FLT_ENV, 0.0f),
    P(SUB_PID_ENV1_ATTACK, 0.005f), P(SUB_PID_ENV1_DECAY, 3.0f),
    P(SUB_PID_ENV1_SUSTAIN, 0.0f), P(SUB_PID_ENV1_RELEASE, 2.0f),
    P(SYNTH_PID_COMMON_GLIDE, 0.5f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.3f),
};

/* square lead with a fifth on top, no filter movement — chiptune-adjacent */
static const preset_pair_t kSubFifthLead[] = {
    P(SUB_PID_OSC1_WAVE, 3), P(SUB_PID_OSC1_PW, 0.5f),
    P(SUB_PID_OSC2_WAVE, 3), P(SUB_PID_OSC2_SEMI, 7),
    P(SUB_PID_OSC2_FINE, 0.0f),
    P(SUB_PID_MIX_OSC1, 0.6f), P(SUB_PID_MIX_OSC2, 0.35f),
    P(SUB_PID_FLT_CUTOFF, 5000.0f), P(SUB_PID_FLT_ENV, 0.0f),
    P(SUB_PID_ENV1_ATTACK, 0.002f), P(SUB_PID_ENV1_SUSTAIN, 1.0f),
    P(SUB_PID_ENV1_RELEASE, 0.05f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.22f), P(FX_PID_DLY_TIME, 0.2f),
};

/* the filter doing all the work on a static waveform — slow bandpass wash */
static const preset_pair_t kSubBandWash[] = {
    P(SUB_PID_FLT_MODE, 6), P(SUB_PID_FLT_TYPE, 1),
    P(SUB_PID_FLT_CUTOFF, 600.0f), P(SUB_PID_FLT_RESO, 0.6f),
    P(SUB_PID_FLT_ENV, 0.0f),
    P(SUB_PID_OSC2_FINE, -10.0f), P(SUB_PID_MIX_OSC2, 0.5f),
    P(SUB_PID_MIX_NOISE, 0.1f),
    P(SUB_PID_LFO2_RATE, 0.12f), P(SUB_PID_LFO2_WAVE, 1),
    P(SUB_PID_LFO2_CUTOFF, 3.5f),
    P(SUB_PID_ENV1_ATTACK, 1.5f), P(SUB_PID_ENV1_SUSTAIN, 1.0f),
    P(SUB_PID_ENV1_RELEASE, 2.0f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.5f), P(FX_PID_REV_SIZE, 0.8f),
};

/* ---- additive (bank 1, linear slots 112-223) -------------------------------- */

#define ADD_P(n) (uint16_t)(ADD_PID_P1_LEVEL + (n) - 1) /* drawbar n, 1-16 */

/* organ drawbars, no sweep, scanner-ish vibrato + light chorus */
static const preset_pair_t kAddTonewheel[] = {
    P(ADD_P(1), 0.32f), P(ADD_P(2), 0.27f), P(ADD_P(3), 0.18f),
    P(ADD_P(4), 0.10f), P(ADD_P(5), 0.06f), P(ADD_P(6), 0.05f),
    P(ADD_P(7), 0.0f), P(ADD_P(8), 0.04f), P(ADD_P(9), 0.0f),
    P(ADD_P(10), 0.0f), P(ADD_P(11), 0.0f), P(ADD_P(12), 0.0f),
    P(ADD_P(13), 0.0f), P(ADD_P(14), 0.0f), P(ADD_P(15), 0.0f),
    P(ADD_P(16), 0.0f),
    P(ADD_PID_BRIGHT, 0.6f), P(ADD_PID_ENV_BRIGHT, 0.0f),
    P(ADD_PID_VEL_BRIGHT, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.002f), P(ADD_PID_ENV1_DECAY, 0.1f),
    P(ADD_PID_ENV1_SUSTAIN, 1.0f), P(ADD_PID_ENV1_RELEASE, 0.06f),
    P(ADD_PID_LFO1_RATE, 6.5f), P(ADD_PID_LFO1_PITCH, 0.05f),
    P(FX_PID_CHO_ON, 1),
    P(FX_PID_CHO_MIX, 0.25f),
};

/* sparse odd partials, slight stretch, long glassy decay */
static const preset_pair_t kAddGlassHarp[] = {
    P(ADD_P(1), 0.55f), P(ADD_P(2), 0.0f), P(ADD_P(3), 0.25f),
    P(ADD_P(4), 0.0f), P(ADD_P(5), 0.12f), P(ADD_P(6), 0.0f),
    P(ADD_P(7), 0.08f), P(ADD_P(8), 0.0f), P(ADD_P(9), 0.0f),
    P(ADD_P(10), 0.0f), P(ADD_P(11), 0.0f), P(ADD_P(12), 0.0f),
    P(ADD_P(13), 0.0f), P(ADD_P(14), 0.0f), P(ADD_P(15), 0.0f),
    P(ADD_P(16), 0.0f),
    P(ADD_PID_INHARM, 0.002f), P(ADD_PID_BRIGHT, 0.5f),
    P(ADD_PID_ENV_BRIGHT, 0.4f), P(ADD_PID_VEL_BRIGHT, 0.4f),
    P(ADD_PID_ENV2_DECAY, 1.2f), P(ADD_PID_ENV2_SUSTAIN, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.004f), P(ADD_PID_ENV1_DECAY, 1.8f),
    P(ADD_PID_ENV1_SUSTAIN, 0.0f), P(ADD_PID_ENV1_RELEASE, 1.2f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.35f), P(FX_PID_REV_SIZE, 0.7f),
};

/* stretched partials on the default spectrum — big church bells */
static const preset_pair_t kAddCarillon[] = {
    P(ADD_PID_INHARM, 0.02f), P(ADD_PID_TILT, -2.0f),
    P(ADD_PID_EVENODD, -0.2f), P(ADD_PID_BRIGHT, 0.55f),
    P(ADD_PID_ENV_BRIGHT, 0.35f),
    P(ADD_PID_ENV2_DECAY, 1.5f), P(ADD_PID_ENV2_SUSTAIN, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.001f), P(ADD_PID_ENV1_DECAY, 2.5f),
    P(ADD_PID_ENV1_SUSTAIN, 0.0f), P(ADD_PID_ENV1_RELEASE, 2.0f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.3f), P(FX_PID_REV_SIZE, 0.7f),
};

/* near-pure fundamental with a soft chiff attack, wheel = vibrato */
static const preset_pair_t kAddChiffFlute[] = {
    P(ADD_P(1), 0.8f), P(ADD_P(2), 0.1f), P(ADD_P(3), 0.05f),
    P(ADD_P(4), 0.0f), P(ADD_P(5), 0.0f), P(ADD_P(6), 0.0f),
    P(ADD_P(7), 0.0f), P(ADD_P(8), 0.0f), P(ADD_P(9), 0.0f),
    P(ADD_P(10), 0.0f), P(ADD_P(11), 0.0f), P(ADD_P(12), 0.0f),
    P(ADD_P(13), 0.0f), P(ADD_P(14), 0.0f), P(ADD_P(15), 0.0f),
    P(ADD_P(16), 0.0f),
    P(ADD_PID_BRIGHT, 0.4f), P(ADD_PID_ENV_BRIGHT, 0.15f),
    P(ADD_PID_VEL_BRIGHT, 0.2f),
    P(ADD_PID_ENV1_ATTACK, 0.06f), P(ADD_PID_ENV1_DECAY, 0.2f),
    P(ADD_PID_ENV1_SUSTAIN, 0.9f), P(ADD_PID_ENV1_RELEASE, 0.25f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, ADD_PID_LFO1_PITCH, 0.3f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.2f),
};

/* odd-leaning soft spectrum swelling bright — vocal-ish pad */
static const preset_pair_t kAddChoirPad[] = {
    P(ADD_PID_EVENODD, -0.35f), P(ADD_PID_BRIGHT, 0.28f),
    P(ADD_PID_ENV_BRIGHT, 0.3f),
    P(ADD_PID_ENV2_ATTACK, 0.8f), P(ADD_PID_ENV2_SUSTAIN, 0.7f),
    P(ADD_PID_ENV1_ATTACK, 0.5f), P(ADD_PID_ENV1_DECAY, 0.5f),
    P(ADD_PID_ENV1_SUSTAIN, 0.95f), P(ADD_PID_ENV1_RELEASE, 1.2f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.4f), P(FX_PID_REV_MIX, 0.4f),
    P(FX_PID_REV_SIZE, 0.75f),
};

/* thin, odd, snappy — clav bite with a velocity-tracked spike */
static const preset_pair_t kAddClavBars[] = {
    P(ADD_PID_EVENODD, -0.6f), P(ADD_PID_TILT, 3.0f),
    P(ADD_PID_BRIGHT, 0.75f), P(ADD_PID_VEL_BRIGHT, 0.5f),
    P(ADD_PID_ENV_BRIGHT, 0.5f),
    P(ADD_PID_ENV2_DECAY, 0.15f), P(ADD_PID_ENV2_SUSTAIN, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.001f), P(ADD_PID_ENV1_DECAY, 0.5f),
    P(ADD_PID_ENV1_SUSTAIN, 0.15f), P(ADD_PID_ENV1_RELEASE, 0.1f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.15f), P(FX_PID_DLY_TIME, 0.28f),
    P(FX_PID_DLY_FB, 0.3f),
};

/* few stretched partials, thumby pluck, ping-pong echo */
static const preset_pair_t kAddKalimba[] = {
    P(ADD_P(1), 0.7f), P(ADD_P(2), 0.0f), P(ADD_P(3), 0.0f),
    P(ADD_P(4), 0.0f), P(ADD_P(5), 0.15f), P(ADD_P(6), 0.0f),
    P(ADD_P(7), 0.0f), P(ADD_P(8), 0.08f), P(ADD_P(9), 0.0f),
    P(ADD_P(10), 0.0f), P(ADD_P(11), 0.0f), P(ADD_P(12), 0.0f),
    P(ADD_P(13), 0.0f), P(ADD_P(14), 0.0f), P(ADD_P(15), 0.0f),
    P(ADD_P(16), 0.0f),
    P(ADD_PID_INHARM, 0.008f), P(ADD_PID_BRIGHT, 0.5f),
    P(ADD_PID_ENV_BRIGHT, 0.4f),
    P(ADD_PID_ENV2_DECAY, 0.2f), P(ADD_PID_ENV2_SUSTAIN, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.001f), P(ADD_PID_ENV1_DECAY, 0.45f),
    P(ADD_PID_ENV1_SUSTAIN, 0.0f), P(ADD_PID_ENV1_RELEASE, 0.35f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.2f), P(FX_PID_DLY_TIME, 0.3f), P(FX_PID_DLY_PP, 1),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.25f),
};

/* even partials take over — hollow octave-up bells */
static const preset_pair_t kAddEvenBells[] = {
    P(ADD_PID_EVENODD, 0.7f), P(ADD_PID_INHARM, 0.014f),
    P(ADD_PID_BRIGHT, 0.6f), P(ADD_PID_ENV_BRIGHT, 0.3f),
    P(ADD_PID_ENV2_DECAY, 1.2f), P(ADD_PID_ENV2_SUSTAIN, 0.0f),
    P(ADD_PID_ENV1_DECAY, 2.0f), P(ADD_PID_ENV1_SUSTAIN, 0.0f),
    P(ADD_PID_ENV1_RELEASE, 1.5f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.35f),
};

/* steep negative tilt, static spectrum, damped room */
static const preset_pair_t kAddDarkOrgan[] = {
    P(ADD_PID_TILT, -6.0f), P(ADD_PID_BRIGHT, 0.3f),
    P(ADD_PID_ENV_BRIGHT, 0.0f), P(ADD_PID_VEL_BRIGHT, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.004f), P(ADD_PID_ENV1_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_RELEASE, 0.1f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.25f), P(FX_PID_REV_DAMP, 0.6f),
};

/* brightness env crawls open over ~3 s while held — spectral riser */
static const preset_pair_t kAddHarmonicRiser[] = {
    P(ADD_PID_BRIGHT, 0.05f), P(ADD_PID_ENV_BRIGHT, 1.0f),
    P(ADD_PID_TILT, -1.0f),
    P(ADD_PID_ENV2_ATTACK, 3.0f), P(ADD_PID_ENV2_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_ATTACK, 0.8f), P(ADD_PID_ENV1_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_RELEASE, 1.0f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.4f),
};

/* slow lfo2 shimmer on the rolloff, soft decaying keys */
static const preset_pair_t kAddShimmerKeys[] = {
    P(ADD_PID_BRIGHT, 0.45f), P(ADD_PID_ENV_BRIGHT, 0.3f),
    P(ADD_PID_VEL_BRIGHT, 0.4f),
    P(ADD_PID_LFO2_RATE, 0.5f), P(ADD_PID_LFO2_BRIGHT, 0.25f),
    P(ADD_PID_ENV1_DECAY, 1.2f), P(ADD_PID_ENV1_SUSTAIN, 0.3f),
    P(ADD_PID_ENV1_RELEASE, 0.8f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.3f), P(FX_PID_REV_MIX, 0.3f),
};

/* unison-widened sustained cluster with a breathing rolloff */
static const preset_pair_t kAddDroneStack[] = {
    P(ADD_PID_INHARM, 0.001f), P(ADD_PID_BRIGHT, 0.4f),
    P(ADD_PID_ENV_BRIGHT, 0.0f),
    P(ADD_PID_LFO2_RATE, 0.15f), P(ADD_PID_LFO2_BRIGHT, 0.2f),
    P(ADD_PID_ENV1_ATTACK, 1.5f), P(ADD_PID_ENV1_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_RELEASE, 2.5f),
    P(SYNTH_PID_COMMON_UNISON, 2), P(SYNTH_PID_COMMON_UNI_DETUNE, 10.0f),
    P(SYNTH_PID_COMMON_UNI_SPREAD, 1.0f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.45f), P(FX_PID_REV_SIZE, 0.85f),
};

/* bright, detuned, positive tilt — cheap and cheerful toy piano */
static const preset_pair_t kAddToyPiano[] = {
    P(ADD_PID_INHARM, 0.01f), P(ADD_PID_TILT, 2.0f),
    P(ADD_PID_BRIGHT, 0.6f), P(ADD_PID_VEL_BRIGHT, 0.5f),
    P(ADD_PID_ENV2_DECAY, 0.3f), P(ADD_PID_ENV2_SUSTAIN, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.001f), P(ADD_PID_ENV1_DECAY, 0.8f),
    P(ADD_PID_ENV1_SUSTAIN, 0.0f), P(ADD_PID_ENV1_RELEASE, 0.5f),
    P(FX_PID_DLY_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_DLY_MIX, 0.12f), P(FX_PID_REV_MIX, 0.2f),
};

/* dark tilted spectrum drifting under a very slow brightness lfo */
static const preset_pair_t kAddDriftPad[] = {
    P(ADD_PID_BRIGHT, 0.3f), P(ADD_PID_TILT, -3.0f),
    P(ADD_PID_LFO2_RATE, 0.2f), P(ADD_PID_LFO2_BRIGHT, 0.35f),
    P(ADD_PID_ENV1_ATTACK, 0.6f), P(ADD_PID_ENV1_SUSTAIN, 0.9f),
    P(ADD_PID_ENV1_RELEASE, 1.5f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.35f), P(FX_PID_REV_MIX, 0.35f),
};

/* fundamental + strong 3rd with a percussive brightness spike */
static const preset_pair_t kAddPercOrgan[] = {
    P(ADD_P(1), 0.4f), P(ADD_P(2), 0.15f), P(ADD_P(3), 0.3f),
    P(ADD_P(4), 0.1f), P(ADD_P(5), 0.05f), P(ADD_P(6), 0.0f),
    P(ADD_P(7), 0.0f), P(ADD_P(8), 0.0f), P(ADD_P(9), 0.0f),
    P(ADD_P(10), 0.0f), P(ADD_P(11), 0.0f), P(ADD_P(12), 0.0f),
    P(ADD_P(13), 0.0f), P(ADD_P(14), 0.0f), P(ADD_P(15), 0.0f),
    P(ADD_P(16), 0.0f),
    P(ADD_PID_BRIGHT, 0.4f), P(ADD_PID_ENV_BRIGHT, 0.6f),
    P(ADD_PID_VEL_BRIGHT, 0.2f),
    P(ADD_PID_ENV2_DECAY, 0.25f), P(ADD_PID_ENV2_SUSTAIN, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.002f), P(ADD_PID_ENV1_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_RELEASE, 0.08f),
};

/* ---- additive, slots 16-47 (S33) ------------------------------------
 *
 * Drawbar registrations mostly, because that is what this engine is for —
 * plus the S33 filter on the ones where a formant or a resonant peak does
 * something 16 sine partials cannot do by themselves. Drawbar sums are kept
 * near 1.0, the engine's gain-staging convention. */

/* every drawbar out: the loudest legal registration */
static const preset_pair_t kAddFullOrgan[] = {
    P(ADD_P(1), 0.22f), P(ADD_P(2), 0.18f), P(ADD_P(3), 0.14f),
    P(ADD_P(4), 0.12f), P(ADD_P(5), 0.09f), P(ADD_P(6), 0.07f),
    P(ADD_P(7), 0.05f), P(ADD_P(8), 0.05f), P(ADD_P(9), 0.03f),
    P(ADD_P(10), 0.02f), P(ADD_P(11), 0.02f), P(ADD_P(12), 0.01f),
    P(ADD_P(13), 0.0f), P(ADD_P(14), 0.0f), P(ADD_P(15), 0.0f),
    P(ADD_P(16), 0.0f),
    P(ADD_PID_BRIGHT, 0.7f), P(ADD_PID_ENV_BRIGHT, 0.0f),
    P(ADD_PID_VEL_BRIGHT, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.003f), P(ADD_PID_ENV1_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_RELEASE, 0.06f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.3f), P(FX_PID_REV_MIX, 0.2f),
};

/* 888 000 000 — the jazz registration, first three drawbars only */
static const preset_pair_t kAddJazzOrgan[] = {
    P(ADD_P(1), 0.42f), P(ADD_P(2), 0.32f), P(ADD_P(3), 0.24f),
    P(ADD_P(4), 0.0f), P(ADD_P(5), 0.0f), P(ADD_P(6), 0.0f),
    P(ADD_P(7), 0.0f), P(ADD_P(8), 0.0f), P(ADD_P(9), 0.0f),
    P(ADD_P(10), 0.0f), P(ADD_P(11), 0.0f), P(ADD_P(12), 0.0f),
    P(ADD_P(13), 0.0f), P(ADD_P(14), 0.0f), P(ADD_P(15), 0.0f),
    P(ADD_P(16), 0.0f),
    P(ADD_PID_BRIGHT, 0.75f), P(ADD_PID_ENV_BRIGHT, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.004f), P(ADD_PID_ENV1_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_RELEASE, 0.05f),
    P(ADD_PID_LFO1_RATE, 6.8f), P(ADD_PID_LFO1_PITCH, 0.04f),
    P(FX_PID_CHO_ON, 1),
    P(FX_PID_CHO_MIX, 0.35f),
};

/* fundamental, fifth and octave pulled hard — gospel bark */
static const preset_pair_t kAddGospelOrgan[] = {
    P(ADD_P(1), 0.34f), P(ADD_P(2), 0.10f), P(ADD_P(3), 0.28f),
    P(ADD_P(4), 0.08f), P(ADD_P(5), 0.06f), P(ADD_P(6), 0.06f),
    P(ADD_P(7), 0.0f), P(ADD_P(8), 0.08f), P(ADD_P(9), 0.0f),
    P(ADD_P(10), 0.0f), P(ADD_P(11), 0.0f), P(ADD_P(12), 0.0f),
    P(ADD_P(13), 0.0f), P(ADD_P(14), 0.0f), P(ADD_P(15), 0.0f),
    P(ADD_P(16), 0.0f),
    P(ADD_PID_BRIGHT, 0.8f), P(ADD_PID_ENV_BRIGHT, 0.1f),
    P(ADD_PID_ENV1_ATTACK, 0.002f), P(ADD_PID_ENV1_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_RELEASE, 0.05f),
    P(ADD_PID_FLT_ON, 1), P(ADD_PID_FLT_DRIVE, 0.45f),
    P(ADD_PID_FLT_CUTOFF, 5000.0f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.3f), P(FX_PID_REV_MIX, 0.2f),
};

/* odd partials only: the stopped-pipe / clarinet spectrum */
static const preset_pair_t kAddReedPipe[] = {
    P(ADD_PID_EVENODD, -0.85f), P(ADD_PID_TILT, -3.0f),
    P(ADD_PID_BRIGHT, 0.6f), P(ADD_PID_ENV_BRIGHT, 0.15f),
    P(ADD_PID_VEL_BRIGHT, 0.2f),
    P(ADD_PID_ENV1_ATTACK, 0.05f), P(ADD_PID_ENV1_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_RELEASE, 0.12f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.3f), P(FX_PID_REV_SIZE, 0.65f),
};

/* soft principal rank, slow speech */
static const preset_pair_t kAddPrincipalPipe[] = {
    P(ADD_P(1), 0.45f), P(ADD_P(2), 0.22f), P(ADD_P(3), 0.14f),
    P(ADD_P(4), 0.09f), P(ADD_P(5), 0.05f), P(ADD_P(6), 0.03f),
    P(ADD_P(7), 0.02f), P(ADD_P(8), 0.0f), P(ADD_P(9), 0.0f),
    P(ADD_P(10), 0.0f), P(ADD_P(11), 0.0f), P(ADD_P(12), 0.0f),
    P(ADD_P(13), 0.0f), P(ADD_P(14), 0.0f), P(ADD_P(15), 0.0f),
    P(ADD_P(16), 0.0f),
    P(ADD_PID_BRIGHT, 0.5f), P(ADD_PID_ENV_BRIGHT, 0.25f),
    P(ADD_PID_ENV1_ATTACK, 0.12f), P(ADD_PID_ENV1_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_RELEASE, 0.5f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.45f), P(FX_PID_REV_SIZE, 0.85f),
};

/* octave-only registration: 16', 8', 4', 2' and nothing between */
static const preset_pair_t kAddOctaveOrgan[] = {
    P(ADD_P(1), 0.34f), P(ADD_P(2), 0.28f), P(ADD_P(3), 0.0f),
    P(ADD_P(4), 0.22f), P(ADD_P(5), 0.0f), P(ADD_P(6), 0.0f),
    P(ADD_P(7), 0.0f), P(ADD_P(8), 0.16f), P(ADD_P(9), 0.0f),
    P(ADD_P(10), 0.0f), P(ADD_P(11), 0.0f), P(ADD_P(12), 0.0f),
    P(ADD_P(13), 0.0f), P(ADD_P(14), 0.0f), P(ADD_P(15), 0.0f),
    P(ADD_P(16), 0.0f),
    P(ADD_PID_BRIGHT, 0.7f), P(ADD_PID_ENV_BRIGHT, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.003f), P(ADD_PID_ENV1_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_RELEASE, 0.06f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.2f), P(FX_PID_REV_MIX, 0.2f),
};

/* quint-heavy: the hollow fifth registration */
static const preset_pair_t kAddFifthOrgan[] = {
    P(ADD_P(1), 0.36f), P(ADD_P(2), 0.0f), P(ADD_P(3), 0.30f),
    P(ADD_P(4), 0.0f), P(ADD_P(5), 0.0f), P(ADD_P(6), 0.20f),
    P(ADD_P(7), 0.0f), P(ADD_P(8), 0.0f), P(ADD_P(9), 0.10f),
    P(ADD_P(10), 0.0f), P(ADD_P(11), 0.0f), P(ADD_P(12), 0.0f),
    P(ADD_P(13), 0.0f), P(ADD_P(14), 0.0f), P(ADD_P(15), 0.0f),
    P(ADD_P(16), 0.0f),
    P(ADD_PID_BRIGHT, 0.68f), P(ADD_PID_ENV_BRIGHT, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.004f), P(ADD_PID_ENV1_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_RELEASE, 0.07f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.25f),
};

/* the vowel filter over a full spectrum: a choir that actually says "ah" */
static const preset_pair_t kAddVocalAh[] = {
    P(ADD_PID_TILT, -4.0f), P(ADD_PID_BRIGHT, 0.75f),
    P(ADD_PID_ENV_BRIGHT, 0.1f), P(ADD_PID_VEL_BRIGHT, 0.15f),
    P(ADD_PID_FLT_ON, 1), P(ADD_PID_FLT_TYPE, 4),
    P(ADD_PID_FLT_VOWEL, 0.0f), P(ADD_PID_FLT_RESO, 0.55f),
    P(ADD_PID_FLT_CUTOFF, 1000.0f),
    P(ADD_PID_ENV1_ATTACK, 0.25f), P(ADD_PID_ENV1_SUSTAIN, 0.95f),
    P(ADD_PID_ENV1_RELEASE, 0.8f),
    P(ADD_PID_LFO1_RATE, 5.2f), P(ADD_PID_LFO1_PITCH, 0.06f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.45f), P(FX_PID_REV_SIZE, 0.75f),
};

/* same idea parked on "oo" — darker, further back in the mouth */
static const preset_pair_t kAddVocalOoh[] = {
    P(ADD_PID_TILT, -5.0f), P(ADD_PID_BRIGHT, 0.7f),
    P(ADD_PID_FLT_ON, 1), P(ADD_PID_FLT_TYPE, 4),
    P(ADD_PID_FLT_VOWEL, 1.0f), P(ADD_PID_FLT_RESO, 0.6f),
    P(ADD_PID_FLT_CUTOFF, 900.0f),
    P(ADD_PID_ENV1_ATTACK, 0.4f), P(ADD_PID_ENV1_SUSTAIN, 0.95f),
    P(ADD_PID_ENV1_RELEASE, 1.0f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.25f), P(FX_PID_REV_MIX, 0.5f),
};

/* env2 sweeps the morph, so every note speaks a -> e -> i on its own */
static const preset_pair_t kAddFormantChoir[] = {
    P(ADD_PID_TILT, -3.5f), P(ADD_PID_BRIGHT, 0.72f),
    P(ADD_PID_FLT_ON, 1), P(ADD_PID_FLT_TYPE, 4),
    P(ADD_PID_FLT_VOWEL, 0.0f), P(ADD_PID_FLT_RESO, 0.65f),
    P(ADD_PID_FLT_CUTOFF, 1050.0f),
    P(ADD_PID_ENV2_ATTACK, 0.6f), P(ADD_PID_ENV2_DECAY, 1.5f),
    P(ADD_PID_ENV2_SUSTAIN, 0.7f),
    P(ADD_PID_ENV1_ATTACK, 0.3f), P(ADD_PID_ENV1_SUSTAIN, 0.9f),
    P(ADD_PID_ENV1_RELEASE, 1.2f),
    MOD(0, SYNTH_MOD_SRC_ENV2, ADD_PID_FLT_VOWEL, 0.55f),
    MOD(1, SYNTH_MOD_SRC_WHEEL, ADD_PID_FLT_VOWEL, 0.4f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.5f), P(FX_PID_REV_SIZE, 0.8f),
};

/* struck metal bar with a soft mallet */
static const preset_pair_t kAddVibraphone[] = {
    P(ADD_P(1), 0.55f), P(ADD_P(2), 0.0f), P(ADD_P(3), 0.0f),
    P(ADD_P(4), 0.22f), P(ADD_P(5), 0.0f), P(ADD_P(6), 0.0f),
    P(ADD_P(7), 0.0f), P(ADD_P(8), 0.0f), P(ADD_P(9), 0.10f),
    P(ADD_P(10), 0.0f), P(ADD_P(11), 0.0f), P(ADD_P(12), 0.0f),
    P(ADD_P(13), 0.0f), P(ADD_P(14), 0.0f), P(ADD_P(15), 0.0f),
    P(ADD_P(16), 0.0f),
    P(ADD_PID_INHARM, 0.0008f), P(ADD_PID_BRIGHT, 0.55f),
    P(ADD_PID_ENV_BRIGHT, 0.35f), P(ADD_PID_VEL_BRIGHT, 0.45f),
    P(ADD_PID_ENV2_DECAY, 0.6f), P(ADD_PID_ENV2_SUSTAIN, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.002f), P(ADD_PID_ENV1_DECAY, 2.2f),
    P(ADD_PID_ENV1_SUSTAIN, 0.0f), P(ADD_PID_ENV1_RELEASE, 1.5f),
    P(ADD_PID_LFO2_RATE, 5.0f), P(ADD_PID_LFO2_BRIGHT, 0.25f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.35f),
};

/* wooden bar: the 4th partial carries it, and it is gone in half a second */
static const preset_pair_t kAddMarimba[] = {
    P(ADD_P(1), 0.52f), P(ADD_P(2), 0.04f), P(ADD_P(3), 0.06f),
    P(ADD_P(4), 0.30f), P(ADD_P(5), 0.0f), P(ADD_P(6), 0.0f),
    P(ADD_P(7), 0.0f), P(ADD_P(8), 0.06f), P(ADD_P(9), 0.0f),
    P(ADD_P(10), 0.0f), P(ADD_P(11), 0.0f), P(ADD_P(12), 0.0f),
    P(ADD_P(13), 0.0f), P(ADD_P(14), 0.0f), P(ADD_P(15), 0.0f),
    P(ADD_P(16), 0.0f),
    P(ADD_PID_BRIGHT, 0.45f), P(ADD_PID_ENV_BRIGHT, 0.5f),
    P(ADD_PID_VEL_BRIGHT, 0.55f),
    P(ADD_PID_ENV2_ATTACK, 0.002f), P(ADD_PID_ENV2_DECAY, 0.12f),
    P(ADD_PID_ENV2_SUSTAIN, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.001f), P(ADD_PID_ENV1_DECAY, 0.55f),
    P(ADD_PID_ENV1_SUSTAIN, 0.0f), P(ADD_PID_ENV1_RELEASE, 0.35f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.25f),
};

/* small, bright, quick — a celeste rather than a bell */
static const preset_pair_t kAddCeleste[] = {
    P(ADD_P(1), 0.42f), P(ADD_P(2), 0.20f), P(ADD_P(3), 0.05f),
    P(ADD_P(4), 0.18f), P(ADD_P(5), 0.0f), P(ADD_P(6), 0.0f),
    P(ADD_P(7), 0.0f), P(ADD_P(8), 0.10f), P(ADD_P(9), 0.0f),
    P(ADD_P(10), 0.0f), P(ADD_P(11), 0.0f), P(ADD_P(12), 0.0f),
    P(ADD_P(13), 0.0f), P(ADD_P(14), 0.0f), P(ADD_P(15), 0.0f),
    P(ADD_P(16), 0.0f),
    P(ADD_PID_INHARM, 0.0015f), P(ADD_PID_BRIGHT, 0.6f),
    P(ADD_PID_ENV_BRIGHT, 0.4f), P(ADD_PID_VEL_BRIGHT, 0.5f),
    P(ADD_PID_ENV2_DECAY, 0.35f), P(ADD_PID_ENV2_SUSTAIN, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.001f), P(ADD_PID_ENV1_DECAY, 1.4f),
    P(ADD_PID_ENV1_SUSTAIN, 0.0f), P(ADD_PID_ENV1_RELEASE, 0.9f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.4f),
};

/* heavy stretch: the partials stop being harmonics and start being a gong */
static const preset_pair_t kAddGong[] = {
    P(ADD_PID_INHARM, 0.045f), P(ADD_PID_TILT, -1.5f),
    P(ADD_PID_EVENODD, 0.25f), P(ADD_PID_BRIGHT, 0.6f),
    P(ADD_PID_ENV_BRIGHT, 0.45f), P(ADD_PID_VEL_BRIGHT, 0.35f),
    P(ADD_PID_ENV2_ATTACK, 0.02f), P(ADD_PID_ENV2_DECAY, 2.5f),
    P(ADD_PID_ENV2_SUSTAIN, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.008f), P(ADD_PID_ENV1_DECAY, 4.0f),
    P(ADD_PID_ENV1_SUSTAIN, 0.0f), P(ADD_PID_ENV1_RELEASE, 3.0f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.5f), P(FX_PID_REV_SIZE, 0.9f),
};

/* two close partials beating against each other, forever */
static const preset_pair_t kAddSingingBowl[] = {
    P(ADD_P(1), 0.5f), P(ADD_P(2), 0.0f), P(ADD_P(3), 0.28f),
    P(ADD_P(4), 0.0f), P(ADD_P(5), 0.14f), P(ADD_P(6), 0.0f),
    P(ADD_P(7), 0.08f), P(ADD_P(8), 0.0f), P(ADD_P(9), 0.0f),
    P(ADD_P(10), 0.0f), P(ADD_P(11), 0.0f), P(ADD_P(12), 0.0f),
    P(ADD_P(13), 0.0f), P(ADD_P(14), 0.0f), P(ADD_P(15), 0.0f),
    P(ADD_P(16), 0.0f),
    P(ADD_PID_INHARM, 0.006f), P(ADD_PID_BRIGHT, 0.5f),
    P(ADD_PID_ENV_BRIGHT, 0.3f),
    P(ADD_PID_ENV1_ATTACK, 0.03f), P(ADD_PID_ENV1_DECAY, 6.0f),
    P(ADD_PID_ENV1_SUSTAIN, 0.15f), P(ADD_PID_ENV1_RELEASE, 4.0f),
    P(ADD_PID_LFO2_RATE, 0.15f), P(ADD_PID_LFO2_BRIGHT, 0.2f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.55f), P(FX_PID_REV_SIZE, 0.9f),
};

/* free-reed: strong odds, fixed spectrum, bellows-slow attack */
static const preset_pair_t kAddHarmonium[] = {
    P(ADD_PID_EVENODD, -0.45f), P(ADD_PID_TILT, -2.0f),
    P(ADD_PID_BRIGHT, 0.68f), P(ADD_PID_ENV_BRIGHT, 0.1f),
    P(ADD_PID_ENV1_ATTACK, 0.09f), P(ADD_PID_ENV1_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_RELEASE, 0.2f),
    P(ADD_PID_FLT_ON, 1), P(ADD_PID_FLT_CUTOFF, 3500.0f),
    P(ADD_PID_FLT_RESO, 0.2f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.2f), P(FX_PID_REV_MIX, 0.3f),
};

/* the same reed with a bandpass squeeze and a faster bellow */
static const preset_pair_t kAddAccordion[] = {
    P(ADD_PID_EVENODD, -0.3f), P(ADD_PID_TILT, -1.0f),
    P(ADD_PID_BRIGHT, 0.75f), P(ADD_PID_ENV_BRIGHT, 0.05f),
    P(ADD_PID_ENV1_ATTACK, 0.03f), P(ADD_PID_ENV1_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_RELEASE, 0.1f),
    P(ADD_PID_FLT_ON, 1), P(ADD_PID_FLT_TYPE, 3),
    P(ADD_PID_FLT_SPREAD, 2.5f), P(ADD_PID_FLT_CUTOFF, 1400.0f),
    P(ADD_PID_FLT_RESO, 0.3f),
    P(ADD_PID_LFO1_RATE, 6.0f), P(ADD_PID_LFO1_PITCH, 0.08f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.3f), P(FX_PID_REV_MIX, 0.22f),
};

/* saw-ish drawbar ramp under a slow filter — an ensemble without a chorus */
static const preset_pair_t kAddStringStack[] = {
    P(ADD_PID_TILT, -6.0f), P(ADD_PID_BRIGHT, 0.62f),
    P(ADD_PID_ENV_BRIGHT, 0.3f), P(ADD_PID_VEL_BRIGHT, 0.25f),
    P(ADD_PID_ENV2_ATTACK, 0.4f), P(ADD_PID_ENV2_DECAY, 2.0f),
    P(ADD_PID_ENV2_SUSTAIN, 0.6f),
    P(ADD_PID_ENV1_ATTACK, 0.3f), P(ADD_PID_ENV1_SUSTAIN, 0.95f),
    P(ADD_PID_ENV1_RELEASE, 1.0f),
    P(ADD_PID_LFO1_RATE, 4.5f), P(ADD_PID_LFO1_PITCH, 0.05f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.35f), P(FX_PID_REV_MIX, 0.4f),
};

/* bright even harmonics + drive: additive brass, no filter sweep needed */
static const preset_pair_t kAddBrassStack[] = {
    P(ADD_PID_TILT, -4.5f), P(ADD_PID_EVENODD, 0.15f),
    P(ADD_PID_BRIGHT, 0.5f), P(ADD_PID_ENV_BRIGHT, 0.5f),
    P(ADD_PID_VEL_BRIGHT, 0.5f),
    P(ADD_PID_ENV2_ATTACK, 0.06f), P(ADD_PID_ENV2_DECAY, 0.5f),
    P(ADD_PID_ENV2_SUSTAIN, 0.75f),
    P(ADD_PID_ENV1_ATTACK, 0.03f), P(ADD_PID_ENV1_SUSTAIN, 0.9f),
    P(ADD_PID_ENV1_RELEASE, 0.2f),
    P(ADD_PID_FLT_ON, 1), P(ADD_PID_FLT_DRIVE, 0.5f),
    P(ADD_PID_FLT_CUTOFF, 4000.0f), P(ADD_PID_FLT_RESO, 0.25f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.25f),
};

/* ladder lowpass on a drawbar spectrum: warm in a way the rolloff is not */
static const preset_pair_t kAddWarmLadder[] = {
    P(ADD_P(1), 0.4f), P(ADD_P(2), 0.24f), P(ADD_P(3), 0.16f),
    P(ADD_P(4), 0.1f), P(ADD_P(5), 0.06f), P(ADD_P(6), 0.04f),
    P(ADD_P(7), 0.0f), P(ADD_P(8), 0.0f), P(ADD_P(9), 0.0f),
    P(ADD_P(10), 0.0f), P(ADD_P(11), 0.0f), P(ADD_P(12), 0.0f),
    P(ADD_P(13), 0.0f), P(ADD_P(14), 0.0f), P(ADD_P(15), 0.0f),
    P(ADD_P(16), 0.0f),
    P(ADD_PID_BRIGHT, 0.85f),
    P(ADD_PID_FLT_ON, 1), P(ADD_PID_FLT_TYPE, 2),
    P(ADD_PID_FLT_CUTOFF, 800.0f), P(ADD_PID_FLT_RESO, 0.45f),
    P(ADD_PID_FLT_ENV, 2.0f), P(ADD_PID_FLT_KBD, 0.5f),
    P(ADD_PID_ENV2_DECAY, 0.5f), P(ADD_PID_ENV2_SUSTAIN, 0.2f),
    P(ADD_PID_ENV1_DECAY, 1.0f), P(ADD_PID_ENV1_SUSTAIN, 0.5f),
    P(ADD_PID_ENV1_RELEASE, 0.4f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.3f),
};

/* a notch wandering through a static spectrum */
static const preset_pair_t kAddNotchDrift[] = {
    P(ADD_PID_TILT, -3.0f), P(ADD_PID_BRIGHT, 0.8f),
    P(ADD_PID_ENV_BRIGHT, 0.0f),
    P(ADD_PID_FLT_ON, 1), P(ADD_PID_FLT_MODE, 3),
    P(ADD_PID_FLT_CUTOFF, 900.0f), P(ADD_PID_FLT_RESO, 0.55f),
    P(ADD_PID_ENV1_ATTACK, 0.6f), P(ADD_PID_ENV1_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_RELEASE, 1.5f),
    P(ADD_PID_LFO2_RATE, 0.2f),
    MOD(0, SYNTH_MOD_SRC_LFO2, ADD_PID_FLT_CUTOFF, 0.45f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.45f), P(FX_PID_REV_SIZE, 0.8f),
};

/* five partials, nothing else — the sound of the engine at its simplest */
static const preset_pair_t kAddPureTones[] = {
    P(ADD_P(1), 0.5f), P(ADD_P(2), 0.25f), P(ADD_P(3), 0.13f),
    P(ADD_P(4), 0.07f), P(ADD_P(5), 0.05f), P(ADD_P(6), 0.0f),
    P(ADD_P(7), 0.0f), P(ADD_P(8), 0.0f), P(ADD_P(9), 0.0f),
    P(ADD_P(10), 0.0f), P(ADD_P(11), 0.0f), P(ADD_P(12), 0.0f),
    P(ADD_P(13), 0.0f), P(ADD_P(14), 0.0f), P(ADD_P(15), 0.0f),
    P(ADD_P(16), 0.0f),
    P(ADD_PID_BRIGHT, 0.8f), P(ADD_PID_ENV_BRIGHT, 0.15f),
    P(ADD_PID_ENV1_ATTACK, 0.02f), P(ADD_PID_ENV1_SUSTAIN, 0.9f),
    P(ADD_PID_ENV1_RELEASE, 0.35f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.3f),
};

/* the fundamental alone: a test tone that happens to be musical */
static const preset_pair_t kAddSineKeys[] = {
    P(ADD_P(1), 1.0f), P(ADD_P(2), 0.0f), P(ADD_P(3), 0.0f),
    P(ADD_P(4), 0.0f), P(ADD_P(5), 0.0f), P(ADD_P(6), 0.0f),
    P(ADD_P(7), 0.0f), P(ADD_P(8), 0.0f), P(ADD_P(9), 0.0f),
    P(ADD_P(10), 0.0f), P(ADD_P(11), 0.0f), P(ADD_P(12), 0.0f),
    P(ADD_P(13), 0.0f), P(ADD_P(14), 0.0f), P(ADD_P(15), 0.0f),
    P(ADD_P(16), 0.0f),
    P(ADD_PID_BRIGHT, 1.0f), P(ADD_PID_ENV_BRIGHT, 0.0f),
    P(ADD_PID_VEL_BRIGHT, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.01f), P(ADD_PID_ENV1_DECAY, 1.0f),
    P(ADD_PID_ENV1_SUSTAIN, 0.6f), P(ADD_PID_ENV1_RELEASE, 0.5f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.25f),
};

/* tuned percussion with a hard mallet and a short bloom */
static const preset_pair_t kAddSteelDrum[] = {
    P(ADD_P(1), 0.4f), P(ADD_P(2), 0.26f), P(ADD_P(3), 0.06f),
    P(ADD_P(4), 0.16f), P(ADD_P(5), 0.0f), P(ADD_P(6), 0.05f),
    P(ADD_P(7), 0.0f), P(ADD_P(8), 0.07f), P(ADD_P(9), 0.0f),
    P(ADD_P(10), 0.0f), P(ADD_P(11), 0.0f), P(ADD_P(12), 0.0f),
    P(ADD_P(13), 0.0f), P(ADD_P(14), 0.0f), P(ADD_P(15), 0.0f),
    P(ADD_P(16), 0.0f),
    P(ADD_PID_INHARM, 0.003f), P(ADD_PID_BRIGHT, 0.5f),
    P(ADD_PID_ENV_BRIGHT, 0.55f), P(ADD_PID_VEL_BRIGHT, 0.5f),
    P(ADD_PID_ENV2_ATTACK, 0.006f), P(ADD_PID_ENV2_DECAY, 0.25f),
    P(ADD_PID_ENV2_SUSTAIN, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.001f), P(ADD_PID_ENV1_DECAY, 1.1f),
    P(ADD_PID_ENV1_SUSTAIN, 0.0f), P(ADD_PID_ENV1_RELEASE, 0.7f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.3f),
};

/* high, thin, and very short — a bell tree struck once */
static const preset_pair_t kAddBellTree[] = {
    P(ADD_PID_INHARM, 0.02f), P(ADD_PID_TILT, 3.0f),
    P(ADD_PID_EVENODD, 0.3f), P(ADD_PID_BRIGHT, 0.9f),
    P(ADD_PID_ENV_BRIGHT, 0.2f), P(ADD_PID_VEL_BRIGHT, 0.4f),
    P(ADD_PID_ENV1_ATTACK, 0.001f), P(ADD_PID_ENV1_DECAY, 0.6f),
    P(ADD_PID_ENV1_SUSTAIN, 0.0f), P(ADD_PID_ENV1_RELEASE, 0.4f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.5f), P(FX_PID_REV_SIZE, 0.75f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.2f), P(FX_PID_DLY_TIME, 0.18f),
};

/* brightness envelope inverted: opens as it dies away */
static const preset_pair_t kAddReverseBloom[] = {
    P(ADD_PID_TILT, -5.0f), P(ADD_PID_BRIGHT, 0.25f),
    P(ADD_PID_ENV_BRIGHT, -0.6f), P(ADD_PID_VEL_BRIGHT, 0.0f),
    P(ADD_PID_ENV2_ATTACK, 0.005f), P(ADD_PID_ENV2_DECAY, 2.0f),
    P(ADD_PID_ENV2_SUSTAIN, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.6f), P(ADD_PID_ENV1_SUSTAIN, 0.85f),
    P(ADD_PID_ENV1_RELEASE, 1.8f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.5f), P(FX_PID_REV_SIZE, 0.85f),
};

/* stretched and slow: metal sheet rather than a note */
static const preset_pair_t kAddMetalDrone[] = {
    P(ADD_PID_INHARM, 0.03f), P(ADD_PID_EVENODD, -0.2f),
    P(ADD_PID_TILT, -2.0f), P(ADD_PID_BRIGHT, 0.55f),
    P(ADD_PID_ENV_BRIGHT, 0.25f),
    P(ADD_PID_FLT_ON, 1), P(ADD_PID_FLT_MODE, 4),
    P(ADD_PID_FLT_CUTOFF, 1600.0f), P(ADD_PID_FLT_RESO, 0.6f),
    P(ADD_PID_ENV1_ATTACK, 1.5f), P(ADD_PID_ENV1_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_RELEASE, 2.5f),
    P(ADD_PID_LFO2_RATE, 0.11f), P(ADD_PID_LFO2_BRIGHT, 0.3f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.55f), P(FX_PID_REV_SIZE, 0.9f),
};

/* velocity picks the register: soft is a flute, hard is a reed */
static const preset_pair_t kAddVelReed[] = {
    P(ADD_PID_TILT, -4.0f), P(ADD_PID_EVENODD, -0.5f),
    P(ADD_PID_BRIGHT, 0.3f), P(ADD_PID_ENV_BRIGHT, 0.25f),
    P(ADD_PID_VEL_BRIGHT, 0.85f),
    P(ADD_PID_ENV1_ATTACK, 0.04f), P(ADD_PID_ENV1_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_RELEASE, 0.18f),
    MOD(0, SYNTH_MOD_SRC_VEL, ADD_PID_FLT_CUTOFF, 0.4f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.32f),
};

/* wheel walks the whole spectrum open — a hands-on riser */
static const preset_pair_t kAddWheelSweep[] = {
    P(ADD_PID_TILT, -3.0f), P(ADD_PID_BRIGHT, 0.15f),
    P(ADD_PID_ENV_BRIGHT, 0.0f), P(ADD_PID_VEL_BRIGHT, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.05f), P(ADD_PID_ENV1_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_RELEASE, 0.6f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, ADD_PID_BRIGHT, 0.85f),
    P(FX_PID_REV_ON, 1), P(FX_PID_DLY_ON, 1),
    P(FX_PID_REV_MIX, 0.4f), P(FX_PID_DLY_MIX, 0.2f),
};

/* octave doubling with a bright top: additive celesta-organ hybrid */
static const preset_pair_t kAddGlassOrgan[] = {
    P(ADD_P(1), 0.36f), P(ADD_P(2), 0.20f), P(ADD_P(3), 0.0f),
    P(ADD_P(4), 0.16f), P(ADD_P(5), 0.0f), P(ADD_P(6), 0.0f),
    P(ADD_P(7), 0.0f), P(ADD_P(8), 0.14f), P(ADD_P(9), 0.0f),
    P(ADD_P(10), 0.0f), P(ADD_P(11), 0.0f), P(ADD_P(12), 0.0f),
    P(ADD_P(13), 0.0f), P(ADD_P(14), 0.0f), P(ADD_P(15), 0.0f),
    P(ADD_P(16), 0.10f),
    P(ADD_PID_INHARM, 0.0004f), P(ADD_PID_BRIGHT, 0.8f),
    P(ADD_PID_ENV_BRIGHT, 0.15f),
    P(ADD_PID_ENV1_ATTACK, 0.006f), P(ADD_PID_ENV1_DECAY, 1.5f),
    P(ADD_PID_ENV1_SUSTAIN, 0.55f), P(ADD_PID_ENV1_RELEASE, 0.8f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.25f), P(FX_PID_REV_MIX, 0.4f),
};

/* a wide dual band on a dense spectrum — hollow, vocal-adjacent pad */
static const preset_pair_t kAddHollowPad[] = {
    P(ADD_PID_TILT, -2.5f), P(ADD_PID_BRIGHT, 0.78f),
    P(ADD_PID_ENV_BRIGHT, 0.1f),
    P(ADD_PID_FLT_ON, 1), P(ADD_PID_FLT_TYPE, 3),
    P(ADD_PID_FLT_SPREAD, 1.5f), P(ADD_PID_FLT_CUTOFF, 1100.0f),
    P(ADD_PID_FLT_RESO, 0.45f), P(ADD_PID_FLT_ENV, 1.2f),
    P(ADD_PID_ENV2_ATTACK, 0.5f), P(ADD_PID_ENV2_DECAY, 2.0f),
    P(ADD_PID_ENV2_SUSTAIN, 0.5f),
    P(ADD_PID_ENV1_ATTACK, 0.5f), P(ADD_PID_ENV1_SUSTAIN, 0.9f),
    P(ADD_PID_ENV1_RELEASE, 1.4f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.48f), P(FX_PID_REV_SIZE, 0.8f),
};

/* one drawbar per octave and a very long tail: additive sub-bass */
static const preset_pair_t kAddSubDrone[] = {
    P(ADD_P(1), 0.7f), P(ADD_P(2), 0.2f), P(ADD_P(3), 0.0f),
    P(ADD_P(4), 0.08f), P(ADD_P(5), 0.0f), P(ADD_P(6), 0.0f),
    P(ADD_P(7), 0.0f), P(ADD_P(8), 0.0f), P(ADD_P(9), 0.0f),
    P(ADD_P(10), 0.0f), P(ADD_P(11), 0.0f), P(ADD_P(12), 0.0f),
    P(ADD_P(13), 0.0f), P(ADD_P(14), 0.0f), P(ADD_P(15), 0.0f),
    P(ADD_P(16), 0.0f),
    P(ADD_PID_BRIGHT, 0.4f), P(ADD_PID_ENV_BRIGHT, 0.1f),
    P(ADD_PID_ENV1_ATTACK, 0.8f), P(ADD_PID_ENV1_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_RELEASE, 2.0f),
    P(ADD_PID_FLT_ON, 1), P(ADD_PID_FLT_TYPE, 1),
    P(ADD_PID_FLT_CUTOFF, 500.0f), P(ADD_PID_FLT_RESO, 0.15f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.35f),
};

/* ---- fm (bank 2, linear slots 224-335) ------------------------------------- */

/* harder e-piano: more index, hotter tine pair */
static const preset_pair_t kFmBrightTines[] = {
    P(FM_PID_A_INDEX, 2.8f), P(FM_PID_A_LEVEL, 0.7f),
    P(FM_PID_B_LEVEL, 0.3f), P(FM_PID_B_INDEX, 1.6f),
    P(FM_PID_B_DETUNE, 5.0f), P(FM_PID_VEL_INDEX, 0.8f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.3f), P(FX_PID_REV_MIX, 0.2f),
};

/* sub-octave carrier, gritty index bite, quick mod decay */
static const preset_pair_t kFmGrowlBass[] = {
    P(FM_PID_A_CRATIO, 0.5f), P(FM_PID_A_MRATIO, 0.5f),
    P(FM_PID_A_INDEX, 4.5f), P(FM_PID_A_FB, 0.25f),
    P(FM_PID_A_LEVEL, 0.85f),
    P(FM_PID_A_ENV_A, 0.002f), P(FM_PID_A_ENV_D, 0.5f),
    P(FM_PID_A_ENV_S, 0.6f), P(FM_PID_A_ENV_R, 0.1f),
    P(FM_PID_A_MENV_D, 0.15f), P(FM_PID_A_MENV_S, 0.2f),
    P(FM_PID_B_CRATIO, 1.0f), P(FM_PID_B_MRATIO, 1.0f),
    P(FM_PID_B_INDEX, 2.0f), P(FM_PID_B_LEVEL, 0.15f),
    P(FM_PID_B_ENV_D, 0.2f), P(FM_PID_B_ENV_S, 0.0f),
    P(FM_PID_VEL_INDEX, 0.7f),
};

/* 3.5:1 body + a high inharmonic pair — long tubular decay */
static const preset_pair_t kFmTubularBell[] = {
    P(FM_PID_A_MRATIO, 3.5f), P(FM_PID_A_INDEX, 6.0f),
    P(FM_PID_A_LEVEL, 0.75f),
    P(FM_PID_A_ENV_A, 0.001f), P(FM_PID_A_ENV_D, 3.0f),
    P(FM_PID_A_ENV_S, 0.0f), P(FM_PID_A_ENV_R, 2.5f),
    P(FM_PID_A_MENV_D, 2.0f), P(FM_PID_A_MENV_S, 0.0f),
    P(FM_PID_B_CRATIO, 5.2f), P(FM_PID_B_MRATIO, 3.7f),
    P(FM_PID_B_INDEX, 3.0f), P(FM_PID_B_LEVEL, 0.25f),
    P(FM_PID_B_ENV_D, 1.5f), P(FM_PID_B_ENV_S, 0.0f),
    P(FM_PID_B_ENV_R, 1.5f),
    P(FM_PID_B_MENV_D, 1.0f), P(FM_PID_B_MENV_S, 0.0f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.35f), P(FX_PID_REV_SIZE, 0.7f),
};

/* feedback grit, low index, short dark bark on the second pair */
static const preset_pair_t kFmWurli[] = {
    P(FM_PID_A_INDEX, 1.4f), P(FM_PID_A_FB, 0.35f),
    P(FM_PID_A_LEVEL, 0.8f),
    P(FM_PID_A_ENV_D, 1.5f), P(FM_PID_A_ENV_S, 0.15f),
    P(FM_PID_A_ENV_R, 0.3f),
    P(FM_PID_A_MENV_D, 0.4f), P(FM_PID_A_MENV_S, 0.35f),
    P(FM_PID_B_CRATIO, 1.0f), P(FM_PID_B_MRATIO, 1.0f),
    P(FM_PID_B_INDEX, 3.5f), P(FM_PID_B_LEVEL, 0.12f),
    P(FM_PID_B_ENV_D, 0.08f), P(FM_PID_B_ENV_S, 0.0f),
    P(FM_PID_VEL_INDEX, 0.75f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.2f), P(FX_PID_REV_MIX, 0.15f),
};

/* slow index swell into a sustained rasp — brass, the DX way */
static const preset_pair_t kFmBrass[] = {
    P(FM_PID_A_INDEX, 3.2f), P(FM_PID_A_FB, 0.15f),
    P(FM_PID_A_LEVEL, 0.7f),
    P(FM_PID_A_ENV_A, 0.05f), P(FM_PID_A_ENV_D, 0.3f),
    P(FM_PID_A_ENV_S, 0.9f), P(FM_PID_A_ENV_R, 0.2f),
    P(FM_PID_A_MENV_A, 0.09f), P(FM_PID_A_MENV_D, 0.3f),
    P(FM_PID_A_MENV_S, 0.75f),
    P(FM_PID_B_CRATIO, 2.0f), P(FM_PID_B_MRATIO, 2.0f),
    P(FM_PID_B_INDEX, 1.5f), P(FM_PID_B_LEVEL, 0.25f),
    P(FM_PID_B_ENV_A, 0.05f), P(FM_PID_B_ENV_S, 0.8f),
    P(FM_PID_B_ENV_R, 0.2f),
    P(FM_PID_B_MENV_A, 0.09f), P(FM_PID_B_MENV_S, 0.6f),
    P(SYNTH_PID_COMMON_UNISON, 2), P(SYNTH_PID_COMMON_UNI_DETUNE, 8.0f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.2f),
};

/* 3:1 body + sparkling 8:8 pair, glassy decaying keys */
static const preset_pair_t kFmGlassKeys[] = {
    P(FM_PID_A_MRATIO, 3.0f), P(FM_PID_A_INDEX, 1.8f),
    P(FM_PID_A_LEVEL, 0.75f),
    P(FM_PID_A_ENV_D, 1.8f), P(FM_PID_A_ENV_S, 0.0f),
    P(FM_PID_A_ENV_R, 0.8f),
    P(FM_PID_A_MENV_D, 0.8f), P(FM_PID_A_MENV_S, 0.1f),
    P(FM_PID_B_CRATIO, 8.0f), P(FM_PID_B_MRATIO, 8.0f),
    P(FM_PID_B_INDEX, 0.8f), P(FM_PID_B_LEVEL, 0.18f),
    P(FM_PID_B_ENV_D, 0.6f), P(FM_PID_B_ENV_S, 0.0f),
    P(FM_PID_B_DETUNE, 4.0f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.3f), P(FX_PID_REV_MIX, 0.3f),
};

/* feedback at max ≈ filtered noise: a snappy percussive hit */
static const preset_pair_t kFmSnappyHit[] = {
    P(FM_PID_A_FB, 1.0f), P(FM_PID_A_MRATIO, 1.4f),
    P(FM_PID_A_INDEX, 9.0f), P(FM_PID_A_LEVEL, 0.6f),
    P(FM_PID_A_ENV_A, 0.001f), P(FM_PID_A_ENV_D, 0.18f),
    P(FM_PID_A_ENV_S, 0.0f), P(FM_PID_A_ENV_R, 0.12f),
    P(FM_PID_A_MENV_D, 0.12f), P(FM_PID_A_MENV_S, 0.0f),
    P(FM_PID_B_CRATIO, 1.0f), P(FM_PID_B_MRATIO, 1.0f),
    P(FM_PID_B_INDEX, 0.5f), P(FM_PID_B_LEVEL, 0.4f),
    P(FM_PID_B_ENV_D, 0.1f), P(FM_PID_B_ENV_S, 0.0f),
    P(FM_PID_VEL_INDEX, 0.5f),
};

/* soft 2:1 thunk + a quick 3.8 ping — wooden log drum */
static const preset_pair_t kFmLogDrum[] = {
    P(FM_PID_A_MRATIO, 2.0f), P(FM_PID_A_INDEX, 1.2f),
    P(FM_PID_A_LEVEL, 0.85f),
    P(FM_PID_A_ENV_D, 0.25f), P(FM_PID_A_ENV_S, 0.0f),
    P(FM_PID_A_ENV_R, 0.2f),
    P(FM_PID_A_MENV_D, 0.08f), P(FM_PID_A_MENV_S, 0.0f),
    P(FM_PID_B_CRATIO, 3.8f), P(FM_PID_B_MRATIO, 3.8f),
    P(FM_PID_B_INDEX, 1.0f), P(FM_PID_B_LEVEL, 0.1f),
    P(FM_PID_B_ENV_D, 0.06f), P(FM_PID_B_ENV_S, 0.0f),
    P(FM_PID_VEL_INDEX, 0.5f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.18f), P(FX_PID_DLY_PP, 1),
};

/* two gently-modulated detuned pairs, slow attack — DX strings */
static const preset_pair_t kFmDxStrings[] = {
    P(FM_PID_A_INDEX, 1.6f), P(FM_PID_A_FB, 0.2f),
    P(FM_PID_A_LEVEL, 0.55f),
    P(FM_PID_A_ENV_A, 0.35f), P(FM_PID_A_ENV_S, 0.9f),
    P(FM_PID_A_ENV_R, 0.8f),
    P(FM_PID_A_MENV_A, 0.3f), P(FM_PID_A_MENV_S, 0.8f),
    P(FM_PID_B_CRATIO, 1.0f), P(FM_PID_B_MRATIO, 1.0f),
    P(FM_PID_B_INDEX, 1.2f), P(FM_PID_B_LEVEL, 0.4f),
    P(FM_PID_B_DETUNE, 8.0f),
    P(FM_PID_B_ENV_A, 0.35f), P(FM_PID_B_ENV_S, 0.9f),
    P(FM_PID_B_ENV_R, 0.8f),
    P(FM_PID_B_MENV_A, 0.3f), P(FM_PID_B_MENV_S, 0.7f),
    P(SYNTH_PID_COMMON_UNISON, 2), P(SYNTH_PID_COMMON_UNI_DETUNE, 12.0f),
    P(SYNTH_PID_COMMON_UNI_SPREAD, 0.9f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.4f), P(FX_PID_REV_MIX, 0.3f),
};

/* sub-octave punch with feedback edge and a fast click pair */
static const preset_pair_t kFmPunchBass[] = {
    P(FM_PID_A_CRATIO, 0.5f), P(FM_PID_A_INDEX, 3.0f),
    P(FM_PID_A_FB, 0.4f), P(FM_PID_A_LEVEL, 0.85f),
    P(FM_PID_A_ENV_D, 0.35f), P(FM_PID_A_ENV_S, 0.5f),
    P(FM_PID_A_ENV_R, 0.09f),
    P(FM_PID_A_MENV_D, 0.1f), P(FM_PID_A_MENV_S, 0.15f),
    P(FM_PID_B_CRATIO, 6.0f), P(FM_PID_B_MRATIO, 6.0f),
    P(FM_PID_B_INDEX, 2.0f), P(FM_PID_B_LEVEL, 0.15f),
    P(FM_PID_B_ENV_D, 0.03f), P(FM_PID_B_ENV_S, 0.0f),
    P(FM_PID_VEL_INDEX, 0.7f),
};

/* octave-up 8:2 tinkle over a soft body — music box */
static const preset_pair_t kFmMusicBox[] = {
    P(FM_PID_A_CRATIO, 2.0f), P(FM_PID_A_MRATIO, 8.0f),
    P(FM_PID_A_INDEX, 1.5f), P(FM_PID_A_LEVEL, 0.6f),
    P(FM_PID_A_ENV_A, 0.001f), P(FM_PID_A_ENV_D, 1.2f),
    P(FM_PID_A_ENV_S, 0.0f), P(FM_PID_A_ENV_R, 0.9f),
    P(FM_PID_A_MENV_D, 0.3f), P(FM_PID_A_MENV_S, 0.0f),
    P(FM_PID_B_CRATIO, 2.0f), P(FM_PID_B_MRATIO, 2.0f),
    P(FM_PID_B_INDEX, 0.6f), P(FM_PID_B_LEVEL, 0.3f),
    P(FM_PID_B_ENV_D, 0.8f), P(FM_PID_B_ENV_S, 0.0f),
    P(FM_PID_VEL_INDEX, 0.4f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.3f),
};

/* odd-harmonic 2:1 tone with a breath chiff, wheel = vibrato */
static const preset_pair_t kFmFlute[] = {
    P(FM_PID_A_MRATIO, 2.0f), P(FM_PID_A_INDEX, 1.2f),
    P(FM_PID_A_FB, 0.1f), P(FM_PID_A_LEVEL, 0.85f),
    P(FM_PID_A_ENV_A, 0.08f), P(FM_PID_A_ENV_S, 0.9f),
    P(FM_PID_A_ENV_R, 0.15f),
    P(FM_PID_A_MENV_S, 0.9f),
    P(FM_PID_B_CRATIO, 4.0f), P(FM_PID_B_MRATIO, 4.0f),
    P(FM_PID_B_INDEX, 1.0f), P(FM_PID_B_LEVEL, 0.1f),
    P(FM_PID_B_ENV_A, 0.001f), P(FM_PID_B_ENV_D, 0.05f),
    P(FM_PID_B_ENV_S, 0.0f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, FM_PID_LFO_PITCH, 0.3f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.2f),
};

/* index + feedback crawling up over seconds — inharmonic sci-fi swell */
static const preset_pair_t kFmSciFiSwell[] = {
    P(FM_PID_A_MRATIO, 1.41f), P(FM_PID_A_INDEX, 7.0f),
    P(FM_PID_A_FB, 0.7f), P(FM_PID_A_LEVEL, 0.65f),
    P(FM_PID_A_ENV_A, 0.5f), P(FM_PID_A_ENV_S, 1.0f),
    P(FM_PID_A_ENV_R, 1.0f),
    P(FM_PID_A_MENV_A, 1.5f), P(FM_PID_A_MENV_S, 1.0f),
    P(FM_PID_B_MRATIO, 2.8f), P(FM_PID_B_INDEX, 5.0f),
    P(FM_PID_B_LEVEL, 0.3f),
    P(FM_PID_B_ENV_A, 0.5f), P(FM_PID_B_ENV_S, 1.0f),
    P(FM_PID_B_MENV_A, 2.5f), P(FM_PID_B_MENV_S, 1.0f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.4f), P(FX_PID_REV_SIZE, 0.8f),
};

/* soft sustained bell spectrum — pad with a metallic core */
static const preset_pair_t kFmBellPad[] = {
    P(FM_PID_A_MRATIO, 3.5f), P(FM_PID_A_INDEX, 2.0f),
    P(FM_PID_A_LEVEL, 0.7f),
    P(FM_PID_A_ENV_A, 0.4f), P(FM_PID_A_ENV_S, 0.8f),
    P(FM_PID_A_ENV_R, 1.5f),
    P(FM_PID_A_MENV_D, 2.0f), P(FM_PID_A_MENV_S, 0.3f),
    P(FM_PID_B_CRATIO, 7.0f), P(FM_PID_B_INDEX, 1.0f),
    P(FM_PID_B_LEVEL, 0.15f),
    P(FM_PID_B_ENV_A, 0.4f), P(FM_PID_B_ENV_S, 0.6f),
    P(FM_PID_B_ENV_R, 1.5f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.35f), P(FX_PID_REV_MIX, 0.4f),
};

/* high index snapped shut instantly — spitty FM clav */
static const preset_pair_t kFmClav[] = {
    P(FM_PID_A_INDEX, 5.0f), P(FM_PID_A_FB, 0.3f),
    P(FM_PID_A_LEVEL, 0.75f),
    P(FM_PID_A_ENV_D, 0.6f), P(FM_PID_A_ENV_S, 0.2f),
    P(FM_PID_A_ENV_R, 0.08f),
    P(FM_PID_A_MENV_D, 0.06f), P(FM_PID_A_MENV_S, 0.1f),
    P(FM_PID_B_CRATIO, 1.0f), P(FM_PID_B_MRATIO, 9.0f),
    P(FM_PID_B_INDEX, 4.0f), P(FM_PID_B_LEVEL, 0.25f),
    P(FM_PID_B_ENV_D, 0.3f), P(FM_PID_B_ENV_S, 0.2f),
    P(FM_PID_B_MENV_D, 0.04f), P(FM_PID_B_MENV_S, 0.0f),
    P(FM_PID_VEL_INDEX, 0.85f),
};

/* ---- fm, slots 16-47 (S33) ------------------------------------------
 *
 * FM had no filter at all until S33, so the ones that switch it on are new
 * ground for this engine: a lowpass after the operators tames the top
 * partials that ratio-and-index alone cannot, and the vowel type turns a
 * two-operator stack into something that speaks. There is no flt.env here —
 * cutoff moves from keyboard tracking, the LFO, velocity and the matrix. */

/* soft mallet, long bell tail — the mk1 rather than the DX */
static const preset_pair_t kFmRhodesMk1[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 1.0f),
    P(FM_PID_A_INDEX, 1.6f), P(FM_PID_A_LEVEL, 0.8f),
    P(FM_PID_A_ENV_D, 2.5f), P(FM_PID_A_ENV_S, 0.0f),
    P(FM_PID_A_ENV_R, 0.5f),
    P(FM_PID_A_MENV_D, 0.35f), P(FM_PID_A_MENV_S, 0.1f),
    P(FM_PID_B_CRATIO, 8.0f), P(FM_PID_B_MRATIO, 8.0f),
    P(FM_PID_B_INDEX, 0.8f), P(FM_PID_B_LEVEL, 0.12f),
    P(FM_PID_B_ENV_D, 0.18f), P(FM_PID_B_ENV_S, 0.0f),
    P(FM_PID_VEL_INDEX, 0.75f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.25f), P(FX_PID_REV_MIX, 0.25f),
};

/* the hard, glassy one: high index on the tine, velocity wide open */
static const preset_pair_t kFmHardTines[] = {
    P(FM_PID_A_INDEX, 3.6f), P(FM_PID_A_LEVEL, 0.65f),
    P(FM_PID_A_ENV_D, 1.6f), P(FM_PID_A_ENV_S, 0.0f),
    P(FM_PID_A_MENV_D, 0.4f), P(FM_PID_A_MENV_S, 0.05f),
    P(FM_PID_B_CRATIO, 14.0f), P(FM_PID_B_MRATIO, 14.0f),
    P(FM_PID_B_INDEX, 1.4f), P(FM_PID_B_LEVEL, 0.3f),
    P(FM_PID_B_ENV_D, 0.12f), P(FM_PID_B_ENV_S, 0.0f),
    P(FM_PID_VEL_INDEX, 0.95f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.2f),
};

/* a lowpass after the operators: FM that does not spit at the top */
static const preset_pair_t kFmSmoothKeys[] = {
    P(FM_PID_A_INDEX, 3.2f), P(FM_PID_A_ENV_D, 1.8f),
    P(FM_PID_A_ENV_S, 0.15f),
    P(FM_PID_B_LEVEL, 0.25f), P(FM_PID_B_INDEX, 1.2f),
    P(FM_PID_FLT_ON, 1), P(FM_PID_FLT_TYPE, 2),
    P(FM_PID_FLT_CUTOFF, 2600.0f), P(FM_PID_FLT_RESO, 0.2f),
    P(FM_PID_FLT_KBD, 0.7f),
    P(FM_PID_VEL_INDEX, 0.7f),
    MOD(0, SYNTH_MOD_SRC_VEL, FM_PID_FLT_CUTOFF, 0.5f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.2f), P(FX_PID_REV_MIX, 0.28f),
};

/* two operators through the vowel filter — FM that speaks */
static const preset_pair_t kFmVoiceBox[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 2.0f),
    P(FM_PID_A_INDEX, 3.0f), P(FM_PID_A_ENV_S, 0.85f),
    P(FM_PID_A_ENV_D, 0.4f), P(FM_PID_A_ENV_R, 0.2f),
    P(FM_PID_B_LEVEL, 0.15f),
    P(FM_PID_FLT_ON, 1), P(FM_PID_FLT_TYPE, 4),
    P(FM_PID_FLT_VOWEL, 0.25f), P(FM_PID_FLT_RESO, 0.65f),
    P(FM_PID_FLT_CUTOFF, 1000.0f),
    P(FM_PID_LFO_RATE, 3.5f),
    MOD(0, SYNTH_MOD_SRC_LFO1, FM_PID_FLT_VOWEL, 0.45f),
    MOD(1, SYNTH_MOD_SRC_WHEEL, FM_PID_FLT_VOWEL, 0.5f),
    P(FX_PID_DLY_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_DLY_MIX, 0.2f), P(FX_PID_REV_MIX, 0.25f),
};

/* index high, filter driven: the nastiest bass this engine can make */
static const preset_pair_t kFmMetalBass[] = {
    P(FM_PID_A_CRATIO, 0.5f), P(FM_PID_A_MRATIO, 1.5f),
    P(FM_PID_A_INDEX, 5.5f), P(FM_PID_A_FB, 0.3f),
    P(FM_PID_A_LEVEL, 0.9f),
    P(FM_PID_A_ENV_D, 0.6f), P(FM_PID_A_ENV_S, 0.55f),
    P(FM_PID_A_ENV_R, 0.1f),
    P(FM_PID_A_MENV_D, 0.2f), P(FM_PID_A_MENV_S, 0.35f),
    P(FM_PID_B_LEVEL, 0.1f),
    P(FM_PID_FLT_ON, 1), P(FM_PID_FLT_TYPE, 2),
    P(FM_PID_FLT_CUTOFF, 420.0f), P(FM_PID_FLT_RESO, 0.5f),
    P(FM_PID_FLT_DRIVE, 0.6f), P(FM_PID_FLT_KBD, 0.3f),
    P(FM_PID_VEL_INDEX, 0.8f),
};

/* ratio 1:1 at low index — a clean, round sub with just enough edge */
static const preset_pair_t kFmSubBass[] = {
    P(FM_PID_A_CRATIO, 0.5f), P(FM_PID_A_MRATIO, 0.5f),
    P(FM_PID_A_INDEX, 1.1f), P(FM_PID_A_LEVEL, 1.0f),
    P(FM_PID_A_ENV_A, 0.003f), P(FM_PID_A_ENV_D, 0.8f),
    P(FM_PID_A_ENV_S, 0.7f), P(FM_PID_A_ENV_R, 0.08f),
    P(FM_PID_A_MENV_D, 0.1f), P(FM_PID_A_MENV_S, 0.0f),
    P(FM_PID_B_LEVEL, 0.0f),
    P(FM_PID_VEL_INDEX, 0.5f),
};

/* slap: a very short, very bright B pair on top of a plain A */
static const preset_pair_t kFmSlapBass[] = {
    P(FM_PID_A_CRATIO, 0.5f), P(FM_PID_A_MRATIO, 1.0f),
    P(FM_PID_A_INDEX, 2.2f), P(FM_PID_A_LEVEL, 0.85f),
    P(FM_PID_A_ENV_D, 0.35f), P(FM_PID_A_ENV_S, 0.4f),
    P(FM_PID_A_ENV_R, 0.08f),
    P(FM_PID_A_MENV_D, 0.09f), P(FM_PID_A_MENV_S, 0.1f),
    P(FM_PID_B_CRATIO, 6.0f), P(FM_PID_B_MRATIO, 7.0f),
    P(FM_PID_B_INDEX, 3.5f), P(FM_PID_B_LEVEL, 0.22f),
    P(FM_PID_B_ENV_D, 0.06f), P(FM_PID_B_ENV_S, 0.0f),
    P(FM_PID_B_ENV_R, 0.05f),
    P(FM_PID_VEL_INDEX, 0.9f),
};

/* feedback near the top of its range: FM noise with a pitch centre */
static const preset_pair_t kFmBuzzLead[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 1.0f),
    P(FM_PID_A_INDEX, 4.0f), P(FM_PID_A_FB, 0.72f),
    P(FM_PID_A_LEVEL, 0.8f), P(FM_PID_A_ENV_S, 0.85f),
    P(FM_PID_A_ENV_D, 0.3f), P(FM_PID_A_ENV_R, 0.12f),
    P(FM_PID_B_LEVEL, 0.1f),
    P(FM_PID_FLT_ON, 1), P(FM_PID_FLT_TYPE, 1),
    P(FM_PID_FLT_CUTOFF, 2200.0f), P(FM_PID_FLT_RESO, 0.35f),
    P(FM_PID_FLT_KBD, 0.8f),
    P(SYNTH_PID_COMMON_GLIDE, 0.05f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.25f), P(FX_PID_DLY_FB, 0.4f),
};

/* singing lead: modest index, full sustain, vibrato on the wheel */
static const preset_pair_t kFmSingLead[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 2.0f),
    P(FM_PID_A_INDEX, 2.4f), P(FM_PID_A_LEVEL, 0.85f),
    P(FM_PID_A_ENV_A, 0.02f), P(FM_PID_A_ENV_D, 0.4f),
    P(FM_PID_A_ENV_S, 0.9f), P(FM_PID_A_ENV_R, 0.2f),
    P(FM_PID_A_MENV_S, 0.6f),
    P(FM_PID_B_LEVEL, 0.12f),
    P(FM_PID_LFO_RATE, 5.0f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, FM_PID_LFO_PITCH, 0.45f),
    P(SYNTH_PID_COMMON_GLIDE, 0.05f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.3f),
};

/* plucked string: ratio 1:1, index falls fast, nothing sustains */
static const preset_pair_t kFmKoto[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 1.0f),
    P(FM_PID_A_INDEX, 4.2f), P(FM_PID_A_LEVEL, 0.85f),
    P(FM_PID_A_ENV_A, 0.001f), P(FM_PID_A_ENV_D, 1.1f),
    P(FM_PID_A_ENV_S, 0.0f), P(FM_PID_A_ENV_R, 0.3f),
    P(FM_PID_A_MENV_A, 0.001f), P(FM_PID_A_MENV_D, 0.08f),
    P(FM_PID_A_MENV_S, 0.0f),
    P(FM_PID_B_LEVEL, 0.08f),
    P(FM_PID_VEL_INDEX, 0.85f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.3f),
};

/* harp: same shape, gentler index, longer bloom */
static const preset_pair_t kFmHarp[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 2.0f),
    P(FM_PID_A_INDEX, 2.0f), P(FM_PID_A_LEVEL, 0.8f),
    P(FM_PID_A_ENV_A, 0.001f), P(FM_PID_A_ENV_D, 2.0f),
    P(FM_PID_A_ENV_S, 0.0f), P(FM_PID_A_ENV_R, 0.6f),
    P(FM_PID_A_MENV_D, 0.25f), P(FM_PID_A_MENV_S, 0.0f),
    P(FM_PID_B_CRATIO, 3.0f), P(FM_PID_B_MRATIO, 3.0f),
    P(FM_PID_B_INDEX, 1.0f), P(FM_PID_B_LEVEL, 0.15f),
    P(FM_PID_B_ENV_D, 0.6f), P(FM_PID_B_ENV_S, 0.0f),
    P(FM_PID_VEL_INDEX, 0.7f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.4f), P(FX_PID_REV_SIZE, 0.7f),
};

/* wooden mallet: ratio 1:4, very short index envelope */
static const preset_pair_t kFmMarimba[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 4.0f),
    P(FM_PID_A_INDEX, 3.0f), P(FM_PID_A_LEVEL, 0.9f),
    P(FM_PID_A_ENV_A, 0.001f), P(FM_PID_A_ENV_D, 0.5f),
    P(FM_PID_A_ENV_S, 0.0f), P(FM_PID_A_ENV_R, 0.2f),
    P(FM_PID_A_MENV_A, 0.001f), P(FM_PID_A_MENV_D, 0.05f),
    P(FM_PID_A_MENV_S, 0.0f),
    P(FM_PID_B_LEVEL, 0.06f),
    P(FM_PID_VEL_INDEX, 0.8f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.22f),
};

/* metal bar, soft mallet, slow tremolo */
static const preset_pair_t kFmVibes[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 4.0f),
    P(FM_PID_A_INDEX, 1.4f), P(FM_PID_A_LEVEL, 0.85f),
    P(FM_PID_A_ENV_A, 0.003f), P(FM_PID_A_ENV_D, 2.5f),
    P(FM_PID_A_ENV_S, 0.0f), P(FM_PID_A_ENV_R, 1.2f),
    P(FM_PID_A_MENV_D, 0.5f), P(FM_PID_A_MENV_S, 0.0f),
    P(FM_PID_B_CRATIO, 9.0f), P(FM_PID_B_MRATIO, 9.0f),
    P(FM_PID_B_INDEX, 0.6f), P(FM_PID_B_LEVEL, 0.1f),
    P(FM_PID_B_ENV_D, 0.3f), P(FM_PID_B_ENV_S, 0.0f),
    P(FM_PID_LFO_RATE, 4.5f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.35f),
};

/* glockenspiel: high ratio, tiny index, instant attack */
static const preset_pair_t kFmGlocken[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 7.0f),
    P(FM_PID_A_INDEX, 1.2f), P(FM_PID_A_LEVEL, 0.8f),
    P(FM_PID_A_ENV_A, 0.001f), P(FM_PID_A_ENV_D, 1.2f),
    P(FM_PID_A_ENV_S, 0.0f), P(FM_PID_A_ENV_R, 0.5f),
    P(FM_PID_A_MENV_D, 0.15f), P(FM_PID_A_MENV_S, 0.0f),
    P(FM_PID_B_CRATIO, 11.0f), P(FM_PID_B_MRATIO, 11.0f),
    P(FM_PID_B_INDEX, 0.7f), P(FM_PID_B_LEVEL, 0.18f),
    P(FM_PID_B_ENV_D, 0.25f), P(FM_PID_B_ENV_S, 0.0f),
    P(FM_PID_VEL_INDEX, 0.85f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.42f),
};

/* inharmonic ratio and a long tail: a struck church bell */
static const preset_pair_t kFmChurchBell[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 3.5f),
    P(FM_PID_A_INDEX, 5.0f), P(FM_PID_A_LEVEL, 0.8f),
    P(FM_PID_A_ENV_A, 0.002f), P(FM_PID_A_ENV_D, 5.0f),
    P(FM_PID_A_ENV_S, 0.0f), P(FM_PID_A_ENV_R, 3.0f),
    P(FM_PID_A_MENV_A, 0.001f), P(FM_PID_A_MENV_D, 1.5f),
    P(FM_PID_A_MENV_S, 0.05f),
    P(FM_PID_B_CRATIO, 2.0f), P(FM_PID_B_MRATIO, 5.0f),
    P(FM_PID_B_INDEX, 2.5f), P(FM_PID_B_LEVEL, 0.2f),
    P(FM_PID_B_ENV_D, 2.0f), P(FM_PID_B_ENV_S, 0.0f),
    P(FM_PID_B_DETUNE, 9.0f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.5f), P(FX_PID_REV_SIZE, 0.9f),
};

/* gamelan: badly-tuned metal, on purpose */
static const preset_pair_t kFmGamelan[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 2.5f),
    P(FM_PID_A_INDEX, 4.5f), P(FM_PID_A_LEVEL, 0.85f),
    P(FM_PID_A_ENV_A, 0.001f), P(FM_PID_A_ENV_D, 1.8f),
    P(FM_PID_A_ENV_S, 0.0f), P(FM_PID_A_ENV_R, 0.8f),
    P(FM_PID_A_MENV_D, 0.3f), P(FM_PID_A_MENV_S, 0.1f),
    P(FM_PID_B_CRATIO, 3.5f), P(FM_PID_B_MRATIO, 4.5f),
    P(FM_PID_B_INDEX, 2.0f), P(FM_PID_B_LEVEL, 0.25f),
    P(FM_PID_B_ENV_D, 0.9f), P(FM_PID_B_ENV_S, 0.0f),
    P(FM_PID_B_DETUNE, 22.0f),
    P(FM_PID_VEL_INDEX, 0.8f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.38f),
};

/* additive-ish FM organ: index near zero, both pairs held flat */
static const preset_pair_t kFmOrgan[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 2.0f),
    P(FM_PID_A_INDEX, 0.9f), P(FM_PID_A_LEVEL, 0.7f),
    P(FM_PID_A_ENV_A, 0.004f), P(FM_PID_A_ENV_D, 0.05f),
    P(FM_PID_A_ENV_S, 1.0f), P(FM_PID_A_ENV_R, 0.05f),
    P(FM_PID_A_MENV_S, 1.0f), P(FM_PID_A_MENV_D, 0.05f),
    P(FM_PID_B_CRATIO, 2.0f), P(FM_PID_B_MRATIO, 2.0f),
    P(FM_PID_B_INDEX, 0.6f), P(FM_PID_B_LEVEL, 0.3f),
    P(FM_PID_B_ENV_S, 1.0f), P(FM_PID_B_ENV_D, 0.05f),
    P(FM_PID_B_ENV_R, 0.05f), P(FM_PID_B_MENV_S, 1.0f),
    P(FM_PID_VEL_INDEX, 0.2f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.35f), P(FX_PID_REV_MIX, 0.2f),
};

/* slow swell, index rising with it — a brass section leaning in */
static const preset_pair_t kFmBrassSwell[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 1.0f),
    P(FM_PID_A_INDEX, 3.4f), P(FM_PID_A_LEVEL, 0.85f),
    P(FM_PID_A_ENV_A, 0.25f), P(FM_PID_A_ENV_D, 0.6f),
    P(FM_PID_A_ENV_S, 0.85f), P(FM_PID_A_ENV_R, 0.3f),
    P(FM_PID_A_MENV_A, 0.35f), P(FM_PID_A_MENV_D, 1.0f),
    P(FM_PID_A_MENV_S, 0.8f),
    P(FM_PID_B_LEVEL, 0.15f), P(FM_PID_B_INDEX, 1.5f),
    P(FM_PID_B_ENV_A, 0.2f), P(FM_PID_B_ENV_S, 0.6f),
    P(FM_PID_VEL_INDEX, 0.5f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.35f),
};

/* single horn: darker, filter rolling the top off */
static const preset_pair_t kFmHorn[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 1.0f),
    P(FM_PID_A_INDEX, 2.6f), P(FM_PID_A_LEVEL, 0.9f),
    P(FM_PID_A_ENV_A, 0.08f), P(FM_PID_A_ENV_D, 0.5f),
    P(FM_PID_A_ENV_S, 0.8f), P(FM_PID_A_ENV_R, 0.25f),
    P(FM_PID_A_MENV_A, 0.1f), P(FM_PID_A_MENV_S, 0.55f),
    P(FM_PID_B_LEVEL, 0.0f),
    P(FM_PID_FLT_ON, 1), P(FM_PID_FLT_CUTOFF, 1800.0f),
    P(FM_PID_FLT_RESO, 0.15f), P(FM_PID_FLT_KBD, 0.6f),
    P(FM_PID_VEL_INDEX, 0.6f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.4f), P(FX_PID_REV_SIZE, 0.7f),
};

/* odd-harmonic ratio: a clarinet's hollow register */
static const preset_pair_t kFmClarinet[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 3.0f),
    P(FM_PID_A_INDEX, 1.8f), P(FM_PID_A_LEVEL, 0.9f),
    P(FM_PID_A_ENV_A, 0.05f), P(FM_PID_A_ENV_D, 0.3f),
    P(FM_PID_A_ENV_S, 0.95f), P(FM_PID_A_ENV_R, 0.12f),
    P(FM_PID_A_MENV_S, 0.7f), P(FM_PID_A_MENV_D, 0.3f),
    P(FM_PID_B_LEVEL, 0.0f),
    P(FM_PID_LFO_RATE, 4.8f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, FM_PID_LFO_PITCH, 0.35f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.32f),
};

/* reedy double: high index at low level, bandpassed */
static const preset_pair_t kFmOboe[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 2.0f),
    P(FM_PID_A_INDEX, 4.0f), P(FM_PID_A_LEVEL, 0.85f),
    P(FM_PID_A_ENV_A, 0.04f), P(FM_PID_A_ENV_S, 0.9f),
    P(FM_PID_A_ENV_D, 0.25f), P(FM_PID_A_ENV_R, 0.15f),
    P(FM_PID_A_MENV_S, 0.75f),
    P(FM_PID_B_LEVEL, 0.0f),
    P(FM_PID_FLT_ON, 1), P(FM_PID_FLT_TYPE, 3),
    P(FM_PID_FLT_SPREAD, 2.0f), P(FM_PID_FLT_CUTOFF, 1500.0f),
    P(FM_PID_FLT_RESO, 0.35f), P(FM_PID_FLT_KBD, 0.5f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.35f),
};

/* pair B detuned hard, both sustaining: a wide, slightly sour pad */
static const preset_pair_t kFmDetunePad[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 1.0f),
    P(FM_PID_A_INDEX, 1.8f), P(FM_PID_A_LEVEL, 0.55f),
    P(FM_PID_A_ENV_A, 0.6f), P(FM_PID_A_ENV_S, 0.9f),
    P(FM_PID_A_ENV_D, 1.0f), P(FM_PID_A_ENV_R, 1.2f),
    P(FM_PID_A_MENV_S, 0.6f), P(FM_PID_A_MENV_A, 0.4f),
    P(FM_PID_B_CRATIO, 1.0f), P(FM_PID_B_MRATIO, 2.0f),
    P(FM_PID_B_INDEX, 1.4f), P(FM_PID_B_LEVEL, 0.45f),
    P(FM_PID_B_ENV_A, 0.8f), P(FM_PID_B_ENV_S, 0.85f),
    P(FM_PID_B_ENV_R, 1.4f), P(FM_PID_B_MENV_S, 0.5f),
    P(FM_PID_B_DETUNE, 14.0f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.3f), P(FX_PID_REV_MIX, 0.45f),
};

/* 24 dB lowpass over a soft FM stack: a pad that stays under the mix */
static const preset_pair_t kFmSoftPad[] = {
    P(FM_PID_A_INDEX, 1.5f), P(FM_PID_A_LEVEL, 0.6f),
    P(FM_PID_A_ENV_A, 0.9f), P(FM_PID_A_ENV_S, 0.9f),
    P(FM_PID_A_ENV_D, 1.5f), P(FM_PID_A_ENV_R, 1.6f),
    P(FM_PID_A_MENV_A, 0.6f), P(FM_PID_A_MENV_S, 0.5f),
    P(FM_PID_B_CRATIO, 2.0f), P(FM_PID_B_MRATIO, 3.0f),
    P(FM_PID_B_INDEX, 1.0f), P(FM_PID_B_LEVEL, 0.3f),
    P(FM_PID_B_ENV_A, 1.2f), P(FM_PID_B_ENV_S, 0.8f),
    P(FM_PID_B_ENV_R, 1.8f),
    P(FM_PID_FLT_ON, 1), P(FM_PID_FLT_TYPE, 1),
    P(FM_PID_FLT_CUTOFF, 1600.0f), P(FM_PID_FLT_RESO, 0.25f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.5f), P(FX_PID_REV_SIZE, 0.8f),
};

/* an allpass on FM: phase motion with the spectrum left alone */
static const preset_pair_t kFmPhasePad[] = {
    P(FM_PID_A_INDEX, 2.2f), P(FM_PID_A_ENV_A, 0.5f),
    P(FM_PID_A_ENV_S, 0.9f), P(FM_PID_A_ENV_R, 1.2f),
    P(FM_PID_A_MENV_S, 0.55f),
    P(FM_PID_B_LEVEL, 0.2f), P(FM_PID_B_ENV_S, 0.7f),
    P(FM_PID_B_ENV_A, 0.6f), P(FM_PID_B_ENV_R, 1.2f),
    P(FM_PID_FLT_ON, 1), P(FM_PID_FLT_MODE, 5),
    P(FM_PID_FLT_CUTOFF, 1200.0f), P(FM_PID_FLT_RESO, 0.7f),
    P(FM_PID_LFO_RATE, 0.5f),
    MOD(0, SYNTH_MOD_SRC_LFO1, FM_PID_FLT_CUTOFF, 0.4f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.42f),
};

/* modulator ratio far off the carrier: metallic, unpitched-ish stab */
static const preset_pair_t kFmClangStab[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 11.0f),
    P(FM_PID_A_INDEX, 6.0f), P(FM_PID_A_LEVEL, 0.8f),
    P(FM_PID_A_ENV_A, 0.001f), P(FM_PID_A_ENV_D, 0.35f),
    P(FM_PID_A_ENV_S, 0.0f), P(FM_PID_A_ENV_R, 0.2f),
    P(FM_PID_A_MENV_D, 0.1f), P(FM_PID_A_MENV_S, 0.0f),
    P(FM_PID_B_CRATIO, 5.0f), P(FM_PID_B_MRATIO, 13.0f),
    P(FM_PID_B_INDEX, 3.0f), P(FM_PID_B_LEVEL, 0.25f),
    P(FM_PID_B_ENV_D, 0.15f), P(FM_PID_B_ENV_S, 0.0f),
    P(FM_PID_VEL_INDEX, 0.9f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.3f),
};

/* low drum with a skin: index collapses fast, the filter takes the rest */
static const preset_pair_t kFmTimpani[] = {
    P(FM_PID_A_CRATIO, 0.5f), P(FM_PID_A_MRATIO, 1.5f),
    P(FM_PID_A_INDEX, 4.0f), P(FM_PID_A_LEVEL, 0.95f),
    P(FM_PID_A_ENV_A, 0.001f), P(FM_PID_A_ENV_D, 1.6f),
    P(FM_PID_A_ENV_S, 0.0f), P(FM_PID_A_ENV_R, 0.8f),
    P(FM_PID_A_MENV_A, 0.001f), P(FM_PID_A_MENV_D, 0.07f),
    P(FM_PID_A_MENV_S, 0.0f),
    P(FM_PID_B_LEVEL, 0.08f), P(FM_PID_B_ENV_D, 0.1f),
    P(FM_PID_FLT_ON, 1), P(FM_PID_FLT_TYPE, 2),
    P(FM_PID_FLT_CUTOFF, 500.0f), P(FM_PID_FLT_RESO, 0.3f),
    P(FM_PID_FLT_KBD, 0.4f),
    P(FM_PID_VEL_INDEX, 0.9f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.35f), P(FX_PID_REV_SIZE, 0.75f),
};

/* short and woody: the block, not the drum */
static const preset_pair_t kFmWoodBlock[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 6.0f),
    P(FM_PID_A_INDEX, 3.5f), P(FM_PID_A_LEVEL, 0.9f),
    P(FM_PID_A_ENV_A, 0.001f), P(FM_PID_A_ENV_D, 0.12f),
    P(FM_PID_A_ENV_S, 0.0f), P(FM_PID_A_ENV_R, 0.08f),
    P(FM_PID_A_MENV_A, 0.001f), P(FM_PID_A_MENV_D, 0.03f),
    P(FM_PID_A_MENV_S, 0.0f),
    P(FM_PID_B_LEVEL, 0.1f), P(FM_PID_B_ENV_D, 0.04f),
    P(FM_PID_VEL_INDEX, 0.85f),
};

/* full feedback, no pitch centre left — an FM noise burst */
static const preset_pair_t kFmNoiseBurst[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 9.0f),
    P(FM_PID_A_INDEX, 8.0f), P(FM_PID_A_FB, 0.95f),
    P(FM_PID_A_LEVEL, 0.75f),
    P(FM_PID_A_ENV_A, 0.001f), P(FM_PID_A_ENV_D, 0.25f),
    P(FM_PID_A_ENV_S, 0.0f), P(FM_PID_A_ENV_R, 0.15f),
    P(FM_PID_A_MENV_D, 0.12f), P(FM_PID_A_MENV_S, 0.0f),
    P(FM_PID_B_LEVEL, 0.0f),
    P(FM_PID_FLT_ON, 1), P(FM_PID_FLT_MODE, 6),
    P(FM_PID_FLT_CUTOFF, 3000.0f), P(FM_PID_FLT_RESO, 0.4f),
    P(FM_PID_FLT_KBD, 0.0f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.3f),
};

/* pitch dropping away under a held note — the classic drop effect */
static const preset_pair_t kFmDropFx[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 1.5f),
    P(FM_PID_A_INDEX, 5.0f), P(FM_PID_A_LEVEL, 0.85f),
    P(FM_PID_A_ENV_A, 0.002f), P(FM_PID_A_ENV_D, 3.0f),
    P(FM_PID_A_ENV_S, 0.0f), P(FM_PID_A_ENV_R, 1.0f),
    P(FM_PID_A_MENV_D, 1.2f), P(FM_PID_A_MENV_S, 0.0f),
    P(FM_PID_B_LEVEL, 0.15f), P(FM_PID_B_ENV_D, 1.5f),
    P(FM_PID_LFO_RATE, 0.15f), P(FM_PID_LFO_WAVE, 2),
    P(FM_PID_LFO_PITCH, 2.0f),
    P(FX_PID_DLY_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_DLY_MIX, 0.3f), P(FX_PID_REV_MIX, 0.4f),
};

/* fast square LFO on pitch: an alarm you can play chords with */
static const preset_pair_t kFmSiren[] = {
    P(FM_PID_A_INDEX, 3.0f), P(FM_PID_A_LEVEL, 0.8f),
    P(FM_PID_A_ENV_S, 0.9f), P(FM_PID_A_ENV_R, 0.1f),
    P(FM_PID_A_MENV_S, 0.7f),
    P(FM_PID_B_LEVEL, 0.15f),
    P(FM_PID_LFO_RATE, 7.5f), P(FM_PID_LFO_WAVE, 3),
    P(FM_PID_LFO_PITCH, 1.2f),
    P(FX_PID_DLY_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_DLY_MIX, 0.25f), P(FX_PID_REV_MIX, 0.25f),
};

/* the ladder on FM: a plucked, resonant, distinctly analogue result */
static const preset_pair_t kFmLadderPluck[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 2.0f),
    P(FM_PID_A_INDEX, 3.5f), P(FM_PID_A_LEVEL, 0.9f),
    P(FM_PID_A_ENV_A, 0.001f), P(FM_PID_A_ENV_D, 0.8f),
    P(FM_PID_A_ENV_S, 0.0f), P(FM_PID_A_ENV_R, 0.25f),
    P(FM_PID_A_MENV_D, 0.15f), P(FM_PID_A_MENV_S, 0.0f),
    P(FM_PID_B_LEVEL, 0.1f),
    P(FM_PID_FLT_ON, 1), P(FM_PID_FLT_TYPE, 2),
    P(FM_PID_FLT_CUTOFF, 1200.0f), P(FM_PID_FLT_RESO, 0.65f),
    P(FM_PID_FLT_DRIVE, 0.3f), P(FM_PID_FLT_KBD, 0.7f),
    MOD(0, SYNTH_MOD_SRC_VEL, FM_PID_FLT_CUTOFF, 0.55f),
    P(FX_PID_DLY_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_DLY_MIX, 0.2f), P(FX_PID_REV_MIX, 0.25f),
};

/* two pairs an octave apart, both short: an FM mallet stack */
static const preset_pair_t kFmOctaveMallet[] = {
    P(FM_PID_A_CRATIO, 1.0f), P(FM_PID_A_MRATIO, 3.0f),
    P(FM_PID_A_INDEX, 2.6f), P(FM_PID_A_LEVEL, 0.6f),
    P(FM_PID_A_ENV_A, 0.001f), P(FM_PID_A_ENV_D, 0.9f),
    P(FM_PID_A_ENV_S, 0.0f), P(FM_PID_A_ENV_R, 0.35f),
    P(FM_PID_A_MENV_D, 0.1f), P(FM_PID_A_MENV_S, 0.0f),
    P(FM_PID_B_CRATIO, 2.0f), P(FM_PID_B_MRATIO, 6.0f),
    P(FM_PID_B_INDEX, 1.8f), P(FM_PID_B_LEVEL, 0.35f),
    P(FM_PID_B_ENV_D, 0.5f), P(FM_PID_B_ENV_S, 0.0f),
    P(FM_PID_B_ENV_R, 0.25f), P(FM_PID_B_DETUNE, 6.0f),
    P(FM_PID_VEL_INDEX, 0.8f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.35f),
};

/* ---- wavetable (bank 3, linear slots 336-447) ------------------------------ */

/* sync table swept hard by the env, wheel drags the position */
static const preset_pair_t kWtSyncLead[] = {
    P(WT_PID_OSC1_TABLE, 1), P(WT_PID_OSC1_POS, 0.15f),
    P(WT_PID_ENV_POS, 0.8f),
    P(WT_PID_ENV2_DECAY, 0.5f), P(WT_PID_ENV2_SUSTAIN, 0.3f),
    P(WT_PID_ENV1_SUSTAIN, 0.8f),
    P(SYNTH_PID_COMMON_UNISON, 2), P(SYNTH_PID_COMMON_UNI_DETUNE, 14.0f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, WT_PID_OSC1_POS, 0.6f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.2f),
};

/* vowel morph drifting under a very slow lfo — choral pad */
static const preset_pair_t kWtVowelPad[] = {
    P(WT_PID_OSC1_TABLE, 2), P(WT_PID_OSC1_POS, 0.3f),
    P(WT_PID_ENV_POS, 0.2f),
    P(WT_PID_LFO2_RATE, 0.2f), P(WT_PID_LFO2_POS, 0.35f),
    P(WT_PID_FLT_CUTOFF, 6000.0f),
    P(WT_PID_ENV1_ATTACK, 0.6f), P(WT_PID_ENV1_SUSTAIN, 0.9f),
    P(WT_PID_ENV1_RELEASE, 1.5f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.35f), P(FX_PID_REV_MIX, 0.4f),
};

/* fm table pluck, velocity pushes the strike brighter into the sweep */
static const preset_pair_t kWtDigiKeys[] = {
    P(WT_PID_OSC1_TABLE, 3), P(WT_PID_OSC1_POS, 0.1f),
    P(WT_PID_ENV_POS, 0.65f),
    P(WT_PID_ENV2_DECAY, 0.6f), P(WT_PID_ENV2_SUSTAIN, 0.1f),
    P(WT_PID_ENV1_DECAY, 1.0f), P(WT_PID_ENV1_SUSTAIN, 0.3f),
    P(WT_PID_ENV1_RELEASE, 0.5f),
    MOD(0, SYNTH_MOD_SRC_VEL, WT_PID_OSC1_POS, 0.3f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.25f),
};

/* parked in the thin pulses, slow position wobble — pwm without pwm */
static const preset_pair_t kWtPwmDrift[] = {
    P(WT_PID_OSC1_POS, 0.8f), P(WT_PID_ENV_POS, -0.25f),
    P(WT_PID_LFO2_RATE, 0.5f), P(WT_PID_LFO2_POS, 0.15f),
    P(WT_PID_ENV1_ATTACK, 0.25f), P(WT_PID_ENV1_SUSTAIN, 0.85f),
    P(WT_PID_ENV1_RELEASE, 0.9f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.45f), P(FX_PID_REV_MIX, 0.25f),
};

/* sync bite over a sine sub-oscillator an octave down */
static const preset_pair_t kWtSyncSubBass[] = {
    P(WT_PID_OSC1_TABLE, 1), P(WT_PID_OSC1_POS, 0.35f),
    P(WT_PID_MIX_OSC1, 0.5f),
    P(WT_PID_OSC2_TABLE, 0), P(WT_PID_OSC2_POS, 0.0f),
    P(WT_PID_OSC2_SEMI, -12), P(WT_PID_OSC2_FINE, 0.0f),
    P(WT_PID_MIX_OSC2, 0.5f),
    P(WT_PID_ENV_POS, 0.4f),
    P(WT_PID_ENV2_DECAY, 0.18f), P(WT_PID_ENV2_SUSTAIN, 0.0f),
    P(WT_PID_FLT_CUTOFF, 2500.0f),
    P(WT_PID_ENV1_SUSTAIN, 0.8f), P(WT_PID_ENV1_RELEASE, 0.1f),
};

/* static near-triangle frame, instant on/off — a wavetable organ */
static const preset_pair_t kWtWaveOrgan[] = {
    P(WT_PID_OSC1_POS, 0.1f), P(WT_PID_ENV_POS, 0.0f),
    P(WT_PID_FLT_CUTOFF, 7000.0f),
    P(WT_PID_ENV1_ATTACK, 0.003f), P(WT_PID_ENV1_SUSTAIN, 1.0f),
    P(WT_PID_ENV1_RELEASE, 0.07f),
    P(FX_PID_CHO_ON, 1),
    P(FX_PID_CHO_MIX, 0.2f),
};

/* two vocal tables detuned + unison — massed choir */
static const preset_pair_t kWtBigChoir[] = {
    P(WT_PID_OSC1_TABLE, 2), P(WT_PID_OSC1_POS, 0.55f),
    P(WT_PID_MIX_OSC1, 0.55f),
    P(WT_PID_OSC2_TABLE, 2), P(WT_PID_OSC2_POS, 0.35f),
    P(WT_PID_OSC2_FINE, -8.0f), P(WT_PID_MIX_OSC2, 0.45f),
    P(WT_PID_ENV_POS, 0.0f),
    P(WT_PID_LFO2_RATE, 0.13f), P(WT_PID_LFO2_POS, 0.15f),
    P(WT_PID_ENV1_ATTACK, 0.8f), P(WT_PID_ENV1_SUSTAIN, 0.95f),
    P(WT_PID_ENV1_RELEASE, 2.0f),
    P(SYNTH_PID_COMMON_UNISON, 2), P(SYNTH_PID_COMMON_UNI_DETUNE, 10.0f),
    P(SYNTH_PID_COMMON_UNI_SPREAD, 1.0f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.3f), P(FX_PID_REV_MIX, 0.45f),
    P(FX_PID_REV_SIZE, 0.8f),
};

/* position crawls the whole sync sweep over ~3 s while held */
static const preset_pair_t kWtSyncRiser[] = {
    P(WT_PID_OSC1_TABLE, 1), P(WT_PID_OSC1_POS, 0.0f),
    P(WT_PID_ENV_POS, 1.0f),
    P(WT_PID_ENV2_ATTACK, 2.8f), P(WT_PID_ENV2_SUSTAIN, 1.0f),
    P(WT_PID_ENV1_ATTACK, 1.0f), P(WT_PID_ENV1_SUSTAIN, 1.0f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.35f),
};

/* fm table struck bright and falling back — metallic chime */
static const preset_pair_t kWtFmChime[] = {
    P(WT_PID_OSC1_TABLE, 3), P(WT_PID_OSC1_POS, 0.7f),
    P(WT_PID_ENV_POS, -0.5f),
    P(WT_PID_ENV2_DECAY, 1.2f), P(WT_PID_ENV2_SUSTAIN, 0.0f),
    P(WT_PID_ENV1_DECAY, 2.0f), P(WT_PID_ENV1_SUSTAIN, 0.0f),
    P(WT_PID_ENV1_RELEASE, 1.2f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.2f), P(FX_PID_DLY_TIME, 0.4f), P(FX_PID_DLY_PP, 1),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.3f),
};

/* granular pitch +12 with feedback over a vocal pad — shimmer */
static const preset_pair_t kWtShimmerPad[] = {
    P(WT_PID_OSC1_TABLE, 2), P(WT_PID_OSC1_POS, 0.4f),
    P(WT_PID_ENV_POS, 0.0f),
    P(WT_PID_ENV1_ATTACK, 0.7f), P(WT_PID_ENV1_SUSTAIN, 0.9f),
    P(WT_PID_ENV1_RELEASE, 1.8f),
    P(FX_PID_GRN_ON, 1),
    P(FX_PID_GRN_MIX, 0.35f), P(FX_PID_GRN_PITCH, 12.0f),
    P(FX_PID_GRN_SIZE, 0.3f), P(FX_PID_GRN_DENS, 18.0f),
    P(FX_PID_GRN_SPRAY, 0.12f), P(FX_PID_GRN_FB, 0.45f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.35f),
};

/* wheel rides the vowel morph — a talk box you play */
static const preset_pair_t kWtTalkBox[] = {
    P(WT_PID_OSC1_TABLE, 2), P(WT_PID_OSC1_POS, 0.0f),
    P(WT_PID_ENV_POS, 0.0f),
    P(WT_PID_FLT_CUTOFF, 5000.0f),
    P(WT_PID_ENV1_SUSTAIN, 0.9f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, WT_PID_OSC1_POS, 1.0f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.15f),
};

/* square-ish frame, snappy env, fixed fast vibrato — chiptune lead */
static const preset_pair_t kWtChipLead[] = {
    P(WT_PID_OSC1_POS, 0.75f), P(WT_PID_ENV_POS, 0.0f),
    P(WT_PID_FLT_CUTOFF, 12000.0f),
    P(WT_PID_ENV1_ATTACK, 0.001f), P(WT_PID_ENV1_DECAY, 0.1f),
    P(WT_PID_ENV1_SUSTAIN, 0.7f), P(WT_PID_ENV1_RELEASE, 0.05f),
    P(WT_PID_LFO1_RATE, 6.0f), P(WT_PID_LFO1_PITCH, 0.15f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.18f), P(FX_PID_DLY_TIME, 0.19f),
    P(FX_PID_DLY_FB, 0.3f),
};

/* dark filtered vocal table wandering in a huge damped room */
static const preset_pair_t kWtEvolvingDrone[] = {
    P(WT_PID_OSC1_TABLE, 2), P(WT_PID_OSC1_POS, 0.2f),
    P(WT_PID_ENV_POS, 0.1f),
    P(WT_PID_FLT_CUTOFF, 1000.0f),
    P(WT_PID_LFO2_RATE, 0.1f), P(WT_PID_LFO2_POS, 0.5f),
    P(WT_PID_ENV1_ATTACK, 1.5f), P(WT_PID_ENV1_SUSTAIN, 1.0f),
    P(WT_PID_ENV1_RELEASE, 2.5f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.5f), P(FX_PID_REV_SIZE, 0.9f),
    P(FX_PID_REV_DAMP, 0.5f),
};

/* short morph blips + the S12 arp: hold a chord and let it run */
static const preset_pair_t kWtArpCascade[] = {
    P(WT_PID_OSC1_POS, 0.6f), P(WT_PID_ENV_POS, -0.35f),
    P(WT_PID_ENV2_DECAY, 0.2f), P(WT_PID_ENV2_SUSTAIN, 0.0f),
    P(WT_PID_ENV1_ATTACK, 0.002f), P(WT_PID_ENV1_DECAY, 0.25f),
    P(WT_PID_ENV1_SUSTAIN, 0.0f), P(WT_PID_ENV1_RELEASE, 0.15f),
    P(SEQ_PID_ARP_MODE, 3), P(SEQ_PID_ARP_OCT, 2),
    P(SEQ_PID_DIV, 3), P(SEQ_PID_GATE, 0.4f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.3f), P(FX_PID_DLY_TIME, 0.375f),
    P(FX_PID_DLY_FB, 0.4f), P(FX_PID_DLY_PP, 1),
};

/* fm table low + a sine an octave up, gentle morph — glassy keys */
static const preset_pair_t kWtGlassyKeys[] = {
    P(WT_PID_OSC1_TABLE, 3), P(WT_PID_OSC1_POS, 0.25f),
    P(WT_PID_MIX_OSC1, 0.65f),
    P(WT_PID_OSC2_TABLE, 0), P(WT_PID_OSC2_POS, 0.05f),
    P(WT_PID_OSC2_SEMI, 12), P(WT_PID_MIX_OSC2, 0.3f),
    P(WT_PID_ENV_POS, 0.3f),
    P(WT_PID_ENV1_DECAY, 1.5f), P(WT_PID_ENV1_SUSTAIN, 0.2f),
    P(WT_PID_ENV1_RELEASE, 0.8f),
    MOD(0, SYNTH_MOD_SRC_VEL, WT_PID_OSC1_POS, 0.25f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.3f), P(FX_PID_REV_MIX, 0.35f),
};

/* ---- wavetable, slots 16-47 (S33) -----------------------------------
 *
 * Table sets: 0 basic, 1 sync, 2 vocal, 3 fm. The vocal set under the S33
 * vowel filter is the pairing worth knowing about — a formant spectrum
 * morphing inside a formant filter, two things moving at once — and several
 * of these are built on it. */

/* the vocal table inside the vowel filter, both morphing together */
static const preset_pair_t kWtDoubleVowel[] = {
    P(WT_PID_OSC1_TABLE, 2), P(WT_PID_OSC1_POS, 0.1f),
    P(WT_PID_OSC2_TABLE, 2), P(WT_PID_OSC2_POS, 0.5f),
    P(WT_PID_OSC2_FINE, -8.0f),
    P(WT_PID_MIX_OSC1, 0.55f), P(WT_PID_MIX_OSC2, 0.45f),
    P(WT_PID_ENV_POS, 0.4f),
    P(WT_PID_FLT_TYPE, 4), P(WT_PID_FLT_VOWEL, 0.0f),
    P(WT_PID_FLT_CUTOFF, 1000.0f), P(WT_PID_FLT_RESO, 0.55f),
    P(WT_PID_ENV1_ATTACK, 0.4f), P(WT_PID_ENV1_SUSTAIN, 0.9f),
    P(WT_PID_ENV1_RELEASE, 1.2f),
    MOD(0, SYNTH_MOD_SRC_ENV2, WT_PID_FLT_VOWEL, 0.5f),
    MOD(1, SYNTH_MOD_SRC_WHEEL, WT_PID_FLT_VOWEL, 0.5f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.45f), P(FX_PID_REV_SIZE, 0.75f),
};

/* LFO on the morph and a short envelope: it says something every note */
static const preset_pair_t kWtRobotVoice[] = {
    P(WT_PID_OSC1_TABLE, 2), P(WT_PID_OSC1_POS, 0.3f),
    P(WT_PID_MIX_OSC1, 0.9f), P(WT_PID_ENV_POS, 0.5f),
    P(WT_PID_FLT_TYPE, 4), P(WT_PID_FLT_VOWEL, 0.2f),
    P(WT_PID_FLT_CUTOFF, 1100.0f), P(WT_PID_FLT_RESO, 0.75f),
    P(WT_PID_FLT_DRIVE, 0.3f),
    P(WT_PID_ENV1_ATTACK, 0.005f), P(WT_PID_ENV1_SUSTAIN, 0.85f),
    P(WT_PID_ENV1_RELEASE, 0.12f),
    P(WT_PID_LFO2_RATE, 4.5f), P(WT_PID_LFO2_WAVE, 4),
    MOD(0, SYNTH_MOD_SRC_LFO2, WT_PID_FLT_VOWEL, 0.6f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.22f),
};

/* formant shift on the cutoff, morph held: a vocal tract changing size */
static const preset_pair_t kWtFormantSweep[] = {
    P(WT_PID_OSC1_TABLE, 2), P(WT_PID_OSC1_POS, 0.6f),
    P(WT_PID_MIX_OSC1, 0.85f),
    P(WT_PID_FLT_TYPE, 4), P(WT_PID_FLT_VOWEL, 0.45f),
    P(WT_PID_FLT_CUTOFF, 700.0f), P(WT_PID_FLT_RESO, 0.6f),
    P(WT_PID_FLT_ENV, 2.0f),
    P(WT_PID_ENV2_ATTACK, 0.3f), P(WT_PID_ENV2_DECAY, 1.8f),
    P(WT_PID_ENV2_SUSTAIN, 0.4f),
    P(WT_PID_ENV1_ATTACK, 0.25f), P(WT_PID_ENV1_SUSTAIN, 0.9f),
    P(WT_PID_ENV1_RELEASE, 1.0f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.4f),
};

/* sync table with the ladder behind it — the scream has a floor now */
static const preset_pair_t kWtSyncScream[] = {
    P(WT_PID_OSC1_TABLE, 1), P(WT_PID_OSC1_POS, 0.2f),
    P(WT_PID_MIX_OSC1, 0.9f), P(WT_PID_ENV_POS, 0.75f),
    P(WT_PID_FLT_TYPE, 2), P(WT_PID_FLT_CUTOFF, 1400.0f),
    P(WT_PID_FLT_RESO, 0.6f), P(WT_PID_FLT_DRIVE, 0.45f),
    P(WT_PID_FLT_KBD, 0.8f),
    P(WT_PID_ENV2_ATTACK, 0.01f), P(WT_PID_ENV2_DECAY, 0.7f),
    P(WT_PID_ENV2_SUSTAIN, 0.3f),
    P(WT_PID_ENV1_SUSTAIN, 0.9f), P(WT_PID_ENV1_RELEASE, 0.15f),
    P(SYNTH_PID_COMMON_GLIDE, 0.04f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.25f), P(FX_PID_DLY_FB, 0.35f),
};

/* hard sync in the bass register, filter tight */
static const preset_pair_t kWtSyncBass[] = {
    P(WT_PID_OSC1_TABLE, 1), P(WT_PID_OSC1_POS, 0.15f),
    P(WT_PID_MIX_OSC1, 0.95f), P(WT_PID_ENV_POS, 0.45f),
    P(WT_PID_FLT_TYPE, 2), P(WT_PID_FLT_CUTOFF, 380.0f),
    P(WT_PID_FLT_RESO, 0.45f), P(WT_PID_FLT_ENV, 2.2f),
    P(WT_PID_FLT_KBD, 0.3f), P(WT_PID_FLT_DRIVE, 0.3f),
    P(WT_PID_ENV2_DECAY, 0.18f), P(WT_PID_ENV2_SUSTAIN, 0.0f),
    P(WT_PID_ENV1_DECAY, 0.4f), P(WT_PID_ENV1_SUSTAIN, 0.5f),
    P(WT_PID_ENV1_RELEASE, 0.1f),
};

/* the fm table run clean and bright: digital, glassy, no filter at all */
static const preset_pair_t kWtDigitalBass[] = {
    P(WT_PID_OSC1_TABLE, 3), P(WT_PID_OSC1_POS, 0.25f),
    P(WT_PID_OSC2_TABLE, 0), P(WT_PID_OSC2_SEMI, -12),
    P(WT_PID_OSC2_FINE, 0.0f),
    P(WT_PID_MIX_OSC1, 0.6f), P(WT_PID_MIX_OSC2, 0.45f),
    P(WT_PID_ENV_POS, 0.3f), P(WT_PID_FLT_ON, 0),
    P(WT_PID_ENV1_ATTACK, 0.002f), P(WT_PID_ENV1_DECAY, 0.5f),
    P(WT_PID_ENV1_SUSTAIN, 0.55f), P(WT_PID_ENV1_RELEASE, 0.1f),
};

/* 24 dB slope on a morphing spectrum — the pad the engine deserved */
static const preset_pair_t kWtWidePad[] = {
    P(WT_PID_OSC1_TABLE, 0), P(WT_PID_OSC1_POS, 0.2f),
    P(WT_PID_OSC2_TABLE, 0), P(WT_PID_OSC2_POS, 0.6f),
    P(WT_PID_OSC2_FINE, -12.0f),
    P(WT_PID_MIX_OSC1, 0.5f), P(WT_PID_MIX_OSC2, 0.5f),
    P(WT_PID_ENV_POS, 0.45f),
    P(WT_PID_FLT_TYPE, 1), P(WT_PID_FLT_CUTOFF, 2400.0f),
    P(WT_PID_FLT_RESO, 0.3f), P(WT_PID_FLT_ENV, 1.5f),
    P(WT_PID_ENV1_ATTACK, 0.7f), P(WT_PID_ENV1_SUSTAIN, 0.95f),
    P(WT_PID_ENV1_RELEASE, 1.6f),
    P(SYNTH_PID_COMMON_UNISON, 2), P(SYNTH_PID_COMMON_UNI_DETUNE, 15.0f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.45f), P(FX_PID_REV_SIZE, 0.8f),
};

/* dual band on a wavetable: the morph only shows through a narrow window */
static const preset_pair_t kWtBandMorph[] = {
    P(WT_PID_OSC1_TABLE, 0), P(WT_PID_OSC1_POS, 0.35f),
    P(WT_PID_MIX_OSC1, 0.9f), P(WT_PID_ENV_POS, 0.6f),
    P(WT_PID_FLT_TYPE, 3), P(WT_PID_FLT_SPREAD, 1.0f),
    P(WT_PID_FLT_CUTOFF, 1200.0f), P(WT_PID_FLT_RESO, 0.5f),
    P(WT_PID_ENV2_DECAY, 1.2f), P(WT_PID_ENV2_SUSTAIN, 0.4f),
    P(WT_PID_ENV1_ATTACK, 0.1f), P(WT_PID_ENV1_SUSTAIN, 0.85f),
    P(WT_PID_ENV1_RELEASE, 0.6f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.35f),
};

/* a notch wandering across the morph — two kinds of motion at once */
static const preset_pair_t kWtNotchMorph[] = {
    P(WT_PID_OSC1_TABLE, 0), P(WT_PID_OSC1_POS, 0.1f),
    P(WT_PID_OSC2_TABLE, 0), P(WT_PID_OSC2_POS, 0.7f),
    P(WT_PID_OSC2_FINE, 6.0f),
    P(WT_PID_MIX_OSC1, 0.5f), P(WT_PID_MIX_OSC2, 0.5f),
    P(WT_PID_FLT_MODE, 3), P(WT_PID_FLT_CUTOFF, 900.0f),
    P(WT_PID_FLT_RESO, 0.55f),
    P(WT_PID_LFO2_RATE, 0.3f), P(WT_PID_LFO2_POS, 0.4f),
    P(WT_PID_ENV1_ATTACK, 0.5f), P(WT_PID_ENV1_SUSTAIN, 0.9f),
    P(WT_PID_ENV1_RELEASE, 1.2f),
    MOD(0, SYNTH_MOD_SRC_LFO2, WT_PID_FLT_CUTOFF, 0.4f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.42f),
};

/* peak mode riding the morph: a resonant formant that is not a vowel */
static const preset_pair_t kWtPeakMorph[] = {
    P(WT_PID_OSC1_TABLE, 3), P(WT_PID_OSC1_POS, 0.4f),
    P(WT_PID_MIX_OSC1, 0.9f), P(WT_PID_ENV_POS, 0.55f),
    P(WT_PID_FLT_MODE, 4), P(WT_PID_FLT_CUTOFF, 1300.0f),
    P(WT_PID_FLT_RESO, 0.8f), P(WT_PID_FLT_ENV, 2.5f),
    P(WT_PID_FLT_KBD, 0.5f),
    P(WT_PID_ENV2_DECAY, 0.6f), P(WT_PID_ENV2_SUSTAIN, 0.2f),
    P(WT_PID_ENV1_DECAY, 0.9f), P(WT_PID_ENV1_SUSTAIN, 0.4f),
    P(WT_PID_ENV1_RELEASE, 0.5f),
    P(FX_PID_DLY_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_DLY_MIX, 0.2f), P(FX_PID_REV_MIX, 0.3f),
};

/* drive on a wavetable lead — grit that the tables cannot make alone */
static const preset_pair_t kWtDriveLead[] = {
    P(WT_PID_OSC1_TABLE, 1), P(WT_PID_OSC1_POS, 0.5f),
    P(WT_PID_OSC2_TABLE, 0), P(WT_PID_OSC2_FINE, 9.0f),
    P(WT_PID_MIX_OSC1, 0.6f), P(WT_PID_MIX_OSC2, 0.4f),
    P(WT_PID_ENV_POS, 0.35f),
    P(WT_PID_FLT_TYPE, 1), P(WT_PID_FLT_CUTOFF, 1800.0f),
    P(WT_PID_FLT_RESO, 0.4f), P(WT_PID_FLT_DRIVE, 0.65f),
    P(WT_PID_FLT_KBD, 0.85f),
    P(WT_PID_ENV1_SUSTAIN, 0.9f), P(WT_PID_ENV1_RELEASE, 0.15f),
    P(SYNTH_PID_COMMON_GLIDE, 0.05f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.28f), P(FX_PID_DLY_FB, 0.4f),
};

/* the basic table swept slowly with nothing else moving */
static const preset_pair_t kWtSlowMorph[] = {
    P(WT_PID_OSC1_TABLE, 0), P(WT_PID_OSC1_POS, 0.0f),
    P(WT_PID_MIX_OSC1, 0.85f), P(WT_PID_ENV_POS, 0.0f),
    P(WT_PID_FLT_CUTOFF, 7000.0f), P(WT_PID_FLT_ENV, 0.0f),
    P(WT_PID_LFO2_RATE, 0.09f), P(WT_PID_LFO2_WAVE, 1),
    P(WT_PID_LFO2_POS, 1.0f),
    P(WT_PID_ENV1_ATTACK, 1.2f), P(WT_PID_ENV1_SUSTAIN, 1.0f),
    P(WT_PID_ENV1_RELEASE, 2.0f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.5f), P(FX_PID_REV_SIZE, 0.85f),
};

/* velocity picks the frame: soft is a sine, hard is the far end */
static const preset_pair_t kWtVelMorph[] = {
    P(WT_PID_OSC1_TABLE, 0), P(WT_PID_OSC1_POS, 0.05f),
    P(WT_PID_MIX_OSC1, 0.9f), P(WT_PID_ENV_POS, 0.2f),
    P(WT_PID_FLT_CUTOFF, 4000.0f), P(WT_PID_FLT_ENV, 1.2f),
    P(WT_PID_ENV1_DECAY, 0.8f), P(WT_PID_ENV1_SUSTAIN, 0.45f),
    P(WT_PID_ENV1_RELEASE, 0.35f),
    MOD(0, SYNTH_MOD_SRC_VEL, WT_PID_OSC1_POS, 0.75f),
    MOD(1, SYNTH_MOD_SRC_VEL, WT_PID_FLT_CUTOFF, 0.4f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.28f),
};

/* two octaves of the fm table, struck and left to ring */
static const preset_pair_t kWtBellStack[] = {
    P(WT_PID_OSC1_TABLE, 3), P(WT_PID_OSC1_POS, 0.7f),
    P(WT_PID_OSC2_TABLE, 3), P(WT_PID_OSC2_POS, 0.35f),
    P(WT_PID_OSC2_SEMI, 12), P(WT_PID_OSC2_FINE, 5.0f),
    P(WT_PID_MIX_OSC1, 0.6f), P(WT_PID_MIX_OSC2, 0.35f),
    P(WT_PID_ENV_POS, -0.4f),
    P(WT_PID_FLT_CUTOFF, 6000.0f), P(WT_PID_FLT_ENV, 0.0f),
    P(WT_PID_ENV2_DECAY, 1.0f), P(WT_PID_ENV2_SUSTAIN, 0.0f),
    P(WT_PID_ENV1_ATTACK, 0.001f), P(WT_PID_ENV1_DECAY, 2.5f),
    P(WT_PID_ENV1_SUSTAIN, 0.0f), P(WT_PID_ENV1_RELEASE, 1.8f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.45f), P(FX_PID_REV_SIZE, 0.8f),
};

/* short, bright, and gone: the wavetable as a pluck */
static const preset_pair_t kWtBrightPluck[] = {
    P(WT_PID_OSC1_TABLE, 0), P(WT_PID_OSC1_POS, 0.55f),
    P(WT_PID_MIX_OSC1, 0.9f), P(WT_PID_ENV_POS, -0.5f),
    P(WT_PID_FLT_CUTOFF, 3500.0f), P(WT_PID_FLT_RESO, 0.35f),
    P(WT_PID_FLT_ENV, 2.0f), P(WT_PID_FLT_KBD, 0.7f),
    P(WT_PID_ENV2_DECAY, 0.12f), P(WT_PID_ENV2_SUSTAIN, 0.0f),
    P(WT_PID_ENV1_ATTACK, 0.001f), P(WT_PID_ENV1_DECAY, 0.4f),
    P(WT_PID_ENV1_SUSTAIN, 0.0f), P(WT_PID_ENV1_RELEASE, 0.2f),
    MOD(0, SYNTH_MOD_SRC_VEL, WT_PID_FLT_CUTOFF, 0.5f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.25f),
};

/* the same pluck with the ladder closed down over it */
static const preset_pair_t kWtDarkPluck[] = {
    P(WT_PID_OSC1_TABLE, 0), P(WT_PID_OSC1_POS, 0.4f),
    P(WT_PID_MIX_OSC1, 0.9f), P(WT_PID_ENV_POS, 0.2f),
    P(WT_PID_FLT_TYPE, 2), P(WT_PID_FLT_CUTOFF, 700.0f),
    P(WT_PID_FLT_RESO, 0.5f), P(WT_PID_FLT_ENV, 1.5f),
    P(WT_PID_FLT_KBD, 0.6f), P(WT_PID_FLT_DRIVE, 0.25f),
    P(WT_PID_ENV2_DECAY, 0.2f), P(WT_PID_ENV2_SUSTAIN, 0.0f),
    P(WT_PID_ENV1_ATTACK, 0.001f), P(WT_PID_ENV1_DECAY, 0.7f),
    P(WT_PID_ENV1_SUSTAIN, 0.0f), P(WT_PID_ENV1_RELEASE, 0.3f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.3f),
};

/* ladder bass on the basic table: round and heavy */
static const preset_pair_t kWtLadderBass[] = {
    P(WT_PID_OSC1_TABLE, 0), P(WT_PID_OSC1_POS, 0.15f),
    P(WT_PID_OSC2_TABLE, 0), P(WT_PID_OSC2_SEMI, -12),
    P(WT_PID_OSC2_FINE, 0.0f),
    P(WT_PID_MIX_OSC1, 0.55f), P(WT_PID_MIX_OSC2, 0.5f),
    P(WT_PID_ENV_POS, 0.25f),
    P(WT_PID_FLT_TYPE, 2), P(WT_PID_FLT_CUTOFF, 300.0f),
    P(WT_PID_FLT_RESO, 0.5f), P(WT_PID_FLT_ENV, 2.0f),
    P(WT_PID_FLT_KBD, 0.3f), P(WT_PID_FLT_DRIVE, 0.3f),
    P(WT_PID_ENV2_DECAY, 0.22f), P(WT_PID_ENV2_SUSTAIN, 0.0f),
    P(WT_PID_ENV1_DECAY, 0.5f), P(WT_PID_ENV1_SUSTAIN, 0.5f),
    P(WT_PID_ENV1_RELEASE, 0.12f),
};

/* the vowel filter down in the bass register — a bass that mumbles */
static const preset_pair_t kWtVowelBass[] = {
    P(WT_PID_OSC1_TABLE, 0), P(WT_PID_OSC1_POS, 0.2f),
    P(WT_PID_MIX_OSC1, 0.95f), P(WT_PID_ENV_POS, 0.3f),
    P(WT_PID_FLT_TYPE, 4), P(WT_PID_FLT_VOWEL, 0.75f),
    P(WT_PID_FLT_CUTOFF, 480.0f), P(WT_PID_FLT_RESO, 0.6f),
    P(WT_PID_FLT_DRIVE, 0.35f), P(WT_PID_FLT_KBD, 0.2f),
    P(WT_PID_ENV1_ATTACK, 0.003f), P(WT_PID_ENV1_DECAY, 0.5f),
    P(WT_PID_ENV1_SUSTAIN, 0.55f), P(WT_PID_ENV1_RELEASE, 0.12f),
    MOD(0, SYNTH_MOD_SRC_VEL, WT_PID_FLT_VOWEL, -0.35f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.18f),
};

/* brass out of a wavetable: position rises with the envelope, not a filter */
static const preset_pair_t kWtBrass[] = {
    P(WT_PID_OSC1_TABLE, 0), P(WT_PID_OSC1_POS, 0.1f),
    P(WT_PID_OSC2_TABLE, 0), P(WT_PID_OSC2_FINE, -7.0f),
    P(WT_PID_MIX_OSC1, 0.55f), P(WT_PID_MIX_OSC2, 0.45f),
    P(WT_PID_ENV_POS, 0.7f),
    P(WT_PID_FLT_CUTOFF, 4500.0f), P(WT_PID_FLT_ENV, 1.0f),
    P(WT_PID_ENV2_ATTACK, 0.06f), P(WT_PID_ENV2_DECAY, 0.6f),
    P(WT_PID_ENV2_SUSTAIN, 0.7f),
    P(WT_PID_ENV1_ATTACK, 0.03f), P(WT_PID_ENV1_SUSTAIN, 0.9f),
    P(WT_PID_ENV1_RELEASE, 0.2f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.3f),
};

/* string ensemble: two positions, slow vibrato, chorus on top */
static const preset_pair_t kWtStrings[] = {
    P(WT_PID_OSC1_TABLE, 0), P(WT_PID_OSC1_POS, 0.3f),
    P(WT_PID_OSC2_TABLE, 0), P(WT_PID_OSC2_POS, 0.45f),
    P(WT_PID_OSC2_FINE, 11.0f),
    P(WT_PID_MIX_OSC1, 0.5f), P(WT_PID_MIX_OSC2, 0.5f),
    P(WT_PID_ENV_POS, 0.2f),
    P(WT_PID_FLT_TYPE, 1), P(WT_PID_FLT_CUTOFF, 3000.0f),
    P(WT_PID_FLT_ENV, 1.0f),
    P(WT_PID_ENV1_ATTACK, 0.35f), P(WT_PID_ENV1_SUSTAIN, 0.95f),
    P(WT_PID_ENV1_RELEASE, 0.9f),
    P(WT_PID_LFO1_RATE, 4.8f), P(WT_PID_LFO1_PITCH, 0.06f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.4f), P(FX_PID_REV_MIX, 0.4f),
};

/* highpassed and thin — a wavetable that sits on top of a full mix */
static const preset_pair_t kWtAirLayer[] = {
    P(WT_PID_OSC1_TABLE, 3), P(WT_PID_OSC1_POS, 0.8f),
    P(WT_PID_MIX_OSC1, 0.8f), P(WT_PID_ENV_POS, -0.3f),
    P(WT_PID_FLT_MODE, 2), P(WT_PID_FLT_CUTOFF, 1500.0f),
    P(WT_PID_FLT_RESO, 0.3f), P(WT_PID_FLT_ENV, 0.0f),
    P(WT_PID_ENV1_ATTACK, 0.6f), P(WT_PID_ENV1_SUSTAIN, 0.85f),
    P(WT_PID_ENV1_RELEASE, 1.4f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.5f), P(FX_PID_REV_SIZE, 0.85f),
};

/* one table frame held still, allpass swinging the phase across it */
static const preset_pair_t kWtPhaseDrift[] = {
    P(WT_PID_OSC1_TABLE, 0), P(WT_PID_OSC1_POS, 0.25f),
    P(WT_PID_OSC2_TABLE, 0), P(WT_PID_OSC2_POS, 0.25f),
    P(WT_PID_OSC2_FINE, 4.0f),
    P(WT_PID_MIX_OSC1, 0.5f), P(WT_PID_MIX_OSC2, 0.5f),
    P(WT_PID_ENV_POS, 0.0f),
    P(WT_PID_FLT_MODE, 5), P(WT_PID_FLT_CUTOFF, 1000.0f),
    P(WT_PID_FLT_RESO, 0.75f),
    P(WT_PID_LFO2_RATE, 0.45f),
    MOD(0, SYNTH_MOD_SRC_LFO2, WT_PID_FLT_CUTOFF, 0.5f),
    P(WT_PID_ENV1_ATTACK, 0.3f), P(WT_PID_ENV1_SUSTAIN, 0.9f),
    P(WT_PID_ENV1_RELEASE, 1.0f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.4f),
};

/* sub octave under a bright frame: bass and top, nothing in the middle */
static const preset_pair_t kWtSplitBass[] = {
    P(WT_PID_OSC1_TABLE, 0), P(WT_PID_OSC1_POS, 0.0f),
    P(WT_PID_OSC2_TABLE, 3), P(WT_PID_OSC2_POS, 0.6f),
    P(WT_PID_OSC2_SEMI, 12), P(WT_PID_OSC2_FINE, 0.0f),
    P(WT_PID_MIX_OSC1, 0.7f), P(WT_PID_MIX_OSC2, 0.3f),
    P(WT_PID_ENV_POS, 0.2f),
    P(WT_PID_FLT_MODE, 3), P(WT_PID_FLT_CUTOFF, 800.0f),
    P(WT_PID_FLT_RESO, 0.4f), P(WT_PID_FLT_ENV, 0.5f),
    P(WT_PID_ENV1_DECAY, 0.6f), P(WT_PID_ENV1_SUSTAIN, 0.6f),
    P(WT_PID_ENV1_RELEASE, 0.15f),
};

/* the vocal table an octave apart, no filter: raw formant chords */
static const preset_pair_t kWtChoirStack[] = {
    P(WT_PID_OSC1_TABLE, 2), P(WT_PID_OSC1_POS, 0.2f),
    P(WT_PID_OSC2_TABLE, 2), P(WT_PID_OSC2_POS, 0.55f),
    P(WT_PID_OSC2_SEMI, 12), P(WT_PID_OSC2_FINE, -5.0f),
    P(WT_PID_MIX_OSC1, 0.6f), P(WT_PID_MIX_OSC2, 0.35f),
    P(WT_PID_ENV_POS, 0.3f), P(WT_PID_FLT_ON, 0),
    P(WT_PID_ENV1_ATTACK, 0.5f), P(WT_PID_ENV1_SUSTAIN, 0.95f),
    P(WT_PID_ENV1_RELEASE, 1.5f),
    P(WT_PID_LFO1_RATE, 4.2f), P(WT_PID_LFO1_PITCH, 0.07f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.3f), P(FX_PID_REV_MIX, 0.5f),
};

/* sharp square-ish frame, no filter movement, fast delay — chiptune arp */
static const preset_pair_t kWtChipArp[] = {
    P(WT_PID_OSC1_TABLE, 0), P(WT_PID_OSC1_POS, 0.65f),
    P(WT_PID_MIX_OSC1, 0.9f), P(WT_PID_ENV_POS, 0.0f),
    P(WT_PID_FLT_ON, 0),
    P(WT_PID_ENV1_ATTACK, 0.001f), P(WT_PID_ENV1_DECAY, 0.12f),
    P(WT_PID_ENV1_SUSTAIN, 0.25f), P(WT_PID_ENV1_RELEASE, 0.05f),
    P(FX_PID_DLY_ON, 1),
    P(FX_PID_DLY_MIX, 0.3f), P(FX_PID_DLY_TIME, 0.14f),
    P(FX_PID_DLY_FB, 0.4f), P(FX_PID_DLY_PP, 1),
};

/* the position LFO faster than the note: a sample-and-hold texture */
static const preset_pair_t kWtStepTexture[] = {
    P(WT_PID_OSC1_TABLE, 3), P(WT_PID_OSC1_POS, 0.3f),
    P(WT_PID_MIX_OSC1, 0.85f), P(WT_PID_ENV_POS, 0.0f),
    P(WT_PID_FLT_TYPE, 1), P(WT_PID_FLT_CUTOFF, 2500.0f),
    P(WT_PID_FLT_RESO, 0.4f),
    P(WT_PID_LFO2_RATE, 9.0f), P(WT_PID_LFO2_WAVE, 4),
    P(WT_PID_LFO2_POS, 0.8f),
    P(WT_PID_ENV1_ATTACK, 0.02f), P(WT_PID_ENV1_SUSTAIN, 0.8f),
    P(WT_PID_ENV1_RELEASE, 0.3f),
    P(FX_PID_DLY_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_DLY_MIX, 0.25f), P(FX_PID_REV_MIX, 0.3f),
};

/* long attack, long release, position crawling: something to leave running */
static const preset_pair_t kWtSlowTexture[] = {
    P(WT_PID_OSC1_TABLE, 2), P(WT_PID_OSC1_POS, 0.1f),
    P(WT_PID_OSC2_TABLE, 3), P(WT_PID_OSC2_POS, 0.4f),
    P(WT_PID_OSC2_SEMI, -12), P(WT_PID_OSC2_FINE, 7.0f),
    P(WT_PID_MIX_OSC1, 0.5f), P(WT_PID_MIX_OSC2, 0.4f),
    P(WT_PID_ENV_POS, 0.6f),
    P(WT_PID_FLT_TYPE, 3), P(WT_PID_FLT_SPREAD, 3.0f),
    P(WT_PID_FLT_CUTOFF, 900.0f), P(WT_PID_FLT_RESO, 0.4f),
    P(WT_PID_ENV2_ATTACK, 2.0f), P(WT_PID_ENV2_DECAY, 4.0f),
    P(WT_PID_ENV2_SUSTAIN, 0.6f),
    P(WT_PID_ENV1_ATTACK, 2.0f), P(WT_PID_ENV1_SUSTAIN, 1.0f),
    P(WT_PID_ENV1_RELEASE, 3.0f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.55f), P(FX_PID_REV_SIZE, 0.9f),
};

/* wheel drives the morph directly — a hands-on spectral sweep */
static const preset_pair_t kWtWheelMorph[] = {
    P(WT_PID_OSC1_TABLE, 1), P(WT_PID_OSC1_POS, 0.0f),
    P(WT_PID_MIX_OSC1, 0.9f), P(WT_PID_ENV_POS, 0.0f),
    P(WT_PID_FLT_TYPE, 1), P(WT_PID_FLT_CUTOFF, 3000.0f),
    P(WT_PID_FLT_RESO, 0.35f),
    P(WT_PID_ENV1_ATTACK, 0.01f), P(WT_PID_ENV1_SUSTAIN, 0.9f),
    P(WT_PID_ENV1_RELEASE, 0.3f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, WT_PID_OSC1_POS, 0.9f),
    MOD(1, SYNTH_MOD_SRC_WHEEL, WT_PID_FLT_CUTOFF, 0.3f),
    P(FX_PID_DLY_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_DLY_MIX, 0.2f), P(FX_PID_REV_MIX, 0.3f),
};

/* organ registration out of two static frames, filter parked open */
static const preset_pair_t kWtDrawOrgan[] = {
    P(WT_PID_OSC1_TABLE, 0), P(WT_PID_OSC1_POS, 0.6f),
    P(WT_PID_OSC2_TABLE, 0), P(WT_PID_OSC2_POS, 0.6f),
    P(WT_PID_OSC2_SEMI, 12), P(WT_PID_OSC2_FINE, 0.0f),
    P(WT_PID_MIX_OSC1, 0.6f), P(WT_PID_MIX_OSC2, 0.35f),
    P(WT_PID_ENV_POS, 0.0f), P(WT_PID_FLT_ON, 0),
    P(WT_PID_ENV1_ATTACK, 0.004f), P(WT_PID_ENV1_DECAY, 0.05f),
    P(WT_PID_ENV1_SUSTAIN, 1.0f), P(WT_PID_ENV1_RELEASE, 0.05f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_REV_ON, 1),
    P(FX_PID_CHO_MIX, 0.3f), P(FX_PID_REV_MIX, 0.2f),
};

/* a full-range riser: position, cutoff and pitch all climbing together */
static const preset_pair_t kWtSpectralRiser[] = {
    P(WT_PID_OSC1_TABLE, 1), P(WT_PID_OSC1_POS, 0.0f),
    P(WT_PID_MIX_OSC1, 0.85f), P(WT_PID_ENV_POS, 1.0f),
    P(WT_PID_FLT_TYPE, 1), P(WT_PID_FLT_CUTOFF, 300.0f),
    P(WT_PID_FLT_RESO, 0.55f), P(WT_PID_FLT_ENV, 4.0f),
    P(WT_PID_ENV2_ATTACK, 3.0f), P(WT_PID_ENV2_DECAY, 4.0f),
    P(WT_PID_ENV2_SUSTAIN, 1.0f),
    P(WT_PID_ENV1_ATTACK, 0.2f), P(WT_PID_ENV1_SUSTAIN, 1.0f),
    P(WT_PID_ENV1_RELEASE, 0.5f),
    P(WT_PID_LFO1_RATE, 6.0f), P(WT_PID_LFO1_PITCH, 0.25f),
    P(FX_PID_REV_ON, 1), P(FX_PID_DLY_ON, 1),
    P(FX_PID_REV_MIX, 0.45f), P(FX_PID_DLY_MIX, 0.25f),
};

/* crushed and narrow: the wavetable put through the lo-fi stage */
static const preset_pair_t kWtLofiKeys[] = {
    P(WT_PID_OSC1_TABLE, 0), P(WT_PID_OSC1_POS, 0.35f),
    P(WT_PID_MIX_OSC1, 0.85f), P(WT_PID_ENV_POS, 0.25f),
    P(WT_PID_FLT_TYPE, 3), P(WT_PID_FLT_SPREAD, 1.6f),
    P(WT_PID_FLT_CUTOFF, 1100.0f), P(WT_PID_FLT_RESO, 0.35f),
    P(WT_PID_ENV1_ATTACK, 0.004f), P(WT_PID_ENV1_DECAY, 0.9f),
    P(WT_PID_ENV1_SUSTAIN, 0.35f), P(WT_PID_ENV1_RELEASE, 0.4f),
    P(FX_PID_CRUSH_ON, 1),
    P(FX_PID_CRUSH_MIX, 0.45f), P(FX_PID_CRUSH_BITS, 7.0f),
    P(FX_PID_CRUSH_DOWN, 3),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.3f),
};

/* grains over a slow morph — the granular delay doing the heavy lifting */
static const preset_pair_t kWtGrainCloud[] = {
    P(WT_PID_OSC1_TABLE, 2), P(WT_PID_OSC1_POS, 0.4f),
    P(WT_PID_MIX_OSC1, 0.8f), P(WT_PID_ENV_POS, 0.3f),
    P(WT_PID_FLT_CUTOFF, 4000.0f), P(WT_PID_FLT_ENV, 0.8f),
    P(WT_PID_ENV1_ATTACK, 0.8f), P(WT_PID_ENV1_SUSTAIN, 0.9f),
    P(WT_PID_ENV1_RELEASE, 1.5f),
    P(WT_PID_LFO2_RATE, 0.13f), P(WT_PID_LFO2_POS, 0.6f),
    P(FX_PID_GRN_ON, 1),
    P(FX_PID_GRN_MIX, 0.45f), P(FX_PID_GRN_SIZE, 0.14f),
    P(FX_PID_GRN_DENS, 18.0f), P(FX_PID_GRN_SPRAY, 0.06f),
    P(FX_PID_REV_ON, 1),
    P(FX_PID_REV_MIX, 0.4f),
};

/* ---- granular (bank 5, linear slots 560-671) ------------------------------
 *
 * A full 48, laid out so the bank is a tour of the engine's axes rather than
 * 48 variations on one of them. A granular patch lives or dies on the
 * interaction of grain length, onset rate and window shape — two settings a
 * semitone apart on paper can be a vowel and a rattle — so each slot is meant
 * to land somewhere the others do not. Between them they cover:
 *
 *   - grn.form as a formant above the key: vowels, bells, brass, reeds, and
 *     the metallic end where the ratio stops being harmonic
 *   - the formant *below* the key, which no oscillator chain reaches
 *   - free mode across the whole density range, 7 grains/s to 240
 *   - grn.shape end to end, from a hard strike to a slow swell
 *   - noise grains both on the sync grid (pitched noise) and off it
 *   - the mod matrix as the performance surface: velocity and the wheel onto
 *     scatter, jitter, size and shape rather than onto volume
 *   - grn.src = in, live and frozen
 *
 * The seven named "in: …" are that last group. They are silent on a build
 * with no audio input, which is why the names say so before loading.
 *
 * None of them ships with buf.freeze on, and that is not an oversight.
 * The ring is calloc'd when the engine binds and freed when it unbinds, so at
 * the moment a preset finishes loading it holds nothing at all — a patch that
 * arrived frozen would be holding silence, permanently, until the player
 * thought to unfreeze it. Freeze is a performance control: play the input in,
 * then latch it. They
 * earn their slots anyway: granulating the input is the half of this engine
 * no other engine reaches, and a device that has an input has nowhere else
 * to hear it demonstrated.
 */
/* the default patch, said out loud: one sine grain per cycle at 2x, so the
 * key is the pitch and the formant sits an octave above it */
static const preset_pair_t kGranFofVowel[] = {
    P(GRAN_PID_FORM, 3.0f), P(GRAN_PID_SIZE, 26.0f),
    P(GRAN_PID_SHAPE, 0.42f),
    P(GRAN_PID_FLT_TYPE, 4), /* vowel */
    P(GRAN_PID_FLT_VOWEL, 0.25f), P(GRAN_PID_FLT_CUTOFF, 1400.0f),
    P(GRAN_PID_FLT_RESO, 0.25f),
    P(GRAN_PID_ENV1_ATTACK, 0.12f), P(GRAN_PID_ENV1_SUSTAIN, 0.85f),
    P(GRAN_PID_ENV1_RELEASE, 0.6f),
    P(GRAN_PID_LFO1_RATE, 4.6f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, GRAN_PID_LFO1_PITCH, 0.3f),
    MOD(1, SYNTH_MOD_SRC_WHEEL, GRAN_PID_FLT_VOWEL, 0.5f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.3f), P(FX_PID_REV_SIZE, 0.65f),
};

/* short saw grains struck at the note rate: a buzzy resonant body, which is
 * what pulsar synthesis sounds like before anything is done to it */
static const preset_pair_t kGranPulsarBass[] = {
    P(GRAN_PID_WAVE, 2), /* saw */
    P(GRAN_PID_FORM, 1.5f), P(GRAN_PID_SIZE, 9.0f),
    P(GRAN_PID_SHAPE, 0.18f), P(GRAN_PID_SPREAD, 0.1f),
    P(GRAN_PID_FLT_CUTOFF, 1800.0f), P(GRAN_PID_FLT_RESO, 0.35f),
    P(GRAN_PID_FLT_ENV, 1.6f), P(GRAN_PID_FLT_KBD, 0.35f),
    P(GRAN_PID_ENV2_DECAY, 0.14f), P(GRAN_PID_ENV2_SUSTAIN, 0.0f),
    P(GRAN_PID_ENV1_DECAY, 0.3f), P(GRAN_PID_ENV1_SUSTAIN, 0.6f),
    P(GRAN_PID_ENV1_RELEASE, 0.12f),
    P(FX_PID_DRV_ON, 1), P(FX_PID_DRV_MIX, 0.3f),
};

/* env2 walks the formant two octaves while the note holds still — the one
 * gesture that is only available on this engine */
static const preset_pair_t kGranFormantSweep[] = {
    P(GRAN_PID_WAVE, 2), P(GRAN_PID_FORM, 1.0f),
    P(GRAN_PID_SIZE, 18.0f), P(GRAN_PID_SHAPE, 0.35f),
    P(GRAN_PID_ENV_FORM, 2.2f),
    P(GRAN_PID_ENV2_ATTACK, 0.004f), P(GRAN_PID_ENV2_DECAY, 0.7f),
    P(GRAN_PID_ENV2_SUSTAIN, 0.1f), P(GRAN_PID_ENV2_RELEASE, 0.5f),
    P(GRAN_PID_FLT_CUTOFF, 7000.0f),
    P(GRAN_PID_ENV1_SUSTAIN, 0.8f), P(GRAN_PID_ENV1_RELEASE, 0.45f),
    P(FX_PID_DLY_ON, 1), P(FX_PID_DLY_MIX, 0.2f), P(FX_PID_DLY_TIME, 0.375f),
    P(FX_PID_DLY_FB, 0.4f), P(FX_PID_DLY_PP, 1),
};

/* free mode: onsets stop tracking the key, so the cloud is asynchronous and
 * the pitch comes from the grain content instead of the train */
static const preset_pair_t kGranGlassCloud[] = {
    P(GRAN_PID_MODE, 1), /* free */
    P(GRAN_PID_DENS, 90.0f), P(GRAN_PID_SIZE, 55.0f),
    P(GRAN_PID_FORM, 4.0f), P(GRAN_PID_SCAT, 7.0f),
    P(GRAN_PID_SPREAD, 0.9f), P(GRAN_PID_JIT, 0.6f),
    P(GRAN_PID_FLT_CUTOFF, 6000.0f), P(GRAN_PID_FLT_MODE, 2), /* hp */
    P(GRAN_PID_ENV1_ATTACK, 0.5f), P(GRAN_PID_ENV1_SUSTAIN, 0.9f),
    P(GRAN_PID_ENV1_RELEASE, 1.6f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.45f), P(FX_PID_REV_SIZE, 0.85f),
};

/* barely-jittered sine train, long grains, slow everything */
static const preset_pair_t kGranGrainChoir[] = {
    P(GRAN_PID_FORM, 2.0f), P(GRAN_PID_SIZE, 70.0f),
    P(GRAN_PID_JIT, 0.12f), P(GRAN_PID_SCAT, 0.4f),
    P(GRAN_PID_SPREAD, 0.7f),
    P(GRAN_PID_LFO2_RATE, 0.35f), P(GRAN_PID_LFO2_FORM, 0.12f),
    P(GRAN_PID_FLT_CUTOFF, 3500.0f),
    P(GRAN_PID_ENV1_ATTACK, 0.9f), P(GRAN_PID_ENV1_DECAY, 1.5f),
    P(GRAN_PID_ENV1_SUSTAIN, 0.9f), P(GRAN_PID_ENV1_RELEASE, 2.2f),
    P(SYNTH_PID_COMMON_UNISON, 2), P(SYNTH_PID_COMMON_UNI_DETUNE, 9.0f),
    P(SYNTH_PID_COMMON_UNI_SPREAD, 0.9f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_CHO_MIX, 0.3f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.4f), P(FX_PID_REV_SIZE, 0.8f),
};

/* formant far above the fundamental: the train reads as a pitched clang */
static const preset_pair_t kGranMetalTrain[] = {
    P(GRAN_PID_WAVE, 3), /* pulse */
    P(GRAN_PID_PW, 0.18f),
    P(GRAN_PID_FORM, 9.0f), P(GRAN_PID_SIZE, 7.0f),
    P(GRAN_PID_SHAPE, 0.22f), P(GRAN_PID_SCAT, 1.5f),
    P(GRAN_PID_FLT_TYPE, 3), /* dual */
    P(GRAN_PID_FLT_CUTOFF, 3000.0f), P(GRAN_PID_FLT_SPREAD, 1.2f),
    P(GRAN_PID_FLT_RESO, 0.4f),
    P(GRAN_PID_ENV1_DECAY, 0.7f), P(GRAN_PID_ENV1_SUSTAIN, 0.25f),
    P(GRAN_PID_ENV1_RELEASE, 0.5f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.35f),
};

/* noise grains: the window is the whole sound, so shape and size are the
 * only controls that matter and the formant does nothing at all */
static const preset_pair_t kGranNoiseSizzle[] = {
    P(GRAN_PID_WAVE, 4), /* noise */
    P(GRAN_PID_MODE, 1), P(GRAN_PID_DENS, 160.0f),
    P(GRAN_PID_SIZE, 12.0f), P(GRAN_PID_SHAPE, 0.3f),
    P(GRAN_PID_SPREAD, 1.0f), P(GRAN_PID_JIT, 0.9f),
    P(GRAN_PID_FLT_MODE, 1), /* bp */
    P(GRAN_PID_FLT_CUTOFF, 4200.0f), P(GRAN_PID_FLT_RESO, 0.5f),
    P(GRAN_PID_FLT_ENV, 2.5f), P(GRAN_PID_FLT_KBD, 1.0f),
    P(GRAN_PID_ENV2_DECAY, 0.5f), P(GRAN_PID_ENV2_SUSTAIN, 0.2f),
    P(GRAN_PID_ENV1_ATTACK, 0.02f), P(GRAN_PID_ENV1_SUSTAIN, 0.7f),
    P(GRAN_PID_ENV1_RELEASE, 0.5f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.4f),
};

/* window peak late in the grain, so each one swells instead of striking */
static const preset_pair_t kGranBowedSwell[] = {
    P(GRAN_PID_WAVE, 1), /* triangle */
    P(GRAN_PID_FORM, 2.0f), P(GRAN_PID_SIZE, 90.0f),
    P(GRAN_PID_SHAPE, 0.88f), P(GRAN_PID_JIT, 0.25f),
    P(GRAN_PID_SPREAD, 0.6f),
    P(GRAN_PID_FLT_CUTOFF, 2400.0f), P(GRAN_PID_FLT_ENV, 1.2f),
    P(GRAN_PID_ENV2_ATTACK, 0.8f), P(GRAN_PID_ENV2_SUSTAIN, 0.7f),
    P(GRAN_PID_ENV1_ATTACK, 0.6f), P(GRAN_PID_ENV1_SUSTAIN, 0.9f),
    P(GRAN_PID_ENV1_RELEASE, 1.2f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.38f), P(FX_PID_REV_SIZE, 0.75f),
};

/* the other end of grn.shape: a hard strike, grains short enough to be a
 * transient rather than a tone */
static const preset_pair_t kGranClickPerc[] = {
    P(GRAN_PID_WAVE, 3), P(GRAN_PID_PW, 0.35f),
    P(GRAN_PID_MODE, 1), P(GRAN_PID_DENS, 26.0f),
    P(GRAN_PID_FORM, 6.0f), P(GRAN_PID_SIZE, 4.0f),
    P(GRAN_PID_SHAPE, 0.07f), P(GRAN_PID_SCAT, 4.0f),
    P(GRAN_PID_SPREAD, 0.8f), P(GRAN_PID_JIT, 0.5f),
    P(GRAN_PID_FLT_CUTOFF, 5000.0f), P(GRAN_PID_FLT_RESO, 0.3f),
    P(GRAN_PID_ENV1_ATTACK, 0.002f), P(GRAN_PID_ENV1_DECAY, 0.5f),
    P(GRAN_PID_ENV1_SUSTAIN, 0.45f), P(GRAN_PID_ENV1_RELEASE, 0.25f),
    P(FX_PID_DLY_ON, 1), P(FX_PID_DLY_MIX, 0.28f), P(FX_PID_DLY_TIME, 0.25f),
    P(FX_PID_DLY_FB, 0.5f), P(FX_PID_DLY_PP, 1),
};

/* two octaves of per-grain scatter across the full stereo field: the cloud
 * stops having a pitch and becomes a texture */
static const preset_pair_t kGranWideScatter[] = {
    P(GRAN_PID_MODE, 1), P(GRAN_PID_DENS, 70.0f),
    P(GRAN_PID_SIZE, 45.0f), P(GRAN_PID_FORM, 3.0f),
    P(GRAN_PID_SCAT, 24.0f), P(GRAN_PID_SPREAD, 1.0f),
    P(GRAN_PID_JIT, 1.0f),
    P(GRAN_PID_FLT_CUTOFF, 5500.0f),
    P(GRAN_PID_ENV1_ATTACK, 0.35f), P(GRAN_PID_ENV1_SUSTAIN, 0.85f),
    P(GRAN_PID_ENV1_RELEASE, 1.8f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.5f), P(FX_PID_REV_SIZE, 0.9f),
};

/* formant *below* the train rate, which a subtractive oscillator cannot do:
 * the grain contributes body under a fundamental it does not carry */
static const preset_pair_t kGranSubPulsar[] = {
    P(GRAN_PID_WAVE, 0), P(GRAN_PID_FORM, 0.5f),
    P(GRAN_PID_SIZE, 30.0f), P(GRAN_PID_SHAPE, 0.3f),
    P(GRAN_PID_SPREAD, 0.0f),
    P(GRAN_PID_FLT_TYPE, 2), /* ladder */
    P(GRAN_PID_FLT_CUTOFF, 700.0f), P(GRAN_PID_FLT_RESO, 0.25f),
    P(GRAN_PID_FLT_DRIVE, 0.3f), P(GRAN_PID_FLT_ENV, 1.2f),
    P(GRAN_PID_ENV2_DECAY, 0.2f), P(GRAN_PID_ENV2_SUSTAIN, 0.0f),
    P(GRAN_PID_ENV1_DECAY, 0.4f), P(GRAN_PID_ENV1_SUSTAIN, 0.7f),
    P(GRAN_PID_ENV1_RELEASE, 0.2f),
};

/* lfo2 wobbles the formant while the key holds the train steady */
static const preset_pair_t kGranTalkbox[] = {
    P(GRAN_PID_WAVE, 2), P(GRAN_PID_FORM, 2.5f),
    P(GRAN_PID_SIZE, 22.0f), P(GRAN_PID_SHAPE, 0.4f),
    P(GRAN_PID_LFO2_RATE, 3.2f), P(GRAN_PID_LFO2_WAVE, 1),
    P(GRAN_PID_LFO2_FORM, 0.8f),
    P(GRAN_PID_FLT_TYPE, 4), P(GRAN_PID_FLT_VOWEL, 0.6f),
    P(GRAN_PID_FLT_CUTOFF, 1100.0f), P(GRAN_PID_FLT_RESO, 0.35f),
    P(GRAN_PID_ENV1_SUSTAIN, 0.85f), P(GRAN_PID_ENV1_RELEASE, 0.4f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, GRAN_PID_LFO2_FORM, 0.5f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.25f),
};

/* velocity opens the cloud out: soft is a single clean train, hard is a
 * scattered swarm — one gesture across two of the engine's axes */
static const preset_pair_t kGranVelSwarm[] = {
    P(GRAN_PID_FORM, 3.0f), P(GRAN_PID_SIZE, 35.0f),
    P(GRAN_PID_SPREAD, 0.5f),
    P(GRAN_PID_FLT_CUTOFF, 4000.0f), P(GRAN_PID_FLT_ENV, 1.5f),
    P(GRAN_PID_ENV1_SUSTAIN, 0.8f), P(GRAN_PID_ENV1_RELEASE, 0.7f),
    MOD(0, SYNTH_MOD_SRC_VEL, GRAN_PID_SCAT, 0.4f),
    MOD(1, SYNTH_MOD_SRC_VEL, GRAN_PID_JIT, 0.5f),
    MOD(2, SYNTH_MOD_SRC_VEL, GRAN_PID_FLT_CUTOFF, 0.35f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.35f),
};

/* --- the three input patches. Silent with nothing plugged in. --- */

/* the ring keeps filling; the keyboard transposes what is in it against
 * buf.root, so middle C plays the input back at its own speed */
static const preset_pair_t kGranInLive[] = {
    P(GRAN_PID_SRC, 1), /* in */
    P(GRAN_PID_MODE, 1), P(GRAN_PID_DENS, 40.0f),
    P(GRAN_PID_SIZE, 80.0f), P(GRAN_PID_SHAPE, 0.5f),
    P(GRAN_PID_BUF_POS, 0.15f), P(GRAN_PID_BUF_SPRAY, 0.06f),
    P(GRAN_PID_SPREAD, 0.7f), P(GRAN_PID_JIT, 0.4f),
    P(GRAN_PID_FLT_CUTOFF, 12000.0f),
    P(GRAN_PID_ENV1_ATTACK, 0.05f), P(GRAN_PID_ENV1_SUSTAIN, 1.0f),
    P(GRAN_PID_ENV1_RELEASE, 0.4f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.3f),
};

/* buf.freeze latches the ring, which turns it into a fixed sample: hold a
 * chord and the frozen moment becomes an instrument */
static const preset_pair_t kGranInFreeze[] = {
    P(GRAN_PID_SRC, 1),
    P(GRAN_PID_MODE, 1), P(GRAN_PID_DENS, 55.0f),
    P(GRAN_PID_SIZE, 120.0f), P(GRAN_PID_SHAPE, 0.5f),
    P(GRAN_PID_BUF_POS, 0.5f), P(GRAN_PID_BUF_SPRAY, 0.35f),
    P(GRAN_PID_SPREAD, 1.0f), P(GRAN_PID_JIT, 0.7f),
    P(GRAN_PID_SCAT, 0.3f),
    P(GRAN_PID_ENV1_ATTACK, 0.4f), P(GRAN_PID_ENV1_SUSTAIN, 1.0f),
    P(GRAN_PID_ENV1_RELEASE, 1.5f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, GRAN_PID_BUF_POS, 1.0f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.45f), P(FX_PID_REV_SIZE, 0.85f),
};

/* every grain backwards over a wide spray: the input stops being a recording
 * and becomes a wash with no attacks in it */
static const preset_pair_t kGranInReverse[] = {
    P(GRAN_PID_SRC, 1), P(GRAN_PID_BUF_REV, 1.0f),
    P(GRAN_PID_MODE, 1), P(GRAN_PID_DENS, 45.0f),
    P(GRAN_PID_SIZE, 140.0f), P(GRAN_PID_SHAPE, 0.75f),
    P(GRAN_PID_BUF_POS, 0.6f), P(GRAN_PID_BUF_SPRAY, 0.5f),
    P(GRAN_PID_SPREAD, 0.95f), P(GRAN_PID_JIT, 0.8f),
    P(GRAN_PID_FLT_CUTOFF, 6000.0f),
    P(GRAN_PID_ENV1_ATTACK, 0.3f), P(GRAN_PID_ENV1_SUSTAIN, 1.0f),
    P(GRAN_PID_ENV1_RELEASE, 2.0f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.5f), P(FX_PID_REV_SIZE, 0.9f),
};

/* Formants that are not vowels. The vowel filter is one way to a voice;
 * a bare high formant ratio on a sine is another, and it is the one that
 * sounds like a struck bar rather than a throat. */
static const preset_pair_t kGranFormantBell[] = {
    P(GRAN_PID_FORM, 6.5f), P(GRAN_PID_SIZE, 14.0f),
    P(GRAN_PID_SHAPE, 0.14f), P(GRAN_PID_SPREAD, 0.5f),
    P(GRAN_PID_ENV_FORM, -1.4f), /* the strike is bright, the tail is not */
    P(GRAN_PID_ENV2_ATTACK, 0.001f), P(GRAN_PID_ENV2_DECAY, 0.5f),
    P(GRAN_PID_ENV2_SUSTAIN, 0.0f),
    P(GRAN_PID_FLT_CUTOFF, 8000.0f),
    P(GRAN_PID_ENV1_ATTACK, 0.001f), P(GRAN_PID_ENV1_DECAY, 1.4f),
    P(GRAN_PID_ENV1_SUSTAIN, 0.0f), P(GRAN_PID_ENV1_RELEASE, 1.0f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.42f), P(FX_PID_REV_SIZE, 0.8f),
};

/* env2 walks the vowel filter instead of the formant, so the patch says a
 * word over the length of a note */
static const preset_pair_t kGranVowelMorph[] = {
    P(GRAN_PID_WAVE, 2), P(GRAN_PID_FORM, 1.0f),
    P(GRAN_PID_SIZE, 24.0f), P(GRAN_PID_SHAPE, 0.45f),
    P(GRAN_PID_FLT_TYPE, 4), P(GRAN_PID_FLT_VOWEL, 0.1f),
    P(GRAN_PID_FLT_CUTOFF, 1000.0f), P(GRAN_PID_FLT_RESO, 0.3f),
    P(GRAN_PID_ENV2_ATTACK, 0.6f), P(GRAN_PID_ENV2_DECAY, 1.4f),
    P(GRAN_PID_ENV2_SUSTAIN, 0.5f),
    P(GRAN_PID_ENV1_ATTACK, 0.25f), P(GRAN_PID_ENV1_SUSTAIN, 0.9f),
    P(GRAN_PID_ENV1_RELEASE, 0.9f),
    MOD(0, SYNTH_MOD_SRC_ENV2, GRAN_PID_FLT_VOWEL, 0.8f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.32f),
};

/* formant locked to the fundamental, driven hard: a reed rather than a
 * cloud — the ladder is what makes it honk instead of buzz */
static const preset_pair_t kGranBuzzReed[] = {
    P(GRAN_PID_WAVE, 2), P(GRAN_PID_FORM, 1.0f),
    P(GRAN_PID_SIZE, 16.0f), P(GRAN_PID_SHAPE, 0.3f),
    P(GRAN_PID_SPREAD, 0.15f),
    P(GRAN_PID_FLT_TYPE, 2), P(GRAN_PID_FLT_CUTOFF, 1600.0f),
    P(GRAN_PID_FLT_RESO, 0.45f), P(GRAN_PID_FLT_DRIVE, 0.55f),
    P(GRAN_PID_FLT_ENV, 1.4f), P(GRAN_PID_FLT_KBD, 0.6f),
    P(GRAN_PID_ENV2_DECAY, 0.25f), P(GRAN_PID_ENV2_SUSTAIN, 0.3f),
    P(GRAN_PID_ENV1_ATTACK, 0.03f), P(GRAN_PID_ENV1_SUSTAIN, 0.85f),
    P(GRAN_PID_ENV1_RELEASE, 0.25f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, GRAN_PID_FLT_DRIVE, 0.4f),
};

/* square grains locked at 2x with nothing scattered: the train is periodic
 * enough to read as an organ stop rather than as grains */
static const preset_pair_t kGranPulseOrgan[] = {
    P(GRAN_PID_WAVE, 3), P(GRAN_PID_PW, 0.5f),
    P(GRAN_PID_FORM, 2.0f), P(GRAN_PID_SIZE, 20.0f),
    P(GRAN_PID_SHAPE, 0.5f), P(GRAN_PID_SPREAD, 0.2f),
    P(GRAN_PID_FLT_CUTOFF, 4500.0f), P(GRAN_PID_FLT_KBD, 0.8f),
    P(GRAN_PID_ENV1_ATTACK, 0.006f), P(GRAN_PID_ENV1_SUSTAIN, 1.0f),
    P(GRAN_PID_ENV1_RELEASE, 0.06f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_CHO_MIX, 0.25f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.22f),
};

/* noise grains on the *sync* grid — the one combination that gives noise a
 * pitch, because the train is periodic even though its contents are not */
static const preset_pair_t kGranRattle[] = {
    P(GRAN_PID_WAVE, 4), P(GRAN_PID_SIZE, 6.0f),
    P(GRAN_PID_SHAPE, 0.25f), P(GRAN_PID_SPREAD, 0.45f),
    P(GRAN_PID_FLT_MODE, 1), P(GRAN_PID_FLT_CUTOFF, 2500.0f),
    P(GRAN_PID_FLT_RESO, 0.55f), P(GRAN_PID_FLT_KBD, 1.0f),
    P(GRAN_PID_ENV1_ATTACK, 0.002f), P(GRAN_PID_ENV1_DECAY, 0.5f),
    P(GRAN_PID_ENV1_SUSTAIN, 0.5f), P(GRAN_PID_ENV1_RELEASE, 0.2f),
    P(FX_PID_DLY_ON, 1), P(FX_PID_DLY_MIX, 0.24f), P(FX_PID_DLY_TIME, 0.1875f),
    P(FX_PID_DLY_FB, 0.42f), P(FX_PID_DLY_PP, 1),
};

/* jitter plus unison: three detuned trains whose onsets never line up, which
 * is the granular route to an ensemble */
static const preset_pair_t kGranStrings[] = {
    P(GRAN_PID_WAVE, 2), P(GRAN_PID_FORM, 2.0f),
    P(GRAN_PID_SIZE, 45.0f), P(GRAN_PID_JIT, 0.3f),
    P(GRAN_PID_SCAT, 0.5f), P(GRAN_PID_SPREAD, 0.85f),
    P(GRAN_PID_FLT_CUTOFF, 3000.0f), P(GRAN_PID_FLT_ENV, 1.0f),
    P(GRAN_PID_ENV2_ATTACK, 0.5f), P(GRAN_PID_ENV2_SUSTAIN, 0.8f),
    P(GRAN_PID_ENV1_ATTACK, 0.32f), P(GRAN_PID_ENV1_SUSTAIN, 0.9f),
    P(GRAN_PID_ENV1_RELEASE, 1.1f),
    P(SYNTH_PID_COMMON_UNISON, 3), P(SYNTH_PID_COMMON_UNI_DETUNE, 12.0f),
    P(SYNTH_PID_COMMON_UNI_SPREAD, 1.0f),
    P(FX_PID_CHO_ON, 1), P(FX_PID_CHO_MIX, 0.35f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.4f), P(FX_PID_REV_SIZE, 0.8f),
};

/* formant a quarter of the key and grains long enough to overlap heavily:
 * the bottom of what the engine can do */
static const preset_pair_t kGranDeepDrone[] = {
    P(GRAN_PID_FORM, 0.25f), P(GRAN_PID_SIZE, 120.0f),
    P(GRAN_PID_SHAPE, 0.5f), P(GRAN_PID_JIT, 0.1f),
    P(GRAN_PID_SPREAD, 0.3f),
    P(GRAN_PID_FLT_TYPE, 2), P(GRAN_PID_FLT_CUTOFF, 500.0f),
    P(GRAN_PID_FLT_DRIVE, 0.25f),
    P(GRAN_PID_LFO2_RATE, 0.12f), P(GRAN_PID_LFO2_FORM, 0.25f),
    P(GRAN_PID_ENV1_ATTACK, 1.2f), P(GRAN_PID_ENV1_SUSTAIN, 1.0f),
    P(GRAN_PID_ENV1_RELEASE, 2.5f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.35f), P(FX_PID_REV_SIZE, 0.9f),
};

/* the density ceiling, high and wide: individual grains stop being audible
 * and the cloud becomes a texture with no grain rate in it */
static const preset_pair_t kGranShimmerHigh[] = {
    P(GRAN_PID_MODE, 1), P(GRAN_PID_DENS, 240.0f),
    P(GRAN_PID_SIZE, 22.0f), P(GRAN_PID_FORM, 8.0f),
    P(GRAN_PID_SCAT, 5.0f), P(GRAN_PID_SPREAD, 1.0f),
    P(GRAN_PID_JIT, 0.8f),
    P(GRAN_PID_FLT_MODE, 2), P(GRAN_PID_FLT_CUTOFF, 3000.0f),
    P(GRAN_PID_ENV1_ATTACK, 0.6f), P(GRAN_PID_ENV1_SUSTAIN, 0.9f),
    P(GRAN_PID_ENV1_RELEASE, 2.2f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.55f), P(FX_PID_REV_SIZE, 0.92f),
    P(FX_PID_REV_COMP, 1),
};

/* the sparse end: a few big grains a second, so the cloud is heard one grain
 * at a time and s&h moves the formant between them */
static const preset_pair_t kGranStutter[] = {
    P(GRAN_PID_MODE, 1), P(GRAN_PID_DENS, 7.0f),
    P(GRAN_PID_SIZE, 110.0f), P(GRAN_PID_FORM, 3.0f),
    P(GRAN_PID_SHAPE, 0.35f), P(GRAN_PID_SPREAD, 1.0f),
    P(GRAN_PID_LFO2_RATE, 6.0f), P(GRAN_PID_LFO2_WAVE, 4), /* s&h */
    P(GRAN_PID_LFO2_FORM, 1.4f),
    P(GRAN_PID_FLT_CUTOFF, 5000.0f),
    P(GRAN_PID_ENV1_ATTACK, 0.02f), P(GRAN_PID_ENV1_SUSTAIN, 1.0f),
    P(GRAN_PID_ENV1_RELEASE, 0.8f),
    /* 0.4 s and not 0.5: that is the classic ESP32's delay ceiling, and a
     * value above it would clamp there rather than sound as written. */
    P(FX_PID_DLY_ON, 1), P(FX_PID_DLY_MIX, 0.3f), P(FX_PID_DLY_TIME, 0.4f),
    P(FX_PID_DLY_FB, 0.5f), P(FX_PID_DLY_PP, 1),
};

/* Three fixed points of the vowel filter, saved as their own patches. The
 * morph is one control, but "ah" and "ee" are destinations a player reaches
 * for by name, and hunting for them on a knob mid-take is not playing. */
static const preset_pair_t kGranVowelAh[] = {
    P(GRAN_PID_WAVE, 2), P(GRAN_PID_FORM, 1.0f),
    P(GRAN_PID_SIZE, 22.0f), P(GRAN_PID_SHAPE, 0.4f),
    P(GRAN_PID_FLT_TYPE, 4), P(GRAN_PID_FLT_VOWEL, 0.0f),
    P(GRAN_PID_FLT_CUTOFF, 900.0f), P(GRAN_PID_FLT_RESO, 0.3f),
    P(GRAN_PID_ENV1_ATTACK, 0.1f), P(GRAN_PID_ENV1_SUSTAIN, 0.9f),
    P(GRAN_PID_ENV1_RELEASE, 0.45f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, GRAN_PID_FLT_VOWEL, 1.0f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.3f),
};

static const preset_pair_t kGranVowelEe[] = {
    P(GRAN_PID_WAVE, 2), P(GRAN_PID_FORM, 1.0f),
    P(GRAN_PID_SIZE, 20.0f), P(GRAN_PID_SHAPE, 0.4f),
    P(GRAN_PID_FLT_TYPE, 4), P(GRAN_PID_FLT_VOWEL, 0.45f),
    P(GRAN_PID_FLT_CUTOFF, 1300.0f), P(GRAN_PID_FLT_RESO, 0.4f),
    P(GRAN_PID_ENV1_ATTACK, 0.09f), P(GRAN_PID_ENV1_SUSTAIN, 0.9f),
    P(GRAN_PID_ENV1_RELEASE, 0.4f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, GRAN_PID_FLT_VOWEL, 1.0f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.3f),
};

static const preset_pair_t kGranVowelOo[] = {
    P(GRAN_PID_WAVE, 0), P(GRAN_PID_FORM, 1.5f),
    P(GRAN_PID_SIZE, 30.0f), P(GRAN_PID_SHAPE, 0.55f),
    P(GRAN_PID_FLT_TYPE, 4), P(GRAN_PID_FLT_VOWEL, 0.95f),
    P(GRAN_PID_FLT_CUTOFF, 700.0f), P(GRAN_PID_FLT_RESO, 0.35f),
    P(GRAN_PID_ENV1_ATTACK, 0.2f), P(GRAN_PID_ENV1_SUSTAIN, 0.9f),
    P(GRAN_PID_ENV1_RELEASE, 0.7f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, GRAN_PID_FLT_VOWEL, -1.0f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.35f), P(FX_PID_REV_SIZE, 0.7f),
};

/* tiny grains, huge scatter, high density: not a note, a swarm */
static const preset_pair_t kGranInsects[] = {
    P(GRAN_PID_WAVE, 3), P(GRAN_PID_PW, 0.25f),
    P(GRAN_PID_MODE, 1), P(GRAN_PID_DENS, 180.0f),
    P(GRAN_PID_SIZE, 3.0f), P(GRAN_PID_FORM, 5.0f),
    P(GRAN_PID_SHAPE, 0.4f), P(GRAN_PID_SCAT, 18.0f),
    P(GRAN_PID_SPREAD, 1.0f), P(GRAN_PID_JIT, 1.0f),
    P(GRAN_PID_FLT_MODE, 1), P(GRAN_PID_FLT_CUTOFF, 3500.0f),
    P(GRAN_PID_FLT_RESO, 0.4f),
    P(GRAN_PID_ENV1_ATTACK, 0.1f), P(GRAN_PID_ENV1_SUSTAIN, 0.9f),
    P(GRAN_PID_ENV1_RELEASE, 0.6f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.4f),
};

/* long noise grains at low density overlap into something continuous, and
 * the lowpass sweep is the only thing that moves */
static const preset_pair_t kGranWindTunnel[] = {
    P(GRAN_PID_WAVE, 4), P(GRAN_PID_MODE, 1),
    P(GRAN_PID_DENS, 30.0f), P(GRAN_PID_SIZE, 180.0f),
    P(GRAN_PID_SHAPE, 0.5f), P(GRAN_PID_SPREAD, 1.0f),
    P(GRAN_PID_JIT, 1.0f),
    P(GRAN_PID_FLT_CUTOFF, 800.0f), P(GRAN_PID_FLT_RESO, 0.35f),
    P(GRAN_PID_FLT_KBD, 0.0f),
    P(GRAN_PID_LFO2_RATE, 0.1f),
    P(GRAN_PID_ENV1_ATTACK, 1.5f), P(GRAN_PID_ENV1_SUSTAIN, 1.0f),
    P(GRAN_PID_ENV1_RELEASE, 3.0f),
    MOD(0, SYNTH_MOD_SRC_LFO2, GRAN_PID_FLT_CUTOFF, 0.45f),
    MOD(1, SYNTH_MOD_SRC_WHEEL, GRAN_PID_FLT_CUTOFF, 0.5f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.5f), P(FX_PID_REV_SIZE, 0.9f),
};

/* narrow pulse grains through the bitcrusher: the grain rate and the
 * sample-rate reduction beat against each other */
static const preset_pair_t kGranBitChoir[] = {
    P(GRAN_PID_WAVE, 3), P(GRAN_PID_PW, 0.12f),
    P(GRAN_PID_FORM, 3.0f), P(GRAN_PID_SIZE, 28.0f),
    P(GRAN_PID_SPREAD, 0.6f), P(GRAN_PID_JIT, 0.15f),
    P(GRAN_PID_FLT_CUTOFF, 6000.0f),
    P(GRAN_PID_ENV1_ATTACK, 0.05f), P(GRAN_PID_ENV1_SUSTAIN, 0.9f),
    P(GRAN_PID_ENV1_RELEASE, 0.6f),
    P(FX_PID_CRUSH_ON, 1), P(FX_PID_CRUSH_MIX, 0.5f),
    P(FX_PID_CRUSH_BITS, 7.0f), P(FX_PID_CRUSH_DOWN, 4.0f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.3f),
};

/* the ladder near self-oscillation, tracking the key, with the formant
 * feeding it harmonics to grab */
static const preset_pair_t kGranHarmLadder[] = {
    P(GRAN_PID_WAVE, 2), P(GRAN_PID_FORM, 4.0f),
    P(GRAN_PID_SIZE, 18.0f), P(GRAN_PID_SHAPE, 0.28f),
    P(GRAN_PID_FLT_TYPE, 2), P(GRAN_PID_FLT_CUTOFF, 1200.0f),
    P(GRAN_PID_FLT_RESO, 0.8f), P(GRAN_PID_FLT_KBD, 1.0f),
    P(GRAN_PID_FLT_ENV, 2.0f),
    P(GRAN_PID_ENV2_DECAY, 0.3f), P(GRAN_PID_ENV2_SUSTAIN, 0.15f),
    P(GRAN_PID_ENV1_DECAY, 0.5f), P(GRAN_PID_ENV1_SUSTAIN, 0.6f),
    P(GRAN_PID_ENV1_RELEASE, 0.3f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, GRAN_PID_FLT_RESO, 0.2f),
};

/* formant under the fundamental with drive on top: weight without depth,
 * for a bass that has to sit under something */
static const preset_pair_t kGranDarkPulsar[] = {
    P(GRAN_PID_WAVE, 1), P(GRAN_PID_FORM, 0.75f),
    P(GRAN_PID_SIZE, 25.0f), P(GRAN_PID_SHAPE, 0.22f),
    P(GRAN_PID_SPREAD, 0.0f),
    P(GRAN_PID_FLT_CUTOFF, 900.0f), P(GRAN_PID_FLT_RESO, 0.2f),
    P(GRAN_PID_FLT_DRIVE, 0.45f), P(GRAN_PID_FLT_ENV, 1.0f),
    P(GRAN_PID_FLT_KBD, 0.4f),
    P(GRAN_PID_ENV2_DECAY, 0.18f), P(GRAN_PID_ENV2_SUSTAIN, 0.0f),
    P(GRAN_PID_ENV1_ATTACK, 0.004f), P(GRAN_PID_ENV1_DECAY, 0.35f),
    P(GRAN_PID_ENV1_SUSTAIN, 0.65f), P(GRAN_PID_ENV1_RELEASE, 0.15f),
    P(FX_PID_DRV_ON, 1), P(FX_PID_DRV_MIX, 0.35f),
};

/* the top of the formant range on a very short grain: a ping with a pitch
 * that has almost nothing below it */
static const preset_pair_t kGranCrystalPing[] = {
    P(GRAN_PID_FORM, 12.0f), P(GRAN_PID_SIZE, 5.0f),
    P(GRAN_PID_SHAPE, 0.1f), P(GRAN_PID_SCAT, 2.0f),
    P(GRAN_PID_SPREAD, 0.75f),
    P(GRAN_PID_FLT_MODE, 2), P(GRAN_PID_FLT_CUTOFF, 1500.0f),
    P(GRAN_PID_ENV1_ATTACK, 0.001f), P(GRAN_PID_ENV1_DECAY, 0.8f),
    P(GRAN_PID_ENV1_SUSTAIN, 0.0f), P(GRAN_PID_ENV1_RELEASE, 0.7f),
    P(FX_PID_DLY_ON, 1), P(FX_PID_DLY_MIX, 0.35f), P(FX_PID_DLY_TIME, 0.375f),
    P(FX_PID_DLY_FB, 0.55f), P(FX_PID_DLY_PP, 1),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.45f), P(FX_PID_REV_SIZE, 0.88f),
};

/* a falling formant is a mouth closing; the same envelope opening the filter
 * is the tongue. Together they are the closest this engine gets to a word. */
static const preset_pair_t kGranBrass[] = {
    P(GRAN_PID_WAVE, 2), P(GRAN_PID_FORM, 2.5f),
    P(GRAN_PID_SIZE, 20.0f), P(GRAN_PID_SHAPE, 0.35f),
    P(GRAN_PID_ENV_FORM, -1.2f),
    P(GRAN_PID_FLT_CUTOFF, 1400.0f), P(GRAN_PID_FLT_ENV, 2.2f),
    P(GRAN_PID_FLT_RESO, 0.25f), P(GRAN_PID_FLT_DRIVE, 0.3f),
    P(GRAN_PID_ENV2_ATTACK, 0.05f), P(GRAN_PID_ENV2_DECAY, 0.45f),
    P(GRAN_PID_ENV2_SUSTAIN, 0.35f),
    P(GRAN_PID_ENV1_ATTACK, 0.04f), P(GRAN_PID_ENV1_SUSTAIN, 0.9f),
    P(GRAN_PID_ENV1_RELEASE, 0.3f),
    MOD(0, SYNTH_MOD_SRC_VEL, GRAN_PID_FLT_ENV, 0.3f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.25f),
};

/* an octave of scatter over a long release: struck once, the cloud keeps
 * finding new pitches as it decays */
static const preset_pair_t kGranScatterBells[] = {
    P(GRAN_PID_MODE, 1), P(GRAN_PID_DENS, 35.0f),
    P(GRAN_PID_SIZE, 60.0f), P(GRAN_PID_FORM, 7.0f),
    P(GRAN_PID_SHAPE, 0.15f), P(GRAN_PID_SCAT, 12.0f),
    P(GRAN_PID_SPREAD, 0.95f), P(GRAN_PID_JIT, 0.7f),
    P(GRAN_PID_FLT_CUTOFF, 7000.0f),
    P(GRAN_PID_ENV1_ATTACK, 0.002f), P(GRAN_PID_ENV1_DECAY, 2.5f),
    P(GRAN_PID_ENV1_SUSTAIN, 0.0f), P(GRAN_PID_ENV1_RELEASE, 2.0f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.5f), P(FX_PID_REV_SIZE, 0.9f),
};

/* the shortest window and the lowest formant the engine has: a body hit with
 * a pitch, and no tail at all */
static const preset_pair_t kGranSubThump[] = {
    P(GRAN_PID_FORM, 0.5f), P(GRAN_PID_SIZE, 45.0f),
    P(GRAN_PID_SHAPE, 0.06f), P(GRAN_PID_SPREAD, 0.0f),
    P(GRAN_PID_FLT_TYPE, 2), P(GRAN_PID_FLT_CUTOFF, 400.0f),
    P(GRAN_PID_FLT_DRIVE, 0.4f), P(GRAN_PID_FLT_KBD, 0.2f),
    P(GRAN_PID_ENV1_ATTACK, 0.001f), P(GRAN_PID_ENV1_DECAY, 0.18f),
    P(GRAN_PID_ENV1_SUSTAIN, 0.0f), P(GRAN_PID_ENV1_RELEASE, 0.1f),
};

/* sparse, tiny, scattered across two octaves and the whole field: the
 * granular equivalent of rain on a roof */
static const preset_pair_t kGranGlitchRain[] = {
    P(GRAN_PID_MODE, 1), P(GRAN_PID_DENS, 14.0f),
    P(GRAN_PID_SIZE, 8.0f), P(GRAN_PID_FORM, 6.0f),
    P(GRAN_PID_SHAPE, 0.2f), P(GRAN_PID_SCAT, 24.0f),
    P(GRAN_PID_SPREAD, 1.0f), P(GRAN_PID_JIT, 1.0f),
    P(GRAN_PID_FLT_MODE, 2), P(GRAN_PID_FLT_CUTOFF, 1200.0f),
    P(GRAN_PID_ENV1_ATTACK, 0.01f), P(GRAN_PID_ENV1_SUSTAIN, 1.0f),
    P(GRAN_PID_ENV1_RELEASE, 1.0f),
    P(FX_PID_DLY_ON, 1), P(FX_PID_DLY_MIX, 0.32f), P(FX_PID_DLY_TIME, 0.25f),
    P(FX_PID_DLY_FB, 0.55f), P(FX_PID_DLY_PP, 1),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.42f),
};

/* velocity onto grain size and formant: the same key played harder is a
 * shorter, brighter grain — the acoustic relationship, not a volume change */
static const preset_pair_t kGranVelSize[] = {
    P(GRAN_PID_FORM, 2.5f), P(GRAN_PID_SIZE, 60.0f),
    P(GRAN_PID_SHAPE, 0.5f), P(GRAN_PID_SPREAD, 0.5f),
    P(GRAN_PID_FLT_CUTOFF, 3000.0f),
    P(GRAN_PID_ENV1_ATTACK, 0.02f), P(GRAN_PID_ENV1_SUSTAIN, 0.85f),
    P(GRAN_PID_ENV1_RELEASE, 0.5f),
    MOD(0, SYNTH_MOD_SRC_VEL, GRAN_PID_SIZE, -0.35f),
    MOD(1, SYNTH_MOD_SRC_VEL, GRAN_PID_FORM, 0.25f),
    MOD(2, SYNTH_MOD_SRC_VEL, GRAN_PID_SHAPE, -0.3f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.32f),
};

/* the keyboard drives the window shape: low notes swell, high notes strike,
 * which is what a struck string does across its range */
static const preset_pair_t kGranKeyShape[] = {
    P(GRAN_PID_WAVE, 1), P(GRAN_PID_FORM, 3.0f),
    P(GRAN_PID_SIZE, 40.0f), P(GRAN_PID_SHAPE, 0.7f),
    P(GRAN_PID_SPREAD, 0.6f),
    P(GRAN_PID_FLT_CUTOFF, 4000.0f), P(GRAN_PID_FLT_KBD, 0.9f),
    P(GRAN_PID_ENV1_ATTACK, 0.01f), P(GRAN_PID_ENV1_DECAY, 1.2f),
    P(GRAN_PID_ENV1_SUSTAIN, 0.4f), P(GRAN_PID_ENV1_RELEASE, 0.6f),
    MOD(0, SYNTH_MOD_SRC_NOTE, GRAN_PID_SHAPE, -0.5f),
    MOD(1, SYNTH_MOD_SRC_NOTE, GRAN_PID_SIZE, -0.3f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.35f),
};

/* the wheel takes the patch from a clean train to a wide cloud in one
 * gesture — three destinations, one hand */
static const preset_pair_t kGranWheelCloud[] = {
    P(GRAN_PID_FORM, 3.0f), P(GRAN_PID_SIZE, 35.0f),
    P(GRAN_PID_SPREAD, 0.3f),
    P(GRAN_PID_FLT_CUTOFF, 5000.0f),
    P(GRAN_PID_ENV1_ATTACK, 0.06f), P(GRAN_PID_ENV1_SUSTAIN, 0.9f),
    P(GRAN_PID_ENV1_RELEASE, 0.8f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, GRAN_PID_SCAT, 0.5f),
    MOD(1, SYNTH_MOD_SRC_WHEEL, GRAN_PID_JIT, 0.8f),
    MOD(2, SYNTH_MOD_SRC_WHEEL, GRAN_PID_SPREAD, 0.7f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.38f), P(FX_PID_REV_SIZE, 0.8f),
};

/* glide plus a sync train: the formant stays put while the grain rate
 * slides, which is a portamento no other engine here can make */
static const preset_pair_t kGranGlideFormant[] = {
    P(GRAN_PID_WAVE, 2), P(GRAN_PID_FORM, 4.0f),
    P(GRAN_PID_SIZE, 22.0f), P(GRAN_PID_SHAPE, 0.4f),
    P(GRAN_PID_SPREAD, 0.4f),
    P(GRAN_PID_FLT_CUTOFF, 3000.0f), P(GRAN_PID_FLT_KBD, 0.0f),
    P(GRAN_PID_ENV1_ATTACK, 0.02f), P(GRAN_PID_ENV1_SUSTAIN, 0.9f),
    P(GRAN_PID_ENV1_RELEASE, 0.4f),
    P(SYNTH_PID_COMMON_GLIDE, 0.12f),
    P(FX_PID_DLY_ON, 1), P(FX_PID_DLY_MIX, 0.25f), P(FX_PID_DLY_TIME, 0.375f),
    P(FX_PID_DLY_FB, 0.4f), P(FX_PID_DLY_PP, 1),
};

/* the dual filter's two passbands six octaves apart, fed a wide cloud:
 * bottom and top with a hole where the note is */
static const preset_pair_t kGranHollowDual[] = {
    P(GRAN_PID_MODE, 1), P(GRAN_PID_DENS, 100.0f),
    P(GRAN_PID_SIZE, 38.0f), P(GRAN_PID_FORM, 4.0f),
    P(GRAN_PID_SCAT, 6.0f), P(GRAN_PID_SPREAD, 0.9f),
    P(GRAN_PID_JIT, 0.6f),
    P(GRAN_PID_FLT_TYPE, 3), P(GRAN_PID_FLT_CUTOFF, 1000.0f),
    P(GRAN_PID_FLT_SPREAD, 5.0f), P(GRAN_PID_FLT_RESO, 0.45f),
    P(GRAN_PID_ENV1_ATTACK, 0.4f), P(GRAN_PID_ENV1_SUSTAIN, 0.9f),
    P(GRAN_PID_ENV1_RELEASE, 1.4f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.45f), P(FX_PID_REV_SIZE, 0.85f),
};

/* the ring transposed up two octaves by the keyboard, tiny spray: a
 * playable instrument made out of whatever went in */
static const preset_pair_t kGranInKeyed[] = {
    P(GRAN_PID_SRC, 1),
    P(GRAN_PID_MODE, 1), P(GRAN_PID_DENS, 60.0f),
    P(GRAN_PID_SIZE, 90.0f), P(GRAN_PID_SHAPE, 0.5f),
    P(GRAN_PID_BUF_POS, 0.35f), P(GRAN_PID_BUF_SPRAY, 0.04f),
    P(GRAN_PID_BUF_ROOT, 48.0f), /* low root: the keyboard sits above it */
    P(GRAN_PID_SPREAD, 0.5f), P(GRAN_PID_JIT, 0.25f),
    P(GRAN_PID_FLT_CUTOFF, 10000.0f),
    P(GRAN_PID_ENV1_ATTACK, 0.02f), P(GRAN_PID_ENV1_SUSTAIN, 1.0f),
    P(GRAN_PID_ENV1_RELEASE, 0.35f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.3f),
};

/* buf.pos parked at the newest audio with almost no spray, so grains follow
 * the input a few milliseconds behind it — a granular delay you play */
static const preset_pair_t kGranInShadow[] = {
    P(GRAN_PID_SRC, 1),
    P(GRAN_PID_MODE, 1), P(GRAN_PID_DENS, 80.0f),
    P(GRAN_PID_SIZE, 50.0f), P(GRAN_PID_SHAPE, 0.5f),
    P(GRAN_PID_BUF_POS, 0.03f), P(GRAN_PID_BUF_SPRAY, 0.02f),
    P(GRAN_PID_SPREAD, 0.6f), P(GRAN_PID_JIT, 0.3f),
    P(GRAN_PID_FLT_CUTOFF, 12000.0f),
    P(GRAN_PID_ENV1_ATTACK, 0.01f), P(GRAN_PID_ENV1_SUSTAIN, 1.0f),
    P(GRAN_PID_ENV1_RELEASE, 0.25f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, GRAN_PID_BUF_POS, 0.6f),
    P(FX_PID_DLY_ON, 1), P(FX_PID_DLY_MIX, 0.25f), P(FX_PID_DLY_TIME, 0.375f),
    P(FX_PID_DLY_FB, 0.4f), P(FX_PID_DLY_PP, 1),
};

/* half the grains backwards over a frozen ring, with an octave of scatter:
 * the input stops being recognisable and becomes material */
static const preset_pair_t kGranInShards[] = {
    P(GRAN_PID_SRC, 1), P(GRAN_PID_BUF_REV, 0.5f),
    P(GRAN_PID_MODE, 1), P(GRAN_PID_DENS, 40.0f),
    P(GRAN_PID_SIZE, 45.0f), P(GRAN_PID_SHAPE, 0.3f),
    P(GRAN_PID_BUF_POS, 0.5f), P(GRAN_PID_BUF_SPRAY, 0.8f),
    P(GRAN_PID_SCAT, 12.0f), P(GRAN_PID_SPREAD, 1.0f),
    P(GRAN_PID_JIT, 0.9f),
    P(GRAN_PID_FLT_CUTOFF, 8000.0f),
    P(GRAN_PID_ENV1_ATTACK, 0.05f), P(GRAN_PID_ENV1_SUSTAIN, 1.0f),
    P(GRAN_PID_ENV1_RELEASE, 1.5f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.5f), P(FX_PID_REV_SIZE, 0.9f),
};

/* env2 sweeps the read position across the frozen ring on every note, so a
 * key press plays *through* the capture rather than sitting in one spot */
static const preset_pair_t kGranInScrub[] = {
    P(GRAN_PID_SRC, 1),
    P(GRAN_PID_MODE, 1), P(GRAN_PID_DENS, 70.0f),
    P(GRAN_PID_SIZE, 60.0f), P(GRAN_PID_SHAPE, 0.5f),
    P(GRAN_PID_BUF_POS, 0.05f), P(GRAN_PID_BUF_SPRAY, 0.05f),
    P(GRAN_PID_SPREAD, 0.7f), P(GRAN_PID_JIT, 0.4f),
    P(GRAN_PID_ENV2_ATTACK, 2.0f), P(GRAN_PID_ENV2_DECAY, 3.0f),
    P(GRAN_PID_ENV2_SUSTAIN, 1.0f),
    P(GRAN_PID_FLT_CUTOFF, 11000.0f),
    P(GRAN_PID_ENV1_ATTACK, 0.03f), P(GRAN_PID_ENV1_SUSTAIN, 1.0f),
    P(GRAN_PID_ENV1_RELEASE, 0.8f),
    MOD(0, SYNTH_MOD_SRC_ENV2, GRAN_PID_BUF_POS, 0.9f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.4f),
};

/* ---- sampler (S44) ---------------------------------------------------
 *
 * A short bank on purpose. Every other engine's presets describe a *sound*;
 * a sampler's sound is whichever kit the player recorded, so what a preset
 * here can honestly offer is a way of *playing* it. These are the handful of
 * distinct playing arrangements, not a survey of timbres that do not exist
 * until someone records one. */
static const preset_pair_t kSmpPitchedKeys[] = {
    P(SMPE_PID_MODE, 1), P(SMPE_PID_PAD, 0), P(SMPE_PID_ROOT, 60),
    P(SMPE_PID_ENV1_ATTACK, 0.002f), P(SMPE_PID_ENV1_SUSTAIN, 1.0f),
    P(SMPE_PID_ENV1_RELEASE, 0.12f),
};
static const preset_pair_t kSmpPitchedPad[] = {
    P(SMPE_PID_MODE, 1), P(SMPE_PID_PAD, 0), P(SMPE_PID_ROOT, 60),
    P(SMPE_PID_ENV1_ATTACK, 0.6f), P(SMPE_PID_ENV1_SUSTAIN, 1.0f),
    P(SMPE_PID_ENV1_RELEASE, 1.4f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.45f),
};
static const preset_pair_t kSmpPluck[] = {
    P(SMPE_PID_MODE, 1), P(SMPE_PID_ROOT, 60), P(SMPE_PID_VELDEPTH, 1.0f),
    P(SMPE_PID_ENV1_ATTACK, 0.001f), P(SMPE_PID_ENV1_DECAY, 0.35f),
    P(SMPE_PID_ENV1_SUSTAIN, 0.0f), P(SMPE_PID_ENV1_RELEASE, 0.2f),
};
static const preset_pair_t kSmpChopped[] = {
    /* Into the body of the sample rather than its attack — the setting that
     * turns a sliced loop into something you can play a line with. */
    P(SMPE_PID_MODE, 1), P(SMPE_PID_ROOT, 48), P(SMPE_PID_START, 0.12f),
    P(SMPE_PID_ENV1_ATTACK, 0.001f), P(SMPE_PID_ENV1_SUSTAIN, 1.0f),
    P(SMPE_PID_ENV1_RELEASE, 0.05f),
};
static const preset_pair_t kSmpPadsTight[] = {
    P(SMPE_PID_MODE, 0), P(SMPE_PID_SPREAD, 0.4f),
    P(SMPE_PID_ENV1_RELEASE, 0.03f),
};
static const preset_pair_t kSmpPadsWide[] = {
    P(SMPE_PID_MODE, 0), P(SMPE_PID_SPREAD, 1.0f),
    P(FX_PID_REV_ON, 1), P(FX_PID_REV_MIX, 0.3f),
};

/* ---- the banks ------------------------------------------------------ */

#define F(nm, t) {nm, t, N(t)}

const factory_preset_t
    g_factory_presets[SYNTH_ENGINE_COUNT][PRESETS_FACTORY_SLOTS] = {
        /* subtractive */
        {
            {"init", nullptr, 0},
            F("fat saw stack", kSubFatSaw),
            F("acid squelch", kSubAcid),
            F("warm pad", kSubWarmPad),
            F("pwm strings", kSubPwmStrings),
            F("funk pluck", kSubFunkPluck),
            F("deep sub", kSubDeepSub),
            F("noise riser fx", kSubRiser),
            F("rave stab", kSubRaveStab),
            F("glide lead", kSubGlideLead),
            F("poly brass", kSubBrass),
            F("wobble bass", kSubWobble),
            F("hollow keys", kSubHollowKeys),
            F("string machine", kSubStringMachine),
            F("zap perc", kSubZapPerc),
            F("soft keys", kSubSoftKeys),
            /* S33: filter-family showcases first, then the classics the
             * original sixteen had no room for */
            F("ladder bass", kSubLadderBass),
            F("ladder lead", kSubLadderLead),
            F("ladder acid", kSubLadderAcid),
            F("vowel pad", kSubVowelPad),
            F("talking lead", kSubTalkingLead),
            F("formant stab", kSubFormantStab),
            F("notch sweep", kSubNotchSweep),
            F("phase keys", kSubPhaseKeys),
            F("peak sweep", kSubPeakSweep),
            F("dual band stab", kSubDualStab),
            F("narrow lead", kSubNarrowLead),
            F("driven saw", kSubDrivenSaw),
            F("reese bass", kSubReeseBass),
            F("super saw", kSubSuperSaw),
            F("hoover", kSubHoover),
            F("organ tone", kSubOrganTone),
            F("clav plink", kSubClavPlink),
            F("rubber bass", kSubRubberBass),
            F("kick synth", kSubKickSynth),
            F("snare synth", kSubSnareSynth),
            F("tom synth", kSubTomSynth),
            F("wind noise", kSubWindNoise),
            F("dark drone", kSubDarkDrone),
            F("glass pad", kSubGlassPad),
            F("bell pluck", kSubBellPluck),
            F("octave stack", kSubOctaveStack),
            F("detuned keys", kSubDetunedKeys),
            F("soft chords", kSubSoftChords),
            F("velocity keys", kSubVelKeys),
            F("drop tail", kSubDropTail),
            F("fifth lead", kSubFifthLead),
            F("band wash", kSubBandWash),
        },
        /* additive */
        {
            {"init", nullptr, 0},
            F("tonewheel", kAddTonewheel),
            F("glass harp", kAddGlassHarp),
            F("carillon", kAddCarillon),
            F("chiff flute", kAddChiffFlute),
            F("choir pad", kAddChoirPad),
            F("clav bars", kAddClavBars),
            F("kalimba", kAddKalimba),
            F("even bells", kAddEvenBells),
            F("dark organ", kAddDarkOrgan),
            F("harmonic riser", kAddHarmonicRiser),
            F("shimmer keys", kAddShimmerKeys),
            F("drone stack", kAddDroneStack),
            F("toy piano", kAddToyPiano),
            F("drift pad", kAddDriftPad),
            F("perc organ", kAddPercOrgan),
            /* S33: more registrations, plus the ones the new filter made
             * possible — formants and resonant peaks the rolloff cannot do */
            F("full organ", kAddFullOrgan),
            F("jazz organ", kAddJazzOrgan),
            F("gospel organ", kAddGospelOrgan),
            F("reed pipe", kAddReedPipe),
            F("principal pipe", kAddPrincipalPipe),
            F("octave organ", kAddOctaveOrgan),
            F("fifth organ", kAddFifthOrgan),
            F("vocal ah", kAddVocalAh),
            F("vocal ooh", kAddVocalOoh),
            F("formant choir", kAddFormantChoir),
            F("marimba", kAddMarimba),
            F("vibraphone", kAddVibraphone),
            F("celeste", kAddCeleste),
            F("gong", kAddGong),
            F("singing bowl", kAddSingingBowl),
            F("harmonium", kAddHarmonium),
            F("accordion", kAddAccordion),
            F("string stack", kAddStringStack),
            F("brass stack", kAddBrassStack),
            F("warm ladder", kAddWarmLadder),
            F("notch drift", kAddNotchDrift),
            F("pure tones", kAddPureTones),
            F("sine keys", kAddSineKeys),
            F("steel drum", kAddSteelDrum),
            F("bell tree", kAddBellTree),
            F("reverse bloom", kAddReverseBloom),
            F("metal drone", kAddMetalDrone),
            F("velocity reed", kAddVelReed),
            F("wheel sweep", kAddWheelSweep),
            F("glass organ", kAddGlassOrgan),
            F("hollow pad", kAddHollowPad),
            F("sub drone", kAddSubDrone),
        },
        /* fm */
        {
            {"init", nullptr, 0},
            F("bright tines", kFmBrightTines),
            F("growl bass", kFmGrowlBass),
            F("tubular bell", kFmTubularBell),
            F("wurli bark", kFmWurli),
            F("fm brass", kFmBrass),
            F("glass keys", kFmGlassKeys),
            F("snappy hit", kFmSnappyHit),
            F("log drum", kFmLogDrum),
            F("dx strings", kFmDxStrings),
            F("punch bass", kFmPunchBass),
            F("music box", kFmMusicBox),
            F("fm flute", kFmFlute),
            F("sci-fi swell", kFmSciFiSwell),
            F("bell pad", kFmBellPad),
            F("fm clav", kFmClav),
            /* S33: FM had no filter until now, so the ones that switch it on
             * are sounds this engine could not make before */
            F("rhodes mk1", kFmRhodesMk1),
            F("hard tines", kFmHardTines),
            F("smooth keys", kFmSmoothKeys),
            F("voice box", kFmVoiceBox),
            F("metal bass", kFmMetalBass),
            F("sub bass", kFmSubBass),
            F("slap bass", kFmSlapBass),
            F("buzz lead", kFmBuzzLead),
            F("singing lead", kFmSingLead),
            F("koto", kFmKoto),
            F("harp", kFmHarp),
            F("marimba", kFmMarimba),
            F("vibes", kFmVibes),
            F("glockenspiel", kFmGlocken),
            F("church bell", kFmChurchBell),
            F("gamelan", kFmGamelan),
            F("fm organ", kFmOrgan),
            F("brass swell", kFmBrassSwell),
            F("horn", kFmHorn),
            F("clarinet", kFmClarinet),
            F("oboe", kFmOboe),
            F("detune pad", kFmDetunePad),
            F("soft pad", kFmSoftPad),
            F("phase pad", kFmPhasePad),
            F("clang stab", kFmClangStab),
            F("timpani", kFmTimpani),
            F("wood block", kFmWoodBlock),
            F("noise burst", kFmNoiseBurst),
            F("drop fx", kFmDropFx),
            F("siren", kFmSiren),
            F("ladder pluck", kFmLadderPluck),
            F("octave mallet", kFmOctaveMallet),
        },
        /* wavetable */
        {
            {"init", nullptr, 0},
            F("sync lead", kWtSyncLead),
            F("vowel pad", kWtVowelPad),
            F("digi keys", kWtDigiKeys),
            F("pwm drift", kWtPwmDrift),
            F("sync sub bass", kWtSyncSubBass),
            F("wave organ", kWtWaveOrgan),
            F("big choir", kWtBigChoir),
            F("sync riser", kWtSyncRiser),
            F("fm chime", kWtFmChime),
            F("shimmer pad", kWtShimmerPad),
            F("talk box", kWtTalkBox),
            F("chip lead", kWtChipLead),
            F("evolving drone", kWtEvolvingDrone),
            F("arp cascade", kWtArpCascade),
            F("glassy keys", kWtGlassyKeys),
            /* S33: the vocal table under the vowel filter is the pairing
             * this engine was waiting for — several of these are built on it */
            F("double vowel", kWtDoubleVowel),
            F("robot voice", kWtRobotVoice),
            F("formant sweep", kWtFormantSweep),
            F("vowel bass", kWtVowelBass),
            F("sync scream", kWtSyncScream),
            F("sync bass", kWtSyncBass),
            F("digital bass", kWtDigitalBass),
            F("wide pad", kWtWidePad),
            F("band morph", kWtBandMorph),
            F("notch morph", kWtNotchMorph),
            F("peak morph", kWtPeakMorph),
            F("drive lead", kWtDriveLead),
            F("slow morph", kWtSlowMorph),
            F("velocity morph", kWtVelMorph),
            F("bell stack", kWtBellStack),
            F("bright pluck", kWtBrightPluck),
            F("dark pluck", kWtDarkPluck),
            F("ladder bass", kWtLadderBass),
            F("wt brass", kWtBrass),
            F("wt strings", kWtStrings),
            F("air layer", kWtAirLayer),
            F("phase drift", kWtPhaseDrift),
            F("split bass", kWtSplitBass),
            F("choir stack", kWtChoirStack),
            F("chip arp", kWtChipArp),
            F("step texture", kWtStepTexture),
            F("slow texture", kWtSlowTexture),
            F("wheel morph", kWtWheelMorph),
            F("draw organ", kWtDrawOrgan),
            F("spectral riser", kWtSpectralRiser),
            F("lofi keys", kWtLofiKeys),
            F("grain cloud", kWtGrainCloud),
        },
        /* modular (S28) — deliberately all "init".
         *
         * Present unconditionally since S38: the engine's *index* is
         * reserved whether or not the graph is compiled in (engines.h), so
         * this row has to exist to keep granular at 5. On a build without
         * the modular engine nothing can reach these slots — presets_load
         * resolves the engine first — so the cost is the row itself.
         *
         * A factory preset is a table of {id, value} pairs, and for the
         * modular engine those ids only mean something *given a graph*: node
         * slot 3's parameter 1 is a cutoff or a decay time depending on what
         * is patched there. A factory bank in this format could therefore
         * only ever describe values for a patch it cannot itself carry, and
         * loading one onto a different graph would scatter plausible numbers
         * across the wrong controls.
         *
         * The engine's own factory patch (graph_engine.cpp) is what a fresh
         * modular voice starts from, and user slots store the graph properly
         * as version-2 preset files. Filling this bank means writing the
         * graph blobs into flash as const data — worth doing, and a session
         * of its own rather than a half-answer here.
         *
         * Every entry still needs a valid name: fetch_snapshot() copies it
         * unconditionally, so a zero-filled row would be a null dereference
         * the moment someone selected a factory slot on this engine. */
        {
            {"init", nullptr, 0},  {"init", nullptr, 0}, {"init", nullptr, 0},
            {"init", nullptr, 0},  {"init", nullptr, 0}, {"init", nullptr, 0},
            {"init", nullptr, 0},  {"init", nullptr, 0}, {"init", nullptr, 0},
            {"init", nullptr, 0},  {"init", nullptr, 0}, {"init", nullptr, 0},
            {"init", nullptr, 0},  {"init", nullptr, 0}, {"init", nullptr, 0},
            {"init", nullptr, 0},  {"init", nullptr, 0}, {"init", nullptr, 0},
            {"init", nullptr, 0},  {"init", nullptr, 0}, {"init", nullptr, 0},
            {"init", nullptr, 0},  {"init", nullptr, 0}, {"init", nullptr, 0},
            {"init", nullptr, 0},  {"init", nullptr, 0}, {"init", nullptr, 0},
            {"init", nullptr, 0},  {"init", nullptr, 0}, {"init", nullptr, 0},
            {"init", nullptr, 0},  {"init", nullptr, 0}, {"init", nullptr, 0},
            {"init", nullptr, 0},  {"init", nullptr, 0}, {"init", nullptr, 0},
            {"init", nullptr, 0},  {"init", nullptr, 0}, {"init", nullptr, 0},
            {"init", nullptr, 0},  {"init", nullptr, 0}, {"init", nullptr, 0},
            {"init", nullptr, 0},  {"init", nullptr, 0}, {"init", nullptr, 0},
            {"init", nullptr, 0},  {"init", nullptr, 0}, {"init", nullptr, 0},
        },

        /* granular (S38) — the full 48. */
        {
            {"init", nullptr, 0},
            F("fof vowel", kGranFofVowel),
            F("pulsar bass", kGranPulsarBass),
            F("formant sweep", kGranFormantSweep),
            F("glass cloud", kGranGlassCloud),
            F("grain choir", kGranGrainChoir),
            F("metal train", kGranMetalTrain),
            F("noise sizzle", kGranNoiseSizzle),
            F("bowed swell", kGranBowedSwell),
            F("click perc", kGranClickPerc),
            F("wide scatter", kGranWideScatter),
            F("sub pulsar", kGranSubPulsar),
            F("talkbox lead", kGranTalkbox),
            F("velocity swarm", kGranVelSwarm),
            F("in: live cloud", kGranInLive),
            F("in: hold pad", kGranInFreeze),
            F("in: reverse wash", kGranInReverse),
            F("formant bell", kGranFormantBell),
            F("vowel morph", kGranVowelMorph),
            F("buzz reed", kGranBuzzReed),
            F("pulse organ", kGranPulseOrgan),
            F("pitched rattle", kGranRattle),
            F("granular strings", kGranStrings),
            F("deep drone", kGranDeepDrone),
            F("shimmer high", kGranShimmerHigh),
            F("stutter cloud", kGranStutter),
            F("vowel ah", kGranVowelAh),
            F("vowel ee", kGranVowelEe),
            F("vowel oo", kGranVowelOo),
            F("insect swarm", kGranInsects),
            F("wind tunnel", kGranWindTunnel),
            F("bit choir", kGranBitChoir),
            F("harmonic ladder", kGranHarmLadder),
            F("dark pulsar", kGranDarkPulsar),
            F("crystal ping", kGranCrystalPing),
            F("granular brass", kGranBrass),
            F("scatter bells", kGranScatterBells),
            F("sub thump", kGranSubThump),
            F("glitch rain", kGranGlitchRain),
            F("velocity size", kGranVelSize),
            F("key shape", kGranKeyShape),
            F("wheel cloud", kGranWheelCloud),
            F("glide formant", kGranGlideFormant),
            F("hollow dual", kGranHollowDual),
            F("in: keyed", kGranInKeyed),
            F("in: shadow", kGranInShadow),
            F("in: shards", kGranInShards),
            F("in: scrub", kGranInScrub),
        },
        /* sampler (S44) */
        {
            {"init", nullptr, 0},
            F("pads: tight", kSmpPadsTight),
            F("pads: wide", kSmpPadsWide),
            F("keys", kSmpPitchedKeys),
            F("pluck", kSmpPluck),
            F("pad wash", kSmpPitchedPad),
            F("chopped", kSmpChopped),
        },
};
