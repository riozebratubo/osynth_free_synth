/*
 * osynth host port — esp_log.h
 *
 * ESP_LOGE/W/I/D reach ~465 call sites across the firmware, which is why the
 * host port shims the header rather than editing them out. Output goes to
 * stderr in IDF's shape ("W (ms) tag: message") so a host log and a serial log
 * read the same way when you are comparing the two builds side by side.
 *
 * Per-tag levels exist because loop_store.cpp raises the sdmmc tags to DEBUG
 * around its mount attempts and puts them back afterwards; that code compiles
 * on the host (the SD backend is the file-backed one we keep), so the calls
 * have to resolve and behave. The table is a linear scan over a handful of
 * entries, touched only from init paths.
 *
 * Nothing here is safe to call from the render chain, and nothing in the
 * render chain does — the firmware already routes audio-task diagnostics
 * through counters in audio_io_stats_t for the same reason.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_LOG_NONE = 0,
    ESP_LOG_ERROR = 1,
    ESP_LOG_WARN = 2,
    ESP_LOG_INFO = 3,
    ESP_LOG_DEBUG = 4,
    ESP_LOG_VERBOSE = 5,
} esp_log_level_t;

void esp_log_level_set(const char* tag, esp_log_level_t level);
esp_log_level_t esp_log_level_get(const char* tag);

/* The one printing entry point; the macros below are sugar over it. */
void esp_log_write_host(esp_log_level_t level, const char* tag,
                        const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/* The format string is left inside __VA_ARGS__ rather than named as its own
 * parameter. That is what keeps these portable across all four preprocessors:
 * naming it would make `ESP_LOGI(TAG, "text")` expand with a trailing comma
 * and need either GNU's `##__VA_ARGS__` or C++20's __VA_OPT__ to remove it --
 * the first is an extension MSVC handles inconsistently, the second needs
 * /Zc:preprocessor. This way there is nothing to remove. Every call site in
 * the tree passes a format, so the one vararg is always there. */
#define ESP_LOGE(tag, ...) esp_log_write_host(ESP_LOG_ERROR,   tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) esp_log_write_host(ESP_LOG_WARN,    tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) esp_log_write_host(ESP_LOG_INFO,    tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) esp_log_write_host(ESP_LOG_DEBUG,   tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) esp_log_write_host(ESP_LOG_VERBOSE, tag, __VA_ARGS__)

#ifdef __cplusplus
}
#endif
