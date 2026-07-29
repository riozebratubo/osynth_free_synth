/*
 * osynth — modular patch graph: the engine vtable (Session 28).
 *
 * Bound by engines.cpp as SYNTH_ENGINE_MODULAR, using the same S6 switch
 * protocol as the four fixed engines. Built only when
 * CONFIG_OSYNTH_ENABLE_MODULAR is set — see engines_get(), which returns
 * null for an engine this build does not carry and whose switch path
 * already handles that by reverting the request.
 */
#pragma once

#include "synth_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const synth_engine_t g_engine_modular;

#ifdef __cplusplus
}
#endif
