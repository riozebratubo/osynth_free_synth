/*
 * osynth — C bridge for the C++ ParamStore (Session 5; NRPN entry Session 9).
 * For C callers (the MIDI router today): set a registered parameter from a
 * normalized controller value, mapped through the parameter's range/curve.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* C mirrors of the osynth:: PID constants needed from C code. Source of
 * truth is synth_params.h; static_asserts in synth_params.cpp keep these in
 * sync. */
#define SYNTH_PID_ENGINE_TYPE       0x0001
#define SYNTH_PID_USB_MODE          0x000C
#define SYNTH_PID_COMMON_GLIDE      0x0100
#define SYNTH_PID_COMMON_BEND_RANGE 0x0101
#define SYNTH_PID_COMMON_UNISON     0x0102
#define SYNTH_PID_COMMON_UNI_DETUNE 0x0103
#define SYNTH_PID_COMMON_UNI_SPREAD 0x0104

/* Sets param `id` from a 0..1 control value (e.g. MIDI CC / 127): Exp-curve
 * params interpolate the range geometrically, everything else linearly;
 * Int/Enum/Bool values are rounded by the store. Origin = Midi. Returns
 * false if `id` is not registered (e.g. the param belongs to an engine that
 * is not active) — harmless, the control is simply ignored. */
bool synth_param_set_norm_midi(uint16_t id, float norm01);

/* Sets param `id` to a raw value (clamped/snapped by the store), origin
 * Midi. Used where the controller value is the value itself — e.g. program
 * change -> engine.type. Returns false if `id` is not registered. */
bool synth_param_set_midi(uint16_t id, float value);

/* NRPN data entry (Session 9 — the general MIDI mapping layer): Float
 * params take the 14-bit value normalized (0..16383 spans the range through
 * the curve, like synth_param_set_norm_midi at 14-bit resolution);
 * Int/Enum/Bool params take `min + value14` (so with min = 0 — enums, mod
 * dest ids — the data value IS the value). Origin = Midi. Returns false if
 * `id` is not registered. */
bool synth_param_set_nrpn_midi(uint16_t id, uint16_t value14);

#ifdef __cplusplus
}
#endif
