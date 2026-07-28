/*
 * osynth — USB composite device (ESP32-S3 only): TinyUSB UAC2 audio source
 * (48 kHz / 16-bit / stereo, synth -> host) + MIDI interface. Session 3.
 * On the classic ESP32 (no USB-OTG hardware) everything compiles to no-ops.
 *
 * Call usb_dev_init() before audio_io_start(): the USB audio sink pushes
 * blocks through usb_dev_audio_write().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the USB PHY, the TinyUSB stack and its task (core 0). */
esp_err_t usb_dev_init(void);

/* Device is enumerated by a host. */
bool usb_dev_mounted(void);

/* Host has the audio streaming interface open (capture running). */
bool usb_dev_audio_streaming(void);

/* Pushes `frames` interleaved L/R int16 frames into the USB audio FIFO,
 * waiting up to `max_wait_ms` for space. Returns the frames accepted:
 * all of them, or 0 (not streaming / timeout — whole blocks only).
 * Called from the audio task via the "usb" sink. */
size_t usb_dev_audio_write(const int16_t* interleaved, size_t frames,
                           uint32_t max_wait_ms);

/* Incoming MIDI, one 4-byte USB-MIDI event packet per call, invoked from the
 * USB task (core 0). While no callback is registered (until the Session-4
 * parser), packets are logged and discarded. */
typedef void (*usb_dev_midi_rx_fn)(const uint8_t packet[4], void* ctx);
void usb_dev_midi_set_rx_callback(usb_dev_midi_rx_fn fn, void* ctx);

#ifdef __cplusplus
}
#endif
