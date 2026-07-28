/*
 * osynth — BLE control (Session 14): NimBLE GATT service + the "SynthCtl v1"
 * binary frame protocol for the companion mobile app. Spec (final v1 frame
 * layouts, UUIDs, opcodes): docs/BLE_PROTOCOL.md.
 *
 * The service exposes the whole ParamStore (get/set/discover, origin Ble),
 * the S13 preset system (list/load/save via presets_request_*), transport/
 * arp shortcuts and test notes routed through the MIDI router. Non-BLE
 * parameter changes (MIDI, presets, internal) are pushed to the app as
 * coalesced EVT notifications — writes with origin Ble are suppressed, so
 * the app never echoes.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts NimBLE, registers the GATT service and begins advertising as
 * "osynth". A controller/host bring-up failure (e.g. no RAM on a loaded
 * classic ESP32) logs a warning and returns ESP_OK — the synth keeps
 * running without BLE (the sink-fallback philosophy). */
esp_err_t ble_ctrl_init(void);

/* Current link state for the heartbeat: "off" (feature disabled),
 * "unavailable" (bring-up failed), "advertising" or "connected".
 * Safe from any task. */
const char* ble_ctrl_state_name(void);

#ifdef __cplusplus
}
#endif
