/*
 * osynth — modular patch graph: the model (Session 28).
 *
 * The four engines (S5-S8) are fixed topologies: an engine's render() is a
 * hand-written chain of shared blocks, and what you can change is the
 * parameters, never the wiring. This is the other half — a small node graph
 * the app patches with cables, bound to the voice manager as a fifth engine
 * (SYNTH_ENGINE_MODULAR) so the S6 switch protocol carries the bind/unbind
 * handshake for free and none of the tuned engines are touched.
 *
 * Three rules shape everything here, and each one is load-bearing:
 *
 * 1. **Topology is not parameter space.** ParamStore ids are a flat u16
 *    space with a fixed budget; a graph's *structure* (which node is in
 *    which slot, which port feeds which) could never live there, exactly as
 *    S23 decided for pattern data. Structure lives in this model, travels
 *    over its own BLE opcodes, and is serialized into presets. Only the
 *    per-node *values* are parameters.
 *
 * 2. **Node parameter ids are positional, not identity-based.** Slot k owns
 *    the 16 ids at PID_ENGINE_BASE + k*16, forever, whatever node is in it.
 *    An identity scheme (allocate ids as nodes are created) would be tidier
 *    to read and catastrophic in practice: the sequencer's parameter locks
 *    and every saved preset store a bare {id, value}, so an id that moved
 *    when you re-patched would silently apply the wrong lock to the wrong
 *    control. Positional ids cannot move. What *can* change is the meaning
 *    of a slot when its node kind changes, which is why the kind is stored
 *    alongside the value in presets and a mismatch invalidates rather than
 *    misapplies.
 *
 * 3. **Nothing here is allocated at run time.** Slots, edges and voice state
 *    are fixed arrays sized by the constants below. The compiler
 *    (graph_compile.cpp) then refuses an edit whose estimated cost exceeds
 *    the block budget, so a patch either fits or is rejected with a reason —
 *    it never becomes an underrun.
 *
 * Threading: the whole model is control-task state, owned by the `graph_ctl`
 * task. The audio task never reads it — it reads the *compiled plan*
 * (graph_compile.h), which is double-buffered and swapped under a ramp. Edit
 * calls below are therefore control-task only.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

#include "synth_params.h"

namespace osynth::graph {

/* ---- capacities ----
 *
 * 12 slots x 16 ids = 192 of the 256 ids in the 0x02xx engine range, leaving
 * 0x02C0..0x02FF for the graph-global parameters. Twelve nodes is not an
 * arbitrary round number: it is what the id range affords at 16 params per
 * node, and 16 is the widest node kind (a 4-input mixer with per-input level
 * and trim). Edges are capped at twice the node count — a graph where the
 * average node has more than two incoming cables is not a synth patch. */
inline constexpr int kMaxNodes = 12;
inline constexpr int kNodeParams = 16;
inline constexpr int kMaxEdges = 24;
inline constexpr int kMaxInputs = 4;

/* Slot 0 is reserved for the output node: every graph has exactly one sink,
 * and pinning it to a known slot means the compiler never has to search for
 * it and the app can always draw it. */
inline constexpr int kOutSlot = 0;

/* ---- node kinds ---- */

enum class Kind : uint8_t {
    Empty = 0,
    /* audio rate — one value per sample per voice */
    Osc,
    Noise,
    Filter,
    Vca,
    Mix,
    Shaper,
    RingMod,
    /* control rate — one value per block per voice */
    /* (the S33 filters are appended after Out — see the note there) */
    Env,
    Lfo,
    SampleHold,
    ModMap,
    MidiSrc,
    /* sink */
    Out,
    /* The rest of the filter family (S33), audio rate. Appended here rather
     * than beside Filter because the value is the on-wire patch format —
     * inserting one would silently turn every saved node above it into a
     * different kind. Each heavy topology is its own kind, deliberately: the
     * compile-time budget check costs a patch from the kind table, and a
     * `type` parameter inside one Filter kind would let a live edit triple
     * the real cost of a patch that was checked at one third of it. */
    Filter24, /* two SVFs in series, Butterworth Q pair */
    Ladder,   /* 4-pole Moog, saturated feedback */
    Dual,     /* lowpass + highpass, spread apart */
    Vowel,    /* three morphing formants */
    /* The analogue input as a source (S31f), audio rate. Appended for the same
     * reason the filters above were: the value is the on-wire patch format.
     *
     * Registered on every build, including ones with no line input at all, so
     * that the kind indices a saved patch stores mean the same thing
     * everywhere; without the hardware it renders silence. */
    LineIn,
    Count
};

