/*
 * osynth — modular patch graph: the compiler and the compiled plan (S28).
 *
 * The model (graph_model.h) is what the app edits: slots, kinds and cables.
 * It is a terrible thing to render from — every block would re-derive the
 * evaluation order, chase input pointers and re-resolve parameters. So an
 * accepted edit is *compiled* once, on the control task, into the flat Plan
 * below, and the audio task renders nothing but that.
 *
 * What compilation does, and why each pass earns its place:
 *
 *  - **Validation and cycle detection.** A cycle has no topological order,
 *    so there is no sample order that could evaluate it. Feedback in a
 *    modular is a real feature, but it needs an explicit one-block delay
 *    node to be well-defined; until that exists, a cycle is rejected at edit
 *    time rather than producing an undefined render.
 *
 *  - **Topological sort.** Yields the order in which nodes may be evaluated
 *    with every input already computed. Nodes unreachable from the Out node
 *    are dropped here: an orphaned node is a node the user unplugged, and
 *    paying for it every block because it is still on the canvas would be
 *    indefensible.
 *
 *  - **Rate classification.** Audio nodes get a block buffer per voice;
 *    control nodes get one float per voice. This is the single biggest
 *    lever on cost — see the Rate comment in graph_model.h.
 *
 *  - **Linear-scan buffer allocation.** The naive shape is one buffer per
 *    node, which at 8 voices x 64 frames is 2 KB each and would put a
 *    twelve-node patch at 24 KB of internal RAM and well outside the cache.
 *    Live-range analysis reuses them instead: a buffer is freed the moment
 *    its last consumer has run, so a deep chain needs two or three buffers
 *    no matter how long it is. The working set then stays small enough to
 *    live in cache, which matters more than the RAM saving.
 *
 *  - **In-place reuse.** A node whose single audio input has fan-out 1 and
 *    dies at that node writes its output over that input's buffer, halving
 *    the memory traffic of a chain. Correct precisely because fan-out 1 and
 *    last-use mean nobody else can observe the overwritten data.
 *
 * What is deliberately *not* here: **kernel fusion**. S28 shipped a pass that
 * matched `osc -> filter -> vca -> out` and rendered the run from one kernel
 * with the intermediates in registers, on the assumption that a graph pays
 * for its generality in memory traffic. Measured on hardware it was worth
 * nothing at all — 30.58% against 30.62% at 8 voices, the same number inside
 * the noise. The render loop is bound by dependent FPU latency (PolyBLEP,
 * then the SVF's serial integrators), and the LX7 issues the buffer loads
 * and stores alongside that work in slots it was not otherwise using. There
 * was no traffic to reclaim.
 *
 * It was removed rather than left in at zero benefit, because a fused kernel
 * duplicates the per-node ones and has to stay bit-identical to them
 * forever: the next parameter added to an oscillator would have had to be
 * mirrored in two places, and forgetting would have made patches sound
 * different depending on a toggle nobody thinks about. If a genuinely
 * memory-bound kernel appears later — a wavetable node reading flash, a
 * delay line — fusion is worth revisiting *for that node*, with a
 * measurement first.
 *
 *  - **Cost estimate and budget check.** Each kind carries a measured cost;
 *    the sum decides whether the edit is accepted. This is what makes a
 *    freely patchable graph safe on a microcontroller: an over-budget patch
 *    is refused with a reason at edit time, and never becomes an underrun
 *    the user has to diagnose by ear.
 *
 * Threading: compile() is a pure function of the model — control task only,
 * no side effects, safe to run on a candidate the user may never adopt.
 */
#pragma once

#include <atomic>
#include <cstdint>

#include "graph_model.h"

namespace osynth::graph {

/* Audio buffer pool size. Live-range reuse means depth costs nothing; what
 * needs buffers is *width* — parallel branches alive at the same time. Six
 * covers a four-input mixer fed by four independent chains, which is wider
 * than any patch this node budget can express. Each is
 * SYNTH_VOICES x SYNTH_BLOCK_SIZE floats. */
inline constexpr int kMaxBufs = 6;

/* Reject above this many cost units (1000 = the whole per-block budget at
 * full polyphony).
 *
 * Measured on an ESP32-S3 at 240 MHz, 8 voices, block 64 (S28 calibration):
 *
 *   FX bus, always on, default reverb        18.3 %
 *   looper                                    0.4 %
 *   idle per-block overhead                   1.0 %
 *   peak / EMA ratio, observed          1.30 - 1.40
 *
 * The first cut of this constant was 700, written as though the graph were
 * the only thing rendering. It is not: a patch at 700 would have sat near
 * 89 % EMA and peaked past 100 %, i.e. continuous underruns waved through
 * by the very check that exists to prevent them.
 *
 * Working back from a peak ceiling of ~90 % at the *worst* observed ratio:
 * EMA <= 90 / 1.40 = 64 %, less 18.7 % for FX and looper, less ~1 % idle,
 * leaves ~44 %. At the calibrated scale (1 unit = 0.1 % of the block) that
 * is 440 units. The factory patch costs 307, so a patch has roughly one
 * more oscillator of room — tight, and an honest reflection of what is left
 * after the FX bus takes its 18 %.
 *
 * Deliberately reserved against *worst-case* FX rather than whatever is
 * switched on right now: the compile-time check cannot know what the FX
 * mixes will be later, and a patch that fits until you add reverb is a
 * worse failure than one refused up front. Raising this is legitimate for a
 * rig that runs the FX bus dry — but do it with a measurement, not a guess.
 * Both previous values of this constant came from guesses, and both were
 * wrong by more than the margin they were guarding. */
inline constexpr uint16_t kCostBudget = 440;

struct PlanNode {
    Kind kind;
    uint8_t slot;      /* model slot: indexes voice state and parameters */
    Rate rate;
    /* Exactly one of these is >= 0 per input, or both are -1 (unpatched):
     * in_buf names an audio buffer, in_ctl names the control-rate slot whose
     * per-voice scalar feeds this input. Keeping them separate lets the
     * render path branch once per node instead of tagging every sample. */
    int8_t in_buf[kMaxInputs];
    int8_t in_ctl[kMaxInputs];
    int8_t out_buf;    /* audio nodes only; control nodes write ctl[slot] */
};

struct Plan {
    uint32_t revision = 0;
    uint8_t n_nodes = 0;
    uint8_t n_bufs = 0;
    uint16_t cost = 0;
    /* Topologically ordered and pruned: rendering is a straight walk from 0
     * to n_nodes with no lookahead and no indirection. */
    PlanNode nodes[kMaxNodes];
    /* Resolved once here rather than per block. Null for a parameter the
     * slot's kind does not define. Only valid while the plan is published —
     * the swap protocol guarantees the audio task has dropped a plan before
     * its parameter range is unregistered. */
    const std::atomic<float>* pp[kMaxNodes][kNodeParams];
};

/* Compiles `m` into `out`. Returns ESP_OK, or:
 *   ESP_ERR_INVALID_STATE — cycle
 *   ESP_ERR_NO_MEM        — needs more than kMaxBufs live buffers
 *   ESP_ERR_NOT_SUPPORTED — over kCostBudget
 * On failure `out` is left unusable and `err` (if non-null) gets a short
 * human-readable reason for the app to show. Parameter pointers are NOT
 * filled — the caller does that after registering the slots' parameter sets,
 * because compilation runs on candidates that may be rejected and must not
 * touch the registry. */
esp_err_t compile(const Model& m, Plan& out, char* err, size_t err_len);

/* Fills out.pp from the live ParamStore. Control task, after registration. */
void resolve_params(const Model& m, Plan& out);

} // namespace osynth::graph
