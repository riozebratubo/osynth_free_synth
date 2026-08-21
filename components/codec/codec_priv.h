/*
 * osynth — private to components/codec: the shared control bus (S37c).
 *
 * Both chips this component knows live on the *same* two wires. On the ESP32-P4
 * carrier that is the pair of pads silkscreened ES_I2C, carrying the external
 * ES8388 at 0x10/0x11 and the board's on-board ES8311 at 0x18 — different
 * addresses, one bus, and i2c_new_master_bus() may only be called once for it.
 *
 * So the ES8388 file owns the bus (it is the one that must exist for the board
 * to make any sound at all) and hands it out through here. codec_es8311.cpp
 * adds its own device handle to it rather than opening a second master, which
 * would fail on the port and, if it somehow did not, would put two drivers on
 * one pair of pins.
 *
 * The consequence worth stating plainly: **the microphone codec needs the
 * output codec's bus.** On a build with no ES8388 there is nothing here to
 * borrow, and codec_mic_init() says so and returns rather than opening a bus
 * of its own. A discrete front end with an ES8311 microphone is a combination
 * no board in front of this has, and inventing bus ownership rules for it
 * would be guessing at hardware nobody has measured.
 */
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#include "synth_config.h"

#if SYNTH_ENABLE_CODEC_ES8388
/* Opens the control bus if it is not open yet and returns it. Idempotent, and
 * safe to call before or after codec_init() — it runs the same ensure_bus()
 * that both of that file's entry points start with, so whoever gets there
 * first builds the bus and the rest find it built.
 *
 * Control-task only, like everything else on this bus. */
esp_err_t codec_i2c_bus(i2c_master_bus_handle_t* out);
#endif