/* A node runs at one of two rates, and this is the single biggest lever on
 * what a patch costs. An audio-rate node touches every sample of every
 * sounding voice; a control-rate node produces one float per voice per block
 * — 64x cheaper at the default block size, and cheap enough that the whole
 * modulation half of a patch is effectively free. It is also why control
 * nodes are not required to live in IRAM: they run once per block, so a
 * flash-cache miss is amortized over the whole block instead of landing in
 * the inner loop. */
enum class Rate : uint8_t { Control = 0, Audio = 1 };

/* Per-kind parameter indices: the position of a parameter inside its slot's
 * 16-id block. The render path indexes Plan::pp with these and presets
 * store values against the resulting ids, so they are part of the on-wire
 * format — appending to a kind is safe, reordering or inserting is not. */
namespace pidx {
enum OscP { OSC_WAVE = 0, OSC_SEMI, OSC_FINE, OSC_PW, OSC_FM, OSC_LEVEL, OSC_N };
enum NoiseP { NOI_LEVEL = 0, NOI_N };
/* FLT_ON/FLT_DRIVE appended in S33 — they land after cutamt in the app's
 * parameter list, which is the price of not moving what came before them.
 * Filter24 repeats the block so the two are interchangeable in a patch. */
enum FilterP {
    FLT_MODE = 0, FLT_CUTOFF, FLT_RESO, FLT_KBD, FLT_CUTAMT, FLT_ON,
    FLT_DRIVE, FLT_N
};
enum LadderP {
    LAD_ON = 0, LAD_CUTOFF, LAD_RESO, LAD_DRIVE, LAD_KBD, LAD_CUTAMT, LAD_N
};
enum DualP {
    DUA_ON = 0, DUA_CUTOFF, DUA_RESO, DUA_SPREAD, DUA_DRIVE, DUA_KBD,
    DUA_CUTAMT, DUA_N
};
/* The mod input drives the vowel morph here rather than a cutoff: sweeping
 * a-e-i-o-u is what this node is for, and the formant shift is left to
 * keyboard tracking. */
enum VowelP {
    VOW_ON = 0, VOW_VOWEL, VOW_RESO, VOW_SHIFT, VOW_DRIVE, VOW_KBD,
    VOW_MODAMT, VOW_N
};
enum VcaP { VCA_GAIN = 0, VCA_DEPTH, VCA_N };
enum MixP { MIX_L0 = 0, MIX_L1, MIX_L2, MIX_L3, MIX_N };
enum ShaperP { SHP_MODE = 0, SHP_DRIVE, SHP_AMT, SHP_N };
enum RingP { RNG_AMOUNT = 0, RNG_N };
enum EnvP { ENV_A = 0, ENV_D, ENV_S, ENV_R, ENV_N };
enum LfoP { LFO_WAVE = 0, LFO_RATE, LFO_DEPTH, LFO_RATEAMT, LFO_RETRIG, LFO_UNI, LFO_N };
enum SahP { SAH_RATE = 0, SAH_N };
enum ModMapP { MM_SCALE = 0, MM_OFFSET, MM_QUANT, MM_N };
enum MidiP { MS_SRC = 0, MS_N };
enum OutP { OUT_LEVEL = 0, OUT_PAN, OUT_N };
/* LineIn (S31f). `mode` leads because it is the parameter that decides whether
 * the node costs anything at all — see LineMode below. */
enum LineP { LIN_MODE = 0, LIN_LEVEL, LIN_CHAN, LIN_N };
} // namespace pidx

