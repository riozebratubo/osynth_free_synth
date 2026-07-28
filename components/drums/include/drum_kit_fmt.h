/*
 * osynth — drum kit image format (Session 22).
 *
 * One flat, position-independent blob describing up to DRUM_KIT_MAX_SLOTS
 * one-shot samples. The same bytes are used three ways:
 *   - embedded in the firmware as .rodata (the factory kit, built by
 *     tools/gen_drumkit.py and linked by components/drums/CMakeLists.txt) —
 *     flash-mapped, so the sample data is read straight from a const pointer
 *     with no RAM cost at all;
 *   - read from an SD card into PSRAM (`/sd/osynth/kits/<name>.okit`);
 *   - written by the generator, verified by tools/drumkit/audition.py.
 *
 * Everything is little-endian, which is the native order on both targets, so
 * the reader casts rather than parses. All offsets are from the start of the
 * image, so a kit works identically whether it sits in flash or in PSRAM.
 *
 * Storage format is 8-bit mu-law (G.711). A sampler needs random access —
 * pitch shifting, start offsets, reverse — which rules out the sequential
 * IMA-ADPCM decoder the looper uses; mu-law decodes with one 256-entry LUT
 * lookup, gives ~38 dB SNR regardless of level, and halves the flash traffic
 * (and therefore the cache pressure) versus int16. PCM16 is accepted too for
 * SD kits converted by other tools.
 *
 * Changing any layout here means bumping DRUM_KIT_VERSION and updating
 * tools/gen_drumkit.py — the generator mirrors these structs by hand.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DRUM_KIT_MAGIC     "OSYNKIT1"
#define DRUM_KIT_MAGIC_LEN 8
#define DRUM_KIT_VERSION   1

/* Ceiling on slots per kit. The runtime exposes DRUM_SLOTS (drums.h) of
 * them as parameters; a kit may declare fewer. */
#define DRUM_KIT_MAX_SLOTS 32
#define DRUM_KIT_NAME_MAX  24
#define DRUM_SLOT_NAME_MAX 12

/* Sample storage formats. */
enum {
    DRUM_FMT_ULAW = 0, /* 8-bit G.711 mu-law, 1 byte per frame */
    DRUM_FMT_PCM16 = 1 /* signed 16-bit LE, 2 bytes per frame  */
};

/* 64 bytes. `crc32` covers everything from the end of this header to the end
 * of the image (slot table + sample data) — a corrupt SD kit must never be
 * handed to the audio task as sample pointers. */
typedef struct {
    char magic[DRUM_KIT_MAGIC_LEN];
    uint16_t version;
    uint16_t slot_count;
    uint32_t total_bytes; /* whole image, header included */
    char name[DRUM_KIT_NAME_MAX];
    uint32_t crc32;
    uint8_t reserved[20];
} drum_kit_header_t;

/* 48 bytes per slot. `data_offset` is 0 for an empty slot (a source file the
 * generator could not read); `frames` is then 0 too and the slot is silent.
 * `loop_end` == 0 means one-shot — the overwhelming case for a drum kit, but
 * the fields exist so the same format can carry sustained samples. */
typedef struct {
    char name[DRUM_SLOT_NAME_MAX];
    uint32_t data_offset;
    uint32_t frames;
    uint32_t rate; /* stored sample rate, Hz; playback resamples to 48 kHz */
    uint32_t loop_start;
    uint32_t loop_end;
    float gain; /* mix trim baked by the generator; the slot param scales it */
    float pan;  /* -1 .. +1 default position */
    uint8_t format;
    uint8_t choke_group; /* 0 = none; equal non-zero groups cut each other */
    uint8_t note;        /* MIDI note this slot answers to (GM drum map) */
    uint8_t flags;       /* reserved, 0 */
    uint8_t reserved[4];
} drum_kit_slot_t;

#ifdef __cplusplus
}
#endif
