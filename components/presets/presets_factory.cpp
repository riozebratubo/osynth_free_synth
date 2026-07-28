/*
 * osynth — factory presets (Session 13): 16 per engine, const in flash.
 *
 * Each preset is a sparse list of {param id, value} overrides on the
 * engine-default patch: loading resets the patch ranges to their defaults
 * first (minus the skip list — presets.cpp), so anything not listed here
 * is the PARAM_MAP.md default. Slot 0 of every bank is "init", the pure
 * default patch. Enum values are numeric: osc waves 0 sine / 1 tri /
 * 2 saw / 3 pulse; filter modes 0 lp / 1 bp / 2 hp; lfo waves 0 sine /
 * 1 tri / 2 saw / 3 square / 4 s&h; wavetable sets 0 basic / 1 sync /
 * 2 vocal / 3 fm; arp modes 0 off / 1 up / 2 down / 3 updown / 4 random /
 * 5 played; seq divisions 0 1/4 … 5 1/32.
 *
 * Mix/level/drawbar sums are kept near 1.0 so full 8-voice polyphony
 * cannot clip (the engines' gain-staging convention since S4).
 */
#include "presets_priv.h"

#include "engine_additive.h"
#include "engine_fm.h"
#include "engine_subtractive.h"
#include "engine_wavetable.h"
#include "fx.h"
#include "seqarp.h"
#include "synth_mod.h"
#include "synth_params_c.h"

#define P(id, v) {(uint16_t)(id), (float)(v)}
#define N(t) (uint16_t)(sizeof(t) / sizeof((t)[0]))

/* Mod-matrix slot k as three pairs. */
#define MOD(k, src, dest, amt)              \
    P(SYNTH_PID_MOD_SRC(k), (src)),         \
    P(SYNTH_PID_MOD_DEST(k), (dest)),       \
    P(SYNTH_PID_MOD_AMOUNT(k), (amt))

/* ---- subtractive (bank 0, slots 0-79) ------------------------------- */

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
    P(FX_PID_DLY_MIX, 0.25f), P(FX_PID_DLY_TIME, 0.25f),
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
    P(FX_PID_CHO_MIX, 0.55f), P(FX_PID_CHO_RATE, 0.7f),
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
    P(FX_PID_CHO_MIX, 0.2f), P(FX_PID_REV_MIX, 0.25f),
};

/* ---- additive (bank 1, slots 80-159) -------------------------------- */

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
    P(FX_PID_REV_MIX, 0.2f),
};

/* odd-leaning soft spectrum swelling bright — vocal-ish pad */
static const preset_pair_t kAddChoirPad[] = {
    P(ADD_PID_EVENODD, -0.35f), P(ADD_PID_BRIGHT, 0.28f),
    P(ADD_PID_ENV_BRIGHT, 0.3f),
    P(ADD_PID_ENV2_ATTACK, 0.8f), P(ADD_PID_ENV2_SUSTAIN, 0.7f),
    P(ADD_PID_ENV1_ATTACK, 0.5f), P(ADD_PID_ENV1_DECAY, 0.5f),
    P(ADD_PID_ENV1_SUSTAIN, 0.95f), P(ADD_PID_ENV1_RELEASE, 1.2f),
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
    P(FX_PID_DLY_MIX, 0.2f), P(FX_PID_DLY_TIME, 0.3f), P(FX_PID_DLY_PP, 1),
    P(FX_PID_REV_MIX, 0.25f),
};

/* even partials take over — hollow octave-up bells */
static const preset_pair_t kAddEvenBells[] = {
    P(ADD_PID_EVENODD, 0.7f), P(ADD_PID_INHARM, 0.014f),
    P(ADD_PID_BRIGHT, 0.6f), P(ADD_PID_ENV_BRIGHT, 0.3f),
    P(ADD_PID_ENV2_DECAY, 1.2f), P(ADD_PID_ENV2_SUSTAIN, 0.0f),
    P(ADD_PID_ENV1_DECAY, 2.0f), P(ADD_PID_ENV1_SUSTAIN, 0.0f),
    P(ADD_PID_ENV1_RELEASE, 1.5f),
    P(FX_PID_REV_MIX, 0.35f),
};

/* steep negative tilt, static spectrum, damped room */
static const preset_pair_t kAddDarkOrgan[] = {
    P(ADD_PID_TILT, -6.0f), P(ADD_PID_BRIGHT, 0.3f),
    P(ADD_PID_ENV_BRIGHT, 0.0f), P(ADD_PID_VEL_BRIGHT, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.004f), P(ADD_PID_ENV1_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_RELEASE, 0.1f),
    P(FX_PID_REV_MIX, 0.25f), P(FX_PID_REV_DAMP, 0.6f),
};