/* What a LineIn node does with the keyboard.
 *
 * The graph is a *per-voice* engine: every node is evaluated once per sounding
 * voice, and the output stage sums those voices. A source that is the same for
 * all of them therefore needs an explicit answer to "how many times does it
 * appear?", and there is no single right one — hence a parameter rather than a
 * choice baked into the node.
 *
 *   Off   Renders silence and, more to the point, does not ask the engine to
 *         keep rendering while nothing is held. Parking a node here costs
 *         nothing; it is the switch, not a mute.
 *   Free  One copy, always, whether or not a key is down — a line input that
 *         behaves like a line input. The graph renders one extra voice row to
 *         hold it (see graph_render.h), which is what this mode costs.
 *   Gate  One copy per sounding voice, shaped by that voice's own chain. Three
 *         keys held means three copies, each with its own envelope and filter
 *         — which is the point: it turns the patch into a processor the
 *         keyboard plays. */
enum class LineMode : uint8_t { Off = 0, Free, Gate, Count };

/* What a MidiSrc node emits. These are the graph's replacement for the S9
 * mod matrix's global sources — in a patchable graph a "source" is just a
 * node you draw a cable from. */
enum class MidiSource : uint8_t {
    Vel = 0,   /* note-on velocity, 0..1 */
    Note,      /* keyboard position, (note - 60) / 60 */
    Gate,      /* 1 while the key is held, 0 after release */
    Bend,      /* pitch bend, -1..1 (raw, before common.bend.range) */
    Wheel,     /* mod wheel (CC 1), 0..1 */
    Rand,      /* one value per note-on, 0..1 */
    Count
};

/* ---- parameter specs ----
 *
 * A node kind declares its parameters as name suffixes; the registered name
 * is "n<slot>.<suffix>" (e.g. "n3.cutoff"), built once into a static table
 * because ParamDesc::name must stay valid for the lifetime of the store. */
struct ParamSpec {
    const char* suffix;
    ParamType type;
    ParamCurve curve;
    float min;
    float max;
    float def;
    const char* const* enum_names; /* Enum only */
    uint8_t enum_count;
};

struct KindDesc {
    const char* name;                    /* "osc", "filter", … */
    Rate rate;
    uint8_t n_inputs;
    const char* const* input_names;      /* n_inputs entries */
    uint8_t n_params;
    const ParamSpec* params;             /* n_params entries */
    /* Cost in units where 1000 = the whole per-block CPU budget at full
     * polyphony. Audio-rate costs scale with frames, control-rate costs do
     * not; graph_compile.cpp turns these into the budget check. Calibrated
     * against the S17 per-engine measurements, then corrected by
     * measurement — see tools/graph_cost_calib.py. */
    uint16_t cost;
};

/* Descriptor for a kind; never null for a valid Kind (Empty included). */
const KindDesc& kind_desc(Kind k);

/* "osc", "filter", … — for the BLE kind list and logs. */
const char* kind_name(Kind k);

/* ---- the model ---- */

struct Node {
    Kind kind = Kind::Empty;
    /* Model-space input wiring: source slot per input, or -1 for "not
     * connected". A disconnected input reads its documented rest value
     * (silence for audio inputs, the node's own parameter for modulation
     * inputs) — an unpatched cable is never an error. */
    int8_t in[kMaxInputs] = {-1, -1, -1, -1};
    /* Layout hint for the app's canvas. The firmware never reads these; they
     * ride along in the model so a patch keeps its shape across a save, a
     * reload and a different phone. */
    int16_t ui_x = 0;
    int16_t ui_y = 0;
};

struct Model {
    Node nodes[kMaxNodes];
    /* Bumped on every accepted structural edit. The app uses it to detect
     * that its view is stale; the compiler stamps it into the plan so the
     * audio task can tell one plan from another. */
    uint32_t revision = 0;
};

/* ---- parameter ids ---- */

/* Slot k, parameter index p. Positional by construction — see the header
 * comment for why this matters more than it looks. */
inline constexpr uint16_t node_pid(int slot, int p) {
    return (uint16_t)(PID_ENGINE_BASE + slot * kNodeParams + p);
}

/* Graph-global parameters live above the node slots, in the tail of the
 * engine range that 12x16 leaves free. */
inline constexpr uint16_t kGraphGlobalBase =
    (uint16_t)(PID_ENGINE_BASE + kMaxNodes * kNodeParams); /* 0x02C0 */
