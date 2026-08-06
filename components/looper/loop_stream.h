/*
 * osynth — SD-streamed looper tracks (S31, private to the looper component).
 *
 * The PSRAM looper holds every track whole, so the loop length is bounded by
 * the free pool (looper.cpp's cap policy — ~40 s base on an 8 MB S3). This
 * backend keeps the tracks in files on the card and moves only a small
 * window of each through PSRAM, so the length is bounded by the card
 * instead. loop.store picks between them; it is a property of the *next*
 * set, latched by its first take exactly like loop.mono and loop.tracks —
 * the two cannot be mixed within one set, and switching with a set loaded
 * would mean rewriting every track.
 *
 * Ownership follows the house contract (looper.cpp's header comment):
 *  - The audio task never touches the card, never blocks and never
 *    allocates. It reads and writes PSRAM rings through the inline
 *    accessors at the bottom of this header, which are index arithmetic
 *    only — safe from the IRAM render path.
 *  - The `loop_io` task owns every file handle and does all the I/O. It
 *    runs on core 0 above loop_ctl: refilling a window is what keeps the
 *    render path fed, so it must not wait behind param mirroring, but it
 *    must never outrank the audio task on core 1.
 *
 * Each direction is a single-producer/single-consumer ring with two
 * monotonic byte counters (`wr` produced, `rd` consumed); available =
 * wr - rd, and the ring offset is the counter modulo the ring size. The
 * counters are plain uint32 and are *meant* to wrap — every use is a
 * difference, so a wrap at 4 GB (~24 h of one stereo track) cancels out.
 * That is the same reasoning as ctl_handshake()'s sequence compare.
 *
 * Underruns are not glitches, they are desyncs. IMA-ADPCM has no
 * re-entry point mid-stream (loop_adpcm.h), so a track whose window runs
 * dry cannot simply skip the missing bytes and carry on — its decoder
 * would be wrong for the rest of the pass. The audio task marks such a
 * track starved and mutes it until the next loop wrap, where every decoder
 * resets anyway. Starvation is therefore audible as a track dropping out
 * for one pass, never as noise, and loop_stream_underruns() counts them so
 * a card that cannot keep up says so instead of being mysterious.
 */
#pragma once

#include <atomic>
#include <stdint.h>

#include "esp_err.h"

#include "looper.h"
#include "synth_config.h"

/* Streamed mode needs all three: PSRAM for the windows, the persistence
 * feature, and the SD store backend (the flash backend has nothing to
 * stream from). Everything below compiles away when it is 0, and
 * loop.store is not registered — so the app's switch disappears on a
 * build that cannot offer it, the same way loop.save does. */
#if CONFIG_SPIRAM && SYNTH_ENABLE_LOOP_PERSIST && SYNTH_LOOP_STORE_SD
#define SYNTH_LOOP_STREAM 1
#else
#define SYNTH_LOOP_STREAM 0
#endif

/* Bytes of PSRAM per track window. At one byte per stereo frame this is
 * ~1.4 s of cushion (~2.8 s mono) — enough to ride out the ~250 ms stalls a
 * card can take for internal housekeeping even with all eight tracks and an
 * overdub competing for the bus. Eight of these plus the record ring is
 * ~576 KB, which is the whole PSRAM cost of a set of any length. */
#define LOOP_STREAM_RING_BYTES (64u * 1024u)

/* Longest loop the streamed backend will record. Not a buffer limit — it
 * bounds loop.len / loop.pos / loop.maxlen's registered ranges (a control
 * surface needs a finite max) and stops a forgotten armed take from filling
 * the card. 30 min is ~85 MB stereo, ~42 MB mono. */
#define LOOP_STREAM_MAX_S 1800.0f