/* brightness env crawls open over ~3 s while held — spectral riser */
static const preset_pair_t kAddHarmonicRiser[] = {
    P(ADD_PID_BRIGHT, 0.05f), P(ADD_PID_ENV_BRIGHT, 1.0f),
    P(ADD_PID_TILT, -1.0f),
    P(ADD_PID_ENV2_ATTACK, 3.0f), P(ADD_PID_ENV2_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_ATTACK, 0.8f), P(ADD_PID_ENV1_SUSTAIN, 1.0f),
    P(ADD_PID_ENV1_RELEASE, 1.0f),
    P(FX_PID_REV_MIX, 0.4f),
};

/* slow lfo2 shimmer on the rolloff, soft decaying keys */
static const preset_pair_t kAddShimmerKeys[] = {
    P(ADD_PID_BRIGHT, 0.45f), P(ADD_PID_ENV_BRIGHT, 0.3f),
    P(ADD_PID_VEL_BRIGHT, 0.4f),
    P(ADD_PID_LFO2_RATE, 0.5f), P(ADD_PID_LFO2_BRIGHT, 0.25f),
    P(ADD_PID_ENV1_DECAY, 1.2f), P(ADD_PID_ENV1_SUSTAIN, 0.3f),
    P(ADD_PID_ENV1_RELEASE, 0.8f),
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
    P(FX_PID_REV_MIX, 0.45f), P(FX_PID_REV_SIZE, 0.85f),
};

/* bright, detuned, positive tilt — cheap and cheerful toy piano */
static const preset_pair_t kAddToyPiano[] = {
    P(ADD_PID_INHARM, 0.01f), P(ADD_PID_TILT, 2.0f),
    P(ADD_PID_BRIGHT, 0.6f), P(ADD_PID_VEL_BRIGHT, 0.5f),
    P(ADD_PID_ENV2_DECAY, 0.3f), P(ADD_PID_ENV2_SUSTAIN, 0.0f),
    P(ADD_PID_ENV1_ATTACK, 0.001f), P(ADD_PID_ENV1_DECAY, 0.8f),
    P(ADD_PID_ENV1_SUSTAIN, 0.0f), P(ADD_PID_ENV1_RELEASE, 0.5f),
    P(FX_PID_DLY_MIX, 0.12f), P(FX_PID_REV_MIX, 0.2f),
};

