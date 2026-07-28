/*
 * osynth — mod matrix (Session 9).
 *
 * 8 general-purpose modulation slots, each source -> destination x amount.
 * Sources mix per-voice signals (the engine's mod envelope and LFOs,
 * velocity, note) with global controllers (pitch bend, mod wheel). The
 * destination is a ParamStore id — but a slot only has an effect where a
 * consumer routes its read through synth_mod_apply(); docs/PARAM_MAP.md
 * lists the destinations each engine consumes per voice. A slot aimed
 * anywhere else is silently inert, like a MIDI CC for an inactive engine.
 *
 * Evaluation is block-rate in the voice path: the voice manager calls
 * synth_mod_begin_block() once per block (only when the bound engine
 * declares SYNTH_CAP_MODMATRIX), which snapshots the slot params, resolves
 * destination descriptors and folds the global sources; the engine then
 * calls synth_mod_apply() per voice for each modulatable parameter it is
 * about to consume. Amounts are normalized: |amount| = 1 sweeps the
 * destination across its whole range (geometrically for Exp-curve params);
 * the result is clamped to the destination's range, and snapped to an
 * integer for Int/Enum/Bool destinations.
 *
 * Threading: init() control task, before the audio task starts;
 * set_wheel() any control task; begin_block()/apply() audio task only.
 * Slot values are ParamStore atomics, so control writes land between
 * blocks. begin_block() is only reached while an engine is bound, which is
 * what makes its registry lookups safe against the engine-switch
 * add()/removeRange() (the switch protocol detaches the engine first).
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYNTH_MOD_SLOTS 8

/* Slot k (0-based; params are named mod1..mod8) occupies
 * SYNTH_PID_MOD_BASE + 4k: src, dest, amount (one id spare per slot). */
#define SYNTH_PID_MOD_BASE      0x0500
#define SYNTH_PID_MOD_SRC(k)    ((uint16_t)(SYNTH_PID_MOD_BASE + (k) * 4 + 0))
#define SYNTH_PID_MOD_DEST(k)   ((uint16_t)(SYNTH_PID_MOD_BASE + (k) * 4 + 1))
#define SYNTH_PID_MOD_AMOUNT(k) ((uint16_t)(SYNTH_PID_MOD_BASE + (k) * 4 + 2))

/* Modulation sources (the modN.src enum). env2/lfo1/lfo2 are the engine's
 * shared blocks — engines that gate a block out (caps) feed 0 for it. */
typedef enum {
    SYNTH_MOD_SRC_OFF = 0,
    SYNTH_MOD_SRC_ENV2,  /* mod envelope level, 0..1 */
    SYNTH_MOD_SRC_LFO1,  /* -1..1 */
    SYNTH_MOD_SRC_LFO2,  /* -1..1 */
    SYNTH_MOD_SRC_VEL,   /* note-on velocity, 0..1 */
    SYNTH_MOD_SRC_NOTE,  /* keyboard position, (note - 60) / 60 */
    SYNTH_MOD_SRC_BEND,  /* pitch bend, -1..1 (raw, before common.bend.range) */
    SYNTH_MOD_SRC_WHEEL, /* mod wheel (CC 1), 0..1 */
    SYNTH_MOD_SRC_COUNT
} synth_mod_src_t;

/* Per-voice source values, filled by the engine before synth_mod_apply().
 * Blocks the engine doesn't compose (module gating) stay 0. */
typedef struct {
    float env2; /* mod envelope level, 0..1 */
    float lfo1; /* -1..1 */
    float lfo2; /* -1..1 */
    float vel;  /* 0..1 */
    float note; /* MIDI note number, 0..127 */
} synth_mod_voice_src_t;

/* Registers the 0x05xx slot parameters (every slot defaults to off).
 * The matrix is global — it survives engine switches; slots whose dest
 * belongs to an inactive engine simply stop resolving. */
esp_err_t synth_mod_init(void);

/* Mod wheel (CC 1) feed, 0..1 — any control task. */
void synth_mod_set_wheel(float wheel01);

/* Audio task, once per block before the engine's begin_block(): snapshots
 * slot configs, resolves destinations, folds bend/wheel into per-slot
 * offsets. bend_norm is the raw [-1, 1] pitch bend. */
void synth_mod_begin_block(float bend_norm);

/* Audio task, per voice: the modulated value of param `pid` given its
 * per-block base value. Returns `base` untouched when no active slot
 * targets `pid`. */
float synth_mod_apply(uint16_t pid, float base,
                      const synth_mod_voice_src_t* s);

#ifdef __cplusplus
}
#endif
