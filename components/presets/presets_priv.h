/*
 * osynth — presets internals shared between presets.cpp and the factory
 * tables (Session 13).
 */
#pragma once

#include <stdint.h>

#include "engines.h"
#include "presets.h"
#include "synth_pack.h"

/* One stored value. Packed — this is also the on-disk pair layout (GCC
 * generates safe byte access for packed members on Xtensa). */
OSYNTH_PACK_PUSH
typedef struct OSYNTH_PACKED {
    uint16_t id;
    float val;
} preset_pair_t;
/* Every .osp, .oss and .osw file is a stream of these, so a compiler that
 * padded it to 8 would read every stored patch off by two bytes per pair from
 * the first one onward. This was the only packed struct in the tree without a
 * size assertion; it is the one that could do the most damage without it. */
static_assert(sizeof(preset_pair_t) == 6, "on-disk layout");
OSYNTH_PACK_POP

typedef struct {
    const char* name;           /* <= PRESETS_NAME_MAX-1 chars */
    const preset_pair_t* pairs; /* sparse overrides on the default patch */
    uint16_t count;
} factory_preset_t;

/* [engine][slot], every entry valid; slot 0 is "init" (no overrides). */
extern const factory_preset_t
    g_factory_presets[SYNTH_ENGINE_COUNT][PRESETS_FACTORY_SLOTS];
