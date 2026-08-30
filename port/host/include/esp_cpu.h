/*
 * osynth host port — esp_cpu.h
 *
 * esp_cpu_get_cycle_count() is the render chain's stopwatch: render_chain()
 * (main.cpp:339) reads it four times a block to attribute the load to voices,
 * FX and looper, and audio_io turns those into the dsp_load / stage_* figures
 * the app's heartbeat draws.
 *
 * The host answer is a nanosecond clock scaled to the sample-rate-derived
 * "cycles" the firmware assumes, so the percentages keep meaning what they
 * mean on hardware: share of one block period. Reporting real CPU cycles here
 * would need the host clock rate, which varies per machine and under boost,
 * and would make the percentages incomparable between two runs on the same
 * laptop. A fixed nominal rate is not a measurement of the CPU -- it is a
 * measurement of the deadline, which is what the meter is for.
 *
 * It is read on the audio thread, so it must stay a clock read and never a
 * syscall that can block; steady_clock is vDSO-backed on the platforms that
 * matter here.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The nominal rate the host build prices a block against.
 *
 * audio_io.cpp:871 sizes the block budget as
 *     esp_rom_get_cpu_ticks_per_us() * 1e6 * BLOCK / RATE
 * and then divides measured cycles by it. So the only requirement is that the
 * two agree, and they do: esp_rom_get_cpu_ticks_per_us() (esp_rom_sys.h)
 * returns this rate divided by a million, and esp_cpu_get_cycle_count() ticks
 * at this rate. The percentages then come out as exact wall-clock fractions of
 * one block period, which is what the meter is claiming to show. */
#define OSYNTH_HOST_NOMINAL_HZ 240000000u

uint32_t esp_cpu_get_cycle_count(void);

#ifdef __cplusplus
}
#endif
