/*
 * osynth — SD-streamed looper tracks (S31). See loop_stream.h for the
 * contract, the ring protocol and the underrun policy.
 *
 * Files live in /sd/osynth alongside the save slots (loopN.olp), under names
 * of their own: liveN.olt for the tracks and live.tmp for the take in
 * progress. This is scratch for the *live* set, rewritten constantly, and it
 * must never be able to damage a saved loop — which is a property of the
 * names, not of a directory. Nothing here ever opens, renames or removes a
 * path it did not spell out itself, so the slots are untouchable whatever
 * happens mid-take. They are cleared when a set starts, so a power cut leaves
 * stale files that the next set removes rather than plays.
 *
 * A subdirectory (/sd/osynth/live) was the obvious layout and is what this
 * originally did, but mkdir turned out to be the one filesystem call that
 * fails on cards where everything else works — see tools/sd_bringup_notes.md.
 * Not needing it is worth more than the tidier listing: /sd/osynth is created
 * on the mount and, failing that, by any PC, so the streamed looper now
 * depends on nothing the slot backend does not already depend on.
 *
 * Track files hold nothing but ADPCM bytes in transport order — no header.
 * The three fields a slot blob needs (length, format, filled mask) describe
 * the set rather than any one track, and they live in one small manifest
 * beside them, live.set. They were originally nowhere at all, on the grounds
 * that loop_ctl already owned them and a second copy would only be one more
 * thing to keep honest; that held right up until a set had to survive the
 * power being cut, after which there is no first copy left to be honest
 * against and eight anonymous files cannot say how long the loop was. Hence
 * the manifest, and hence its being written from the same place the state is
 * mirrored. loop.save still produces a normal .olp blob, track by track.
 *
 * Two control-plane tasks touch the files: loop_ctl for the set/track
 * lifecycle and loop_io for the periodic refill and drain. They share one
 * mutex. The audio task takes no lock and opens nothing — it only moves the
 * ring counters (loop_stream.h).
 */
#include "loop_stream.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "loop_store.h"

static const char* TAG = "loop_stream";

#if SYNTH_LOOP_STREAM

namespace osynth::loopstream {

Ring g_play[LOOP_TRACKS];
Ring g_rec;
std::atomic<uint32_t> g_underruns{0};
std::atomic<uint8_t> g_resync{0};
std::atomic<uint8_t> g_hold{0};

} // namespace osynth::loopstream

using osynth::loopstream::g_hold;
using osynth::loopstream::g_play;
using osynth::loopstream::g_rec;
using osynth::loopstream::g_resync;
using osynth::loopstream::g_underruns;

