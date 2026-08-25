/*
 * osynth — sampler: recording into kit pads (Session 44).
 *
 * The drum bus (drums.h) has always been able to *play* a kit. This is the
 * other half: capturing one from whatever the synth can hear, into any pad of
 * any kit, while the instrument keeps playing.
 *
 * ---------------------------------------------------------------------------
 * The three pieces, and why they are separate
 *
 *   the pool      Recorded pads are individually heap_caps_malloc'd out of
 *                 PSRAM and counted against one global budget
 *                 (OSYNTH_SAMPLE_POOL_KB). Per-pad blocks rather than one
 *                 arena per kit: pads are erased and re-recorded constantly
 *                 and at wildly different lengths, which is precisely the
 *                 shape a bump allocator handles worst and a general heap
 *                 handles for free. The budget counter is what a per-kit
 *                 arena was going to buy, and it costs one atomic.
 *
 *   the ring      A short mono ring, fed every single block whether or not
 *                 anything is armed. It exists because the record button is
 *                 at the far end of a BLE link: by the time "record" arrives
 *                 the transient that made you press it is already 50-150 ms
 *                 gone. Pre-roll means the take starts *before* the press,
 *                 which is the difference between sampling being usable over
 *                 a wireless control surface and being an exercise in
 *                 anticipation.
 *
 *   the staging   One PSRAM buffer, OSYNTH_SAMPLE_MAX_SEC long, allocated on
 *   buffer        the first arm and then kept. A take lands here at full
 *                 length; the trimming, normalising and slicing all happen on
 *                 the way *out* of it, on a control task, so the audio task's
 *                 job during a recording is a bounded memcpy and a peak
 *                 compare and nothing else.
 *
 * ---------------------------------------------------------------------------
 * Threading
 *
 * sampler_capture() is audio-task only and is the only thing on that side. It
 * pushes the ring, appends to staging while recording, and sets a flag when
 * the take hits its ceiling. Everything that allocates, frees, trims, slices,
 * writes a card or republishes a slot runs on the drum control task, which is
 * where the parameter listener hands it.
 *
 * Publishing a finished sample into a live kit is the same hazard drums.cpp's
 * kit swap documents — a voice latches a raw sample pointer at trigger time
 * and holds it for its whole decay — so the same remedy applies, scoped to one
 * slot: silence the voices playing that slot, wait two render boundaries, then
 * release the block it was pointing at. Which is also why undo is nearly free
 * here: "release" for a replaced pad means "move to the undo stash", and the
 * stash is what the next commit releases instead.
 * ---------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "drums.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- parameters (0x075x-0x076x, inside the drum bus's 0x07xx range) ----
 *
 * Ranges, defaults and units in docs/PARAM_MAP.md. `smp.arm` / `smp.rec` are
 * deliberately two parameters and not one: the destination and the gate are
 * set by different gestures at different times (tap a pad, then hold a
 * button — or the reverse), and a single "record into slot N" write could
 * only ever express one of the two orderings. */
#define SMP_PID_SRC       0x0750 /* enum  input | bus                        */
#define SMP_PID_ARM       0x0751 /* int   -1 = none, else the destination pad */
#define SMP_PID_REC       0x0752 /* bool  the gate: 1 while held             */
#define SMP_PID_ERASE     0x0753 /* int   pad to erase, then self-clears     */
#define SMP_PID_UNDO      0x0754 /* bool  trigger: undo the last commit      */
#define SMP_PID_THRESH    0x0755 /* float 0..1, 0 = start on the gate        */
#define SMP_PID_TRIM      0x0756 /* bool  drop the silent tail               */
#define SMP_PID_NORM      0x0757 /* bool  normalise into the slot's gain     */
#define SMP_PID_PREROLL   0x0758 /* float ms of pre-roll to prepend          */
#define SMP_PID_MAXSEC    0x0759 /* float take ceiling, seconds              */
#define SMP_PID_SLICES    0x075A /* int   1 = off, 2..16 = slice the take    */
#define SMP_PID_SLICEMODE 0x075B /* enum  even | transient                   */
#define SMP_PID_MONITOR   0x075C /* bool  hear the source while armed        */
#define SMP_PID_COUNTIN   0x075D /* int   beats of click before the take     */
#define SMP_PID_SAVE      0x075E /* bool  trigger: write this kit to storage */
#define SMP_PID_GAIN      0x075F /* float record trim, applied on capture    */
#define SMP_PID_COPYFROM  0x0760 /* int   source pad for a copy, -1 = none   */
#define SMP_PID_COPYKIT   0x0761 /* int   source kit, -1 = the current one   */
#define SMP_PID_COPYTO    0x0762 /* int   dest pad; writing it runs the copy */
#define SMP_PID_DUPKIT    0x0763 /* int   dest kit; writing it duplicates    */
/* Read-only telemetry. The app draws the record button's state from these
 * rather than from what it last sent, because the firmware is what actually
 * decides when a threshold-armed take starts and when the ceiling stops it —
 * an app that assumed its own writes were the truth would show "recording"
 * through a whole take that never triggered. */
