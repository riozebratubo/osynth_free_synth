/*
 * osynth — local UI (LCD + encoders + buttons), behind
 * CONFIG_OSYNTH_ENABLE_LOCAL_UI (off by default).
 *
 * Interface fixed in Session 14; hardware bring-up is future work. A local
 * control surface needs no API of its own: it reads/writes the ParamStore
 * with ParamOrigin::LocalUi (echo suppression works exactly as it does for
 * BLE — register a listener, ignore your own origin), discovers parameters
 * via describe()/listIds() like the BLE PARAM_INFO path, gates pages on the
 * active engine's caps mask (engines_active_type()/engines_get()) and
 * drives presets through the S13 request/slot-info API in presets.h.
 * Enabling it later is purely additive — no other component changes.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t local_ui_init(void);

#ifdef __cplusplus
}
#endif
