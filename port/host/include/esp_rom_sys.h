/*
 * osynth host port — esp_rom_sys.h
 *
 * esp_rom_get_cpu_ticks_per_us() is the other half of the render-load meter;
 * see esp_cpu.h for why the two have to agree and what they agree on.
 *
 * esp_rom_delay_us() is a busy-wait used by the codec bring-up and the mic pad
 * probe, neither of which is in the host build. It is here so that a file
 * naming it still compiles if one is ever pulled in, and it busy-waits rather
 * than sleeping because the callers are timing register writes in the low
 * microseconds, where a sleep would overshoot by orders of magnitude.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t esp_rom_get_cpu_ticks_per_us(void);
void esp_rom_delay_us(uint32_t us);

#ifdef __cplusplus
}
#endif
