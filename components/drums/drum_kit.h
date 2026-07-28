/*
 * osynth — drum kit loading (Session 22), private to components/drums.
 *
 * Turns a kit image (drum_kit_fmt.h) into a table of ready-to-play samples.
 * Three sources, all producing the same in-memory shape so the voices never
 * learn where a kit came from:
 *
 *   factory   the image linked into the firmware as .rodata. Flash-mapped:
 *             `data` points straight into the XIP window, nothing is copied,
 *             the kit costs 0 bytes of RAM.
 *   .okit     the same image read off an SD card into PSRAM.
 *   WAV dir   a folder of .wav one-shots on an SD card, converted to mono
 *             PCM16 in PSRAM. Slot order follows the sorted file names, and
 *             a leading "NN_" (or "NN-") prefix pins a file to a slot.
 *
 * A kit is only ever published to the audio task once it is fully built and
 * validated (magic, version, CRC, and every slot's extent inside the image);
 * a half-parsed kit must never become a sample pointer.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "drum_kit_fmt.h"
#include "drums.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One playable slot. `data` is const because the factory kit lives in
 * read-only flash; SD kits allocate into `owned` on the kit as a whole. */
typedef struct {
    const uint8_t* data; /* frame 0; nullptr for an empty slot */
    uint32_t frames;
    uint32_t rate;
    uint32_t loop_start;
    uint32_t loop_end; /* 0 = one-shot */
    float gain;
    float pan;
    uint8_t format; /* DRUM_FMT_* */
    uint8_t choke_group;
    uint8_t note;
    char name[DRUM_SLOT_NAME_MAX];
} drum_sample_t;

typedef struct {
    char name[DRUM_KIT_NAME_MAX];
    int slot_count;
    drum_sample_t slots[DRUM_KIT_MAX_SLOTS];
    void* owned; /* heap block backing the data, or nullptr for the ROM kit */
    size_t owned_bytes;
} drum_kit_t;

/* Parses `img` in place — the returned kit points into it, so `img` must
 * outlive the kit. `owned` is stored on the kit for drum_kit_free() to
 * release (pass nullptr for flash-resident images). Returns false and leaves
 * `out` untouched on any structural problem; the reason is logged. */
bool drum_kit_parse(const uint8_t* img, size_t len, void* owned,
                    size_t owned_bytes, drum_kit_t* out);

/* The kit built into the firmware. Fails only if the build embedded an empty
 * image (no sample pack was present at build time). */
esp_err_t drum_kit_load_rom(drum_kit_t* out);

void drum_kit_free(drum_kit_t* kit);

/* ---- SD-card kits (PSRAM targets with the SD bus configured) ---- */

/* Mounts the card if it is not already mounted (idempotent, and safe when
 * the looper's SD backend got there first) and lists selectable kits under
 * /sd/osynth/kits: `<name>.okit` files and folders of .wav files. Returns
 * the number written, 0 if there is no card or no kits. */
int drum_kit_scan_sd(char names[][DRUM_KIT_NAME_MAX], int max);

/* Loads a kit previously reported by drum_kit_scan_sd(). Blocking I/O —
 * control tasks only. */
esp_err_t drum_kit_load_sd(const char* name, drum_kit_t* out);

/* True when the build can talk to an SD card at all. */
bool drum_kit_sd_supported(void);

#ifdef __cplusplus
}
#endif
