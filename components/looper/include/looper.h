/*
 * osynth — 8-track loop recorder (Session 15).
 *
 * One-track-at-a-time looper on the master output: the record tap sits
 * after the FX bus in the render chain (main.cpp: voice sum -> fx ->
 * looper), so a track captures exactly what the live synth sounds like —
 * including its FX print — while the playback mix of the *other* tracks is
 * added after the tap and is never re-recorded or re-effected. Tracks stay
 * separable.
 *
 * Tracks live IMA-ADPCM-encoded in PSRAM since S20 (loop_adpcm.h): stereo
 * one byte per frame (~47 KB/s), mono half that (loop.mono, S19). The
 * looper's access pattern is strictly sequential from the loop start —
 * playback wraps there, punch-ins land there — so per-track decoder state
 * reset at every wrap is the entire seek story, and the stored blobs are
 * byte-identical to the live buffers (save/load are copies). The first
 * recording defines the loop length up to the cap: 40 s stereo, x2 mono,
 * x2 in 4-track mode (loop.tracks) — 160 s mono/4-track max; a first take
 * that cannot allocate its cap halves it until it fits (the achieved
 * ceiling is mirrored via loop.maxlen), and a punch-in that cannot
 * allocate rejects the record request with a warning (sink-fallback
 * philosophy). At the full cap roughly four tracks fit the ~7 MB PSRAM
 * pool in 8-track mode (~two in 4-track mode); everything fits at half
 * the cap.
 *
 * S31 adds a second track store, chosen per set by loop.store: instead of
 * whole tracks in PSRAM, each track is a file on the SD card and only a
 * small window of it passes through PSRAM (loop_stream.h). That trades the
 * pool-sized cap for a card-sized one — up to LOOP_STREAM_MAX_S — and the
 * requirement that the card keep up. Registered only on a build with the SD
 * store backend.
 *
 * Requires PSRAM: without CONFIG_SPIRAM (classic ESP32) looper_init()
 * registers nothing and looper_process() is a no-op.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Looper parameter IDs (0x06xx) — names, ranges and defaults in
 * docs/PARAM_MAP.md. loop.filled and loop.len are firmware-written status
 * (read-only for control surfaces). */
#define LOOP_PID_TRACK  0x0600 /* loop.track  int 1..8                       */
#define LOOP_PID_MODE   0x0601 /* loop.mode   enum stop|play|rec             */
#define LOOP_PID_CLEAR  0x0602 /* loop.clear  enum none|track|all (command)  */
#define LOOP_PID_FILLED 0x0603 /* loop.filled int bitmask, track 1 = bit 0   */
#define LOOP_PID_LEN    0x0604 /* loop.len    float seconds, 0 = no loop     */
/* S16, registered only with CONFIG_OSYNTH_LOOP_PERSIST: triggers like
 * preset.load/save — writing the slot number performs the operation
 * (flash backend: slot 0 only; SD: 0..7). */
#define LOOP_PID_SAVE   0x0605 /* loop.save   int, trigger                   */
#define LOOP_PID_LOAD   0x0606 /* loop.load   int, trigger                   */
/* Read-only transport telemetry (S18): mirrored ~4x/s while the transport
 * runs so BLE clients can show the loop/record position; loop.rectrk is the
 * track the audio task is actually writing (0 = none), which distinguishes
 * an armed punch-in (mode rec, rectrk 0) from live recording. */
#define LOOP_PID_POS    0x0607 /* loop.pos    float seconds, read-only       */
#define LOOP_PID_RECTRK 0x0608 /* loop.rectrk int 0 | 1..8, read-only        */
/* Cap policies (S19 mono, S20 track mode): both pick properties of the
 * *next* loop set — latched by the first recording; a load adopts the
 * slot's stored format and writes loop.mono back. Mono folds (L+R)/2 at
 * the record tap for half the bytes; 4-track mode trades slots for cap.
 * Each doubles the recording cap, so loop.maxlen (read-only, seconds)
 * mirrors the live cap: base 40 s (stereo, 8 tracks), 160 s max (mono,
 * 4 tracks). Its registered default is the base cap and its max the
 * ceiling — clients derive any combo as default x2 per enabled toggle.
 * During a fallback-capped first take loop.maxlen shows the achieved
 * ceiling instead. loop.tracks does not restrict loop.track writes — it
 * is a cap/UI policy, not an enforcement (state, not patch). Both default
 * on, so a fresh boot records mono, 4-track and caps at the 160 s ceiling;
 * loop.maxlen's *registered* default stays the base cap for the client-side
 * derivation above and its value is mirrored to the real cap at init. */
#define LOOP_PID_MONO   0x0609 /* loop.mono   enum stereo|mono               */
#define LOOP_PID_MAXLEN 0x060A /* loop.maxlen float seconds, read-only       */
#define LOOP_PID_TRACKMODE 0x060B /* loop.tracks enum 8|4                    */
/* Start-of-recording alignment (S24). Both delay the moment recording
 * actually begins, so a take lands on the grid instead of wherever the
 * button was pressed:
 *   loop.sync    — wait for the sequencer clock's next downbeat. The clock
 *                  free-runs, so this works with the sequencer stopped.
 *   loop.countin — click four beats first, then start on the downbeat.
 * With both on the count-in runs and recording starts at its end. Neither
 * does anything if the seq/arp clock is unavailable. */
