/*
 * osynth — audio sink interface (private to the audio_io component).
 *
 * A sink consumes interleaved int16 stereo blocks. write() blocks until the
 * sink has accepted the whole block — the sink's DMA (or esp_timer, for the
 * null sink) is the real-time clock that paces the audio task.
 *
 * One sink is the primary and owns that clock; audio_io_start() picks it from
 * the build configuration. A build may additionally attach one *tap* — a
 * second sink fed the same blocks, whose write must never block and never
 * pace, because only the primary is allowed to decide when the next block is
 * due. A tap that cannot accept a block drops it (S29).
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

#if SYNTH_ENABLE_USB_TAP
/* The same EP-IN FIFO as audio_sink_usb(), attached as a tap: never blocks,
 * never paces, drops a block rather than make the I2S DAC wait on a host. */
const audio_sink_t* audio_sink_usb_tap(void);
#endif

#if SYNTH_ENABLE_I2S_DAC
/* External I2S DAC (e.g. PCM5102A). Pins per target: docs/HARDWARE.md. */
const audio_sink_t* audio_sink_i2s(void);
#endif

#if SYNTH_ENABLE_LINE_IN
/* Line input (S31): the RX half of the same I2S port audio_sink_i2s() drives.
 *
 * Two free functions rather than a second vtable, because there is nothing to
 * dispatch on: both handles come out of one i2s_new_channel() call, share one
 * config object and one teardown, and there is exactly one source with no
 * fallback. A lookalike vtable would need a private header re-exporting the
 * handle and the failure state — a split with negative information hiding.
 *
 * The contract is the *opposite* of the sink's, which is the other reason not
 * to make them look alike: read() must never block and never pace. It is
 * called from the audio task with a zero timeout, returns short (with
 * `frames_read` set) rather than wait, and the caller zero-fills the tail. The
 * primary sink alone decides when the next block is due. */
bool audio_source_i2s_ready(void);
esp_err_t audio_source_i2s_read(int16_t* interleaved, size_t frames,
                                size_t* frames_read);
#endif

#if SYNTH_ENABLE_MIC_IN
/* Microphone input (S37): a digital MEMS mic on the *second* I2S controller.
 *
 * Same contract as the line source above — read() never blocks, never paces,
 * returns short rather than wait — and the same shape for the same reason,
 * since audio_io.cpp calls one or the other through the identical code path.
 * What differs is entirely below this line: its own port, its own pins, and a
 * mono slot widened to the stereo block every consumer expects.
 *
 * start() is separate here where the line input has none, because the mic's
 * port is not the sink's. The line source comes up inside audio_sink_i2s()'s
 * own start() — one i2s_new_channel() call produces both halves — while this
 * one is a second controller that has to be told when to open. It must be
 * called *after* the sink has started: with SYNTH_MIC_SHARE_CLOCKS the mic is
 * a slave on the output port's BCLK and WS, and a slave enabled before there
 * is a master driving those pins sits waiting for an edge that has not
 * happened yet.
 *
 * Never fails in a way the caller must handle: a mic that does not come up
 * logs and leaves ready() false, exactly as the RX half of the main port
 * does. An input is an accessory, and losing one must never cost the output. */
esp_err_t audio_source_mic_start(void);
bool audio_source_mic_ready(void);
esp_err_t audio_source_mic_read(int16_t* interleaved, size_t frames,
                                size_t* frames_read);

/* Every bit seen on each raw slot since the last call, and clears the
 * accumulator (S37d). The control-task side of the bring-up telemetry described
 * in source_mic.cpp — see there for why a peak meter cannot answer the same
 * question. Reads 0 on a mic that never came up, which is also what a mic with
 * a dead data pin reads; the boot log separates those two. */
void audio_source_mic_raw_take(uint32_t* or_l, uint32_t* or_r);

/* Sweeps this port's pads for activity and logs one line each, tagged with
 * `when` (S37f). Boot-time only: it busy-polls each pad a few thousand times.
 * See the definition in source_mic.cpp for what the readings mean. */
void audio_source_mic_probe_pads(const char* when);
#endif

#if SYNTH_HAS_INTERNAL_DAC
/* Classic-ESP32 internal 8-bit DAC on GPIO25 (L) / GPIO26 (R). */
const audio_sink_t* audio_sink_dac(void);
#endif