namespace {

/* Created by loop_store on the card mount; shared with the save slots. */
constexpr const char* kSetDir = "/sd/osynth";

/* Transfer size for one fread/fwrite. A multiple of the 512 B sector and
 * well under the ring, so a refill pass tops up every track a few times
 * rather than filling one and starving the rest. */
constexpr uint32_t kIoChunk = 4096;

/* Refill cadence. The window holds ~1.4 s, so this is 70x oversampled — it
 * exists to keep each individual read small (a long read holds the bus and
 * delays the other tracks), not because the deadline is anywhere near. */
constexpr uint32_t kIoPeriodMs = 20;

constexpr int kIoTaskPrio = 6; /* above loop_ctl (5), below anything audio */
constexpr int kIoTaskStack = 4096;

SemaphoreHandle_t s_lock = nullptr;
TaskHandle_t s_io_task = nullptr;
bool s_ready = false;

/* ---- set state, guarded by s_lock ---- */

bool s_mono = true;
bool s_set_open = false;
bool s_suspended = false;  /* set intact, card out of the slot */
uint32_t s_set_serial = 0; /* the card its files were written to */
uint32_t s_pass_bytes = 0; /* bytes per loop pass, 0 until the length is known */
uint8_t s_playing = 0;     /* tracks open for playback */

FILE* s_play_f[LOOP_TRACKS] = {};
uint32_t s_play_pos[LOOP_TRACKS] = {}; /* byte offset within the pass */

FILE* s_rec_f = nullptr;
int s_rec_trk = -1;
uint32_t s_rec_written = 0;

/* Both stay inside 8.3 ("live0" + "olt", "live" + "tmp") so the layout does
 * not depend on long-filename support being compiled in. */
void track_path(char* out, size_t n, int t) {
    snprintf(out, n, "%s/live%d.olt", kSetDir, t);
}

void tmp_path(char* out, size_t n) { snprintf(out, n, "%s/live.tmp", kSetDir); }

void man_path(char* out, size_t n) { snprintf(out, n, "%s/live.set", kSetDir); }

/* The set's shape, and the one thing the track files cannot carry.
 *
 * Track files are nibbles and nothing else, which was right while a set only
 * ever had to outlive the next block: length, format and filled mask are
 * loop_ctl's own state, and a second copy on the card would only be one more
 * thing to keep honest. A set that has to outlive a power cycle changes that —
 * after a reboot there is no first copy left to be honest against, and eight
 * anonymous files cannot say how long the loop was, whether it was mono, or
 * which of them were real. So exactly those three go here, written whenever
 * the set's shape changes and read back when a set is adopted. */
struct Manifest {
    char magic[4];        /* "OLS1" */
    uint8_t version;      /* 1 */
    uint8_t filled;       /* which tracks are real */
    uint8_t mono;         /* the set's packing */
    uint8_t rsvd;
    uint32_t loop_frames; /* the loop length every track shares */
    uint32_t sample_rate; /* refuse a card written at another rate */
};

/* Bytes one track occupies for `frames`, in the set's packing. Mirrors
 * looper.cpp's track_bytes_for() and loop_store.cpp's codec_track_bytes():
 * stereo one byte per frame, mono two frames per byte with the odd tail
 * padding a nibble. */
uint32_t pass_bytes_for(uint32_t frames, bool mono) {
    return mono ? (frames + 1u) / 2u : frames;
}

void ring_reset(osynth::loopstream::Ring& r) {
    r.wr.store(0, std::memory_order_relaxed);
    r.rd.store(0, std::memory_order_relaxed);
}

void close_play(int t) {
    if (s_play_f[t] != nullptr) {
        fclose(s_play_f[t]);
        s_play_f[t] = nullptr;
    }
    s_play_pos[t] = 0;
    ring_reset(g_play[t]);
}

/* Tops up one track's window. Lock held. Returns false on a read error, in
 * which case the caller drops the track rather than serving it garbage. */
bool fill_track(int t) {
    FILE* f = s_play_f[t];
    if (f == nullptr || s_pass_bytes == 0) return true;
    osynth::loopstream::Ring& r = g_play[t];
    for (;;) {
        const uint32_t wr = r.wr.load(std::memory_order_relaxed);
        const uint32_t used = wr - r.rd.load(std::memory_order_acquire);
        const uint32_t space = LOOP_STREAM_RING_BYTES - used;
        if (space < kIoChunk) return true;
        const uint32_t off = wr % LOOP_STREAM_RING_BYTES;
        uint32_t n = kIoChunk;
        /* three ceilings: the free space, the wrap of the ring, and the end
         * of the pass — reading past the pass end would serve the next pass's
         * bytes at this pass's positions */
        if (n > space) n = space;
        if (n > LOOP_STREAM_RING_BYTES - off) n = LOOP_STREAM_RING_BYTES - off;
        if (n > s_pass_bytes - s_play_pos[t]) n = s_pass_bytes - s_play_pos[t];
        if (n == 0) return true;
        const size_t got = fread(r.buf + off, 1, n, f);
        if (got != n) {
            if (ferror(f) != 0) {
                ESP_LOGW(TAG, "track %d: read failed at %u", t + 1,
                         (unsigned)s_play_pos[t]);
                return false;
            }
            /* The file ends before the pass does — a take the transport
             * stopped part way through. looper.cpp encodes a short tail that
             * fades to digital silence and leaves the decoder at the one
             * state where a run of zero bytes decodes as silence rather than
             * a frozen DC offset, then writes no more: the rest of the pass
             * costs neither card space nor the seconds it would take to write
             * it, which on a loop that may be half an hour long is the whole
             * point. */
            memset(r.buf + off + got, 0, n - got);
        }
        s_play_pos[t] += n;
        r.wr.store(wr + n, std::memory_order_release);
        if (s_play_pos[t] >= s_pass_bytes) {
            /* the transport wraps to the loop start; so does the file. The
             * window stays continuous across the seam — the audio task's
             * decoders reset at the same boundary (looper.cpp). */
            if (fseek(f, 0, SEEK_SET) != 0) return false;
            s_play_pos[t] = 0;
        }
    }
}

/* Writes whatever the audio task has put in the record ring. Lock held. */
bool drain_rec() {
    if (s_rec_f == nullptr) return true;
    for (;;) {
        const uint32_t rd = g_rec.rd.load(std::memory_order_relaxed);
        const uint32_t avail = g_rec.wr.load(std::memory_order_acquire) - rd;
        if (avail == 0) return true;
        const uint32_t off = rd % LOOP_STREAM_RING_BYTES;
        uint32_t n = avail;
        if (n > LOOP_STREAM_RING_BYTES - off) n = LOOP_STREAM_RING_BYTES - off;
        if (n > kIoChunk) n = kIoChunk;
        if (fwrite(g_rec.buf + off, 1, n, s_rec_f) != n) {
            ESP_LOGE(TAG, "record write failed (%s)", strerror(errno));
            return false;
        }
        s_rec_written += n;
        g_rec.rd.store(rd + n, std::memory_order_release);
    }
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
        /* Re-prime the tracks the audio task gave up on, before the ordinary
         * top-up: they are the ones actually silent right now. */
        const uint8_t resync = g_resync.load(std::memory_order_acquire) &
                               (uint8_t)~g_hold.load(std::memory_order_acquire);
        for (int t = 0; t < LOOP_TRACKS; ++t) {
            if (((resync >> t) & 1) == 0) continue;
            bool ok = ((s_playing >> t) & 1) != 0 && s_play_f[t] != nullptr;
            if (ok) {
                ring_reset(g_play[t]);
                s_play_pos[t] = 0;
                ok = fseek(s_play_f[t], 0, SEEK_SET) == 0 && fill_track(t);
            }
            if (!ok) {
                close_play(t);
                s_playing &= (uint8_t)~(1u << t);
            }
            /* Cleared last: while the bit is set the audio task leaves this
             * track alone, which is what makes resetting its ring safe. */
            g_resync.fetch_and((uint8_t)~(1u << t), std::memory_order_release);
        }
        for (int t = 0; t < LOOP_TRACKS; ++t) {
            if (((s_playing >> t) & 1) == 0) continue;
            if (!fill_track(t)) {
                close_play(t);
                s_playing &= (uint8_t)~(1u << t);
                ESP_LOGW(TAG, "track %d dropped from the streamed set", t + 1);
            }
        }
        xSemaphoreGive(s_lock);
    }
}