namespace osynth::loopstream {

/* One direction of one track's ring. Producer writes `wr`, consumer writes
 * `rd`; neither ever writes the other's counter, which is what makes this
 * safe without a lock. */
struct Ring {
    uint8_t* buf;              /* PSRAM, LOOP_STREAM_RING_BYTES */
    std::atomic<uint32_t> wr;  /* bytes produced, monotonic (wraps) */
    std::atomic<uint32_t> rd;  /* bytes consumed, monotonic (wraps) */
};

/* Playback windows, one per track; `rec` is the record direction (the audio
 * task produces, loop_io drains to the card). Defined in loop_stream.cpp;
 * declared here so the render path's accessors can inline. */
extern Ring g_play[LOOP_TRACKS];
extern Ring g_rec;

/* Counted by the audio task when a window runs dry (see the header comment
 * on why a starved track mutes rather than skips). Read for telemetry. */
extern std::atomic<uint32_t> g_underruns;

/* Two mute masks with two different owners. Both keep a track silent, and
 * they are separate because they end on different events — collapsing them
 * let loop_io clear a hold that only loop_ctl was in a position to lift.
 *
 * g_resync — audio -> loop_io. "My window ran dry; re-prime it from the
 * loop start." loop_io seeks, refills and clears the bit. No notify: the
 * refill pass already runs every 20 ms, and a track that has just dropped
 * out for a whole pass is not helped by shaving one tick off that.
 *
 * g_hold — audio -> loop_ctl. "A take just replaced this track's file, so
 * the open reader describes audio that no longer exists." Only loop_ctl can
 * lift it, because only loop_ctl knows when the new file has been renamed
 * into place (loop_stream_add_track). loop_io must leave it alone: re-priming
 * from the *old* file here is precisely the stale-audio bug this split
 * exists to prevent. */
extern std::atomic<uint8_t> g_resync;
extern std::atomic<uint8_t> g_hold;

/* ---- audio-task accessors: index arithmetic only, no I/O, no locks ---- */

/* Bytes the audio task may still read from track `t`'s window. */
inline uint32_t play_avail(int t) {
    const Ring& r = g_play[t];
    return r.wr.load(std::memory_order_acquire) -
           r.rd.load(std::memory_order_relaxed);
}

/* Byte at `off` bytes past the read cursor. The caller must have checked
 * play_avail() first — this does no bounds work beyond the ring wrap. */
inline uint8_t play_at(int t, uint32_t off) {
    const Ring& r = g_play[t];
    const uint32_t pos = r.rd.load(std::memory_order_relaxed) + off;
    return r.buf[pos % LOOP_STREAM_RING_BYTES];
}

/* Releases `n` bytes of track `t`'s window back to loop_io. */
inline void play_consume(int t, uint32_t n) {
    Ring& r = g_play[t];
    r.rd.store(r.rd.load(std::memory_order_relaxed) + n,
               std::memory_order_release);
}

/* Room left in the record ring, in bytes. */
inline uint32_t rec_space() {
    return LOOP_STREAM_RING_BYTES -
           (g_rec.wr.load(std::memory_order_relaxed) -
            g_rec.rd.load(std::memory_order_acquire));
}

/* Appends one byte to the record ring. Call rec_space() first: overrunning
 * would overwrite bytes loop_io has not written to the card yet. */
inline void rec_put(uint8_t b) {
    const uint32_t w = g_rec.wr.load(std::memory_order_relaxed);
    g_rec.buf[w % LOOP_STREAM_RING_BYTES] = b;
    g_rec.wr.store(w + 1, std::memory_order_release);
}

/* Read-modify-write of the byte at the head of the record ring, for mono's
 * two-frames-per-byte packing: the high nibble is written by rec_put(), the
 * low nibble ORed in by the next frame without advancing the cursor again. */
inline void rec_or_last(uint8_t bits) {
    const uint32_t w = g_rec.wr.load(std::memory_order_relaxed);
    g_rec.buf[(w - 1u) % LOOP_STREAM_RING_BYTES] |= bits;
}

} // namespace osynth::loopstream

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the loop_io task and allocates the rings. Called from looper_init()
 * only when the SD store backend is compiled in; a failure here leaves the
 * streamed mode unavailable (loop_stream_ready() false) and the looper falls
 * back to PSRAM, which is always present. */
esp_err_t loop_stream_init(void);

