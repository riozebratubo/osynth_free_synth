/*
 * osynth — the interface a master-bus reverb algorithm implements (S36).
 *
 * The FX bus has one reverb unit and four topologies behind it (fx.rev.algo).
 * They live in two components for licensing reasons — freeverb and WetReverb
 * are MIT and build inside `fx`, MVerb and DuskVerb are GPL-3 and build inside
 * `fx_gpl` under CONFIG_OSYNTH_FX_GPL — so the contract between them has to
 * be somewhere neither component owns. That is here.
 *
 * Contract, all of which fx.cpp's reverb_process() relies on:
 *
 *  - init() runs once, from fx_init(), before the audio task exists. It is
 *    the *only* place an algorithm may allocate. Returning false means "I
 *    could not get my lines"; the bus then refuses to select this algorithm
 *    rather than rendering silence, which is the same sink-fallback rule the
 *    rest of the FX bus follows.
 *
 *  - render() runs on the audio task. No locks, no allocation, no logging.
 *    It writes the WET signal only: pre-delay, tone, width and the dry/wet
 *    crossfade are the bus's job, done identically for every algorithm, so
 *    that switching topology changes the character and not the level.
 *
 *  - reset() must leave no audible trace of what came before. It is called on
 *    an algorithm switch, and separately the bus scrubs lines() incrementally
 *    — reset() clears the cheap recursive state (filter memories, phases),
 *    lines() hands over the expensive buffers for the bus to zero a chunk at
 *    a time, because a 200 KB memset does not fit in a 1.33 ms block.
 *
 * The four parameters are normalized 0..1 and mean the same thing to every
 * algorithm; each maps them onto its own controls. They are deliberately
 * fewer than any of these reverbs exposes in its native plugin: the unit has
 * one set of knobs whatever is selected, because a control surface that
 * relabels itself on an enum change is one nobody can learn.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "synth_line.h"

namespace osynth {
namespace fx {

struct RevParams {
    float size;  /* 0 = small/short, 1 = large/long */
    float damp;  /* 0 = bright tail, 1 = dark tail */
    float diff;  /* 0 = sparse and echoey, 1 = dense and smooth */
    float early; /* 0 = late field only, 1 = early reflections only */
};

class RevAlgorithm {
   public:
    virtual ~RevAlgorithm() = default;

    /* Boot-time. Allocates; may fail. */
    virtual bool init(uint32_t sample_rate) = 0;

    /* Clears recursive state. Buffers are the bus's problem — see lines(). */
    virtual void reset() = 0;

    /* One block of wet, audio task only. `n` is at most SYNTH_BLOCK_SIZE.
     *
     * in_* and out_* MAY be the same buffers — the bus calls it that way,
     * with the pre-delay's output as both. An implementation must therefore
     * read every input sample it needs before writing the output at that
     * index, and must not mark these __restrict__. */
    virtual void render(const float* in_l, const float* in_r, float* out_l,
                        float* out_r, size_t n, const RevParams& p) = 0;

    /* Every delay line this algorithm owns, for the bus's bypass scrub.
     * Returns how many were written to `out`. */
    virtual size_t lines(osynth::dsp::Line** out, size_t max) = 0;
};

}  // namespace fx
}  // namespace osynth
