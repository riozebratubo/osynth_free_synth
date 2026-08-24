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

/* ---- what the graph is allowed to cost ----
 *
 * The budget is what is left of the block after everything *else* in
 * render_chain() has taken its share, so it is derived here from a
 * reservation per stage rather than written down as a number. Two reasons for
 * the arithmetic being in the code instead of in the paragraph it used to
 * live in: the total can no longer drift away from the reasons for it, and a
 * stage that gets measured later is one line to change rather than a sum to
 * redo by hand.
 *
 * Scale: 1000 units = the whole per-block CPU budget at full polyphony, so
 * one unit is 0.1 % of the block and the percentages below convert by x10.
 *
 * Measured on an ESP32-S3 at 240 MHz, 8 voices, block 64 (S28 calibration),
 * except where marked. The stages are exactly the calls render_chain() makes,
 * in order — that list is the thing this table has to stay in step with. */
namespace budget {

/* The block is not allowed to run to 100 %: the render is bursty and the
 * check has only the EMA to work with, so the ceiling is on the *peak* and
 * the observed peak/EMA ratio converts it. 1.30-1.40 observed; the worst is
 * the one to reserve against. */
inline constexpr float kPeakCeilingPct = 90.0f;
inline constexpr float kPeakEmaRatio = 1.40f;

/* audio_in_capture(), for every compiled device. audio_io.cpp puts one device
 * at ~0.7 % of the block and reads *every* device every block by design, so a
 * two-input board pays twice. Reserved on all builds rather than only where
 * an input is compiled: a build-dependent budget would mean a patch saved on
 * one board being refused on another, which is a worse failure than 14 units
 * of headroom on a board with no input socket. */
inline constexpr float kInputPct = 1.4f;

/* fx_process(). Measured with the bus in its default state — which has the
 * reverb on — and NOT with all fourteen units enabled at once.
 *
 * This is the number the old note claimed was a worst case and was not. It is
 * left at the measurement rather than inflated to a guess, because a guess is
 * how this constant went wrong twice already; what changed is that it no
 * longer *says* it covers the worst case. A patch that fits with the default
 * bus and underruns once you switch on the delay, the granular delay and the
 * vocoder is a real and currently unguarded case — see kUnmeasuredNote. */
inline constexpr float kFxPct = 18.3f;

/* looper_process(). Measured idle. Eight tracks of serial ADPCM decode out of
 * PSRAM is not this number, and nobody has measured what it is. */
inline constexpr float kLooperPct = 0.4f;

/* drums_pre_fx() + drums_post_fx() + drums_render_click().
 *
 * Unmeasured, and reserved at zero, which is what the budget was already
 * doing by leaving the drum bus out of its table entirely — this line exists
 * so that the omission is visible in the arithmetic instead of absent from
 * it. The per-voice kernel is cheap by construction (two table reads, a lerp,
 * two multiply-accumulates and a decay multiply — no filter, no envelope
 * generator; see drums.cpp), so eight of them are not a large fraction of a
 * synth voice's cost. "Not large" is not a measurement. */
inline constexpr float kDrumsPct = 0.0f;

/* Per-block overhead outside every stage: the clear, the master gain ramp,
 * the metering, the int16 conversion and the sink write. */
inline constexpr float kIdlePct = 1.0f;

inline constexpr float kReservedPct =
    kInputPct + kFxPct + kLooperPct + kDrumsPct + kIdlePct;
inline constexpr float kGraphPct =
    kPeakCeilingPct / kPeakEmaRatio - kReservedPct;

} // namespace budget

/* Reject a patch above this many cost units.
 *
 * The first cut of this constant was 700, written as though the graph were
 * the only thing rendering. It is not: a patch at 700 would have sat near
 * 89 % EMA and peaked past 100 %, i.e. continuous underruns waved through by
 * the very check that exists to prevent them. The second was 440, which had
 * the right shape but priced only three of the six stages above.
 *
 * As the reservations stand this comes out at 431 (the truncation is in the
 * safe direction). The factory patch costs 307, so a patch still has roughly
 * one more oscillator of room.
 *
 * kUnmeasuredNote: two of the reservations above are honest about not being
 * worst cases — the FX bus is priced at its default rather than fully loaded,
 * and the drum bus is priced at zero. A patch inside this budget can still
 * underrun in combination with those, and the compile-time check cannot see
 * it coming, because what the FX switches and the drum pattern will be later
 * is not a property of the patch. What catches it instead is the runtime
 * warning in main.cpp's heartbeat, which names this constant when the
 * measured load actually goes over. Closing the gap properly needs bench
 * numbers for a loaded FX bus and a busy kit — tools/graph_cost_calib.py is
 * the harness. Change these with a measurement, not a guess: both previous
 * values of this constant came from guesses, and both were wrong by more than
 * the margin they were guarding. */
inline constexpr uint16_t kCostBudget = (uint16_t)(budget::kGraphPct * 10.0f);
static_assert(kCostBudget > 0 && kCostBudget < 1000,
              "graph budget must leave room for the rest of the render chain");

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