/* The one directory involved, checked (loop_store.h). Not assumed to exist:
 * loop_store creates it on the mount transition, so a card mounted before
 * that code ran has never seen it, and it can be missing after a card swap.
 * A failure here is a card that mounts but cannot be written — reported by
 * loop_store with its errno, and answered by the caller falling back to
 * PSRAM. */
bool ensure_set_dir() { return loop_store_ensure_dir(kSetDir); }

/* Removes every scratch file, by name — the slots share this directory and
 * are never touched. Lock held. */
void wipe_live_files() {
    char path[64];
    /* The manifest goes first. While it exists it claims the tracks exist, so
     * a wipe interrupted half way through would otherwise leave one pointing
     * at files that are gone. This order leaves the opposite — tracks with
     * nothing claiming them, which is just litter for the next set. */
    man_path(path, sizeof(path));
    remove(path);
    for (int t = 0; t < LOOP_TRACKS; ++t) {
        track_path(path, sizeof(path), t);
        remove(path);
    }
    tmp_path(path, sizeof(path));
    remove(path);
}

} // namespace

extern "C" esp_err_t loop_stream_init(void) {
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == nullptr) return ESP_ERR_NO_MEM;
    for (int t = 0; t < LOOP_TRACKS; ++t) {
        g_play[t].buf = (uint8_t*)heap_caps_malloc(
            LOOP_STREAM_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (g_play[t].buf == nullptr) {
            ESP_LOGW(TAG, "no PSRAM for the track windows — sd mode disabled");
            return ESP_OK; /* not fatal: the looper still has PSRAM mode */
        }
        ring_reset(g_play[t]);
    }
    g_rec.buf = (uint8_t*)heap_caps_malloc(
        LOOP_STREAM_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (g_rec.buf == nullptr) {
        ESP_LOGW(TAG, "no PSRAM for the record ring — sd mode disabled");
        return ESP_OK;
    }
    ring_reset(g_rec);
    if (xTaskCreatePinnedToCore(io_task, "loop_io", kIoTaskStack, nullptr,
                                kIoTaskPrio, &s_io_task, 0) != pdPASS) {
        ESP_LOGW(TAG, "loop_io task did not start — sd mode disabled");
        return ESP_OK;
    }
    s_ready = true;
    ESP_LOGI(TAG,
             "sd streaming available: %u KB window x %d tracks + %u KB record "
             "ring, max %.0f s",
             (unsigned)(LOOP_STREAM_RING_BYTES / 1024), LOOP_TRACKS,
             (unsigned)(LOOP_STREAM_RING_BYTES / 1024), LOOP_STREAM_MAX_S);
    return ESP_OK;
}

/* Last probe's answer. Written only by loop_stream_probe_card(); read by
 * loop_stream_ready() from anywhere, hence atomic. */
static std::atomic<bool> s_card_ok{false};

extern "C" bool loop_stream_ready(void) {
    return s_ready && s_card_ok.load(std::memory_order_relaxed);
}

extern "C" bool loop_stream_probe_card(void) {
    if (!s_ready) return false;
    const bool ok = loop_store_mount();
    s_card_ok.store(ok, std::memory_order_relaxed);
    return ok;
}

extern "C" esp_err_t loop_stream_begin_set(bool mono) {
    /* Probe rather than trust the cache: this is the moment the answer has
     * to be right, and it is the one call where blocking on a mount is
     * already expected. */
    if (!loop_stream_probe_card()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int t = 0; t < LOOP_TRACKS; ++t) close_play(t);
    if (s_rec_f != nullptr) {
        fclose(s_rec_f);
        s_rec_f = nullptr;
    }
    s_rec_trk = -1;
    s_playing = 0;
    s_pass_bytes = 0;
    s_mono = mono;
    g_resync.store(0, std::memory_order_release);
    g_hold.store(0, std::memory_order_release);
    if (!ensure_set_dir()) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "no usable %s — the card mounts but cannot be written",
                 kSetDir);
        return ESP_FAIL;
    }
    wipe_live_files();
    s_set_open = true;
    s_suspended = false;
    /* Remembered now so a card swap can be told from a re-insertion later. */
    s_set_serial = loop_store_card_serial();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "streamed set opened (%s)", mono ? "mono" : "stereo");
    return ESP_OK;
}

