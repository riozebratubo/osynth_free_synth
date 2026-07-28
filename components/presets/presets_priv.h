/*
 * osynth — presets internals shared between presets.cpp and the factory
 * tables (Session 13).
 */
#pragma once

#include <stdint.h>

#include "engines.h"
#include "presets.h"

/* One stored value. Packed — this is also the on-disk pair layout (GCC
 * generates safe byte access for packed members on Xtensa). */
typedef struct __attribute__((packed)) {
    uint16_t id;
    float val;
} preset_pair_t;

typedef struct {
    const char* name;           /* <= PRESETS_NAME_MAX-1 chars */
    const preset_pair_t* pairs; /* sparse overrides on the default patch */
    uint16_t count;
} factory_preset_t;

/* [engine][slot], every entry valid; slot 0 is "init" (no overrides). */
extern const factory_preset_t
    g_factory_presets[SYNTH_ENGINE_COUNT][PRESETS_FACTORY_SLOTS];
