#!/usr/bin/env python3
"""S41 punch-in pre-arm, part 3: close_record, adopt, prejoined, stubs.

See s41g_punchin_prearm.py for what the whole change is for.
"""
import io

P = "components/looper/loop_stream.cpp"
s = io.open(P, encoding="utf-8").read()


def sub(old, new):
    global s
    n = s.count(old)
    assert n == 1, f"anchor {n}x, expected 1:\n{old[:200]}"
    s = s.replace(old, new, 1)


# A fresh take clears the "gave up" sentinel maybe_prearm() may have left.
sub("""        s_rec_trk = t;
        s_rec_written = 0;
        ring_reset(g_rec);""",
    """        s_rec_trk = t;
        s_rec_written = 0;
        s_pre_trk = -1; /* clears the sentinel a previous take may have left */
        ring_reset(g_rec);""")

# close_record: keep a joined window alive across the rename.
sub("""        char tmp[64], dst[64];
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
    s_rec_trk = -1;""",
    """        char tmp[64], dst[64];
        tmp_path(tmp, sizeof(tmp));
        track_path(dst, sizeof(dst), t);
        /* Did the audio task take the staged window at the wrap? Read before
         * anything below can clear it. */
        const bool joined =
            (g_prejoin.load(std::memory_order_acquire) & (uint8_t)(1u << t)) !=
            0;
        if (keep && err == ESP_OK) {
            if (joined) {
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
            } else {
                /* the track being recorded is never open for playback (the
                 * render path skips it), so replacing the file under the set
                 * is safe */
                close_play(t);
                s_playing &= (uint8_t)~(1u << t);
            }
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
        /* Anything still staged describes a take that is not becoming this
         * track — a discard, a failed rename, or a window nothing joined. */
        if (s_pre_trk == t && !(joined && keep && err == ESP_OK)) {
            drop_prearm();
        }
    }
    s_rec_trk = -1;""")

# adopt_prearmed + prejoined, next to add_track.
sub("""extern "C" void loop_stream_release_track(int t) {
    if (!s_ready || t < 0 || t >= LOOP_TRACKS) return;
    g_hold.fetch_and((uint8_t)~(1u << t), std::memory_order_release);
}""",
    """extern "C" bool loop_stream_prejoined(int t) {
    if (!s_ready || t < 0 || t >= LOOP_TRACKS) return false;
    return (g_prejoin.load(std::memory_order_acquire) &
            (uint8_t)(1u << t)) != 0;
}

extern "C" esp_err_t loop_stream_adopt_prearmed(int t) {
    if (!s_ready || t < 0 || t >= LOOP_TRACKS) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_pre_trk != t || s_pass_bytes == 0) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        char path[64];
        track_path(path, sizeof(path), t);
        FILE* f = fopen(path, "rb");
        /* Where the staging stopped, not 0: the window already holds
         * everything before this and the audio task is reading it. A whole
         * pass staged (a loop barely longer than the ring) leaves the file
         * exactly at the seam, which is the loop start again. */
        uint32_t pos = s_pre_pos;
        if (pos >= s_pass_bytes) pos = 0;
        if (f == nullptr || fseek(f, (long)pos, SEEK_SET) != 0) {
            if (f != nullptr) fclose(f);
            err = ESP_FAIL;
        } else {
            /* Nothing to close: staging took this slot when it built the
             * window, and close_record left it alone. Checked anyway, because
             * leaking a handle here costs the mount one of its file slots. */
            if (s_play_f[t] != nullptr) fclose(s_play_f[t]);
            s_play_f[t] = f;
            s_play_pos[t] = pos;
            s_playing |= (uint8_t)(1u << t);
            s_pre_trk = -1;
            s_pre_pos = 0;
            g_prearm.fetch_and((uint8_t)~(1u << t), std::memory_order_release);
            g_prejoin.fetch_and((uint8_t)~(1u << t), std::memory_order_release);
            /* Top the window back up behind the audio task, exactly as the
             * ordinary refill would — from here on the ring is a live
             * producer/consumer pair and needs no further ceremony. */
            if (!fill_track(t)) {
                close_play(t);
                s_playing &= (uint8_t)~(1u << t);
                err = ESP_FAIL;
            }
        }
    }
    if (err != ESP_OK) {
        /* Holds the track on its way out, so it goes quiet within a block
         * rather than reading a window nothing is filling. The caller falls
         * back to loop_stream_add_track(): a pass late, but audible. */
        drop_prearm();
        s_playing &= (uint8_t)~(1u << t);
    }
    xSemaphoreGive(s_lock);
    return err;
}

extern "C" void loop_stream_release_track(int t) {
    if (!s_ready || t < 0 || t >= LOOP_TRACKS) return;
    g_hold.fetch_and((uint8_t)~(1u << t), std::memory_order_release);
}""")

# rewind clears the masks wholesale; a staged window must go with them.
sub("""    xSemaphoreTake(s_lock, portMAX_DELAY);
    g_resync.store(0, std::memory_order_release);
    g_hold.store(0, std::memory_order_release);
    for (int t = 0; t < LOOP_TRACKS; ++t) {
        if (((s_playing >> t) & 1) == 0) continue;
        if (s_play_f[t] == nullptr) continue;""",
    """    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Before the masks are cleared: a staged window describes a take, and the
     * transport is being put back to the top of the set. */
    drop_prearm();
    g_resync.store(0, std::memory_order_release);
    g_hold.store(0, std::memory_order_release);
    for (int t = 0; t < LOOP_TRACKS; ++t) {
        if (((s_playing >> t) & 1) == 0) continue;
        if (s_play_f[t] == nullptr) continue;""")

# non-streamed stubs
sub("""extern "C" esp_err_t loop_stream_add_track(int) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" void loop_stream_release_track(int) {}""",
    """extern "C" esp_err_t loop_stream_add_track(int) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" bool loop_stream_prejoined(int) { return false; }
extern "C" esp_err_t loop_stream_adopt_prearmed(int) {
    return ESP_ERR_NOT_SUPPORTED;
}
extern "C" void loop_stream_release_track(int) {}""")

io.open(P, "w", encoding="utf-8", newline="").write(s)
print("patched", P)
