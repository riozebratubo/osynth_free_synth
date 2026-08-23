#!/usr/bin/env python3
"""S41: close the one-pass gap after a streamed (SD) punch-in.

A take ends exactly at a wrap and IMA-ADPCM only lets a track join there, so
the window for the track's next pass has to exist *at* the instant the take
closes. It cannot be built then — the file is still live.tmp, the tail is
still in the record ring, and close+rename+open+prime is card I/O — so the
punch-in stayed silent for one whole pass, every time.

This stages the window during the take instead: loop_io reads the opening of
live.tmp back out (it wrote it a pass ago) into the track's play ring, the
audio task takes that ring at the wrap instead of holding the track, and
loop_ctl adopts the live ring rather than resetting it.

Idempotent-ish: every replacement asserts its anchor appears exactly once.
Run from the repo root.
"""
import io
import sys

def patch(path, subs):
    s = io.open(path, encoding="utf-8").read()
    for old, new in subs:
        n = s.count(old)
        assert n == 1, f"{path}: anchor appears {n}x, expected 1:\n{old[:200]}"
        s = s.replace(old, new, 1)
    io.open(path, "w", encoding="utf-8", newline="").write(s)
    print(f"patched {path} ({len(subs)} edit(s))")

# --------------------------------------------------------------- header ----
patch("components/looper/loop_stream.h", [(
"""extern std::atomic<uint8_t> g_resync;
extern std::atomic<uint8_t> g_hold;
""",
"""extern std::atomic<uint8_t> g_resync;
extern std::atomic<uint8_t> g_hold;

/* Two more, for the punch-in handoff (S41).
 *
 * A take ends exactly at a wrap, and IMA-ADPCM only lets a track join there,
 * so the window for that track's *next* pass has to already exist at the
 * instant the take closes. It cannot be built then: the file is still
 * live.tmp, the tail is still in the record ring, and close + rename + open +
 * prime is card I/O measured in tens of milliseconds. What that cost was a
 * punch-in nobody could hear until the pass after the one it ended on — every
 * time, not occasionally.
 *
 * It is built *during* the take instead. The bytes needed are the take's
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
extern std::atomic<uint8_t> g_prejoin;
"""), (
"""esp_err_t loop_stream_add_track(int t);
""",
"""esp_err_t loop_stream_add_track(int t);

/* Streamed punch-in handoff (S41), loop_ctl only. True when the audio task
 * joined track `t`'s staged window at the closing wrap — i.e. the track is
 * audible already and its ring must not be reset. */
bool loop_stream_prejoined(int t);

/* Adopts a joined window: opens the renamed track file, seeks to where the
 * staging stopped, and resumes the ordinary refill — leaving the ring and the
 * audio task's read cursor exactly where they are. The counterpart of
 * loop_stream_add_track() for a track that is already playing. On failure the
 * track is held (so it goes quiet within a block and rejoins at the next
 * wrap) and the caller can fall back to add_track. */
esp_err_t loop_stream_adopt_prearmed(int t);
""")])
