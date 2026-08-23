#!/usr/bin/env python3
"""S41h: replace the read-back pre-arm with a pre-roll the audio task mirrors.

s41g staged the punch-in's next window by reading the take back off the card
(fsync + a second handle on live.tmp). It worked, but it staged only whatever
had been written when it first fired — ~340 ms — and the field promptly found
a card where loop_ctl's close + rename + open took longer than that:

    W (98873) looper: sd cannot keep up: 1 window underrun(s)

The bytes were never worth fetching: the audio task encoded them itself, a
pass earlier. So it mirrors them into the track's own play window as it goes.
No fsync, no second handle, no short-read failure mode, and the window ends up
as full as the ring allows instead of as full as the timing happened to make
it. The ring also grows 64 -> 128 KB, which turns ~1.3 s of runway into ~2.6 s
and gives ordinary playback the same slack.

Run from the repo root.
"""
import io

# --------------------------------------------------------------------------
H = "components/looper/loop_stream.h"
h = io.open(H, encoding="utf-8").read()


def hsub(old, new):
    global h
    n = h.count(old)
    assert n == 1, f"{H}: anchor {n}x, expected 1:\n{old[:200]}"
    h = h.replace(old, new, 1)


hsub("""/* Bytes of PSRAM per track window. At one byte per stereo frame this is
 * ~1.4 s of cushion (~2.8 s mono) — enough to ride out the ~250 ms stalls a
 * card can take for internal housekeeping even with all eight tracks and an
 * overdub competing for the bus. Eight of these plus the record ring is
 * ~576 KB, which is the whole PSRAM cost of a set of any length. */
#define LOOP_STREAM_RING_BYTES (64u * 1024u)""",
     """/* Bytes of PSRAM per track window. At one byte per stereo frame this is
 * ~2.7 s of cushion (~5.5 s mono) — enough to ride out the ~250 ms stalls a
 * card can take for internal housekeeping even with all eight tracks and an
 * overdub competing for the bus. Eight of these plus the record ring is
 * ~1.15 MB, which is the whole PSRAM cost of a set of any length.
 *
 * It was 64 KB until S41, and the thing that moved it was not ordinary
 * playback but the punch-in pre-roll below: that runway is bounded by this
 * ring, and 64 KB of it (~1.3 s) was close enough to what loop_ctl's close +
 * rename + open costs on a slow card to be worth doubling. Ordinary playback
 * gets the same slack for free. What it costs, beyond the PSRAM: a set being
 * loaded primes every window before the transport is allowed to run, so
 * loop_stream_start_playback() now reads ~1 MB rather than ~0.5 MB before the
 * first sample. */
#define LOOP_STREAM_RING_BYTES (128u * 1024u)""")

hsub("""/* Two more, for the punch-in handoff (S41).""",
     """/* One more, for the punch-in handoff (S41), plus the pre-roll below.""")

