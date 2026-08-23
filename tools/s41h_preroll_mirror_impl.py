#!/usr/bin/env python3
"""S41h part 2: loop_stream.cpp — drop the read-back staging, arm the mirror.

See s41h_preroll_mirror.py for why.
"""
import io

P = "components/looper/loop_stream.cpp"
s = io.open(P, encoding="utf-8").read()


def sub(old, new):
    global s
    n = s.count(old)
    assert n == 1, f"anchor {n}x, expected 1:\n{old[:200]}"
    s = s.replace(old, new, 1)


# fsync was only for the read-back that is going away.
sub("""#include <sys/stat.h>
#include <unistd.h> /* fsync — see maybe_prearm() */
""", """#include <sys/stat.h>
""")

sub("""std::atomic<uint8_t> g_prearm{0};
std::atomic<uint8_t> g_prejoin{0};
""",
    """std::atomic<uint8_t> g_prejoin{0};
std::atomic<uint8_t*> g_pre_buf{nullptr};
std::atomic<uint32_t> g_pre_cap{0};
std::atomic<uint32_t> g_preroll{0};
""")

sub("""using osynth::loopstream::g_prearm;
using osynth::loopstream::g_prejoin;""",
    """using osynth::loopstream::g_pre_buf;
using osynth::loopstream::g_pre_cap;
using osynth::loopstream::g_prejoin;
using osynth::loopstream::g_preroll;""")

sub("""/* How much of the take has to be on the card before its opening is staged.
 * ~340 ms of stereo: comfortably more runway than the close + rename + open
 * loop_ctl does at the wrap needs, and early enough in a take that it is
 * reached long before the pass ends. A loop shorter than this never stages
 * and keeps the original one-pass-late behaviour, which at a third of a
 * second is not a loop anyone is punching into. */
constexpr uint32_t kPreStageMin = 16u * 1024u;

FILE* s_pre_f = nullptr; /* read handle on live.tmp while staging */
/* Three states, not two: >= 0 is "staged, for that track", -1 is "not yet,
 * and worth trying", -2 is "this take already said no" — reset to -1 by
 * loop_stream_open_record() when the next take starts. */
int s_pre_trk = -1;
uint32_t s_pre_pos = 0; /* bytes of the pass staged into the window */""",
    """/* The track whose window the audio task is mirroring the take into, or -1.
 * Armed by loop_stream_open_record() and disarmed by drop_prearm(); how much
 * of it was actually covered is g_preroll, which only the audio task writes
 * and only at the wrap. */
int s_pre_trk = -1;""")

# drop_prearm: disarm the mirror rather than close a file.
sub("""void drop_prearm() {
    if (s_pre_trk < 0) return;
    const int t = s_pre_trk;
    const uint8_t bit = (uint8_t)(1u << t);
    if (s_pre_f != nullptr) {
        fclose(s_pre_f);
        s_pre_f = nullptr;
    }
    g_prearm.fetch_and((uint8_t)~bit, std::memory_order_release);
    if ((g_prejoin.load(std::memory_order_acquire) & bit) != 0) {""",
    """void drop_prearm() {
    if (s_pre_trk < 0) return;
    const int t = s_pre_trk;
    const uint8_t bit = (uint8_t)(1u << t);
    /* Stops the mirror first: everything below assumes the audio task is no
     * longer writing into this window. */
    g_pre_cap.store(0, std::memory_order_release);
    g_pre_buf.store(nullptr, std::memory_order_relaxed);
    if ((g_prejoin.load(std::memory_order_acquire) & bit) != 0) {""")

# maybe_prearm and its call go away entirely.
start = s.index("/* Stages the opening of the take in progress into its track's play window")
end = s.index("void io_task(void* arg) {")
s = s[:start] + s[end:]
sub("""            s_rec_trk = -1;
        }
        maybe_prearm();
""", """            s_rec_trk = -1;
        }
""")

