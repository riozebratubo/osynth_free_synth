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

/* ---- Chip capabilities (fixed by hardware) ----
 *
 * SYNTH_HAS_I2S_MCLK is "the I2S port can carry a system clock out and run
 * full duplex", which is what the line input and the ES8388 both need. It is
 * a capability and not a target test because the two chips that have it are
 * unrelated (Xtensa S3, RISC-V P4) and the one that does not is specific:
 * on the classic ESP32 MCLK cannot leave through the GPIO matrix (it goes via
 * esp_clock_output, restricted to the CLK_OUT/strapping pins) and a duplex
 * configuration mismatch is a hard error rather than a silent demotion.
 *
 * SYNTH_HAS_RADIO is "the die contains a Bluetooth controller". The P4 has
 * none, so its BLE runs the NimBLE *host* here and reaches a controller on a
 * companion chip — see SYNTH_BLE_VIA_HOSTED below. */
#if CONFIG_IDF_TARGET_ESP32S3
#define SYNTH_HAS_USB          1  /* native USB-OTG: UAC2 audio + MIDI composite */
#define SYNTH_HAS_INTERNAL_DAC 0
#define SYNTH_HAS_I2S_MCLK     1
#define SYNTH_HAS_RADIO        1
#elif CONFIG_IDF_TARGET_ESP32P4
#define SYNTH_HAS_USB          1  /* USB-OTG port 0 (full speed), same DWC2 core */
#define SYNTH_HAS_INTERNAL_DAC 0
#define SYNTH_HAS_I2S_MCLK     1
#define SYNTH_HAS_RADIO        0  /* no on-die radio: BLE comes from a companion */
#elif CONFIG_IDF_TARGET_ESP32
#define SYNTH_HAS_USB          0  /* no USB-OTG peripheral on classic ESP32 */
#define SYNTH_HAS_INTERNAL_DAC 1  /* 8-bit DAC on GPIO25/26 */
#define SYNTH_HAS_I2S_MCLK     0
#define SYNTH_HAS_RADIO        1
#else
#error "osynth supports the esp32, esp32s3 and esp32p4 targets only"
#endif

/* ---- Feature switches ---- */
#define SYNTH_ENABLE_USB SYNTH_HAS_USB

#ifdef CONFIG_OSYNTH_ENABLE_BLE
#define SYNTH_ENABLE_BLE 1
#else
#define SYNTH_ENABLE_BLE 0
#endif

/* BLE whose controller is on another chip. The NimBLE host is the same code
 * either way — ble_ctrl.cpp only ever calls host APIs — but with no on-die
 * controller IDF compiles the host with *no* HCI transport
 * (CONFIG_BT_CONTROLLER_DISABLED, see sdkconfig.defaults.esp32p4), so the
 * link has to be brought up before nimble_port_init() rather than after. */
#define SYNTH_BLE_VIA_HOSTED (SYNTH_ENABLE_BLE && !SYNTH_HAS_RADIO)

#ifdef CONFIG_OSYNTH_ENABLE_I2S_DAC
#define SYNTH_ENABLE_I2S_DAC 1
#else
#define SYNTH_ENABLE_I2S_DAC 0
#endif

/* Fan-out: the I2S DAC is the output *and* the USB interface still streams.
 * The DAC owns the clock; the USB copy is best-effort. Kconfig already makes
 * this depend on the I2S DAC and a USB-capable target, but the gate is repeated
 * here so the flag is false on any target without USB-OTG rather than relying
 * on a Kconfig dependency staying correct. */
#if defined(CONFIG_OSYNTH_USB_AUDIO_TAP) && SYNTH_ENABLE_USB && SYNTH_ENABLE_I2S_DAC
#define SYNTH_ENABLE_USB_TAP 1
#else
#define SYNTH_ENABLE_USB_TAP 0
#endif

/* Stereo line input (S31): an I2S ADC in slave mode on the DAC's own port,
 * so capture and playback share one clock. The capability gate is repeated
 * here for the same reason the USB tap's is — the classic-ESP32 exclusion is
 * real hardware, not defensiveness (see SYNTH_HAS_I2S_MCLK above). */
#if defined(CONFIG_OSYNTH_ENABLE_I2S_LINE_IN) && SYNTH_ENABLE_I2S_DAC && \
    SYNTH_HAS_I2S_MCLK
#define SYNTH_ENABLE_LINE_IN 1
#else
#define SYNTH_ENABLE_LINE_IN 0
#endif

/* ES8388 codec (S31b) instead of discrete converters: one chip for both
 * directions, configured over I2C because it has no strapping pins that
 * would do it. Same capability-gate idiom as above. */
#if defined(CONFIG_OSYNTH_FRONTEND_ES8388) && SYNTH_ENABLE_I2S_DAC && \
    SYNTH_HAS_I2S_MCLK
#define SYNTH_ENABLE_CODEC_ES8388 1
#else
#define SYNTH_ENABLE_CODEC_ES8388 0
#endif

/* Whether the I2S port carries MCLK and runs 32-bit slots (BCLK 64x fs)
 * rather than the bare 16-bit / 32x fs arrangement a PCM5102A is happy with.
 *
 * It is one flag rather than two because the two reasons for it are the same
 * reason: anything on that bus other than a PCM5102A needs a system clock —
 * the PCM1808 to convert at all, the ES8388 in either direction. Deriving it
 * here keeps sink_i2s.cpp from having to know *which* of those is present,
 * which is the only thing it does not care about. */
#define SYNTH_I2S_MCLK_MODE (SYNTH_ENABLE_LINE_IN || SYNTH_ENABLE_CODEC_ES8388)

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
