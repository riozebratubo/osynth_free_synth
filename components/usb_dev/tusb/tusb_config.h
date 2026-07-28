/*
 * osynth — TinyUSB stack configuration (ESP32-S3, USB-OTG full speed).
 *
 * Composite device: UAC2 audio source (synth -> host, one function, one
 * format) + USB-MIDI. This directory is injected into the espressif__tinyusb
 * component's include path (usb_dev/CMakeLists.txt), so the TinyUSB class
 * drivers are compiled with exactly this configuration.
 */
#pragma once

#include "sdkconfig.h"
#include "usb_descriptors.h" /* format constants + descriptor lengths */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Board / port ---- */
/* ESP32-S3 USB-OTG is full speed only. */
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

/* ---- Common ---- */
#ifndef CFG_TUSB_MCU
#error "CFG_TUSB_MCU must be defined (espressif__tinyusb sets it per target)"
#endif

#define CFG_TUSB_OS             OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH    freertos/

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG          0
#endif

#ifndef ESP_PLATFORM
#define ESP_PLATFORM 1
#endif

/* Keep TinyUSB buffers in internal RAM: the USB DMA cannot reach PSRAM. */
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN      __attribute__ ((aligned(4)))

/* ---- Device ---- */
#define CFG_TUD_ENABLED         1
#define CFG_TUD_ENDPOINT0_SIZE  64

/* ---- Enabled classes ---- */
#define CFG_TUD_AUDIO           1
#define CFG_TUD_MIDI            1

/* ---- MIDI ---- */
/* Stream FIFO sizes: midi_device.c has no defaults for these (only
 * CFG_TUD_MIDI_EP_BUFSIZE gets one), and TinyUSB 0.19 spells them
 * ..._BUFSIZE, not ..._BUFSZ. */
#define CFG_TUD_MIDI_RX_BUFSIZE 128
#define CFG_TUD_MIDI_TX_BUFSIZE 64

/* ---- UAC2: one audio function, device-to-host (EP IN) only ---- */
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN                       OSYNTH_UAC2_DESC_LEN
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT                       1
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ                    64
#define CFG_TUD_AUDIO_FUNC_1_N_FORMATS                      1

#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE                OSYNTH_USB_SAMPLE_RATE
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX                  OSYNTH_USB_CHANNELS
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX OSYNTH_USB_BYTES_PER_SAMPLE
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_TX         OSYNTH_USB_BITS_PER_SAMPLE
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_FRAME_SZ_TX \
    (OSYNTH_USB_CHANNELS * OSYNTH_USB_BYTES_PER_SAMPLE)

#define CFG_TUD_AUDIO_ENABLE_EP_IN                          1
/* Iso EP size: one millisecond of audio plus one frame of headroom (same
 * formula as the usb_device_uac reference). */
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_IN \
    (OSYNTH_USB_BYTES_PER_MS + CFG_TUD_AUDIO_FUNC_1_FORMAT_1_FRAME_SZ_TX)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX   CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_IN
/* Software FIFO the audio task writes into (usb_dev_audio_write); the class
 * driver drains it once per iso frame. */
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ \
    (OSYNTH_USB_BYTES_PER_MS * OSYNTH_USB_FIFO_MS)

#ifdef __cplusplus
}
#endif
