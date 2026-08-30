/*
 * osynth host port — esp_system.h
 *
 * Only esp_restart(), reached from the protocol's REBOOT opcode. See the
 * definition in esp_sys_host.cpp for why it logs and returns rather than doing
 * anything: there is no firmware to restart when the engine is a library
 * inside the app.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void esp_restart(void);

#ifdef __cplusplus
}
#endif