/* True when the rings exist, the task runs and a card was present at the
 * last probe — i.e. loop.store may be set to sd. Pure read: safe from any
 * task and cheap enough for the loop.maxlen mirror, which runs on every
 * toggle. It can therefore be one probe stale; loop_stream_probe_card()
 * below is what refreshes it, and a set that starts on a card that has since
 * gone away falls back to PSRAM with a log line either way. */
bool loop_stream_ready(void);

/* Mounts the card if it is not mounted (lazily, via loop_store_mount) and
 * refreshes what loop_stream_ready() reports. Blocks on the card, and shares
 * loop_store's single-logical-caller rule — call it from loop_ctl, or from
 * looper_init before the loop_ctl task can reach a card path. */
bool loop_stream_probe_card(void);

/* ---- set lifecycle (loop_ctl only; all of these block on the card) ---- */

/* Opens a fresh streamed set: removes /sd/osynth/live*.olt and live.tmp and
 * forgets any track state (the save slots in the same directory are never
 * touched). `mono` latches the frame packing for the whole set. */
esp_err_t loop_stream_begin_set(bool mono);

/* Drops the streamed set and its scratch files. Safe to call when no set is
 * open; the audio task must already be detached (kCmdDetach + handshake),
 * exactly as ctl_release_all() requires for PSRAM buffers. */
void loop_stream_end_set(void);

/* ---- a set that outlives the session ----
 *
 * The track files are nibbles and nothing else, so on their own they cannot
 * say how long the loop was, whether it was mono, or which of the eight were
 * ever recorded. A small manifest beside them carries exactly those three,
 * which is what lets a set on a card be picked up again after a reboot — or
 * after being carried to another osynth.
 *
 * Written whenever the set's shape changes (see ctl_mirror_state), removed
 * with the rest of the scratch when a set ends. A loop length of 0 or an
 * empty filled mask removes it: there is nothing to come back to. */
void loop_stream_save_manifest(uint32_t loop_frames, bool mono, uint8_t filled);

/* Reads it back, and checks it against the card: magic, version, sample rate,
 * a sane length, and — because the manifest and the tracks are separate files
 * that a pulled card can leave disagreeing — that every track it claims is
 * actually present and non-empty. False if any of that fails, in which case
 * the outputs are untouched and the card should be treated as having no set. */
bool loop_stream_load_manifest(uint32_t* loop_frames, bool* mono,
                               uint8_t* filled);

/* Takes a set that is already on the card and makes it the live one: opens
 * every filled track and primes its window, *without* the wipe begin_set does.
 * The caller is expected to have got its arguments from
 * loop_stream_load_manifest(). Fails if any track will not open, leaving no
 * set behind. loop_ctl only, and only with the transport stopped. */
esp_err_t loop_stream_adopt_set(uint32_t loop_frames, bool mono,
                                uint8_t filled);

/* ---- surviving a card that is pulled while idle ----
 *
 * A streamed set's tracks are files, so a card leaving the slot does not
 * destroy them — it only makes them unreachable. The set is therefore
 * suspended rather than cleared: the length, the format and the filled mask
 * all stand, and re-inserting the same card brings the audio back.
 *
 * Suspend closes every handle (which is what makes it safe for loop_store to
 * unmount afterwards — the unmount frees the FATFS context those handles point
 * into) and mutes every track through g_hold, so the render path stays silent
 * on them without counting underruns. It does not remove the scratch files:
 * they are the set. Call it before loop_store_card_gone(), with the same
 * detach requirement as loop_stream_end_set(). */
void loop_stream_suspend_set(void);

/* True between suspend and a successful resume. */
bool loop_stream_suspended(void);

/* Re-opens a suspended set's tracks and lets the audio task have them again.
 * Fails — and the caller should then clear the set — if the card now in the
 * slot is not the one the files were written to (CID serial, checked because
 * a stale live set on a different card would otherwise play as this one), or
 * if any of `filled`'s tracks is missing from it.
 *
 * The transport must be stopped. Priming a window is a ring reset, and doing
 * that under a live reader is exactly what loop_stream_add_track() exists to
 * avoid; there is no equivalent dance here because a suspended set has nothing
 * to add to. */
