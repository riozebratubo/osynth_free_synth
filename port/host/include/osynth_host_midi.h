/*
 * osynth host port — MIDI input from the host's own ports.
 *
 * Separate from osynth_host.h, and started separately, because it is optional
 * in a way the engine is not: an app may want the synth without opening every
 * MIDI device on the machine, and on Android and iOS there is no backend to
 * open at all. See midi_in_host.cpp for what it does with the ports it finds.
 */
#pragma once

#include "esp_err.h"

/* Whether this platform has a MIDI backend compiled in. Set by the build
 * (port/host/CMakeLists.txt) from what RtMidi supports: Windows, macOS and
 * Linux yes; Android and iOS no, where the answer is USB host or BLE MIDI
 * rather than a missing flag. */
#ifndef OSYNTH_HOST_MIDI_IN
#define OSYNTH_HOST_MIDI_IN 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Opens every MIDI input port and merges them into the firmware's router.
 *
 * Returns ESP_OK when there is nothing to open as readily as when there is:
 * no ports, no MIDI stack, and a platform with no backend are all ordinary
 * states, not failures -- the synth still plays from the app's keyboard.
 * ESP_ERR_NOT_SUPPORTED only where the backend is absent at compile time. */
esp_err_t osynth_host_midi_in_start(void);

/* Closes them. Safe if start was never called. */
void osynth_host_midi_in_stop(void);

/* How many ports are open. 0 is normal. */
int osynth_host_midi_in_ports(void);

#ifdef __cplusplus
}
#endif
