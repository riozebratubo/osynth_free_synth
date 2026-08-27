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

/* USB host mode (S35): the OTG port drives a MIDI controller instead of
 * enumerating on a computer. One controller, one port — the two roles cannot
 * share it, so which one comes up is decided at boot from the persisted
 * `usb.mode` parameter (main.cpp) and changing it costs a restart.
 *
 * SYNTH_USB_IS_AUDIO_CLOCK is the reason host mode is not offered everywhere.
 * audio_io.cpp picks audio_sink_usb() exactly when there is USB but no I2S
 * DAC, and that sink's blocking write *is* the audio clock (sink_usb.cpp:
 * "USB as the output, and therefore as the clock"). Taking the device role
 * away on such a build leaves the render loop on the timer-paced null sink
 * with nowhere to send audio: alive, and silent. So there host mode is
 * refused rather than offered and regretted — usb_mode_resolve() clamps to
 * device, and the app greys the control out on the same flag. */
#define SYNTH_USB_IS_AUDIO_CLOCK (SYNTH_ENABLE_USB && !SYNTH_ENABLE_I2S_DAC)
#define SYNTH_ENABLE_USB_HOST (SYNTH_ENABLE_USB && !SYNTH_USB_IS_AUDIO_CLOCK)

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

/* Microphone input (S37): a digital MEMS mic on the *second* I2S controller.
 *
 * A separate gate from the line input rather than a mode of it, because it is
 * separate hardware on a separate port and the two are independently useful —
 * a build may have either, both, or neither. Same capability gate as the line
 * input for the same reason: the classic ESP32 cannot put MCLK through the
 * GPIO matrix, and without the main port in MCLK mode there is no shared
 * clock to slave to. */
#if defined(CONFIG_OSYNTH_ENABLE_MIC_IN) && SYNTH_ENABLE_I2S_DAC && \
    SYNTH_HAS_I2S_MCLK
#define SYNTH_ENABLE_MIC_IN 1
#else
#define SYNTH_ENABLE_MIC_IN 0
#endif

/* Whether the render chain has an audio *input* at all, whichever device
 * provides it.
 *
 * The distinction this draws is the one that kept the mic from being a second
 * copy of the line input: everything from the capture buffer downwards — the
 * three mix stages, the route smoothers, the meters, `in.route` and `in.gain`
 * — belongs to "there is an input", while the port, the pins and the slot
 * width belong to a particular device. Code that only cares that a block
 * arrived tests this; code that talks to hardware tests the two above. */
#define SYNTH_ENABLE_AUDIO_IN (SYNTH_ENABLE_LINE_IN || SYNTH_ENABLE_MIC_IN)

/* Registered only where there are genuinely two devices to choose between. A
 * single-input build would otherwise expose a selector with one position,
 * which the app would draw and no one could use. */
#define SYNTH_ENABLE_IN_SOURCE_SEL (SYNTH_ENABLE_LINE_IN && SYNTH_ENABLE_MIC_IN)

/* ES8388 codec (S31b) instead of discrete converters: one chip for both
 * directions, configured over I2C because it has no strapping pins that
 * would do it. Same capability-gate idiom as above. */
#if defined(CONFIG_OSYNTH_FRONTEND_ES8388) && SYNTH_ENABLE_I2S_DAC && \
    SYNTH_HAS_I2S_MCLK
#define SYNTH_ENABLE_CODEC_ES8388 1
#else
#define SYNTH_ENABLE_CODEC_ES8388 0
#endif

/* ES8311 on the microphone port (S37c): the analogue front end for a board's
 * on-board mic, ADC only. Needs the mic port for its clocks and the ES8388's
 * I2C bus for its registers — see codec_priv.h for why the bus is borrowed
 * rather than opened twice, and codec_es8311.cpp for why an analogue mic
 * leaves no alternative. */
#if defined(CONFIG_OSYNTH_MIC_CODEC_ES8311) && SYNTH_ENABLE_MIC_IN && \
    SYNTH_ENABLE_CODEC_ES8388
#define SYNTH_ENABLE_CODEC_ES8311 1
#else
#define SYNTH_ENABLE_CODEC_ES8311 0
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

