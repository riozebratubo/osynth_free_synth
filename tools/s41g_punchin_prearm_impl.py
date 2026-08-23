#!/usr/bin/env python3
"""S41 punch-in pre-arm, part 2: loop_stream.cpp. See s41g_punchin_prearm.py."""
import io

P = "components/looper/loop_stream.cpp"
s = io.open(P, encoding="utf-8").read()

def sub(old, new):
    global s
    n = s.count(old)
    assert n == 1, f"anchor {n}x, expected 1:\n{old[:200]}"
    s = s.replace(old, new, 1)

# unistd.h for fsync()
sub("""#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
""",
"""#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h> /* fsync — see maybe_prearm() */
""")

sub("""std::atomic<uint8_t> g_resync{0};
std::atomic<uint8_t> g_hold{0};
""",
"""std::atomic<uint8_t> g_resync{0};
std::atomic<uint8_t> g_hold{0};
std::atomic<uint8_t> g_prearm{0};
std::atomic<uint8_t> g_prejoin{0};
""")

sub("""using osynth::loopstream::g_hold;
using osynth::loopstream::g_play;""",
"""using osynth::loopstream::g_hold;
using osynth::loopstream::g_play;
using osynth::loopstream::g_prearm;
using osynth::loopstream::g_prejoin;""")

sub("""FILE* s_rec_f = nullptr;
int s_rec_trk = -1;
uint32_t s_rec_written = 0;
""",
"""FILE* s_rec_f = nullptr;
int s_rec_trk = -1;
uint32_t s_rec_written = 0;

/* ---- punch-in pre-arm (S41, loop_stream.h), guarded by s_lock ---- */

/* How much of the take has to be on the card before its opening is staged.
 * ~340 ms of stereo: comfortably more runway than the close + rename + open
 * loop_ctl does at the wrap needs, and early enough in a take that it is
 * reached long before the pass ends. A loop shorter than this never stages
 * and keeps the original one-pass-late behaviour, which at a third of a
 * second is not a loop anyone is punching into. */
constexpr uint32_t kPreStageMin = 16u * 1024u;

FILE* s_pre_f = nullptr; /* read handle on live.tmp while staging */
int s_pre_trk = -1;
uint32_t s_pre_pos = 0; /* bytes of the pass staged into the window */
""")

# drop_prearm() has to exist before close_play(), which calls it.
sub("""void close_play(int t) {
    if (s_play_f[t] != nullptr) {
        fclose(s_play_f[t]);
        s_play_f[t] = nullptr;
    }
    s_play_pos[t] = 0;
    ring_reset(g_play[t]);
}
""",
"""/* Abandons a staged window. Lock held, and safe when nothing is staged.
 *
 * The two cases differ in who is looking at the ring. If the audio task never
 * joined it, nobody is, and it is reset. If it did, it is playing out of it
 * right now — so the track is held instead, which quiets it within a block
 * and rejoins it at the next wrap, and the ring is left alone for whoever
 * gives the track a reader again to reset. Resetting it here would be a torn
 * read straight into the mix. */
void drop_prearm() {
    if (s_pre_trk < 0) return;
    const int t = s_pre_trk;
    const uint8_t bit = (uint8_t)(1u << t);
    if (s_pre_f != nullptr) {
        fclose(s_pre_f);
        s_pre_f = nullptr;
    }
    g_prearm.fetch_and((uint8_t)~bit, std::memory_order_release);
    if ((g_prejoin.load(std::memory_order_acquire) & bit) != 0) {
        g_hold.fetch_or(bit, std::memory_order_release);
        g_prejoin.fetch_and((uint8_t)~bit, std::memory_order_release);
    } else {
        ring_reset(g_play[t]);
    }
    s_pre_trk = -1;
    s_pre_pos = 0;
}

void close_play(int t) {
    /* Every set-lifecycle path funnels through here, so this is the one place
     * a staged window has to be cleaned up from. */
    if (s_pre_trk == t) drop_prearm();
    if (s_play_f[t] != nullptr) {
        fclose(s_play_f[t]);
        s_play_f[t] = nullptr;
    }
    s_play_pos[t] = 0;
    ring_reset(g_play[t]);
}
""")

# maybe_prearm(), after drain_rec() and before io_task.
sub("""void io_task(void* arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kIoPeriodMs));
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        if (!drain_rec()) {
            fclose(s_rec_f);
            s_rec_f = nullptr;
            s_rec_trk = -1;
        }
""",
"""/* Stages the opening of the take in progress into its track's play window, so
 * the punch-in is audible from the pass that follows it rather than the one
 * after that (loop_stream.h). Lock held; called from io_task straight after
 * the drain, so everything it reads back is something it has just written.
 *
 * **fsync, not just fflush.** FatFs writes a file's size into the directory
 * entry on f_sync/f_close and not before, so a second handle opened on
 * live.tmp without it reads a file the filesystem still believes is empty —
 * fflush only moves stdio's own buffer into f_write. Both are needed, in this
 * order, and getting that wrong fails as a short read rather than as an
 * error.
 *
 * Once per take: one sync and one ~16 KB read, on the control-plane task. */
void maybe_prearm() {
    if (s_pre_trk >= 0) return;                     /* already staged */
    if (s_rec_trk < 0 || s_rec_f == nullptr) return;
    if (s_pass_bytes == 0) return; /* first take of a set: no length yet, and
                                    * nothing to punch into either */
    if (s_rec_written < kPreStageMin) return;
    const int t = s_rec_trk;
    uint32_t want = s_rec_written;
    if (want > LOOP_STREAM_RING_BYTES - kIoChunk) {
        want = LOOP_STREAM_RING_BYTES - kIoChunk;
    }
    if (want > s_pass_bytes) want = s_pass_bytes;

    if (fflush(s_rec_f) != 0 || fsync(fileno(s_rec_f)) != 0) {
        ESP_LOGW(TAG,
                 "track %d: take will not sync — the punch-in lands a pass "
                 "late (%s)",
                 t + 1, strerror(errno));
        /* Not retried: s_pre_trk stays -1, so the next pass through here
         * would try again on every tick of a take that has already said no. */
        s_pre_trk = -2;
        return;
    }
    char path[64];
    tmp_path(path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        s_pre_trk = -2;
        return;
    }
    /* The window is this track's to take: it is the one being recorded, the
     * render path has skipped it since the take started, and g_hold has been
     * set on it that whole time. */
    close_play(t);
    s_playing &= (uint8_t)~(1u << t);
    osynth::loopstream::Ring& r = g_play[t];
    const size_t got = fread(r.buf, 1, want, f);
    if (got != want) {
        fclose(f);
        ring_reset(r);
        s_pre_trk = -2;
        return;
    }
    r.wr.store(want, std::memory_order_release);
    s_pre_f = f;
    s_pre_trk = t;
    s_pre_pos = want;
    /* Published last, so the audio task either finds a whole window or none:
     * until this bit is set it holds the track at the wrap exactly as it
     * always did. */
    g_prearm.fetch_or((uint8_t)(1u << t), std::memory_order_release);
}

void io_task(void* arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kIoPeriodMs));
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        if (!drain_rec()) {
            fclose(s_rec_f);
            s_rec_f = nullptr;
            s_rec_trk = -1;
        }
        maybe_prearm();
""")

io.open(P, "w", encoding="utf-8", newline="").write(s)
print("patched", P)
