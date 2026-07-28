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
 * Requires PSRAM: without CONFIG_SPIRAM (classic ESP32) looper_init()
 * registers nothing and looper_process() is a no-op.
 */
#pragma once

#include <stddef.h>

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
 * is a cap/UI policy, not an enforcement (state, not patch). */
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

#define LOOP_PID_LEVEL(t) (0x0610 + (t)) /* loop.lvl1..8, t = 0..7           */

#define LOOP_TRACKS 8

/* Registers the 0x06xx params, hooks the ParamStore listener and starts the
 * loop_ctl task (owns all buffer allocation). Call before audio_io_start()
 * (the S9 registration-race rule). No-op without PSRAM support. */
esp_err_t looper_init(void);

/* Record tap + playback mix, in place after the FX bus. Audio task only —
 * no locks, no allocation (buffers come from loop_ctl ahead of time). */
void looper_process(float* l, float* r, size_t frames);

#ifdef __cplusplus
}
#endif
