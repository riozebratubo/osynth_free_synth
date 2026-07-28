/*
 * osynth — audio sink interface (private to the audio_io component).
 *
 * A sink consumes interleaved int16 stereo blocks. write() blocks until the
 * sink has accepted the whole block — the sink's DMA (or esp_timer, for the
 * null sink) is the real-time clock that paces the audio task. Exactly one
 * sink is active; audio_io_start() picks it from the build configuration.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "synth_config.h"

struct audio_sink_t {
    const char* name;
    esp_err_t (*start)(void);
    /* Blocking write of `frames` interleaved L/R int16 frames. */
    esp_err_t (*write)(const int16_t* interleaved, size_t frames);
};

/* Timer-paced, no hardware output; the fallback when another sink fails to
 * start. */
const audio_sink_t* audio_sink_null(void);

#if SYNTH_ENABLE_USB
/* USB (UAC2) bridge to usb_dev (ESP32-S3). Host iso polling paces the audio
 * task while the capture stream is open; timer pacing (as in the null sink)
 * otherwise. */
const audio_sink_t* audio_sink_usb(void);
#endif

#if SYNTH_ENABLE_I2S_DAC
/* External I2S DAC (e.g. PCM5102A). Pins per target: docs/HARDWARE.md. */
const audio_sink_t* audio_sink_i2s(void);
#endif

#if SYNTH_HAS_INTERNAL_DAC
/* Classic-ESP32 internal 8-bit DAC on GPIO25 (L) / GPIO26 (R). */
const audio_sink_t* audio_sink_dac(void);
#endif