extern "C" void loop_stream_end_set(void) {
    if (!s_ready) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int t = 0; t < LOOP_TRACKS; ++t) close_play(t);
    if (s_rec_f != nullptr) {
        fclose(s_rec_f);
        s_rec_f = nullptr;
    }
    s_rec_trk = -1;
    s_playing = 0;
    s_pass_bytes = 0;
    if (s_set_open) wipe_live_files();
    s_set_open = false;
    s_suspended = false;
    s_set_serial = 0;
    /* A suspended set held every track; nothing is left to hold. */
    g_resync.store(0, std::memory_order_release);
    g_hold.store(0, std::memory_order_release);
    xSemaphoreGive(s_lock);
}

extern "C" void loop_stream_save_manifest(uint32_t loop_frames, bool mono,
                                           uint8_t filled) {
    if (!loop_stream_ready() || !s_set_open) return;
    char path[64];
    man_path(path, sizeof(path));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (loop_frames == 0 || filled == 0) {
        remove(path); /* nothing left worth coming back to */
    } else {
        Manifest m = {};
        memcpy(m.magic, "OLS1", 4);
        m.version = 1;
        m.filled = filled;
        m.mono = mono ? 1u : 0u;
        m.loop_frames = loop_frames;
        m.sample_rate = SYNTH_SAMPLE_RATE;
        FILE* f = fopen(path, "wb");
        if (f == nullptr) {
            ESP_LOGW(TAG, "cannot write %s (%s)", path, strerror(errno));
        } else {
            if (fwrite(&m, 1, sizeof(m), f) != sizeof(m)) {
                ESP_LOGW(TAG, "manifest write failed (%s)", strerror(errno));
            }
            fclose(f);
        }
    }
    xSemaphoreGive(s_lock);
}

