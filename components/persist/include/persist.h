/*
 * osynth — persisted settings (Session 25).
 *
 * Keeps a chosen set of parameters across power cycles. Storage is NVS (the
 * `nvs` partition, already initialised in main.cpp) rather than the LittleFS
 * `storage` partition the presets use: this is a handful of small values that
 * must survive anything, NVS gives wear levelling and atomic commits for
 * free, and keeping it off `storage` means a corrupt filesystem cannot take
 * the master volume down with it.
 *
 * ---------------------------------------------------------------------------
 * Why this is not simply "write on change"
 *
 * A flash write stalls the render chain. ESP-IDF disables the flash cache and
 * parks the other core for the duration of the operation, so the audio task
 * cannot run and cannot reach any of its flash-resident data — the wavetables
 * and the drum kit are both .rodata. That is the same hazard the looper's
 * flash backend documents, which is why it refuses to save while the
 * transport runs.
 *
 * So writes are made rare, and then made inaudible:
 *
 *   1. Coalesced. A parameter is marked dirty, not written. Dragging a volume
 *      slider produces one write, not three hundred.
 *   2. Settled. Nothing is written until the dirty set has stopped changing
 *      for a few seconds.
 *   3. Silent. The write then waits for the *output* to go quiet
 *      (audio_io_quiet_ms). Waiting on the output rather than on "are any
 *      voices active" is the part that makes it correct: a reverb tail, a
 *      delay repeat or a looper track is still sound, and a voice-count check
 *      would write straight through one.
 *   4. Bounded. If the synth never goes quiet, the write happens anyway after
 *      kMaxDeferMs — losing a setting because someone left a drone running is
 *      worse than one stall a couple of minutes after the last edit.
 *   5. Skipped if identical. The blob is compared against what was last
 *      written before touching flash at all.
 *
 * The result: in normal playing there is no write while a note sounds, and a
 * session of knob-twiddling costs a few writes rather than thousands.
 * ---------------------------------------------------------------------------
 *
 * Adding more later is one call: `persist_add()` from the owning component's
 * init, before persist_init() runs. Only parameters with a stable meaning
 * belong here — the 0x02xx engine range is re-registered per engine, so an id
 * there means something different depending on the bound engine and must not
 * be persisted (presets are the right home for patch data).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ceiling on persisted parameters. Deliberately small: this is for settings,
 * not for patches. */
#define PERSIST_MAX_PARAMS 32

/* Marks parameter ids as persisted. Call from a component's init, before
 * persist_init(). Ids that are not registered by the time persist_init() runs
 * are dropped with a warning. Returns how many were added. */
size_t persist_add(const uint16_t* ids, size_t count);

/* Loads the stored values into the ParamStore and starts the writer task.
 *
 * Call after every persist_add() and after all parameter registration, but
 * *before* audio_io_start(): the values land through ParamStore::set() like
 * any other write, and the S9 rule is that the audio task starts last. */
esp_err_t persist_init(void);

/* Writes now if anything is dirty, ignoring the settle and silence waits.
 * For a caller that knows a stall is acceptable — a settings screen's
 * explicit "save", or before a deliberate reboot. Blocking; control tasks
 * only. Returns ESP_OK when there was nothing to do. */
esp_err_t persist_save_now(void);

/* Forgets everything stored and reverts the persisted parameters to their
 * registered defaults. */
esp_err_t persist_reset(void);

#ifdef __cplusplus
}
#endif
