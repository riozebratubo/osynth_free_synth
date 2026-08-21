/*
 * osynth — synth engines: subtractive, additive, FM, wavetable, granular.
 * Session 5: subtractive engine. Session 6: FM engine + live switching —
 * writing engine.type (MIDI program change, later BLE/presets) runs the
 * switch protocol on a dedicated task. Session 7: wavetable engine.
 * Session 8: additive engine — all four slots filled. Session 28: the
 * modular graph, Kconfig-gated. Session 38: granular — a per-voice grain
 * cloud, and the first fixed engine that can granulate the audio input.
 */
#pragma once

#include "esp_err.h"

#include "synth_config.h"
#include "synth_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Values match the engine.type enum parameter (PID_ENGINE_TYPE), and the
 * name table in main.cpp must stay in step.
 *
 * Every value here is unconditional, including the Kconfig-gated modular
 * engine (S28). Until S38 that one was compiled out of the enum entirely, on
 * the reasoning that a shorter enum means no stored value can point at an
 * engine that is not there. Adding a sixth engine is what retired the idea:
 * an entry after a conditional one has no fixed number, and the engine index
 * is not a private detail — it is in the preset filename (p<engine>_<slot>
 * .osp), in that file's header, in OP_SELECT_ENGINE and in program change.
 * An index that moved with a Kconfig option would orphan a user's saved
 * presets on a rebuild, and renumbering the modular engine to keep the tail
 * contiguous would do worse: p4_*.osp files would still match on engine id
 * and load a graph patch onto whatever took the slot.
 *
 * So the numbers are frozen and *availability* is what varies:
 * engines_get() returns NULL for an engine this build does not have, which
 * every caller already handles — engines_init() falls back to subtractive at
 * boot, the switch task warns and reverts engine.type, and PARAM_INFO serves
 * caps 0. That path was always reachable; it is now simply also how an
 * absent modular engine reports itself. */
typedef enum {
    SYNTH_ENGINE_SUBTRACTIVE = 0,
    SYNTH_ENGINE_ADDITIVE = 1,
    SYNTH_ENGINE_FM = 2,
    SYNTH_ENGINE_WAVETABLE = 3,
    SYNTH_ENGINE_MODULAR = 4, /* NULL unless SYNTH_ENABLE_MODULAR */
    SYNTH_ENGINE_GRANULAR = 5,
    SYNTH_ENGINE_COUNT
} synth_engine_type_t;

/* Vtable for an engine type, or NULL if that engine isn't built yet. */
const synth_engine_t* engines_get(synth_engine_type_t type);

/* The currently bound engine type (updated by the switch task). Safe from
 * any task — used by the MIDI router to pick the per-engine CC map. */
synth_engine_type_t engines_active_type(void);

/* Initializes the engine named by engine.type (registers its 0x02xx
 * params), binds it to the voice manager, and starts the engine-switch
 * listener + task. Call after voice_manager_init(), before
 * audio_io_start(). */
esp_err_t engines_init(void);

#ifdef __cplusplus
}
#endif
