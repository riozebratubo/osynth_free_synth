/*
 * osynth — engine interface (Session 5).
 *
 * An engine is a static vtable plus a per-voice state blob. The voice
 * manager owns allocation/retrigger/stealing, glide, unison fan-out and the
 * sustain pedal, and calls into the bound engine; the engine owns the
 * per-voice DSP (shared blocks from synth_dsp.h) and its 0x02xx parameter
 * set. Engines are singletons — one engine is bound at a time.
 *
 * Threading: init()/deinit() run on a control task (the S6 engine-switch
 * protocol). Everything else — begin_block(), voice_reset(), note_on/off(),
 * render(), busy(), level() — is called by the audio task only.
 *
 * Module gating: `caps` declares which shared blocks the engine uses.
 * Undeclared blocks are neither allocated nor processed (they simply are
 * not part of the engine's voice state). The mask is also served to the
 * BLE/UI side (S14) so control surfaces can hide dead controls.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYNTH_CAP_FILTER    (1u << 0)
#define SYNTH_CAP_ENV2      (1u << 1)
#define SYNTH_CAP_LFO1      (1u << 2)
#define SYNTH_CAP_LFO2      (1u << 3)
#define SYNTH_CAP_MIXER     (1u << 4)
/* engine routes per-voice param reads through synth_mod_apply() (S9); the
 * voice manager only builds the matrix plan for engines that declare it */
#define SYNTH_CAP_MODMATRIX (1u << 5)

/* Per-voice, per-block values computed by the voice manager: pitch after
 * glide + bend + unison detune; gains after voice headroom + unison pan. */
typedef struct {
    float freq_hz;
    float gain_l;
    float gain_r;
} synth_voice_frame_t;

typedef struct synth_engine {
    const char* name;
    uint32_t caps;     /* SYNTH_CAP_* bitmask (module gating) */
    size_t voice_size; /* bytes of per-voice state */

    /* Control task. init registers the engine's 0x02xx parameters (and
     * builds any tables); deinit unregisters them again (engine switch, S6). */
    esp_err_t (*init)(void);
    void (*deinit)(void);

    /* Audio task, once per block before any render(): read parameters and
     * build the block-shared coefficients. */
    void (*begin_block)(size_t frames);

    void (*voice_reset)(void* v); /* hard-silence one voice */

    /* was_sounding: the voice is being retriggered or stolen while audible —
     * keep phases/envelope level continuous instead of restarting. */
    void (*note_on)(void* v, uint8_t note, float vel01, bool was_sounding);
    void (*note_off)(void* v); /* enter release */

    /* Mixes one voice additively into out_l/out_r. */
    void (*render)(void* v, const synth_voice_frame_t* f, float* out_l,
                   float* out_r, size_t frames);

    bool (*busy)(const void* v);   /* still audible (or gate held) */
    float (*level)(const void* v); /* amp-env level — voice-steal ranking */

    /* Optional batched render (S28), placed last so adding it did not
     * renumber the existing positional initializers. Every vtable must
     * still list it explicitly — the build runs with
     * -Werror=missing-field-initializers, so an omitted trailing member is
     * an error rather than an implicit null.
     *
     * When non-null the voice manager calls this ONCE with every active
     * voice instead of calling render() per voice, and render() is not
     * called at all.
     *
     * It exists for the modular graph, where the per-voice loop is the wrong
     * nesting: a graph's topology is identical for every voice, so entering
     * each node once and looping voices inside it pays the dispatch, the
     * parameter reads and the block coefficient math once rather than once
     * per voice. A fixed engine has nothing to amortize — its whole chain is
     * already fused into a single loop — which is why this is an option and
     * not the contract.
     *
     * `states` and `frames` are parallel arrays of length n_voices, holding
     * only the voices that are actually sounding. */
    void (*render_block)(void* const* states, const synth_voice_frame_t* frames,
                         size_t n_voices, float* out_l, float* out_r,
                         size_t frames_n);
} synth_engine_t;

#ifdef __cplusplus
}
#endif
