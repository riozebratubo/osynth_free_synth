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
 * Ordering: call *after* audio_io_start(). The I2S port is enabled inside
 * that call, so by the time it returns MCLK, BCLK and WS are already running
 * — which is the state an ES8388 wants to be brought out of reset into. The
 * audio task is meanwhile writing into a muted codec, which is silence, and
 * the unmute at the end of codec_init() then lands on a settled chip instead
 * of a popping one.
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

/* "none" on a discrete front end, "es8388" once configured, "es8388?" if the
 * chip was expected but never answered. Never NULL, valid before init. */
const char* codec_name(void);

#ifdef __cplusplus
}
#endif
