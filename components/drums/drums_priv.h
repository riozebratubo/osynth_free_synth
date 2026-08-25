/*
 * osynth — internals shared between drums.cpp and sampler.cpp (Session 44),
 * private to components/drums.
 *
 * The recorder and the player are two files because they are two jobs with
 * two threading stories, but they are one component because they share the
 * one thing neither can own alone: the resident kit table. This header is
 * that seam, and it is deliberately narrow — the recorder never touches a
 * voice, and the player never learns where a sample came from.
 *
 * Everything here is control-task only unless it says otherwise.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "drum_kit.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The resident kit table. Index 0 is the factory kit; 1..SYNTH_SAMPLE_KITS
 * are the recordable ones. Returns nullptr for an out-of-range index.
 *
 * Mutable, and only safe to *mutate* through drums_slot_replace() below or
 * before the audio task starts. Reading fields that no voice latches — names,
 * mix values, the dirty flag — is fine from any control task. */
drum_kit_t* drums_kit_at(int index);

/* Swaps one pad in one kit, safely, and hands back what was there.
 *
 * This is the whole of the S22 kit-swap protocol reduced to a single slot,
 * and it is the only correct way to change a pad that a kit already has:
 *
 *   1. bump the kit generation, so the sampler engine drops any voice holding
 *      the old block at its next block start;
 *   2. silence the drum voices playing this slot, with the standard declick —
 *      they hold `data` directly and will not re-read it;
 *   3. wait two render boundaries, which is what makes step 2 a guarantee
 *      rather than a hope (the first block may still have loaded the old
 *      pointer; the second provably saw the new one);
 *   4. write the slot and mark the kit dirty.
 *
 * `fresh` is copied in; pass a zeroed sample to empty the pad. `out_old`
 * receives the previous contents — including its `owned` block, which the
 * caller now owns and must either stash for undo or release into the pool.
 * Releasing it *before* this returns would be the use-after-free the four
 * steps above exist to prevent.
 *
 * Blocks for up to ~1 s in the pathological case (a stalled audio task) and
 * returns ESP_ERR_TIMEOUT having changed nothing, so a caller that cannot
 * afford to block should not be on this path at all. */
esp_err_t drums_slot_replace(int kit, int slot, const drum_sample_t* fresh,
                             drum_sample_t* out_old);

/* Pushes a kit's stored mixer values into the drumN.* parameters. Called on a
 * kit switch; see drum_slot_mix_t in drum_kit.h for why a kit carries them. */
void drums_apply_kit_mix(int index);

/* Snapshots the live drumN.* parameters back into the bound kit's stored mix,
 * so a switch away does not lose what was just dialled in. */
void drums_capture_kit_mix(void);

/* Marks a user kit as changed since its last write to storage. */
void drums_kit_mark_dirty(int index);

#ifdef __cplusplus
}
#endif
