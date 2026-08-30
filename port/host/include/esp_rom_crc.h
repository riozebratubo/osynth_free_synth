/*
 * osynth host port — esp_rom_crc.h
 *
 * One caller: drum_kit.cpp validates a kit image against a stored CRC before
 * trusting its offsets. That check has to accept the same images the firmware
 * accepts, and tools/gen_drumkit.py writes the CRC with python's zlib.crc32 —
 * so this must be the same standard CRC-32 (reflected, polynomial 0xEDB88320,
 * init and final xor 0xFFFFFFFF) that IDF's ROM routine is.
 *
 * IDF's signature takes the running value first, which is what makes it
 * chainable across chunks, and it inverts on the way in and on the way out.
 * drum_kit.cpp:94 passes 0, so the result must equal zlib.crc32(body) exactly
 * -- that equality is the whole contract, and getting the inversion wrong
 * would reject every kit the generator has ever written.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t* buf, uint32_t len);

#ifdef __cplusplus
}
#endif