# open_record: hand the window to the audio task for the length of the take.
sub("""    } else {
        s_rec_trk = t;
        s_rec_written = 0;
        s_pre_trk = -1; /* clears the sentinel a previous take may have left */
        ring_reset(g_rec);
        if (loop_frames != 0) s_pass_bytes = pass_bytes_for(loop_frames, s_mono);
    }""",
    """    } else {
        s_rec_trk = t;
        s_rec_written = 0;
        ring_reset(g_rec);
        if (loop_frames != 0) s_pass_bytes = pass_bytes_for(loop_frames, s_mono);
        /* Arm the punch-in pre-roll (loop_stream.h). The track's window has to
         * have exactly one producer while the take runs, so its reader is
         * closed and it leaves s_playing: loop_io will not touch this ring
         * again until loop_stream_adopt_prearmed() puts it back. Both are what
         * would have happened at close anyway — the file behind that reader is
         * the one the take is replacing.
         *
         * Not armed for the first take of a set: s_pass_bytes is still 0, so
         * there is no pass to mirror against — and no loop to punch into
         * either, which is the case this whole mechanism exists for. */
        drop_prearm();
        close_play(t);
        s_playing &= (uint8_t)~(1u << t);
        g_preroll.store(0, std::memory_order_relaxed);
        g_prejoin.fetch_and((uint8_t)~(1u << t), std::memory_order_release);
        if (s_pass_bytes != 0) {
            uint32_t cap = LOOP_STREAM_RING_BYTES - kIoChunk;
            if (cap > s_pass_bytes) cap = s_pass_bytes;
            s_pre_trk = t;
            g_pre_buf.store(g_play[t].buf, std::memory_order_relaxed);
            /* Published last: until the cap is non-zero the mirror is off, so
             * the audio task can never write through a stale buffer. */
            g_pre_cap.store(cap, std::memory_order_release);
        }
    }""")

# close_record: no tmp reader to close any more.
sub("""            if (joined) {
                /* That window is what the audio task is playing *now*, so it
                 * has to survive the rename that is about to make it real.
                 * Only the tmp reader goes; the ring, s_pre_pos and the audio
                 * task's cursor all stand, and loop_stream_adopt_prearmed()
                 * picks them up. This is the one path that must not call
                 * close_play(), which would reset the ring under it. */
                if (s_pre_f != nullptr) {
                    fclose(s_pre_f);
                    s_pre_f = nullptr;
                }
            } else {""",
    """            if (joined) {
                /* That window is what the audio task is playing *now*, so it
                 * has to survive the rename that is about to make it real —
                 * the ring, g_preroll and the audio task's cursor all stand,
                 * and loop_stream_adopt_prearmed() picks them up. This is the
                 * one path that must not call close_play(), which would reset
                 * the ring under a live reader. The mirror itself has already
                 * stopped: the take is over. */
                g_pre_cap.store(0, std::memory_order_release);
            } else {""")

# adopt: seek past what the pre-roll covered.
sub("""        uint32_t pos = s_pre_pos;
        if (pos >= s_pass_bytes) pos = 0;""",
    """        uint32_t pos = g_preroll.load(std::memory_order_acquire);
        if (pos >= s_pass_bytes) pos = 0;""")

sub("""            s_play_f[t] = f;
            s_play_pos[t] = pos;
            s_playing |= (uint8_t)(1u << t);
            s_pre_trk = -1;
            s_pre_pos = 0;
            g_prearm.fetch_and((uint8_t)~(1u << t), std::memory_order_release);
            g_prejoin.fetch_and((uint8_t)~(1u << t), std::memory_order_release);""",
    """            s_play_f[t] = f;
            s_play_pos[t] = pos;
            s_playing |= (uint8_t)(1u << t);
            s_pre_trk = -1;
            g_pre_cap.store(0, std::memory_order_release);
            g_pre_buf.store(nullptr, std::memory_order_relaxed);
            g_prejoin.fetch_and((uint8_t)~(1u << t), std::memory_order_release);""")

sub("""    if (s_pre_trk < 0) return;
    const int t = s_pre_trk;
    const uint8_t bit = (uint8_t)(1u << t);
    /* Stops the mirror first""",
    """    if (s_pre_trk < 0) return;
    const int t = s_pre_trk;
    const uint8_t bit = (uint8_t)(1u << t);
    s_pre_trk = -1;
    /* Stops the mirror first""")

sub("""        g_prejoin.fetch_and((uint8_t)~bit, std::memory_order_release);
    } else {
        ring_reset(g_play[t]);
    }
    s_pre_trk = -1;
    s_pre_pos = 0;
}""",
    """        g_prejoin.fetch_and((uint8_t)~bit, std::memory_order_release);
    } else {
        ring_reset(g_play[t]);
    }
}""")

io.open(P, "w", encoding="utf-8", newline="").write(s)
print("patched", P)
