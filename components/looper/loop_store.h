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

/* SD backend: ensures the card is mounted, doing the same lazy retry the
 * save/load paths do, and returns whether it is. The mount has one owner —
 * the streamed looper backend (loop_stream.cpp) shares this one rather than
 * making its own, so a card pulled and re-inserted recovers for both at
 * once. Always false on the flash backend. */
bool loop_store_mount(void);

/* SD backend: creates `path` if it is not already a directory and says
 * whether it is one afterwards. Every failure is logged with its errno —
 * this is the one place a card that mounts but cannot be used announces
 * itself, and an unchecked mkdir turns that into a bare ENOENT from the next
 * fopen (which is exactly how it was found). A card that has stopped
 * answering is unmounted here rather than left to time out on every later
 * call. The parent must exist; this does not create a chain. Always false on
 * the flash backend. */
bool loop_store_ensure_dir(const char* path);

/* The directory the backend keeps its files in, without a trailing separator;
 * "" on the flash backend, which has no filesystem.
 *
 * Exists because loop_stream.cpp writes its live takes alongside the save
 * slots and needs to name the same place. It used to spell "/sd/osynth" a
 * second time, which was correct exactly as long as the location was a
 * constant -- and it stopped being one when the host port put it under a
 * user-profile path resolved at run time. One owner, asked rather than
 * copied. */
const char* loop_store_dir(void);

/* What the idle card poll found. Three states, not two: "no card" and "the
 * card just left" call for opposite responses, and collapsing them into one
 * boolean made a mount that was merely between retries look like a removal. */
typedef enum {
    LOOP_STORE_CARD_OK = 0, /* mounted and answering */
    LOOP_STORE_CARD_LOST,   /* was mounted, has stopped answering */
    LOOP_STORE_CARD_NONE,   /* nothing mounted; a mount is being retried */
} loop_store_card_t;

/* SD backend, loop_ctl only (the same single-caller rule as
 * loop_store_mount). Asks after the card and lazily mounts one if none is
 * there — which is how a re-inserted card comes back, with the attempts
 * backed off so a synth running with no card does not spend its control task
 * on them. The usual breakout has no card-detect pin, so presence has to be
 * asked about: a CMD13 status read, one command with no data transfer, cheap
 * enough for a once-a-second idle poll.
 *
 * LOST means every open handle on that card is now dangling, and the mount is
 * deliberately left in place so the caller can give them up before
 * loop_store_card_gone() drops it. NONE means exactly what it says — nothing
 * is mounted — and in particular is *not* a removal: a caller holding
 * something that lives on a card must not tear it down on NONE, because the
 * card it belongs to may simply not be mounted yet. Always OK on the flash
 * backend: there is no card to lose. */
loop_store_card_t loop_store_poll_card(void);

/* SD backend: unmounts a card the poll has reported LOST, once
 * the caller has closed everything it had open on it. Idempotent and a no-op
 * when nothing is mounted; the next demand re-mounts. */
void loop_store_card_gone(void);

/* SD backend: the mounted card's CID serial number, 0 when none is mounted.
 * Identity, not a handle — it is what lets a caller tell "the card I had is
 * back" from "some other card is now in the slot", which matters to anything
 * still holding paths that only mean something on the first one. 0 on the
 * flash backend. */
uint32_t loop_store_card_serial(void);

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

/* ---- reading a slot out piecemeal (S33: the app's WAV export) ----
 *
 * The two calls above are the *player's* view of a slot: probe tells the
 * caller how much PSRAM to commit, read_track converts the blob into the live
 * format and wants the whole track in one buffer. An export wants neither. It
 * has ~230 bytes of BLE frame to fill at a time, it never allocates a track,
 * and it is perfectly able to decode — so what it needs is the stored bytes
 * verbatim plus the header's own codec to read them by. Hence these two
 * rather than another mode bolted onto read_track, which would have had to
 * carry the legacy encoder's state across calls to stay correct.
 *
 * Same loop_ctl-only rule as everything else here. */
#define LOOP_STORE_CODEC_RAW 0        /* v1 blobs: raw interleaved s16 */
#define LOOP_STORE_CODEC_ADPCM 1      /* stereo, one byte per frame */
#define LOOP_STORE_CODEC_ADPCM_MONO 2 /* mono, two frames per byte */

typedef struct {
    uint32_t loop_frames;
    uint32_t sample_rate;
    uint32_t track_bytes; /* per stored track, in the stored codec */
    uint8_t filled;       /* bitmask, track 1 = bit 0 */
    uint8_t codec;        /* LOOP_STORE_CODEC_* */
} loop_store_info_t;

/* The slot's header, as stored. ESP_ERR_NOT_FOUND when the slot holds no
 * valid blob — which is the ordinary answer for a slot never saved to. */
esp_err_t loop_store_slot_info(int slot, loop_store_info_t* out);

/* `len` bytes at `offset` of the packed_idx-th stored track, exactly as they
 * sit in the blob (no codec conversion — see above). Reads past track_bytes
 * are rejected rather than clamped: the caller has the header and a short
 * answer here would be indistinguishable from a torn file. */
esp_err_t loop_store_read_slot_bytes(int slot, int packed_idx, uint32_t offset,
                                     uint8_t* dst, uint32_t len);

#ifdef __cplusplus
}
#endif