extern "C" bool loop_stream_load_manifest(uint32_t* loop_frames, bool* mono,
                                           uint8_t* filled) {
    if (!loop_stream_ready()) return false;
    char path[64];
    man_path(path, sizeof(path));
    Manifest m = {};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    FILE* f = fopen(path, "rb");
    const bool got = f != nullptr && fread(&m, 1, sizeof(m), f) == sizeof(m);
    if (f != nullptr) fclose(f);
    bool ok =
        got && memcmp(m.magic, "OLS1", 4) == 0 && m.version == 1 &&
        m.filled != 0 && m.loop_frames != 0 &&
        m.sample_rate == SYNTH_SAMPLE_RATE &&
        m.loop_frames <= (uint32_t)(LOOP_STREAM_MAX_S * SYNTH_SAMPLE_RATE);
    if (ok) {
        /* Every track it claims has to be there. The manifest and the tracks
         * are separate files, so a card pulled mid-write — or edited on a PC —
         * can disagree, and a set that is only partly on the card is not one
         * to hand the transport. */
        char t_path[64];
        struct stat st;
        for (int t = 0; t < LOOP_TRACKS && ok; ++t) {
            if (((m.filled >> t) & 1) == 0) continue;
            track_path(t_path, sizeof(t_path), t);
            if (stat(t_path, &st) != 0 || st.st_size == 0) {
                ESP_LOGW(TAG, "the card claims track %d, but %s is not there",
                         t + 1, t_path);
                ok = false;
            }
        }
    }
    xSemaphoreGive(s_lock);
    if (!ok) return false;
    *loop_frames = m.loop_frames;
    *mono = m.mono != 0;
    *filled = m.filled;
    return true;
}

extern "C" esp_err_t loop_stream_adopt_set(uint32_t loop_frames, bool mono,
                                            uint8_t filled) {
    if (!loop_stream_probe_card() || loop_frames == 0 || filled == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int t = 0; t < LOOP_TRACKS; ++t) close_play(t);
    if (s_rec_f != nullptr) {
        fclose(s_rec_f);
        s_rec_f = nullptr;
    }
    s_rec_trk = -1;
    s_playing = 0;
    s_mono = mono;
    s_pass_bytes = pass_bytes_for(loop_frames, mono);
    /* Opened, unlike begin_set, *without* the wipe: here the files are the
     * set rather than something in its way. */
    s_set_open = true;
    s_suspended = false;
    s_set_serial = loop_store_card_serial();
    g_resync.store(0, std::memory_order_release);
    g_hold.store(0, std::memory_order_release);
    esp_err_t err = ESP_OK;
    char path[64];
    for (int t = 0; t < LOOP_TRACKS; ++t) {
        if (((filled >> t) & 1) == 0) continue;
        track_path(path, sizeof(path), t);
        s_play_f[t] = fopen(path, "rb");
        if (s_play_f[t] == nullptr || !fill_track(t)) {
            ESP_LOGW(TAG, "track %d: %s could not be opened", t + 1, path);
            close_play(t);
            err = ESP_ERR_NOT_FOUND;
            break;
        }
        s_playing |= (uint8_t)(1u << t);
    }
    if (err != ESP_OK) {
        for (int t = 0; t < LOOP_TRACKS; ++t) close_play(t);
        s_playing = 0;
        s_set_open = false;
        s_pass_bytes = 0;
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "adopted the set on the card: %.2f s, %d %s track(s)",
                 (float)loop_frames / SYNTH_SAMPLE_RATE,
                 __builtin_popcount(filled), mono ? "mono" : "stereo");
    }
    return err;
}

