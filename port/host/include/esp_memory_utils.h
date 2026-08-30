/*
 * osynth host port — esp_memory_utils.h
 *
 * synth_line.h asks esp_ptr_external_ram() which of its two byte counters an
 * allocation lands in, and fx_init() logs the pair at boot — that line is how
 * a board whose PSRAM did not come up announces itself as "buffers 0 KB PSRAM"
 * instead of as a mysterious shortage later.
 *
 * On the host the answer is a real one rather than a convenience: the block
 * header heap_caps records the pool in is what this reads, so a line that came
 * from the budgeted large pool reports as external and one that fell back to
 * the internal budget does not. The boot line then means on a host exactly
 * what it means on a P4.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool esp_ptr_external_ram(const void* p);
bool esp_ptr_internal(const void* p);

#ifdef __cplusplus
}
#endif
