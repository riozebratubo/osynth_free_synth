/*
 * osynth host port — nvs_flash.h
 *
 * main.cpp calls nvs_flash_init() before anything else and erases on
 * ESP_ERR_NVS_NO_FREE_PAGES. The host entry point does not run that sequence
 * (there is no flash to initialise, and the store creates its files on demand),
 * but the header exists so that a file including it still compiles, and the
 * functions do the sensible host thing rather than nothing surprising.
 */
#pragma once

#include "esp_err.h"
#include "nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Nothing to bring up: namespaces are files, created when first written. */
esp_err_t nvs_flash_init(void);

/* Deletes every namespace file under the data root. Real, not a stub: the
 * firmware calls this when NVS is unusable, and a host build asked to do the
 * same should genuinely clear the settings rather than pretend. */
esp_err_t nvs_flash_erase(void);

#ifdef __cplusplus
}
#endif
