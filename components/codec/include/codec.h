/*
 * osynth — analogue front-end control (Session 31b).
 *
 * Some converters are configured by strapping pins and need no firmware at
 * all: a PCM5102A plays whatever arrives on its I2S port, a PCM1808 captures
 * whatever its MD0/MD1 pins told it to. That is the whole reason S2 and S31
 * could treat the analogue side as "wiring" and stop there.
 *
 * An ES8388 is not like that. It powers up unconfigured and silent, and only
 * an I2C register sequence makes it a codec at all — which path is connected
 * to which pin, what serial format it expects, which output stages are on,
 * whether the DAC is muted. This component is that sequence, and nothing
 * else: the audio path itself is unchanged, still audio_io -> sink_i2s.
 *
 * On a discrete build every function here is a no-op that returns ESP_OK, so
 * the call in main.cpp needs no #if.
 *
 * Ordering (revised, S31d): codec_init() runs *before* audio_io_start().
 *
 * It used to run after the port was up, on the reasoning that an
 * ES8388 wants to come out of reset into a running MCLK. That turned out to
 * be both unnecessary and actively harmful. Unnecessary because — unlike
 * codecs such as the SGTL5000 — the ES8388 needs no clock at all to use its
 * control port. Harmful because the control lines run alongside a 12.288 MHz
 * MCLK and a 3.072 MHz BCLK, and on a hand-wired rig that is enough to make
 * the bus marginal: on an M5Stack M144 the codec answered a bus scan reliably
 * with the port stopped and intermittently vanished, NACKed writes, or
 * answered at a phantom address once it was running. The same "works before
 * i2s_begin, NACKs after" report exists upstream (esp-adf #1334,
 * arduino-audio-tools #1664), unexplained in both.
 *
 * So the whole sequence — including the unmute — now happens while the port
 * is stopped. Keeping just the unmute for afterwards was tried first and does
 * not work: that single write NACKed five times in about a millisecond on the
 * M144, and a codec that is perfectly configured and permanently muted is no
 * better than one that never answered.
 *
 * Unmuting a DAC that has no clock is safe: with no MCLK it is not converting,
 * so the outputs sit at VMID exactly as they did muted, and the first thing it
 * converts once the clocks arrive is the render chain's silence.
 *
 * Beyond that ordering the driver is deliberately plain — one I2C transfer per
 * register, no retries, no read-back, same as the vendor's own driver. S31d
 * tried retries, then verification, then a background reconciler; none of it
 * fixed the rig that prompted it, and each layer made the next symptom harder
 * to read. A bad control bus is a wiring fault, and a driver that says so is
 * more useful than one that hides it.
 */
#pragma once

#include "esp_err.h"

#include "synth_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Brings the front end up. Returns ESP_ERR_NOT_FOUND if a codec was expected
 * on I2C and did not answer — worth logging loudly (the board will be silent)
 * but never worth panicking over, since a wiring fault should not bootloop a
 * synth whose USB and BLE control surfaces still work. */
esp_err_t codec_init(void);

/* "none" on a discrete front end, "es8388" once up, "es8388?" if the chip was
 * expected but never answered. Never NULL, valid before init. */
const char* codec_name(void);

#ifdef __cplusplus
}
#endif