extern "C" void loop_stream_suspend_set(void) {
    if (!s_ready) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Closing a read handle costs no card access — FatFs only syncs a file it
     * wrote to — so the loop below is free even with the card already gone.
     * The take, if one is somehow open, is not; but leaking its handle into
     * an unmount would be worse than paying one timeout for it. */
    for (int t = 0; t < LOOP_TRACKS; ++t) close_play(t);
    if (s_rec_f != nullptr) {
        fclose(s_rec_f);
        s_rec_f = nullptr;
    }
    s_rec_trk = -1;
    /* s_playing, s_pass_bytes, s_mono and s_set_open all stand: this set is
     * only out of reach, and every one of them describes files that still
     * exist on a card sitting on the desk. */
    s_suspended = s_set_open;
    g_resync.store(0, std::memory_order_release);
    /* Held, not starved. The render path mutes a held track without counting
     * an underrun or asking loop_io to re-prime it — which is exactly right
     * when the answer is "wait for the card", and would otherwise be a log
     * line every block. */
    g_hold.store(0xFF, std::memory_order_release);
    /* No new streamed set may be offered until a probe says otherwise. */
    s_card_ok.store(false, std::memory_order_relaxed);
    xSemaphoreGive(s_lock);
}

extern "C" bool loop_stream_suspended(void) { return s_ready && s_suspended; }

extern "C" bool loop_stream_resume_set(uint8_t filled) {
    if (!s_ready || !s_suspended) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_set_open && s_pass_bytes != 0 && s_set_serial != 0 &&
              loop_store_card_serial() == s_set_serial;
    if (!ok) {
        ESP_LOGW(TAG, "the card in the slot is not the one this set was "
                      "recorded to");
    } else {
        char path[64];
        s_playing = 0;
        for (int t = 0; t < LOOP_TRACKS; ++t) {
            close_play(t);
            if (((filled >> t) & 1) == 0) continue;
            track_path(path, sizeof(path), t);
            s_play_f[t] = fopen(path, "rb");
            if (s_play_f[t] == nullptr) {
                ESP_LOGW(TAG, "track %d: %s is gone", t + 1, path);
                ok = false;
                break;
            }
            s_play_pos[t] = 0;
            s_playing |= (uint8_t)(1u << t);
            if (!fill_track(t)) {
                ok = false;
                break;
            }
        }
    }
    if (ok) {
        s_suspended = false;
        g_resync.store(0, std::memory_order_release);
        g_hold.store(0, std::memory_order_release); /* the tracks are back */
    }
    xSemaphoreGive(s_lock);
    return ok;
}

extern "C" esp_err_t loop_stream_open_record(int t, uint32_t loop_frames) {
    if (!loop_stream_ready() || t < 0 || t >= LOOP_TRACKS) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    char path[64];
    tmp_path(path, sizeof(path));
    /* Not only begin_set's job: a punch-in comes here without one, and the
     * directory can be gone by then (card swapped, or removed from a host). */
    if (!ensure_set_dir()) {
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    remove(path);
    s_rec_f = fopen(path, "wb");
    if (s_rec_f == nullptr) {
        ESP_LOGE(TAG, "cannot open %s (%s)", path, strerror(errno));
        err = ESP_FAIL;
    } else {
        s_rec_trk = t;
        s_rec_written = 0;
        ring_reset(g_rec);
        if (loop_frames != 0) s_pass_bytes = pass_bytes_for(loop_frames, s_mono);
    }
    xSemaphoreGive(s_lock);
    return err;
}

extern "C" esp_err_t loop_stream_pad_record(const uint8_t* bytes, uint32_t n) {
    if (!s_ready || bytes == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_rec_f == nullptr) {
        err = ESP_ERR_INVALID_STATE;
    } else if (!drain_rec()) {
        /* the audio task's last bytes have to reach the file before the tail
         * does — the tail continues the encoder state they ended in */
        err = ESP_FAIL;
    } else if (n > 0 && fwrite(bytes, 1, n, s_rec_f) != n) {
        ESP_LOGE(TAG, "tail write failed (%s)", strerror(errno));
        err = ESP_FAIL;
    } else {
        s_rec_written += n;
    }
    xSemaphoreGive(s_lock);
    return err;
}

extern "C" esp_err_t loop_stream_close_record(int t, bool keep) {
    if (!s_ready || t < 0 || t >= LOOP_TRACKS) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_rec_f == nullptr) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        if (!drain_rec()) err = ESP_FAIL; /* the tail the audio task left */
        if (fclose(s_rec_f) != 0) err = ESP_FAIL;
        s_rec_f = nullptr;
        ESP_LOGI(TAG, "track %d take: %u KB %s", t + 1,
                 (unsigned)(s_rec_written / 1024),
                 keep ? "kept" : "discarded");
        char tmp[64], dst[64];
        tmp_path(tmp, sizeof(tmp));
        track_path(dst, sizeof(dst), t);
        if (keep && err == ESP_OK) {
            /* the track being recorded is never open for playback (the render
             * path skips it), so replacing the file under the set is safe */
            close_play(t);
            s_playing &= (uint8_t)~(1u << t);
            remove(dst); /* FAT rename does not overwrite */
            if (rename(tmp, dst) != 0) {
                ESP_LOGE(TAG, "track %d: rename failed (%s)", t + 1,
                         strerror(errno));
                remove(tmp);
                err = ESP_FAIL;
            }
        } else {
            remove(tmp);
        }
    }
    s_rec_trk = -1;
    xSemaphoreGive(s_lock);
    return err;
}

