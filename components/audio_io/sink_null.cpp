/*
 * osynth — null sink: no hardware output, real-time pacing from esp_timer.
 *
 * Keeps block/underrun/load accounting meaningful when no physical sink is
 * configured (ESP32-S3 before the USB sink lands in Session 3) and serves as
 * the fallback when a hardware sink fails to start.
 */
#include "audio_sink.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

int64_t s_deadline_us = 0;

esp_err_t null_start(void) {
    s_deadline_us = 0;
    return ESP_OK;
}

esp_err_t null_write(const int16_t* /*interleaved*/, size_t frames) {
    const int64_t block_us = (int64_t)frames * 1000000 / SYNTH_SAMPLE_RATE;
    const int64_t tick_us = (int64_t)portTICK_PERIOD_MS * 1000;
    const int64_t now = esp_timer_get_time();

    if (s_deadline_us == 0) s_deadline_us = now;
    s_deadline_us += block_us;

    const int64_t ahead_us = s_deadline_us - now;
    if (ahead_us >= tick_us) {
        /* Sleep off the accumulated lead in whole ticks; the fractional
         * remainder carries over in s_deadline_us, so the average rate is
         * exact even when the block time is shorter than one tick. */
        vTaskDelay((TickType_t)(ahead_us / tick_us));
    } else if (ahead_us < -20 * block_us) {
        /* Fell far behind (e.g. a long stall elsewhere): resync instead of
         * spinning through a burst of catch-up blocks. */
        s_deadline_us = esp_timer_get_time();
    }
    return ESP_OK;
}

const audio_sink_t s_sink = {"null", null_start, null_write};

} // namespace

const audio_sink_t* audio_sink_null(void) { return &s_sink; }
