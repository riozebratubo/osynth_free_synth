/*
 * osynth — synth engines: subtractive, additive, FM, wavetable.
 * Session 5: subtractive engine. Session 6: FM engine + live switching —
 * writing engine.type (MIDI program change, later BLE/presets) runs the
 * switch protocol on a dedicated task. Session 7: wavetable engine.
 * Session 8: additive engine — all four slots filled.
 */
#pragma once

#include "esp_err.h"

#include "synth_config.h"
#include "synth_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Values match the engine.type enum parameter (PID_ENGINE_TYPE), and the
 * name table in main.cpp must stay in step. The modular engine (S28) is
 * Kconfig-gated and therefore last: a build without it simply has a shorter
 * enum, and no stored value can point at an engine that is not there. */
typedef enum {
    SYNTH_ENGINE_SUBTRACTIVE = 0,
    SYNTH_ENGINE_ADDITIVE = 1,
    SYNTH_ENGINE_FM = 2,
    SYNTH_ENGINE_WAVETABLE = 3,
#if SYNTH_ENABLE_MODULAR
    SYNTH_ENGINE_MODULAR = 4,
#endif
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