#define LOOP_PID_SYNC    0x060C /* loop.sync    bool                          */
#define LOOP_PID_COUNTIN 0x060D /* loop.countin bool                          */
#define LOOP_PID_ARMED   0x060E /* loop.armed   int, read-only: beats to go   */

/* Track storage for the *next* set (S31), latched by its first take like
 * loop.mono and loop.tracks. psram holds every track whole and is bounded by
 * the free pool (loop.maxlen); sd streams the tracks off the card through
 * small PSRAM windows, so the length is bounded by the card instead — at the
 * cost of the card having to keep up (loop_stream.h). Registered only on a
 * build with the SD store backend, so the app's switch is absent rather than
 * dead where it cannot work. */
#define LOOP_PID_STORE  0x060F /* loop.store  enum psram|sd                  */

#define LOOP_PID_LEVEL(t) (0x0610 + (t)) /* loop.lvl1..8, t = 0..7           */

#define LOOP_TRACKS 8

/* Registers the 0x06xx params, hooks the ParamStore listener and starts the
 * loop_ctl task (owns all buffer allocation). Call before audio_io_start()
 * (the S9 registration-race rule). No-op without PSRAM support. */
esp_err_t looper_init(void);

/* Record tap + playback mix, in place after the FX bus. Audio task only —
 * no locks, no allocation (buffers come from loop_ctl ahead of time). */
void looper_process(float* l, float* r, size_t frames);

/* ---- reading a recorded track back out (S33) ------------------------------
 *
 * What the app's "download this track as a WAV" needs, and the reason it is a
 * pair of calls here rather than a reach into loop_store: a track's bytes live
 * in one of three places depending on how it was recorded and whether it has
 * been saved — PSRAM for a live in-memory set, /sd/osynth/liveN.olt for a live
 * streamed one, a slot blob (flash or SD) for a saved one — and only loop_ctl
 * is in a position to know which, or to touch any of them. So these marshal
 * onto loop_ctl and block until it answers; call them from any task *except*
 * loop_ctl, one at a time (an internal mutex enforces the second part).
 *
 * The bytes come out in the codec they are stored in, uninterpreted. The
 * caller is a decoder either way, and re-encoding on an ESP32 to save a client
 * the same work would only cost audio quality for nothing.
 *
 * The transport is *not* stopped for this and the audio task is not detached:
 * a read is a read. Two things are refused rather than raced (ESP_ERR_
 * INVALID_STATE): any export while a take is open, since the buffer or the
 * file is being written under it, and a *slot* export on the flash backend
 * while the transport runs, which is the same XIP-stall rule save and load
 * already answer to (loop_store_needs_stopped).
 *
 * The block is bounded, and ESP_ERR_TIMEOUT is a normal answer: loop_ctl
 * services this at the end of a pass that may first run a whole set load off
 * a card that has stopped responding, and the caller is the same task that
 * flushes BLE parameter events. Treat it as "not now" and retry.
 *
 * That bound puts one requirement on the caller: **`dst` must outlive the
 * call.** A request that times out is abandoned, not cancelled — loop_ctl may
 * still be about to write into it — so it cannot be a stack buffer in a frame
 * that is about to go away. A static or a kept allocation is what this wants
 * (ble_ctrl's dump window is one). Nothing else the caller owns is written:
 * the info reply lands in looper-owned storage and is copied out on success. */
#define LOOPER_EXPORT_LIVE 0 /* the set that is loaded now */
#define LOOPER_EXPORT_SLOT 1 /* a saved slot (flash: 0 only; SD: 0..7) */

/* Mirrors loop_store's codec numbering, which is where the values come from
 * for a slot; a live set is always one of the two ADPCM packings. */
#define LOOPER_EXPORT_CODEC_RAW 0        /* legacy v1 slot: raw s16 stereo */
#define LOOPER_EXPORT_CODEC_ADPCM 1      /* stereo, one byte per frame */
#define LOOPER_EXPORT_CODEC_ADPCM_MONO 2 /* mono, two frames per byte */

typedef struct {
    uint32_t loop_frames; /* the loop length every track shares */
    uint32_t sample_rate;
    uint32_t track_bytes; /* a full pass in `codec`; a streamed track can be
                           * shorter, which reads report by ending early */
    uint8_t filled;       /* tracks that have audio; bit 0 = track 1 */
    uint8_t codec;        /* LOOPER_EXPORT_CODEC_* */
} looper_export_info_t;

/* What is there to download. An empty live set or an unused slot is not an
 * error: `filled` comes back 0 and the caller has its answer. */
esp_err_t looper_export_info(int source, int slot, looper_export_info_t* out);

/* `len` bytes at `offset` of track `track` (0-based). `*out_read` is what was
 * actually available: less than asked for — zero included — means the track
 * ended there, which a streamed take stopped mid-pass legitimately does. */
esp_err_t looper_export_read(int source, int slot, int track, uint32_t offset,
                             uint8_t* dst, uint32_t len, uint32_t* out_read);

#ifdef __cplusplus
}
#endif
