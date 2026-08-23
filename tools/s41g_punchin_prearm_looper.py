#!/usr/bin/env python3
"""S41 punch-in pre-arm, part 4: looper.cpp (the audio task and loop_ctl).

See s41g_punchin_prearm.py for what the whole change is for.
"""
import io

P = "components/looper/looper.cpp"
s = io.open(P, encoding="utf-8").read()


def sub(old, new):
    global s
    n = s.count(old)
    assert n == 1, f"anchor {n}x, expected 1:\n{old[:200]}"
    s = s.replace(old, new, 1)


# ---- audio task: join the staged window instead of holding ---------------
sub("""inline int16_t f2i16(float v) {""",
    """/* Streamed punch-in: take the window loop_io staged during the take rather
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
}

inline int16_t f2i16(float v) {""")

sub("""                    /* streamed: its file is being replaced — no reader until
                     * loop_ctl re-opens it (the a_starved reset just above
                     * cleared the hold this track was already under) */
                    audio_hold_track(a_rec_trk);""",
    """                    /* streamed: loop_io normally staged this track's next
                     * window during the take, and joining it is what lets the
                     * punch-in be heard from this pass instead of the one
                     * after. Failing that, its file is being replaced and
                     * there is no reader until loop_ctl re-opens it (the
                     * a_starved reset just above cleared the hold this track
                     * was already under). */
                    if (!audio_join_prearm(a_rec_trk)) {
                        audio_hold_track(a_rec_trk);
                    }""")

# ---- loop_ctl: adopt the live ring rather than resetting it --------------
sub("""void ctl_finish_streamed_take() {
    if (s_ctl_rec_trk < 0) return;
    const int trk = s_ctl_rec_trk;
    s_ctl_rec_trk = -1;""",
    """void ctl_finish_streamed_take() {
    if (s_ctl_rec_trk < 0) return;
    const int trk = s_ctl_rec_trk;
    s_ctl_rec_trk = -1;
    /* Asked before close_record, which is where the answer gets consumed. */
    const bool joined = loop_stream_prejoined(trk);""")

sub("""    const uint32_t L = s_loop_frames.load(std::memory_order_acquire);
    if (L == 0) return;
    loop_stream_set_length(L);
    if (loop_stream_add_track(trk) != ESP_OK) {
        ESP_LOGW(TAG, "track %d: recorded but could not be re-opened to play",
                 trk + 1);
        s_filled.fetch_and((uint8_t)~(1u << trk), std::memory_order_release);
    }
}""",
    """    const uint32_t L = s_loop_frames.load(std::memory_order_acquire);
    if (L == 0) return;
    loop_stream_set_length(L);
    esp_err_t err;
    if (joined) {
        /* The audio task is already playing out of the staged window, so the
         * file just has to be opened behind it and the refill resumed — the
         * ring must not be reset, which is the one thing add_track does. */
        err = loop_stream_adopt_prearmed(trk);
        if (err != ESP_OK) {
            /* adopt held the track on its way out, so this is the ordinary
             * path again: a pass late, but audible. */
            ESP_LOGW(TAG, "track %d: staged window lost, re-opening", trk + 1);
            err = loop_stream_add_track(trk);
        }
    } else {
        err = loop_stream_add_track(trk);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "track %d: recorded but could not be re-opened to play",
                 trk + 1);
        s_filled.fetch_and((uint8_t)~(1u << trk), std::memory_order_release);
    }
}""")

io.open(P, "w", encoding="utf-8", newline="").write(s)
print("patched", P)