hsub(""" * It is built *during* the take instead. The bytes needed are the take's
 * opening, which the audio task produced a whole pass earlier and loop_io has
 * long since written, so loop_io reads them back out of live.tmp and stages
 * them in the track's play window while the take is still running.
 *
 * g_prearm — loop_io -> audio. "Track t's window holds the opening of the
 * take now recording it." Nothing acts on it until the take closes, so a
 * half-built window is never something the audio task can find.
 *
 * g_prejoin — audio -> loop_ctl. "I took that window at the wrap instead of
 * holding the track, and I am playing out of it now." It is what tells
 * loop_ctl to *adopt* the live ring — open the renamed file, seek to where
 * the staging stopped — rather than reset it the way loop_stream_add_track()
 * does. Resetting a ring under a live reader is the thing this whole split
 * exists to prevent.
 *
 * g_hold stays loop_ctl's to lift, with one exception, at the one instant the
 * rule is actually about: the audio task clears its own hold at the closing
 * wrap when g_prearm says the window is staged. That is not the stale-audio
 * case the rule guards against — the staged bytes are the new take's, not the
 * replaced file's. Every other way out of a take leaves g_prearm untaken and
 * keeps the original path. */
extern std::atomic<uint8_t> g_prearm;
extern std::atomic<uint8_t> g_prejoin;""",
     """ * It is built *during* the take instead, and by the one task that already
 * has the bytes: the audio task encodes each one into the record ring, and
 * mirrors the opening of the take into that track's own play window on the
 * way past. Nothing is read back off the card, so there is no fsync, no
 * second handle on live.tmp, and no way for the window to be shorter than the
 * timing of a refill tick happened to make it — it is always as full as the
 * ring allows, or the whole pass when the loop is shorter than that.
 *
 * The mirror is safe because the window has exactly one producer at a time:
 * loop_stream_open_record() drops the track from s_playing and closes its
 * reader, so loop_io stops touching that ring for the length of the take, and
 * the render path has skipped the track being recorded since before ADPCM.
 * At the wrap the audio task publishes the count and it becomes an ordinary
 * window with an ordinary reader behind it.
 *
 * g_prejoin — audio -> loop_ctl. "I took the pre-roll at the wrap instead of
 * holding the track, and I am playing out of it now." It is what tells
 * loop_ctl to *adopt* the live ring — open the renamed file, seek past what
 * the pre-roll already covers — rather than reset it the way
 * loop_stream_add_track() does. Resetting a ring under a live reader is the
 * thing this whole split exists to prevent.
 *
 * g_hold stays loop_ctl's to lift, with one exception, at the one instant the
 * rule is actually about: the audio task clears its own hold at the closing
 * wrap when it has a pre-roll to hand over. That is not the stale-audio case
 * the rule guards against — those bytes are the new take's, not the replaced
 * file's. Every other way out of a take leaves the pre-roll untaken and keeps
 * the original path. */
extern std::atomic<uint8_t> g_prejoin;

/* The pre-roll itself. g_pre_buf is the recording track's window and g_pre_cap
 * how much of it to mirror, both published by loop_ctl in
 * loop_stream_open_record() before the record command reaches the audio task
 * and cleared only once the take is over — the same before-the-command rule
 * the PSRAM track buffers follow. g_pre_cap == 0 disables the mirror, which is
 * the state for a first take (no pass length to mirror against) and for PSRAM
 * mode, and is why the hot path costs one compare rather than a branch into
 * anything.
 *
 * g_preroll is written once, by the audio task at the wrap: how many bytes the
 * mirror actually covered, which is where loop_ctl seeks the new reader to. */
extern std::atomic<uint8_t*> g_pre_buf;
extern std::atomic<uint32_t> g_pre_cap;
extern std::atomic<uint32_t> g_preroll;""")

hsub("""inline void rec_put(uint8_t b) {
    const uint32_t w = g_rec.wr.load(std::memory_order_relaxed);
    g_rec.buf[w % LOOP_STREAM_RING_BYTES] = b;
    g_rec.wr.store(w + 1, std::memory_order_release);
}""",
     """inline void rec_put(uint8_t b) {
    const uint32_t w = g_rec.wr.load(std::memory_order_relaxed);
    g_rec.buf[w % LOOP_STREAM_RING_BYTES] = b;
    /* Pre-roll mirror. g_rec.wr is reset when the take opens, so it is also
     * the take's byte offset from the pass start — which is exactly the index
     * this byte will be read back at. */
    if (w < g_pre_cap.load(std::memory_order_relaxed)) {
        g_pre_buf.load(std::memory_order_relaxed)[w] = b;
    }
    g_rec.wr.store(w + 1, std::memory_order_release);
}""")

hsub("""inline void rec_or_last(uint8_t bits) {
    const uint32_t w = g_rec.wr.load(std::memory_order_relaxed);
    g_rec.buf[(w - 1u) % LOOP_STREAM_RING_BYTES] |= bits;
}""",
     """inline void rec_or_last(uint8_t bits) {
    const uint32_t w = g_rec.wr.load(std::memory_order_relaxed);
    g_rec.buf[(w - 1u) % LOOP_STREAM_RING_BYTES] |= bits;
    if (w - 1u < g_pre_cap.load(std::memory_order_relaxed)) {
        g_pre_buf.load(std::memory_order_relaxed)[w - 1u] |= bits;
    }
}""")

hsub(""" * loop_stream_add_track() for a track that is already playing. On failure the
 * track is held (so it goes quiet within a block and rejoins at the next
 * wrap) and the caller can fall back to add_track. */""",
     """ * loop_stream_add_track() for a track that is already playing. On failure the
 * track is held (so it goes quiet within a block and rejoins at the next
 * wrap) and the caller can fall back to add_track. */""")

io.open(H, "w", encoding="utf-8", newline="").write(h)
print("patched", H)
