/*
 * osynth — TinyUSB stack configuration (USB-OTG, full speed).
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
/* The DWC2 port table puts OTG_FS at index 0 and OTG_HS at index 1
 * (dwc2_esp32.h). The S3 has only OTG_FS, so it stays on port 0. The P4 has
 * both, and which one is usable is a property of the board rather than the
 * chip — see OSYNTH_USB_HS in usb_descriptors.h.
 *
 * A 48 kHz stereo stream does not need high speed; the P4 uses it because that
 * is the controller wired to the socket. The cost is smaller than it looks:
 * only defining RHPORT1 here makes tusb_option.h derive TUD_OPT_RHPORT = 1 and
 * CFG_TUD_MAX_SPEED = high speed on its own, the iso interval stays 1 ms (it is
 * just spelled 4 instead of 1), and the bulk MIDI endpoints grow to 512 bytes.
 * What is genuinely lost is byte-identical descriptors across the two targets. */
#if OSYNTH_USB_HS
#define CFG_TUSB_RHPORT1_MODE   (OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED)
#else
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#endif

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

/* Keep TinyUSB buffers in internal RAM: the USB DMA cannot reach PSRAM.
 *
 * 4-byte alignment is enough because DWC2 DMA stays off — CFG_TUD_DWC2_DMA_ENABLE
 * defaults to 0 (tusb_option.h), so the driver copies through the FIFOs and no
 * buffer is ever handed to a bus master. If that is ever turned on for the P4,
 * this has to become cache-line alignment and CFG_TUD_MEM_DCACHE_LINE_SIZE has
 * to equal CONFIG_CACHE_L1_CACHE_LINE_SIZE (64), which dwc2_esp32.h enforces
 * with an #error. */
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
 * ..._BUFSIZE, not ..._BUFSZ.
 *
 * **The RX FIFO must stay larger than the endpoint buffer.** midi_device.c
 * refuses to arm the OUT endpoint unless tu_fifo_remaining(rx_ff) is at least
 * CFG_TUD_MIDI_EP_BUFSIZE, and that endpoint buffer defaults to
 * (TUD_OPT_HIGH_SPEED ? 512 : 64) — so switching the P4 to high speed
 * multiplies the requirement eightfold. Left at the full-speed 128 the
 * condition can never hold, and incoming MIDI is not merely slow but never
 * scheduled at all: no error, no log, just a MIDI IN port that is silent
 * forever. Sized to twice the endpoint buffer so a partly-drained FIFO still
 * clears the bar rather than stalling until the app happens to empty it. */
#if OSYNTH_USB_HS
#define CFG_TUD_MIDI_RX_BUFSIZE 1024
#define CFG_TUD_MIDI_TX_BUFSIZE 512
#else
#define CFG_TUD_MIDI_RX_BUFSIZE 128
#define CFG_TUD_MIDI_TX_BUFSIZE 64
#endif

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
