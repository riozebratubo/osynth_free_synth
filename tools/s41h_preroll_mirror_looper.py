#!/usr/bin/env python3
"""S41h part 3: looper.cpp — hand the mirrored pre-roll over at the wrap."""
import io

P = "components/looper/looper.cpp"
s = io.open(P, encoding="utf-8").read()


def sub(old, new):
    global s
    n = s.count(old)
    assert n == 1, f"anchor {n}x, expected 1:\n{old[:200]}"
    s = s.replace(old, new, 1)


sub("""/* Streamed punch-in: take the window loop_io staged during the take rather
 * than hold the track for a pass (loop_stream.h, g_prearm). Returns false when
 * there is nothing staged, and the caller then holds the track exactly as it
 * always did — a loop too short to reach the staging threshold, a first take
 * that had no length to stage against, a card that would not sync.
 *
 * Called only from the wrap, which is the one place that knows a take just
 * closed cleanly on a pass boundary. Every other way out of a take leaves
 * g_prearm untaken, so they all keep the original path.
 *
 * A take whose record ring overran is refused here rather than left to
 * loop_ctl. The staged bytes are real — they are the opening of the take, and
 * the overrun happened later — but the take as a whole is about to be thrown
 * away, and playing its first few milliseconds before loop_ctl catches up
 * would be audible. */
inline bool audio_join_prearm(int t) {
#if SYNTH_LOOP_STREAM
    if (!s_streamed.load(std::memory_order_relaxed)) return false;
    if (s_stream_overrun.load(std::memory_order_relaxed)) return false;
    namespace ls = osynth::loopstream;
    const uint8_t bit = (uint8_t)(1u << t);
    if ((ls::g_prearm.load(std::memory_order_acquire) & bit) == 0) return false;
    /* a_starved and a_win_off were cleared for every track a few lines above,
     * which is exactly the state a window sitting at the pass start needs. */
    ls::g_prejoin.fetch_or(bit, std::memory_order_release);
    /* The one place the audio task lifts a hold of its own, at the one instant
     * the rule is about: these bytes are this take's opening, not the replaced
     * file's, so this is not the stale-audio case g_hold guards. */
    ls::g_hold.fetch_and((uint8_t)~bit, std::memory_order_release);
    return true;
#else
    (void)t;
    return false;
#endif
}""",
    """/* Streamed punch-in: hand the pre-roll this task has been mirroring all take
 * over as track `t`'s playback window, instead of holding the track for a
 * whole pass (loop_stream.h). Returns false when there is no pre-roll, and the
 * caller then holds the track exactly as it always did — PSRAM mode, or a
 * first take, which has no pass to mirror against and no loop to punch into.
 *
 * Called only from the wrap, which is the one place that knows a take just
 * closed cleanly on a pass boundary. Every other way out of a take leaves the
 * pre-roll untaken, so they all keep the original path.
 *
 * A take whose record ring overran is refused here rather than left to
 * loop_ctl. The mirrored bytes are real — they are the opening of the take,
 * and the overrun happened later — but the take as a whole is about to be
 * thrown away, and playing its first seconds before loop_ctl catches up would
 * be audible.
 *
 * Writing g_play[t].wr from the audio task is the one moment it may: loop_io
 * gave this ring up when the take opened (loop_stream_open_record drops the
 * track from s_playing) and does not touch it again until
 * loop_stream_adopt_prearmed() hands it back. This is the handover itself. */
inline bool audio_join_prearm(int t) {
#if SYNTH_LOOP_STREAM
    if (!s_streamed.load(std::memory_order_relaxed)) return false;
    if (s_stream_overrun.load(std::memory_order_relaxed)) return false;
    namespace ls = osynth::loopstream;
    const uint32_t cap = ls::g_pre_cap.load(std::memory_order_acquire);
    if (cap == 0) return false;
    /* g_rec.wr was reset when the take opened, so it counts this take's bytes;
     * a punch-in that reached the wrap wrote exactly one pass of them. */
    uint32_t staged = ls::g_rec.wr.load(std::memory_order_relaxed);
    if (staged > cap) staged = cap;
    if (staged == 0) return false;
    const uint8_t bit = (uint8_t)(1u << t);
    ls::g_play[t].rd.store(0, std::memory_order_relaxed);
    ls::g_play[t].wr.store(staged, std::memory_order_release);
    /* Before g_prejoin, which is what lets loop_ctl read it. */
    ls::g_preroll.store(staged, std::memory_order_release);
    /* a_starved and a_win_off were cleared for every track a few lines above,
     * which is exactly the state a window sitting at the pass start needs. */
    ls::g_prejoin.fetch_or(bit, std::memory_order_release);
    /* The one place the audio task lifts a hold of its own, at the one instant
     * the rule is about: these bytes are this take's opening, not the replaced
     * file's, so this is not the stale-audio case g_hold guards. */
    ls::g_hold.fetch_and((uint8_t)~bit, std::memory_order_release);
    return true;
#else
    (void)t;
    return false;
#endif
}""")

sub("""                    /* streamed: loop_io normally staged this track's next
                     * window during the take, and joining it is what lets the
                     * punch-in be heard from this pass instead of the one
                     * after. Failing that, its file is being replaced and
                     * there is no reader until loop_ctl re-opens it (the
                     * a_starved reset just above cleared the hold this track
                     * was already under). */""",
    """                    /* streamed: the take's opening has been mirrored into
                     * this track's own window all along, and taking it here is
                     * what lets the punch-in be heard from this pass instead
                     * of the one after. Failing that, its file is being
                     * replaced and there is no reader until loop_ctl re-opens
                     * it (the a_starved reset just above cleared the hold this
                     * track was already under). */""")

# The file header paragraph named loop_io as the stager.
sub(""" * already exist at that instant. Building it there is card I/O — close,
 * rename, open, prime — and while that was where it happened, the track the
 * user had just recorded stayed silent until the pass after. loop_io stages
 * the window during the take instead, and audio_join_prearm() takes it.""",
    """ * already exist at that instant. Building it there is card I/O — close,
 * rename, open, prime — and while that was where it happened, the track the
 * user had just recorded stayed silent until the pass after. So the audio
 * task mirrors the take's opening into that window as it encodes it (the
 * bytes are already in its hand; fetching them back off the card was the
 * mistake), and audio_join_prearm() hands the window over at the wrap.""")

io.open(P, "w", encoding="utf-8", newline="").write(s)
print("patched", P)
