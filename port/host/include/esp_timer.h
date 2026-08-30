/*
 * osynth host port — esp_timer.h
 *
 * Two things live behind this header, and only one of them is a timer.
 *
 * esp_timer_get_time() is the monotonic microsecond clock eleven call sites
 * use for deadlines and rate limits, including audio_io_quiet_ms() — the one
 * persist.cpp and presets.cpp wait on before writing storage.
 *
 * The periodic timer has exactly one user: seqarp.cpp's 96 PPQN clock
 * (seqarp.cpp:1151), which is the heartbeat of the arpeggiator, the sequencer,
 * the looper's bar sync and both count-ins. It is created once, started when
 * the transport runs, and restarted with a new period whenever the tempo
 * changes.
 *
 * skip_unhandled_events is not decoration. seqarp asks for it with the comment
 * "after a stall: no tick avalanche", and the host implementation honours it
 * the same way IDF does: if the thread wakes late, the next deadline is
 * computed from now rather than from the missed one, so a scheduling hiccup
 * costs a late tick instead of a burst of them. On a non-realtime host that
 * matters more than it does on the ESP32, not less.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_timer* esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void* arg);

typedef enum {
    ESP_TIMER_TASK = 0,
    ESP_TIMER_ISR = 1,
} esp_timer_dispatch_t;

typedef struct {
    esp_timer_cb_t callback;
    void* arg;
    esp_timer_dispatch_t dispatch_method;
    const char* name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;

/* Microseconds since process start, monotonic. */
int64_t esp_timer_get_time(void);

esp_err_t esp_timer_create(const esp_timer_create_args_t* args,
                           esp_timer_handle_t* out);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
/* Changes the period of a running timer; ESP_ERR_INVALID_STATE if stopped. */
esp_err_t esp_timer_restart(esp_timer_handle_t timer, uint64_t period_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
esp_err_t esp_timer_delete(esp_timer_handle_t timer);

#ifdef __cplusplus
}
#endif
