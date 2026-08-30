/*
 * osynth host port — nvs.h
 *
 * NVS is the firmware's settings store: a small key/value area with wear
 * levelling and atomic commits, deliberately kept off the LittleFS partition
 * so a corrupt filesystem cannot take the master volume with it
 * (components/persist/persist.cpp explains the reasoning at length).
 *
 * On a host it is one file per namespace under the data root, rewritten
 * whole on commit through a temporary file and a rename. That is the same
 * guarantee NVS offers and the one persist.cpp depends on: a commit either
 * lands completely or not at all, so a settings file is never half-written.
 *
 * Only the six calls persist.cpp makes are here. The real NVS has typed
 * getters, iterators and statistics; none of them is used, and shimming them
 * would be inventing a contract nothing tests.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nvs_handle_impl* nvs_handle_t;

typedef enum {
    NVS_READONLY = 0,
    NVS_READWRITE = 1,
} nvs_open_mode_t;

/* Opens (and creates, in READWRITE) a namespace. */
esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* out);

/* IDF semantics, which persist.cpp relies on: `len` is in/out. With `out`
 * non-NULL it is the buffer size going in and the actual size coming out;
 * ESP_ERR_NVS_NOT_FOUND if the key is absent. */
esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out,
                       size_t* len);

esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value,
                       size_t len);

esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key);

/* Writes the namespace to disk. Until this is called, set/erase live only in
 * the handle -- matching NVS, where a commit is what makes a write durable. */
esp_err_t nvs_commit(nvs_handle_t handle);

void nvs_close(nvs_handle_t handle);

#ifdef __cplusplus
}
#endif