/* dark tilted spectrum drifting under a very slow brightness lfo */
static const preset_pair_t kAddDriftPad[] = {
    P(ADD_PID_BRIGHT, 0.3f), P(ADD_PID_TILT, -3.0f),
    P(ADD_PID_LFO2_RATE, 0.2f), P(ADD_PID_LFO2_BRIGHT, 0.35f),
    P(ADD_PID_ENV1_ATTACK, 0.6f), P(ADD_PID_ENV1_SUSTAIN, 0.9f),
    P(ADD_PID_ENV1_RELEASE, 1.5f),
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

/* ---- fm (bank 2, slots 160-239) ------------------------------------- */

/* harder e-piano: more index, hotter tine pair */
static const preset_pair_t kFmBrightTines[] = {
    P(FM_PID_A_INDEX, 2.8f), P(FM_PID_A_LEVEL, 0.7f),
    P(FM_PID_B_LEVEL, 0.3f), P(FM_PID_B_INDEX, 1.6f),
    P(FM_PID_B_DETUNE, 5.0f), P(FM_PID_VEL_INDEX, 0.8f),
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

/* ---- wavetable (bank 3, slots 240-319) ------------------------------ */

/* sync table swept hard by the env, wheel drags the position */
static const preset_pair_t kWtSyncLead[] = {
    P(WT_PID_OSC1_TABLE, 1), P(WT_PID_OSC1_POS, 0.15f),
    P(WT_PID_ENV_POS, 0.8f),
    P(WT_PID_ENV2_DECAY, 0.5f), P(WT_PID_ENV2_SUSTAIN, 0.3f),
    P(WT_PID_ENV1_SUSTAIN, 0.8f),
    P(SYNTH_PID_COMMON_UNISON, 2), P(SYNTH_PID_COMMON_UNI_DETUNE, 14.0f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, WT_PID_OSC1_POS, 0.6f),
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
    P(FX_PID_REV_MIX, 0.25f),
};

/* parked in the thin pulses, slow position wobble — pwm without pwm */
static const preset_pair_t kWtPwmDrift[] = {
    P(WT_PID_OSC1_POS, 0.8f), P(WT_PID_ENV_POS, -0.25f),
    P(WT_PID_LFO2_RATE, 0.5f), P(WT_PID_LFO2_POS, 0.15f),
    P(WT_PID_ENV1_ATTACK, 0.25f), P(WT_PID_ENV1_SUSTAIN, 0.85f),
    P(WT_PID_ENV1_RELEASE, 0.9f),
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
    P(FX_PID_CHO_MIX, 0.3f), P(FX_PID_REV_MIX, 0.45f),
    P(FX_PID_REV_SIZE, 0.8f),
};

/* position crawls the whole sync sweep over ~3 s while held */
static const preset_pair_t kWtSyncRiser[] = {
    P(WT_PID_OSC1_TABLE, 1), P(WT_PID_OSC1_POS, 0.0f),
    P(WT_PID_ENV_POS, 1.0f),
    P(WT_PID_ENV2_ATTACK, 2.8f), P(WT_PID_ENV2_SUSTAIN, 1.0f),
    P(WT_PID_ENV1_ATTACK, 1.0f), P(WT_PID_ENV1_SUSTAIN, 1.0f),
    P(FX_PID_REV_MIX, 0.35f),
};

/* fm table struck bright and falling back — metallic chime */
static const preset_pair_t kWtFmChime[] = {
    P(WT_PID_OSC1_TABLE, 3), P(WT_PID_OSC1_POS, 0.7f),
    P(WT_PID_ENV_POS, -0.5f),
    P(WT_PID_ENV2_DECAY, 1.2f), P(WT_PID_ENV2_SUSTAIN, 0.0f),
    P(WT_PID_ENV1_DECAY, 2.0f), P(WT_PID_ENV1_SUSTAIN, 0.0f),
    P(WT_PID_ENV1_RELEASE, 1.2f),
    P(FX_PID_DLY_MIX, 0.2f), P(FX_PID_DLY_TIME, 0.4f), P(FX_PID_DLY_PP, 1),
    P(FX_PID_REV_MIX, 0.3f),
};

/* granular pitch +12 with feedback over a vocal pad — shimmer */
static const preset_pair_t kWtShimmerPad[] = {
    P(WT_PID_OSC1_TABLE, 2), P(WT_PID_OSC1_POS, 0.4f),
    P(WT_PID_ENV_POS, 0.0f),
    P(WT_PID_ENV1_ATTACK, 0.7f), P(WT_PID_ENV1_SUSTAIN, 0.9f),
    P(WT_PID_ENV1_RELEASE, 1.8f),
    P(FX_PID_GRN_MIX, 0.35f), P(FX_PID_GRN_PITCH, 12.0f),
    P(FX_PID_GRN_SIZE, 0.3f), P(FX_PID_GRN_DENS, 18.0f),
    P(FX_PID_GRN_SPRAY, 0.12f), P(FX_PID_GRN_FB, 0.45f),
    P(FX_PID_REV_MIX, 0.35f),
};

/* wheel rides the vowel morph — a talk box you play */
static const preset_pair_t kWtTalkBox[] = {
    P(WT_PID_OSC1_TABLE, 2), P(WT_PID_OSC1_POS, 0.0f),
    P(WT_PID_ENV_POS, 0.0f),
    P(WT_PID_FLT_CUTOFF, 5000.0f),
    P(WT_PID_ENV1_SUSTAIN, 0.9f),
    MOD(0, SYNTH_MOD_SRC_WHEEL, WT_PID_OSC1_POS, 1.0f),
    P(FX_PID_DLY_MIX, 0.15f),
};

/* square-ish frame, snappy env, fixed fast vibrato — chiptune lead */
static const preset_pair_t kWtChipLead[] = {
    P(WT_PID_OSC1_POS, 0.75f), P(WT_PID_ENV_POS, 0.0f),
    P(WT_PID_FLT_CUTOFF, 12000.0f),
    P(WT_PID_ENV1_ATTACK, 0.001f), P(WT_PID_ENV1_DECAY, 0.1f),
    P(WT_PID_ENV1_SUSTAIN, 0.7f), P(WT_PID_ENV1_RELEASE, 0.05f),
    P(WT_PID_LFO1_RATE, 6.0f), P(WT_PID_LFO1_PITCH, 0.15f),
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
    P(FX_PID_CHO_MIX, 0.3f), P(FX_PID_REV_MIX, 0.35f),
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
        },
};
