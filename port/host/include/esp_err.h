/*
 * osynth host port — esp_err.h
 *
 * Every public firmware header returns esp_err_t, so this is the first thing
 * the host build needs. The numeric values match ESP-IDF's so that a log line
 * or a protocol status byte reads the same on both builds — ble_ctrl's
 * status_from() maps these to ST_* codes the app already knows, and having
 * them drift would make one build's error mean another's on the wire.
 *
 * Only the codes the firmware actually raises are here. The list came from
 * grepping ESP_ERR_* across components/; adding one is a line, and leaving out
 * the two hundred IDF defines nobody uses keeps this readable.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;

#define ESP_OK   0
#define ESP_FAIL -1

#define ESP_ERR_NO_MEM            0x101
#define ESP_ERR_INVALID_ARG       0x102
#define ESP_ERR_INVALID_STATE     0x103
#define ESP_ERR_INVALID_SIZE      0x104
#define ESP_ERR_NOT_FOUND         0x105
#define ESP_ERR_NOT_SUPPORTED     0x106
#define ESP_ERR_TIMEOUT           0x107
#define ESP_ERR_INVALID_RESPONSE  0x108
#define ESP_ERR_INVALID_CRC       0x109

/* NVS lives behind persist_host.cpp here, but presets.cpp and persist.cpp both
 * test for this specific code, so it keeps its IDF value. */
#define ESP_ERR_NVS_NOT_FOUND     0x1102

/* Never returns NULL: callers pass the result straight to a %s. */
const char* esp_err_to_name(esp_err_t code);

/* Aborts, as IDF's does. Only reached from init paths (7 sites), never from
 * the render chain. */
void esp_error_check_failed(esp_err_t rc, const char* file, int line,
                            const char* expr);

#define ESP_ERROR_CHECK(x)                                             \
    do {                                                               \
        const esp_err_t esp_err_rc_ = (x);                             \
        if (esp_err_rc_ != ESP_OK) {                                   \
            esp_error_check_failed(esp_err_rc_, __FILE__, __LINE__, #x);\
        }                                                              \
    } while (0)

#ifdef __cplusplus
}
#endif