extern "C" esp_err_t loop_stream_start_playback(uint32_t loop_frames,
                                                uint8_t filled) {
    if (!loop_stream_ready() || loop_frames == 0) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_pass_bytes = pass_bytes_for(loop_frames, s_mono);
    s_playing = 0;
    g_resync.store(0, std::memory_order_release);
    g_hold.store(0, std::memory_order_release);
    char path[64];
    for (int t = 0; t < LOOP_TRACKS; ++t) {
        close_play(t);
        if (((filled >> t) & 1) == 0) continue;
        track_path(path, sizeof(path), t);
        s_play_f[t] = fopen(path, "rb");
        if (s_play_f[t] == nullptr) {
            ESP_LOGW(TAG, "track %d: %s missing — not played", t + 1, path);
            continue;
        }
        s_play_pos[t] = 0;
        s_playing |= (uint8_t)(1u << t);
        /* prime the window before the transport is allowed to run: the first
         * pass must not be served from an empty ring */
        if (!fill_track(t)) {
            close_play(t);
            s_playing &= (uint8_t)~(1u << t);
        }
    }
    xSemaphoreGive(s_lock);
    return s_playing != 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

extern "C" void loop_stream_set_length(uint32_t loop_frames) {
    if (!s_ready || loop_frames == 0) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_pass_bytes == 0) s_pass_bytes = pass_bytes_for(loop_frames, s_mono);
    xSemaphoreGive(s_lock);
}

extern "C" esp_err_t loop_stream_add_track(int t) {
    if (!s_ready || t < 0 || t >= LOOP_TRACKS) return ESP_ERR_INVALID_STATE;
    /* Set before the lock: the audio task does not take the lock, so this
     * bit is the only thing that stops it reading the ring we are about to
     * reset. io_task does take it, so it cannot act on the bit meanwhile.
     * The audio task normally set g_hold itself when the take closed; doing
     * it again here covers the first take of a set, where the track was
     * never a playing one to begin with. */
    g_hold.fetch_or((uint8_t)(1u << t), std::memory_order_release);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_pass_bytes == 0) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        close_play(t);
        char path[64];
        track_path(path, sizeof(path), t);
        s_play_f[t] = fopen(path, "rb");
        if (s_play_f[t] == nullptr) {
            ESP_LOGW(TAG, "track %d: %s missing — not played", t + 1, path);
            err = ESP_ERR_NOT_FOUND;
        } else if (!fill_track(t)) {
            close_play(t);
            err = ESP_FAIL;
        } else {
            s_playing |= (uint8_t)(1u << t);
        }
    }
    if (err != ESP_OK) s_playing &= (uint8_t)~(1u << t);
    xSemaphoreGive(s_lock);
    /* Cleared last, so the audio task only starts reading a primed window.
     * Both masks: a track can be under a hold from the take that just closed
     * and a resync from an underrun in the pass before it. */
    const uint8_t clear = (uint8_t)~(1u << t);
    g_resync.fetch_and(clear, std::memory_order_release);
    g_hold.fetch_and(clear, std::memory_order_release);
    return err;
}

extern "C" void loop_stream_release_track(int t) {
    if (!s_ready || t < 0 || t >= LOOP_TRACKS) return;
    g_hold.fetch_and((uint8_t)~(1u << t), std::memory_order_release);
}