/* Bits on the wire per slot on the microphone's port (S37).
 *
 * Derived rather than configured whenever the mic slaves off the output
 * port's clocks, because there it is not a choice: a slave decodes the frame
 * the master is emitting, so anything but the master's own slot width reads
 * the wrong bits out of the right signal. Deriving it here is what keeps that
 * from being a Kconfig field two people have to keep agreeing about — the one
 * failure mode that would look exactly like a broken microphone.
 *
 * Only when the mic masters its own pins does the Kconfig value apply. */
#if SYNTH_ENABLE_MIC_IN
#if defined(CONFIG_OSYNTH_MIC_SHARE_CLOCKS)
#define SYNTH_MIC_SHARE_CLOCKS 1
#define SYNTH_MIC_SLOT_BITS    (SYNTH_I2S_MCLK_MODE ? 32 : 16)
#else
#define SYNTH_MIC_SHARE_CLOCKS 0
#define SYNTH_MIC_SLOT_BITS    CONFIG_OSYNTH_MIC_SLOT_BITS
#endif
#else
#define SYNTH_MIC_SHARE_CLOCKS 0
#define SYNTH_MIC_SLOT_BITS    16
#endif

/* ---- bring-up: let the microphone codec master the bus (S37f) --------------
 *
 * Off. A one-shot instrument for one unresolved question on the ESP32-P4
 * carrier, and it lives here rather than in either .cpp because it has to move
 * both ends at once: source_mic.cpp stops driving BCLK and WS, and
 * codec_es8311.cpp sets REG00 bit 6. Flipping only one of them puts two drivers
 * on two pins.
 *
 * The question it answers: **do this port's clocks actually reach the codec?**
 * Everything measured so far stops at the pad. The GPIO matrix routes the four
 * signals correctly, and the pads switch (bclk 50%, ws 50%, mclk 49% of a
 * 4096-sample sweep) — but a pad that switches at the die says nothing about
 * the far end of the trace, and the ES8311 is configured, identified
 * (`chip id 8311`) and emitting a hard zero on its data pin.
 *
 * With this on, the P4 drives MCLK and nothing else. If the codec is receiving
 * that clock it will divide it into BCLK (MCLK/4) and LRCK (DIG_MCLK/256) and
 * drive them onto GPIO12 and GPIO10 by itself, which the pad probe in
 * source_mic.cpp will report. Stuck pads mean MCLK is not arriving, and the
 * fault is between the pin and the part.
 *
 * Expect `in starve` to climb the whole time it is on: with no BCLK or WS of
 * its own the port never completes a frame, and that is the arrangement, not a
 * symptom. */
#define SYNTH_MIC_CODEC_MASTER_TEST 0

/* The ES8388's differential input mode presents the *same* pair to both ADC
 * channels with opposite polarity, so the capture arrives as L = +d, R = -d.
 *
 * That meters and monitors perfectly — two healthy per-channel peaks, and a
 * stereo bus keeps the legs apart all the way to the DAC — and is destroyed by
 * anything that sums to mono. The looper folds (L+R)/2 whenever loop.mono is on,
 * which is its default, so a differentially wired input recorded exact silence
 * into every take while being plainly audible: confirmed on both an S3 and a P4
 * with OSYNTH_ES8388_IN_DIFF, and by the take succeeding as soon as the set was
 * switched to stereo.
 *
 * A differential input is one signal, not a stereo pair, so this is resolved
 * once at the capture (line_in_capture) rather than worked around in each
 * consumer. Only for the differential selection: with LINE1/LINE2 the two
 * channels are genuinely independent and inverting one would wreck a real
 * stereo source. */
#if SYNTH_ENABLE_LINE_IN && SYNTH_ENABLE_CODEC_ES8388 && \
    defined(CONFIG_OSYNTH_ES8388_IN_DIFF)
#define SYNTH_LINE_IN_INVERT_R 1
#else
#define SYNTH_LINE_IN_INVERT_R 0
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