bool loop_stream_resume_set(uint8_t filled);

/* Prepares track `t` to be recorded. `loop_frames` is the known loop length,
 * or 0 for a first take (length decided when it closes). Writes land in a
 * temp file and only replace the track's own file at close, so a cancelled
 * or torn punch-in cannot leave half a pass on the card. */
esp_err_t loop_stream_open_record(int t, uint32_t loop_frames);

/* Appends `n` bytes to the open take, after everything the audio task left in
 * the record ring. This is the tail loop_ctl encodes when the transport stops
 * a punch-in before its pass ends (looper.cpp): the take keeps what it
 * recorded and fades to digital silence, and the rest of the pass is
 * deliberately *not* written — a file shorter than the pass plays as silence
 * from where it ends. Call before loop_stream_close_record(). */
esp_err_t loop_stream_pad_record(const uint8_t* bytes, uint32_t n);

/* Finishes the take on `t`. `keep` false discards it (a punch-in that
 * recorded nothing, too-short first take) and leaves the previous content in
 * place. Blocks until the record ring has been drained to the card. */
esp_err_t loop_stream_close_record(int t, bool keep);

/* Publishes the set's loop length once the first take has closed, and opens
 * every filled track for playback (seek to 0, prime the windows). Playback
 * must not start until this returns. */
esp_err_t loop_stream_start_playback(uint32_t loop_frames, uint8_t filled);

/* Publishes the loop length once the first take has decided it. Everything
 * that reads or wraps a file needs it, and until it is set no track can be
 * opened for playback. Idempotent; changing it under a running set is not
 * meaningful and is ignored. */
void loop_stream_set_length(uint32_t loop_frames);

/* Brings a single track into the playing set with the transport running —
 * the track a punch-in (or the first take) has just written. The whole
 * operation happens with the track's resync bit set, so the audio task
 * leaves it muted throughout and only picks it up at the next wrap, where
 * its window and the transport are both at the loop start. This is the only
 * safe way to add a reader under a live transport: loop_stream_rewind() and
 * loop_stream_start_playback() both reset rings the audio task may be
 * reading, so they need it detached. */
esp_err_t loop_stream_add_track(int t);

/* Lifts track `t`'s hold without opening anything — the counterpart to
 * add_track for a take that was discarded. The track stays out of the
 * playing set (it is empty); this just stops it being muted forever. */
void loop_stream_release_track(int t);

/* Re-primes every window from the loop start. loop_ctl calls this whenever
 * the transport is repositioned to the top with the audio detached (stop,
 * a punch-in that changes the filled set) so the first pass after it is
 * never served from a stale window. */
esp_err_t loop_stream_rewind(void);

/* Drops track `t`'s file and window (loop.clear on one track). */
void loop_stream_clear_track(int t);

/* Reads `len` bytes at `offset` out of the live set's track `t` for the app's
 * WAV export (S33). Its own handle, opened and closed inside the call: an
 * export runs at BLE speed, so the directory lookup is noise next to the link,
 * and a handle held across a whole transfer would be one more thing the
 * card-lost path has to collect before loop_store unmounts.
 *
 * `*out_read` is what was actually there. A track can legitimately be shorter
 * than the pass — a punch-in the transport stopped keeps what it recorded and
 * the player serves silence for the rest (looper.cpp, ctl_pad_take) — so a
 * short read, zero included, means end of track and not failure. loop_ctl
 * only, like everything else that reaches the card. */
esp_err_t loop_stream_export_read(int t, uint32_t offset, uint8_t* dst,
                                  uint32_t len, uint32_t* out_read);

/* Number of window underruns since boot, and the track bitmask that has
 * starved during the current pass. Both are telemetry for the "card cannot
 * keep up" log line; the mask is cleared by the audio task at each wrap. */
uint32_t loop_stream_underruns(void);

#ifdef __cplusplus
}
#endif
