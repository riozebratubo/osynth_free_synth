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
 * S37c — the ES8311 is a known gap, not an oversight. The ESP32-P4 carrier's
 * on-board microphone is an *analogue* mic into an ES8311's PGA at I2C 0x18,
 * so it needs exactly what the ES8388 needs and gets none of it here: this
 * component drives one chip. codec_es8311.cpp closes that gap, and osynth
 * clocks the mic's I2S pins for it (DIN 48, BCLK 12, WS 10, MCLK 13 — see
 * sdkconfig.defaults.esp32p4). The external-mic path
 * (OSYNTH_MIC_SHARE_CLOCKS, a digital MEMS part) needs none of this and is
 * proven working.
 *
 * Ordering: OPEN QUESTION, currently back to the original — codec_init() runs
 * *after* audio_io_start(), with the port and MCLK already up.
 *
 * S31d moved it to before the port, and that move is what
 * OSYNTH_CODEC_INIT_BEFORE_I2S below switches back on. Its reasoning was that
 * the ES8388 needs no clock to use its control port (true, unlike an
 * SGTL5000), while the control lines running alongside a 12.288 MHz MCLK and a
 * 3.072 MHz BCLK make the bus marginal on hand-wired rigs: on an M5Stack M144
 * the codec answered a bus scan reliably with the port stopped and
 * intermittently vanished, NACKed writes, or answered at a phantom address
 * once it was running. The same "works before i2s_begin, NACKs after" report
 * exists upstream (esp-adf #1334, arduino-audio-tools #1664), unexplained in
 * both. Keeping only the unmute for afterwards was tried and does not work:
 * that single write NACKed five times in about a millisecond on the M144, and
 * a codec that is perfectly configured and permanently muted is no better than
 * one that never answered.
 *
 * What that reasoning did not account for is the ADC. It is about the *control*
 * bus, and it is the output side it was verified against — the DAC genuinely
 * does not care, since with no MCLK it is not converting and its outputs sit at
 * VMID exactly as they did muted. The input side is a different circuit with a
 * different question: configuring the PGA, the input mux and the ADC's own
 * power-up while the chip has no clock leaves it to reach its operating point
 * when the clocks arrive rather than under the register writes that set it, and
 * a persistent line-in hiss was reported after the move. That has not been
 * root-caused. Until it is, the safe default is the ordering that has no such
 * report against it.
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

/* Where codec_init() sits relative to audio_io_start() (main.cpp).
 *
 *   0 — after the port, MCLK running. The original ordering, and the default:
 *       no line-in noise has been reported against it.
 *   1 — before the port, no clocks. S31d's ordering. Try this if the codec is
 *       unreliable on the control bus — a bus scan that finds nothing, NACKed
 *       writes, a phantom address, or a board that comes up silent — which is
 *       the failure it was introduced to fix.
 *
 * Independent of codec_early_mute(), which runs first either way and is what
 * actually removes the power-on scratch. */
/* Per target, because the two boards genuinely differ rather than because one
 * ordering is better. The S3 rig has run at 0 throughout with no bus complaint
 * and no line-in hiss, so it keeps the ordering it was verified on. The P4
 * carrier does not tolerate it: with the port running, the codec answers its
 * address probe and then NACKs the *first* register write —
 *
 *   register 0x19 = 0x04 failed at step 1/32 on the device that answered at
 *   0x10 (ESP_ERR_INVALID_RESPONSE)
 *
 * with a bus scan immediately afterwards finding all three devices (0x10, 0x18,
 * 0x33). Answering but refusing to be written is the exact failure this switch
 * was introduced for, and codec_early_mute() landing its two writes every time —
 * it runs before the port — is the other half of the proof. */
#ifdef CONFIG_IDF_TARGET_ESP32P4
#define OSYNTH_CODEC_INIT_BEFORE_I2S 1
#else
#define OSYNTH_CODEC_INIT_BEFORE_I2S 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Silences the codec's output stages, and nothing else. First call in
 * app_main() — before NVS, before any parameter is registered.
 *
 * Not the first mute, though: bootloader_components/osynth_codec_mute writes
 * the same two registers from the second-stage bootloader, hundreds of
 * milliseconds earlier on a cold boot, by bit-banging the control bus before
 * any driver exists. This one stays because the two compose (identical values,
 * identical order, and codec_init()'s table opens with them anyway) and
 * because it is the one that can report a failure. What neither can reach is
 * power-on up to the bootloader — the ROM runs first, and only hardware covers
 * that: hold the codec in reset, or gate the amp. See PINMAP.md.
 *
 * codec_init() below cannot run that early: it applies `in.pga` and
 * `out.level` from the ParamStore and subscribes to it, so it has to follow
 * every registration and persist_init()'s restore. On an S3 that is NVS, ~250
 * parameters, the FX delay lines, the drum kit, a LittleFS mount and the
 * looper's PSRAM sizing — a second or more on a first boot, all of it with an
 * ES8388 sitting in its power-on register state driving its outputs. That is
 * the scratch heard between power-on and the codec coming up, and no amount of
 * care inside codec_init() can help, because the noise is over before it runs.
 *
 * So the mute is split out and moved to the front: bring up I2C, find the chip,
 * mute the DAC and power the output drivers down. Nothing that depends on
 * parameters, nothing that has to be undone — codec_init() starts from exactly
 * this state anyway (its table opens with the same two writes), so the two
 * compose rather than fight.
 *
 * Errors are the caller's to ignore. A board with no codec, a bus fault, a
 * chip that does not answer: all of them mean there is nothing to mute, and
 * codec_init() is the place that reports the problem properly.
 *
 * A no-op returning ESP_OK on a discrete front end. */
esp_err_t codec_early_mute(void);

/* Brings the front end up. Returns ESP_ERR_NOT_FOUND if a codec was expected
 * on I2C and did not answer — worth logging loudly (the board will be silent)
 * but never worth panicking over, since a wiring fault should not bootloop a
 * synth whose USB and BLE control surfaces still work. */
esp_err_t codec_init(void);

/* "none" on a discrete front end, "es8388" once up, "es8388?" if the chip was
 * expected but never answered. Never NULL, valid before init. */
const char* codec_name(void);

/* Where codec_mic_init() sits relative to audio_io_start(), and note that this
 * is a *different question* from OSYNTH_CODEC_INIT_BEFORE_I2S above rather
 * than the same one applied twice.
 *
 *   0 — after the port, MCLK running. The default, and what every reference
 *       driver does: the ES8311's clock manager is configured from the MCLK
 *       pin, so writing that configuration before the clock exists leaves the
 *       chip to reach its operating point afterwards rather than under the
 *       writes that set it. The symptom is a clean init log and a converter
 *       producing silence — which is exactly where this landed on the first
 *       attempt, when the call sat next to codec_init() and the P4's
 *       BEFORE_I2S branch put the port start in between.
 *   1 — before the port. Try this if the *control bus* refuses the table here,
 *       which is the failure OSYNTH_CODEC_INIT_BEFORE_I2S exists for on this
 *       board. The two switches pull in opposite directions on purpose: that
 *       one is about a bus going deaf while the main port clocks pins next to
 *       it, this one is about a converter needing its clock. If both turn out
 *       to be true at once the answer is a bus retry, not an ordering.
 */
#define OSYNTH_ES8311_INIT_BEFORE_I2S 0

/* Brings up the *microphone* codec (S37c) — an ES8311 whose ADC is the only
 * path to a board's on-board analogue mic. ADC only: its DAC is powered down
 * and audio output stays entirely with the ES8388 on the other port.
 *
 * Call after audio_io_start(), so the mic port is driving MCLK — see
 * OSYNTH_ES8311_INIT_BEFORE_I2S above.
 *
 * Returns ESP_ERR_NOT_FOUND when nothing answers at 0x18, which means a board
 * without such a codec rather than a fault. A no-op returning ESP_OK on builds
 * with no on-board mic codec, so the call site needs no #if. */
esp_err_t codec_mic_init(void);

/* "none", "es8311" once up, or "es8311?" if it was expected and never
 * answered. Never NULL, valid before init. */
const char* codec_mic_name(void);

#ifdef __cplusplus
}
#endif
