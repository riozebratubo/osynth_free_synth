/*
 * osynth — private: serial (UART/DIN) MIDI input. Real implementation when
 * SYNTH_ENABLE_SERIAL_MIDI is set, a successful no-op otherwise.
 */
#pragma once

#include "esp_err.h"

esp_err_t midi_serial_start(void);
