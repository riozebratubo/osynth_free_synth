/*
 * osynth — MIDI parser and router (Session 4; seqarp taps Session 12).
 * Inputs: USB MIDI (S3), serial/DIN MIDI (optional, SYNTH_ENABLE_SERIAL_MIDI),
 * the sequencer/arpeggiator (S12), BLE NOTE_ON/OFF commands (S14).
 * Outputs: voice manager (notes, pitch bend), the seqarp note tap and
 * real-time callback (S12); future work: USB MIDI OUT.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the USB MIDI RX callback and starts serial MIDI if enabled. */
esp_err_t midi_init(void);

/* Routes one complete channel-voice message (status 0x8n..0xEn plus data
 * bytes; d2 = 0 for two-byte messages) to the voice manager. Omni mode: the
 * channel nibble is ignored. Safe from any control task — this is also the
 * entry point for the sequencer/arpeggiator/BLE later. */
void midi_route_channel_message(uint8_t status, uint8_t d1, uint8_t d2);

/* Routes one System Real-Time byte (0xF8 clock / 0xFA start / 0xFB continue /
 * 0xFC stop) to the seqarp callback below; other real-time bytes (active
 * sensing, reset) are ignored. Called by the USB and serial inputs (S12). */
void midi_route_realtime(uint8_t status);

/* seqarp hooks (S12), registered once by seqarp_init(). The note tap sees
 * every incoming note on/off before the voice manager and returns true to
 * consume the event (the arpeggiator's key input; the sequencer's recorder
 * observes without consuming). Notes emitted by seqarp itself re-enter
 * midi_route_channel_message() and must pass the tap untouched — seqarp
 * guards on its own task handle. */
typedef bool (*midi_note_tap_fn)(uint8_t note, uint8_t velocity, bool on,
                                 void* ctx);
void midi_set_note_tap(midi_note_tap_fn fn, void* ctx);

/* Real-time callback: receives 0xF8/0xFA/0xFB/0xFC on the input's task. */
typedef void (*midi_realtime_fn)(uint8_t status, void* ctx);
void midi_set_realtime_callback(midi_realtime_fn fn, void* ctx);

#ifdef __cplusplus
}
#endif