inline constexpr uint16_t PID_GRAPH_COST = kGraphGlobalBase + 0; /* read-only */
/* Mirrors Model::revision. Read-only, and it exists so that "the graph
 * changed" needs no new event opcode and no dependency from the graph up
 * into ble_ctrl: a parameter written with a non-Ble origin is already
 * batched to the app at ~20 Hz by the S14 listener, so the app learns to
 * re-read the model through machinery that is known to work. It also
 * covers the case a dedicated opcode would have missed — a preset load
 * replacing the graph without the app having asked for anything. */
inline constexpr uint16_t PID_GRAPH_REV = kGraphGlobalBase + 1; /* read-only */
/* kGraphGlobalBase + 2 was `graph.fuse` in the first cut of S28 and is left
 * unused rather than recycled: preset files written during that window may
 * still carry it, and a value landing on a different control would be worse
 * than an id that resolves to nothing. */

/* ---- edit API (control task only) ----
 *
 * Every edit is validated, compiled and cost-checked before it is adopted;
 * a rejected edit leaves the model exactly as it was. The functions below
 * therefore either fully succeed or change nothing, and the error tells the
 * app which it was:
 *   ESP_ERR_INVALID_ARG   — bad slot / kind / port index
 *   ESP_ERR_INVALID_STATE — the edit would create a cycle
 *   ESP_ERR_NO_MEM        — over the node/edge budget
 *   ESP_ERR_NOT_SUPPORTED — over the CPU budget (the patch is too expensive)
 */

/* Snapshot of the current model — for the app's full-graph read and for
 * preset serialization. */
const Model& model();

/* Replaces the node in `slot` (registering that slot's parameter set for
 * the new kind and dropping the old one). Kind::Empty clears the slot and
 * every cable touching it. Slot kOutSlot may not be cleared or changed. */
esp_err_t set_kind(int slot, Kind kind);

/* Connects `src` slot's output to `dst` slot's input `port`; src < 0
 * disconnects. Rejected if it would close a loop — a cycle has no
 * topological order, so there is no sample order that could evaluate it. */
esp_err_t connect(int dst, int port, int src);

/* Canvas position, app-owned, no audio effect. */
esp_err_t set_ui_pos(int slot, int16_t x, int16_t y);

/* Replaces the whole model in one edit — a preset load or the app pushing a
 * patch. Compiled and cost-checked as a unit, so a bad file cannot leave a
 * half-applied graph behind. */
esp_err_t load_model(const Model& m);

/* Clears to the default patch: a single Out node fed by nothing (silence).
 * Used at engine bind when no preset supplies a graph. */
void reset_model();

/* ---- engine bind / unbind (control task, S6 switch protocol) ----
 *
 * bind() registers the parameter set of every occupied slot, compiles the
 * model and publishes the plan; unbind() retires the plan first and only
 * then drops the engine range, which is the ordering that makes the
 * teardown safe — a plan holds resolved value pointers into that range. */
esp_err_t bind();
void unbind();

/* ---- serialization ----
 *
 * Format v1: {magic 'OGR1', revision, node count} then per node
 * {kind, in[4], ui_x, ui_y}. Parameter *values* are not stored here — they
 * are ordinary ParamStore ids and travel through the existing preset
 * mechanism, which is what keeps a modular patch a normal preset rather
 * than a special case. The node kinds stored alongside are what let a load
 * detect that a slot's meaning changed and drop that slot's stale values
 * instead of applying them to the wrong control. */
inline constexpr uint32_t kSerialMagic = 0x3152474Fu; /* 'OGR1' little-endian */

/* Returns bytes written, or 0 if `cap` is too small. */
size_t serialize(const Model& m, uint8_t* out, size_t cap);

/* Parses into `out`; returns false on a bad magic, a truncated buffer, or a
 * node count larger than this build supports (a graph authored on a build
 * with more slots loads the slots that fit, like S27's oversized patterns).
 */
bool deserialize(const uint8_t* in, size_t len, Model& out);

/* Upper bound on serialize() output — for preset buffer sizing. */
inline constexpr size_t kSerialMaxBytes = 12 + kMaxNodes * 10;

} // namespace osynth::graph
