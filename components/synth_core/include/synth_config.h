/*
 * osynth — central feature-flag and capability configuration.
 *
 * Chip capabilities are derived from the build target (capability gating):
 * the classic ESP32 has no USB-OTG peripheral, so all USB-device code
 * compiles out on that target. User-facing feature switches come from
 * Kconfig ("osynth Synthesizer" menu).
 */
#pragma once

#include "esp_attr.h"
#include "sdkconfig.h"

/* ---- Chip capabilities (fixed by hardware) ---- */
#if CONFIG_IDF_TARGET_ESP32S3
#define SYNTH_HAS_USB          1  /* native USB-OTG: UAC2 audio + MIDI composite */
#define SYNTH_HAS_INTERNAL_DAC 0
#elif CONFIG_IDF_TARGET_ESP32
#define SYNTH_HAS_USB          0  /* no USB-OTG peripheral on classic ESP32 */
#define SYNTH_HAS_INTERNAL_DAC 1  /* 8-bit DAC on GPIO25/26 */
#else
#error "osynth supports the esp32 and esp32s3 targets only"
#endif

/* ---- Feature switches ---- */
#define SYNTH_ENABLE_USB SYNTH_HAS_USB

#ifdef CONFIG_OSYNTH_ENABLE_BLE
#define SYNTH_ENABLE_BLE 1
#else
#define SYNTH_ENABLE_BLE 0
#endif

#ifdef CONFIG_OSYNTH_ENABLE_I2S_DAC
#define SYNTH_ENABLE_I2S_DAC 1
#else
#define SYNTH_ENABLE_I2S_DAC 0
#endif

/* Fan-out: the I2S DAC is the output *and* the USB interface still streams.
 * The DAC owns the clock; the USB copy is best-effort. Kconfig already makes
 * this depend on the I2S DAC and the S3, but the capability gate is repeated
 * here so the flag is false on any target without USB-OTG rather than relying
 * on a Kconfig dependency staying correct. */
#if defined(CONFIG_OSYNTH_USB_AUDIO_TAP) && SYNTH_ENABLE_USB && SYNTH_ENABLE_I2S_DAC
#define SYNTH_ENABLE_USB_TAP 1
#else
#define SYNTH_ENABLE_USB_TAP 0
#endif

#ifdef CONFIG_OSYNTH_ENABLE_LOCAL_UI
#define SYNTH_ENABLE_LOCAL_UI 1
#else
#define SYNTH_ENABLE_LOCAL_UI 0
#endif

#ifdef CONFIG_OSYNTH_ENABLE_SERIAL_MIDI
#define SYNTH_ENABLE_SERIAL_MIDI 1
#else
#define SYNTH_ENABLE_SERIAL_MIDI 0
#endif

/* Modular patch graph (S28): a fifth engine whose signal path is a node
 * graph the app patches, rather than a fixed chain. Costs an internal-RAM
 * buffer pool and IRAM for its kernels — see the Kconfig help. */
#ifdef CONFIG_OSYNTH_ENABLE_MODULAR
#define SYNTH_ENABLE_MODULAR 1
#else
#define SYNTH_ENABLE_MODULAR 0
#endif

/* Looper persistence (S16): save/load the S15 track set to flash or SD.
 * Gated on the S3 + PSRAM like the looper itself. */
#ifdef CONFIG_OSYNTH_LOOP_PERSIST
#define SYNTH_ENABLE_LOOP_PERSIST 1
#else
#define SYNTH_ENABLE_LOOP_PERSIST 0
#endif

#ifdef CONFIG_OSYNTH_LOOP_STORE_SD
#define SYNTH_LOOP_STORE_SD 1
#else
#define SYNTH_LOOP_STORE_SD 0 /* flash-region backend when persist is on */
#endif

/* ---- Audio engine constants ---- */
#define SYNTH_SAMPLE_RATE CONFIG_OSYNTH_SAMPLE_RATE
#define SYNTH_BLOCK_SIZE  CONFIG_OSYNTH_BLOCK_SIZE
/* Heartbeat log period; 0 silences it (also a click-diagnosis knob — see
 * the Kconfig help text). */
#define SYNTH_HEARTBEAT_MS CONFIG_OSYNTH_HEARTBEAT_MS
#define SYNTH_VOICES      CONFIG_OSYNTH_VOICES

/* ---- Render-path placement and branch hints (S17) ----
 *
 * SYNTH_RENDER_IRAM marks the per-block render path (engine render,
 * voice manager, FX bus, looper, audio-task conversion) for IRAM so a
 * flash-cache miss — BLE/Wi-Fi activity, LittleFS traffic — can't add
 * jitter to the audio deadline. It is not full flash-write immunity:
 * newlib math and driver calls stay in flash. If the image stops linking
 * ("region iram0 overflowed"), turn OSYNTH_RENDER_IN_IRAM off. */
#ifdef CONFIG_OSYNTH_RENDER_IN_IRAM
#define SYNTH_RENDER_IRAM IRAM_ATTR
#else
#define SYNTH_RENDER_IRAM
#endif

/* Branch-predictor hints for the render path: wrap conditions that are
 * almost never (or almost always) true inside per-sample loops. */
#ifndef SYNTH_LIKELY
#define SYNTH_LIKELY(x) __builtin_expect(!!(x), 1)
#define SYNTH_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif
