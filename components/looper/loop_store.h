/*
 * osynth — looper track persistence backends (Session 16, private).
 *
 * One blob format, two backends behind SYNTH_LOOP_STORE_SD:
 *  - flash: the raw region above the partition table (0x400000 up to the
 *    module's flash end, N8R8/N16R8 per Kconfig), slot 0 only. Erase and
 *    program stall XIP code fetches, so callers must keep the looper
 *    transport stopped around save/load (loop_store_needs_stopped()).
 *  - sd: FAT files over SDSPI (/sd/osynth/loopN.olp), slots 0..7, no XIP
 *    interference — safe while the loop plays.
 *
 * Blob: 32-byte header {magic "OSL1", version, filled mask, codec, loop
 * frames, sample rate, track bytes} + the *filled* tracks packed in
 * ascending track order. Tracks are stored IMA-ADPCM (v2, S18): stereo one
 * byte per frame (codec 1), mono two frames per byte (codec 2, S19). Since
 * S20 the PSRAM tracks hold exactly these bytes (loop_adpcm.h runs in the
 * render path), so save and v2 load are plain copies — no codec work here.
 * Legacy v1 blobs (raw stereo int16) are encoded on load; pre-S19 firmware
 * cleanly refuses mono blobs (unknown codec). The flash backend writes the
 * header last so a torn save can never validate; the SD backend writes a
 * temp file and rename()s (the presets pattern).
 *
 * All functions run on the loop_ctl task only — they block for seconds on
 * multi-MB sets and are never called from the audio task.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "looper.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOOP_STORE_SLOTS_FLASH 1
#define LOOP_STORE_SLOTS_SD 8

/* Size of one stereo frame (IMA ADPCM: 2 x 4-bit nibbles) — since S20 this
 * is both the stored and the PSRAM size. Mono frames (S19) are one nibble,
 * half a byte per frame. */
#define LOOP_STORE_BYTES_PER_FRAME 1

/* Probes the backend (flash size check / SD mount attempt). Never fatal:
 * an absent card or too-small chip logs a warning and leaves the store
 * not-ready; SD mounting is retried lazily on each save/load. */
esp_err_t loop_store_init(void);

bool loop_store_ready(void);
const char* loop_store_backend_name(void); /* "flash" | "sd" */
int loop_store_slots(void);

/* True when save/load must not run concurrently with audio rendering from
 * flash (the flash backend). */
bool loop_store_needs_stopped(void);

/* Writes the set (only tracks whose bit is set in `filled`; bufs[] hold the
 * ADPCM track bytes, indexed by track). `mono` states the live format (S19)
 * and picks the stored codec. Validates capacity first. */
esp_err_t loop_store_save(int slot, uint32_t loop_frames, uint8_t filled,
                          uint8_t* const bufs[LOOP_TRACKS], bool mono);

/* Reads and validates a slot's header. `mono` reports the stored format so
 * the caller can size the PSRAM buffers before read_track. */
esp_err_t loop_store_probe(int slot, uint32_t* loop_frames, uint8_t* filled,
                           bool* mono);

/* Reads the packed_idx-th *stored* track (0-based over the filled bits in
 * ascending track order) into dst as PSRAM-format ADPCM bytes (v2 blobs:
 * straight copy; legacy v1 raw: encoded on the way in). */
esp_err_t loop_store_read_track(int slot, int packed_idx, uint8_t* dst,
                                uint32_t loop_frames);

#ifdef __cplusplus
}
#endif