/* GPL-3 reverb algorithms (S36): MVerb and DuskVerb, in components/fx_gpl.
 *
 * This is a *licensing* switch, not a feature switch, which is why it is off
 * by default and why the two live in their own component. osynth is MIT;
 * those two algorithms are GPL-3, and linking them in makes the firmware
 * image a GPL-3 combined work. Leaving this off keeps the default build MIT
 * and still ships two reverbs (freeverb and the MIT-licensed WetReverb).
 * Turning it on is a deliberate choice to distribute under GPL-3 — see
 * components/fx_gpl/LICENSE and the licence section of README.md. */
#ifdef CONFIG_OSYNTH_FX_GPL
#define SYNTH_ENABLE_FX_GPL 1
#else
#define SYNTH_ENABLE_FX_GPL 0
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

/* Sample kits (S44): recordable kits alongside the factory one, and the PSRAM
 * budget every pad in every kit shares.
 *
 * SYNTH_SAMPLE_KITS is 0 without PSRAM, which is the whole gate — the kits are
 * resident and the takes land in PSRAM, so there is nothing to fall back to on
 * a board without it. The three constants exist unconditionally so that
 * `if (SYNTH_SAMPLE_KITS > 0)` is a compile-time-foldable test rather than a
 * preprocessor fence around every caller; the sampler registers its parameters
 * either way and refuses takes with a reason, so the app's controls are
 * present and explain themselves instead of silently missing. */
#if defined(CONFIG_OSYNTH_SAMPLE_KITS) && CONFIG_OSYNTH_SAMPLE_KITS > 0
#define SYNTH_SAMPLE_KITS     CONFIG_OSYNTH_SAMPLE_KITS
#define SYNTH_SAMPLE_POOL_KB  CONFIG_OSYNTH_SAMPLE_POOL_KB
#define SYNTH_SAMPLE_MAX_SEC  CONFIG_OSYNTH_SAMPLE_MAX_SEC
#else
#define SYNTH_SAMPLE_KITS     0
#define SYNTH_SAMPLE_POOL_KB  0
#define SYNTH_SAMPLE_MAX_SEC  1
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

/* Two-core render pipeline (S45).
 *
 * The chain is cut in one place — after the voice manager — and the two halves
 * run as pipeline stages on separate cores: a voice stage producing block N+1,
 * and a bus stage finishing block N with everything after the voices (drum bus,
 * FX bus, looper, sampler tap, metronome) plus the sink. The stages overlap, so
 * the budget for one block period is roughly doubled. Which core each is pinned
 * to is kVoiceCore in audio_io.cpp, and it is not arbitrary.
 *
 * That cut point is not a preference. Every other candidate has state crossing
 * it inside a single block: drums_pre_fx() renders into a scratch that
 * drums_post_fx() and the FX bus compressor's key tap both read back, and the
 * three input mix positions have to agree with audio_io_in_fx_block() down to
 * the last multiply. Cutting after the voices leaves all of that whole on core
 * 1 and hands the other core a stage with one output and no readers.
 *
 * The capability gate is repeated here rather than left to Kconfig, the same
 * way SYNTH_ENABLE_USB_TAP's is, and for a reason that is real hardware rather
 * than defensiveness. The bus stage shares its core with everything this
 * firmware pins — the BLE host, the sequencer clock, the preset loader — and it
 * is the half holding the sink, and therefore the hard deadline. On the P4 that
 * is affordable: the BLE controller lives on the companion radio chip
 * (SYNTH_BLE_VIA_HOSTED below), so that core sees only the transport driver's
 * interrupts. On the S3 the on-die controller runs high-priority interrupts
 * pinned to core 0 that no task priority can preempt, landing them squarely on
 * the stage that must not miss a block. There the single-core chain keeps the
 * whole render on core 1, where it never meets them. */
#if defined(CONFIG_OSYNTH_SPLIT_RENDER) && defined(CONFIG_IDF_TARGET_ESP32P4)
#define SYNTH_ENABLE_SPLIT_RENDER 1
#else
#define SYNTH_ENABLE_SPLIT_RENDER 0
#endif

/* Branch-predictor hints for the render path: wrap conditions that are
 * almost never (or almost always) true inside per-sample loops. */
#ifndef SYNTH_LIKELY
#define SYNTH_LIKELY(x) __builtin_expect(!!(x), 1)
#define SYNTH_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif
