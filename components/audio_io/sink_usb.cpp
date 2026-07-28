/*
 * osynth — USB (UAC2) sink: bridges the audio task to usb_dev (ESP32-S3).
 *
 * While the host has the capture stream open, the blocking FIFO write is
 * paced by the host's 1 ms iso IN polling — the USB bus is the audio clock.
 * When the host is not streaming, blocks are dropped and pacing falls back
 * to the esp_timer deadline scheme of the null sink, so stats stay honest
 * and the transition into/out of streaming is seamless.
 */
#include "audio_sink.h"

#if SYNTH_ENABLE_USB

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "usb_dev.h"

namespace {

/* Cap the FIFO wait well above one block time (1.33 ms) but low enough that
 * a stream teardown mid-write cannot wedge the audio task. */
constexpr uint32_t kWriteWaitMs = 20;

int64_t s_deadline_us = 0;

esp_err_t usb_start(void) {
    s_deadline_us = 0;
    /* usb_dev_init() runs before audio_io_start() (see main.cpp); if it
     * failed, writes simply fall through to timer pacing. */
    return ESP_OK;
}

esp_err_t usb_write(const int16_t* interleaved, size_t frames) {
    if (usb_dev_audio_streaming()) {
        if (usb_dev_audio_write(interleaved, frames, kWriteWaitMs) == frames) {
            s_deadline_us = 0; /* resync timer pacing when the stream stops */
            return ESP_OK;
        }
        /* Stream vanished mid-write: pace this block from the timer. */
    }

    /* Same deadline-accumulator pacing as the null sink. */
    const int64_t block_us = (int64_t)frames * 1000000 / SYNTH_SAMPLE_RATE;
    const int64_t tick_us = (int64_t)portTICK_PERIOD_MS * 1000;
    const int64_t now = esp_timer_get_time();

    if (s_deadline_us == 0) s_deadline_us = now;
    s_deadline_us += block_us;

    const int64_t ahead_us = s_deadline_us - now;
    if (ahead_us >= tick_us) {
        vTaskDelay((TickType_t)(ahead_us / tick_us));
    } else if (ahead_us < -20 * block_us) {
        s_deadline_us = esp_timer_get_time();
    }
    return ESP_OK;
}

const audio_sink_t s_sink = {"usb", usb_start, usb_write};

} // namespace

const audio_sink_t* audio_sink_usb(void) { return &s_sink; }

#endif /* SYNTH_ENABLE_USB */