#define SMP_PID_STATE     0x0764 /* int   sampler_state_t                    */
#define SMP_PID_POS       0x0765 /* float 0..1 through the take ceiling      */
#define SMP_PID_FREE      0x0766 /* float pool bytes still available, KB     */
#define SMP_PID_PEAK      0x0767 /* float loudest |sample| of the live take  */

/* What the recorder is doing, published as SMP_PID_STATE. */
typedef enum {
    SAMPLER_IDLE = 0,      /* nothing armed                                  */
    SAMPLER_ARMED = 1,     /* a pad is chosen, waiting for the gate          */
    SAMPLER_WAITING = 2,   /* gate held, waiting for smp.thresh or a count-in */
    SAMPLER_RECORDING = 3, /* capturing into staging                         */
    SAMPLER_COMMITTING = 4 /* trimming/slicing/publishing on the control task */
} sampler_state_t;

/* Registers the 0x075x parameters and seeds the pool accounting. Called from
 * drums_init() *after* the kit table exists, since arming a pad has to be able
 * to name one. Never fails hard: a build with no PSRAM registers the
 * parameters anyway and refuses every take with a logged reason, so the app's
 * controls exist and explain themselves rather than silently missing. */
esp_err_t sampler_init(void);

/* Audio task, once per block, from the render chain.
 *
 * `l` / `r` are the master bus at the point this is called, which is what
 * `smp.src = bus` records — so where the call sits in render_chain() *is* the
 * definition of that source. It goes after the looper and before the
 * metronome, for the same two reasons the looper's own tap does: a resample
 * should capture loops that are playing, and a count-in click must never end
 * up inside the sample it was counting in. */
void sampler_capture(const float* l, const float* r, size_t frames);

/* True while a take is being captured — the render chain uses it to keep the
 * input monitored through `smp.monitor` without the player having to set
 * `in.route` by hand. */
bool sampler_recording(void);

/* Erase one pad, publish the change, and stash what was there for undo.
 * Control task; blocks for two render boundaries. */
esp_err_t sampler_erase(int kit, int slot);

/* Put back whatever the last commit, erase or copy replaced. One deep: the
 * stash holds exactly one pad, because the block it is holding is the block
 * the pool cannot reuse, and an unbounded undo history is an unbounded leak
 * dressed as a feature. Returns ESP_ERR_NOT_FOUND when there is nothing to
 * undo. */
esp_err_t sampler_undo(void);

/* Copy a pad, within a kit or across two. `from_kit` < 0 means the current
 * one. The destination gets its own block — sharing one between two kits
 * would make erasing either of them a use-after-free in the other. */
esp_err_t sampler_copy_pad(int from_kit, int from_slot, int to_kit, int to_slot);

/* Duplicate a whole kit onto another index, samples included. Fails with
 * ESP_ERR_NO_MEM without touching the destination if the pool cannot hold the
 * copy — a half-duplicated kit is worse than a refused one. */
esp_err_t sampler_dup_kit(int from_kit, int to_kit);

/* Pool accounting, for the app's "space left" readout and for the log line. */
size_t sampler_pool_used(void);
size_t sampler_pool_total(void);

/* Allocate/release sample data against the pool budget. Exposed because the
 * kit loader (kit_store.h) fills user kits from storage through the same
 * accounting the recorder uses — two budgets for one pool would let a card
 * full of long samples starve live recording without either side noticing. */
void* sampler_pool_alloc(size_t bytes);
void sampler_pool_free(void* p, size_t bytes);

#ifdef __cplusplus
}
#endif