extern "C" esp_err_t loop_stream_rewind(void) {
    if (!s_ready || s_pass_bytes == 0) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    g_resync.store(0, std::memory_order_release);
    g_hold.store(0, std::memory_order_release);
    for (int t = 0; t < LOOP_TRACKS; ++t) {
        if (((s_playing >> t) & 1) == 0) continue;
        if (s_play_f[t] == nullptr) continue;
        ring_reset(g_play[t]);
        s_play_pos[t] = 0;
        if (fseek(s_play_f[t], 0, SEEK_SET) != 0 || !fill_track(t)) {
            close_play(t);
            s_playing &= (uint8_t)~(1u << t);
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

extern "C" void loop_stream_clear_track(int t) {
    if (!s_ready || t < 0 || t >= LOOP_TRACKS) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    close_play(t);
    s_playing &= (uint8_t)~(1u << t);
    char path[64];
    track_path(path, sizeof(path), t);
    remove(path);
    xSemaphoreGive(s_lock);
}

extern "C" esp_err_t loop_stream_export_read(int t, uint32_t offset,
                                             uint8_t* dst, uint32_t len,
                                             uint32_t* out_read) {
    if (out_read == nullptr) return ESP_ERR_INVALID_ARG;
    *out_read = 0;
    if (!s_ready || t < 0 || t >= LOOP_TRACKS || dst == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0) return ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!s_set_open || s_suspended) {
        /* Suspended means the card is out of the slot: the files still exist,
         * just not here. Nothing to read and nothing to diagnose. */
        err = ESP_ERR_INVALID_STATE;
    } else {
        char path[64];
        track_path(path, sizeof(path), t);
        FILE* f = fopen(path, "rb");
        if (f == nullptr) {
            err = ESP_ERR_NOT_FOUND;
        } else {
            /* FatFs clamps a read-only seek to the file size, so seeking past
             * a short track lands at its end and the read below answers 0 —
             * which is exactly the end-of-track signal the caller wants. */
            if (fseek(f, (long)offset, SEEK_SET) != 0) {
                err = ESP_FAIL;
            } else {
                *out_read = (uint32_t)fread(dst, 1, len, f);
                if (*out_read < len && ferror(f) != 0) {
                    *out_read = 0;
                    err = ESP_FAIL;
                }
            }
            fclose(f);
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

extern "C" uint32_t loop_stream_underruns(void) {
    return g_underruns.load(std::memory_order_relaxed);
}

#else /* streamed mode not compiled in */

extern "C" esp_err_t loop_stream_init(void) { return ESP_OK; }
extern "C" bool loop_stream_ready(void) { return false; }
extern "C" bool loop_stream_probe_card(void) { return false; }
extern "C" esp_err_t loop_stream_begin_set(bool) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" void loop_stream_end_set(void) {}
extern "C" void loop_stream_save_manifest(uint32_t, bool, uint8_t) {}
extern "C" bool loop_stream_load_manifest(uint32_t*, bool*, uint8_t*) {
    return false;
}
extern "C" esp_err_t loop_stream_adopt_set(uint32_t, bool, uint8_t) {
    return ESP_ERR_NOT_SUPPORTED;
}
extern "C" void loop_stream_suspend_set(void) {}
extern "C" bool loop_stream_suspended(void) { return false; }
extern "C" bool loop_stream_resume_set(uint8_t) { return false; }
extern "C" esp_err_t loop_stream_open_record(int, uint32_t) {
    return ESP_ERR_NOT_SUPPORTED;
}
extern "C" esp_err_t loop_stream_pad_record(const uint8_t*, uint32_t) {
    return ESP_ERR_NOT_SUPPORTED;
}
extern "C" esp_err_t loop_stream_close_record(int, bool) {
    return ESP_ERR_NOT_SUPPORTED;
}
extern "C" esp_err_t loop_stream_start_playback(uint32_t, uint8_t) {
    return ESP_ERR_NOT_SUPPORTED;
}
extern "C" esp_err_t loop_stream_rewind(void) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" void loop_stream_set_length(uint32_t) {}
extern "C" esp_err_t loop_stream_add_track(int) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" void loop_stream_release_track(int) {}
extern "C" void loop_stream_clear_track(int) {}
extern "C" esp_err_t loop_stream_export_read(int, uint32_t, uint8_t*, uint32_t,
                                             uint32_t* out_read) {
    if (out_read != nullptr) *out_read = 0;
    return ESP_ERR_NOT_SUPPORTED;
}
extern "C" uint32_t loop_stream_underruns(void) { return 0; }

#endif
