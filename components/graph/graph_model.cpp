/*
 * osynth — modular patch graph: the model (Session 28).
 *
 * Node kind table, positional parameter registration, the edit API and
 * serialization. See graph_model.h for the three rules this file exists to
 * enforce; the interesting part below is apply(), which is the only path by
 * which the audio task's view of the world changes.
 */
#include "graph_model.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"

#include "graph_compile.h"
#include "graph_render.h"
#include "synth_params.h"

static const char* TAG = "graph";

namespace osynth::graph {
namespace {

/* ---- enum value names (served over BLE PARAM_INFO) ---- */

const char* const kOscWaves[] = {"sine", "triangle", "saw", "pulse"};
/* Append-only: the index is what a saved patch stores. */
const char* const kFltModes[] = {"lp",   "bp", "hp", "notch",
                                 "peak", "ap", "bp norm"};
const char* const kLfoWaves[] = {"sine", "triangle", "saw", "square", "s&h"};
const char* const kShaperModes[] = {"tanh", "fold", "clip"};
const char* const kMidiSources[] = {"vel", "note", "gate", "bend", "wheel",
                                    "rand"};

/* ---- input port names (the app labels its cable jacks with these) ---- */

const char* const kInOsc[] = {"fm", "pitch"};
const char* const kInFilter[] = {"in", "cut"};
const char* const kInVowel[] = {"in", "vowel"};
const char* const kInVca[] = {"in", "gain"};
const char* const kInMix[] = {"in1", "in2", "in3", "in4"};
const char* const kInShaper[] = {"in", "drive"};
const char* const kInRing[] = {"a", "b"};
const char* const kInLfo[] = {"rate"};
const char* const kInSah[] = {"in"};
const char* const kInModMap[] = {"in"};
const char* const kInOut[] = {"in", "amp"};

/* ---- parameter specs ----
 *
 * Ranges and defaults deliberately mirror the equivalents in the fixed
 * engines (synth_subtractive's cutoff range, the shared ADSR bounds) so a
 * patch rebuilt in the graph lands in the same territory as the engine it
 * imitates, and so the app's existing control widgets and curve handling
 * apply unchanged. */

const ParamSpec kPOsc[] = {
    {"wave", ParamType::Enum, ParamCurve::Linear, 0, 3, 2 /*saw*/, kOscWaves, 4},
    {"semi", ParamType::Int, ParamCurve::Linear, -48, 48, 0, nullptr, 0},
    {"fine", ParamType::Float, ParamCurve::Linear, -100, 100, 0, nullptr, 0},
    {"pw", ParamType::Float, ParamCurve::Linear, 0.05f, 0.95f, 0.5f, nullptr, 0},
    /* phase-modulation index applied to the "fm" input: 0 leaves the
     * oscillator unmodulated however loud the cable is, so plugging one in
     * is never a surprise. */
    {"fm", ParamType::Float, ParamCurve::Linear, 0, 8, 0, nullptr, 0},
    {"level", ParamType::Float, ParamCurve::Linear, 0, 1, 1, nullptr, 0},
};

const ParamSpec kPNoise[] = {
    {"level", ParamType::Float, ParamCurve::Linear, 0, 1, 1, nullptr, 0},
};

/* The S33 filter block, shared by "filter" (12 dB) and "filter24" (24 dB):
 * identical parameters, so swapping one for the other in a patch keeps every
 * setting. `on` bypasses the node — the render path copies its input through
 * and skips the sample loop entirely, which is the point of having it. */
const ParamSpec kPFilter[] = {
    {"mode", ParamType::Enum, ParamCurve::Linear, 0, 6, 0 /*lp*/, kFltModes, 7},
    {"cutoff", ParamType::Float, ParamCurve::Exp, 20, 18000, 1200, nullptr, 0},
    {"reso", ParamType::Float, ParamCurve::Linear, 0, 1, 0.15f, nullptr, 0},
    {"kbd", ParamType::Float, ParamCurve::Linear, 0, 1, 0.5f, nullptr, 0},
    /* how far the "cut" input moves the cutoff, in octaves — the cable
     * carries a normalized signal, this sets its depth */
    {"cutamt", ParamType::Float, ParamCurve::Linear, -8, 8, 2.5f, nullptr, 0},
    {"on", ParamType::Bool, ParamCurve::Linear, 0, 1, 1, nullptr, 0},
    {"drive", ParamType::Float, ParamCurve::Linear, 0, 1, 0, nullptr, 0},
};

const ParamSpec kPLadder[] = {
    {"on", ParamType::Bool, ParamCurve::Linear, 0, 1, 1, nullptr, 0},
    {"cutoff", ParamType::Float, ParamCurve::Exp, 20, 18000, 1200, nullptr, 0},
    /* reso reaches self-oscillation at 1; the ladder's own saturation is
     * what stops that from being a runaway rather than a note */
    {"reso", ParamType::Float, ParamCurve::Linear, 0, 1, 0.3f, nullptr, 0},
    {"drive", ParamType::Float, ParamCurve::Linear, 0, 1, 0, nullptr, 0},
    {"kbd", ParamType::Float, ParamCurve::Linear, 0, 1, 0.5f, nullptr, 0},
    {"cutamt", ParamType::Float, ParamCurve::Linear, -8, 8, 2.5f, nullptr, 0},
};

const ParamSpec kPDual[] = {
    {"on", ParamType::Bool, ParamCurve::Linear, 0, 1, 1, nullptr, 0},
    {"cutoff", ParamType::Float, ParamCurve::Exp, 20, 18000, 1200, nullptr, 0},
    {"reso", ParamType::Float, ParamCurve::Linear, 0, 1, 0.15f, nullptr, 0},
    /* passband width in octaves: 0 is a razor, 6 is most of the spectrum */
    {"spread", ParamType::Float, ParamCurve::Linear, 0, 6, 2, nullptr, 0},
    {"drive", ParamType::Float, ParamCurve::Linear, 0, 1, 0, nullptr, 0},
    {"kbd", ParamType::Float, ParamCurve::Linear, 0, 1, 0.5f, nullptr, 0},
    {"cutamt", ParamType::Float, ParamCurve::Linear, -8, 8, 2.5f, nullptr, 0},
};

const ParamSpec kPVowel[] = {
    {"on", ParamType::Bool, ParamCurve::Linear, 0, 1, 1, nullptr, 0},
    /* morph a-e-i-o-u; the "vowel" input adds to this */
    {"vowel", ParamType::Float, ParamCurve::Linear, 0, 1, 0, nullptr, 0},
    /* formant bandwidth: low is breathy, high is a talking robot */
    {"reso", ParamType::Float, ParamCurve::Linear, 0, 1, 0.5f, nullptr, 0},
    /* vocal-tract shift, neutral at 1 kHz — the cutoff of this node */
    {"shift", ParamType::Float, ParamCurve::Exp, 100, 8000, 1000, nullptr, 0},
    {"drive", ParamType::Float, ParamCurve::Linear, 0, 1, 0, nullptr, 0},
    {"kbd", ParamType::Float, ParamCurve::Linear, 0, 1, 0, nullptr, 0},
    /* how far the "vowel" input moves the morph, over the whole a..u range */
    {"modamt", ParamType::Float, ParamCurve::Linear, -1, 1, 1, nullptr, 0},
};

const ParamSpec kPVca[] = {
    {"gain", ParamType::Float, ParamCurve::Linear, 0, 1, 1, nullptr, 0},
    {"depth", ParamType::Float, ParamCurve::Linear, 0, 1, 1, nullptr, 0},
};

const ParamSpec kPMix[] = {
    {"lv1", ParamType::Float, ParamCurve::Linear, 0, 1, 1, nullptr, 0},
    {"lv2", ParamType::Float, ParamCurve::Linear, 0, 1, 1, nullptr, 0},
    {"lv3", ParamType::Float, ParamCurve::Linear, 0, 1, 1, nullptr, 0},
    {"lv4", ParamType::Float, ParamCurve::Linear, 0, 1, 1, nullptr, 0},
};

const ParamSpec kPShaper[] = {
    {"mode", ParamType::Enum, ParamCurve::Linear, 0, 2, 0, kShaperModes, 3},
    {"drive", ParamType::Float, ParamCurve::Exp, 1, 24, 1, nullptr, 0},
    {"amt", ParamType::Float, ParamCurve::Linear, 0, 1, 0, nullptr, 0},
};

const ParamSpec kPRing[] = {
    {"amount", ParamType::Float, ParamCurve::Linear, 0, 1, 1, nullptr, 0},
};

const ParamSpec kPEnv[] = {
    {"attack", ParamType::Float, ParamCurve::Exp, 0.001f, 10, 0.005f, nullptr, 0},
    {"decay", ParamType::Float, ParamCurve::Exp, 0.001f, 10, 0.25f, nullptr, 0},
    {"sustain", ParamType::Float, ParamCurve::Linear, 0, 1, 0.7f, nullptr, 0},
    {"release", ParamType::Float, ParamCurve::Exp, 0.001f, 10, 0.25f, nullptr, 0},
};

const ParamSpec kPLfo[] = {
    {"wave", ParamType::Enum, ParamCurve::Linear, 0, 4, 0, kLfoWaves, 5},
    {"rate", ParamType::Float, ParamCurve::Exp, 0.02f, 40, 5, nullptr, 0},
    {"depth", ParamType::Float, ParamCurve::Linear, 0, 1, 1, nullptr, 0},
    {"rateamt", ParamType::Float, ParamCurve::Linear, -4, 4, 0, nullptr, 0},
    {"retrig", ParamType::Bool, ParamCurve::Linear, 0, 1, 1, nullptr, 0},
    /* unipolar: 0..1 instead of -1..1, for the common "this should only
     * ever add" routing (a filter that opens but never closes) */
    {"uni", ParamType::Bool, ParamCurve::Linear, 0, 1, 0, nullptr, 0},
};

const ParamSpec kPSah[] = {
    {"rate", ParamType::Float, ParamCurve::Exp, 0.05f, 50, 8, nullptr, 0},
};

const ParamSpec kPModMap[] = {
    {"scale", ParamType::Float, ParamCurve::Linear, -4, 4, 1, nullptr, 0},
    {"offset", ParamType::Float, ParamCurve::Linear, -2, 2, 0, nullptr, 0},
    /* 0 = off, else quantize the signal to N steps across its range — turns
     * an LFO or an S&H into an arpeggio-ish stepped modulator */
    {"quant", ParamType::Int, ParamCurve::Linear, 0, 32, 0, nullptr, 0},
};

const ParamSpec kPMidi[] = {
    {"src", ParamType::Enum, ParamCurve::Linear, 0, 5, 0, kMidiSources, 6},
};

const ParamSpec kPOut[] = {
    {"level", ParamType::Float, ParamCurve::Linear, 0, 1, 1, nullptr, 0},
    {"pan", ParamType::Float, ParamCurve::Linear, -1, 1, 0, nullptr, 0},
};

/* ---- the kind table ----
 *
 * Costs are in units where 1000 is the whole per-block budget at full
 * polyphony. **Calibrated against hardware** (S28, ESP32-S3 at 240 MHz, 8
 * voices, block 64): the factory patch — osc + filter + vca + out, two
 * envelopes and a velocity source — totalled 307 here and measured `voi`
 * 30.58%, so one unit is 0.1% of the block, which is what the scale is
 * supposed to mean. (S33 raised `filter` by 8 for its drive path, so the
 * same patch now prices at 315 against an unchanged measurement — the
 * calibration anchor is the 92, not the 307.)
 *
 * The first cut of these numbers was 25% higher, because each audio-rate
 * entry carried a flat allowance for buffer traffic — the store and load
 * per sample that carry a node's result to the next one. Measured on
 * hardware that allowance is zero: the render loop is bound by dependent
 * FPU latency, not by memory, and the LX7 issues those accesses in slots it
 * was not otherwise using. The allowance was removed and every audio entry
 * scaled by 0.80. (The same measurement is why there is no kernel fusion —
 * see the note in graph_compile.h.)
 *
 * Control-rate kinds cost a per-block handful of operations and are rounded
 * up to whole units, which overstates them; that is the safe direction, and
 * at 1-4 units each it is not worth measuring. */
const KindDesc kKinds[(int)Kind::Count] = {
    /* Empty */
    {"empty", Rate::Control, 0, nullptr, 0, nullptr, 0},
    /* Osc */
    {"osc", Rate::Audio, 2, kInOsc, pidx::OSC_N, kPOsc, 128},
    /* Noise */
    {"noise", Rate::Audio, 0, nullptr, pidx::NOI_N, kPNoise, 28},
    /* Filter */
    {"filter", Rate::Audio, 2, kInFilter, pidx::FLT_N, kPFilter, 100},
    /* Vca */
    {"vca", Rate::Audio, 2, kInVca, pidx::VCA_N, kPVca, 36},
    /* Mix */
    {"mix", Rate::Audio, 4, kInMix, pidx::MIX_N, kPMix, 44},
    /* Shaper */
    {"shaper", Rate::Audio, 2, kInShaper, pidx::SHP_N, kPShaper, 68},
    /* RingMod */
    {"ringmod", Rate::Audio, 2, kInRing, pidx::RNG_N, kPRing, 32},
    /* Env */
    {"env", Rate::Control, 0, nullptr, pidx::ENV_N, kPEnv, 3},
    /* Lfo */
    {"lfo", Rate::Control, 1, kInLfo, pidx::LFO_N, kPLfo, 4},
    /* SampleHold */
    {"s&h", Rate::Control, 1, kInSah, pidx::SAH_N, kPSah, 3},
    /* ModMap */
    {"modmap", Rate::Control, 1, kInModMap, pidx::MM_N, kPModMap, 2},
    /* MidiSrc */
    {"midi", Rate::Control, 0, nullptr, pidx::MS_N, kPMidi, 1},
    /* Out */
    {"out", Rate::Audio, 2, kInOut, pidx::OUT_N, kPOut, 44},
    /* ---- the S33 filters ----
     *
     * Separate kinds so each carries its own honest number. These four are
     * *estimates*, scaled off the measured 92 for one SVF and not yet
     * confirmed on hardware — unlike every figure above them, which came
     * from the S28 calibration run. Re-measure with `voi` at 8 voices and
     * correct them here; the ratios are the part worth trusting.
     *
     *   filter    one SVF, +8 over the S28 figure for the drive path's two
     *             soft_clips (a parameter, so it is priced in, not costed)
     *   filter24  two SVFs in series, so twice the work
     *   ladder    four one-poles + feedback + one clip, ~1.3x an SVF
     *   dual      two SVFs, same as filter24
     *   vowel     three SVFs plus the gain sum, ~3x
     *
     * Bypass (`on` = 0) is not modelled: the check has to hold whatever the
     * switch is set to later, and reserving for the on case is the safe
     * direction — same argument as kCostBudget reserving against worst-case
     * FX in graph_compile.h. */
    {"filter24", Rate::Audio, 2, kInFilter, pidx::FLT_N, kPFilter, 190},
    {"ladder", Rate::Audio, 2, kInFilter, pidx::LAD_N, kPLadder, 120},
    {"dual", Rate::Audio, 2, kInFilter, pidx::DUA_N, kPDual, 190},
    {"vowel", Rate::Audio, 2, kInVowel, pidx::VOW_N, kPVowel, 280},
};

/* ---- registered parameter names ----
 *
 * ParamDesc::name must outlive the registration, so the "n<slot>.<suffix>"
 * strings are built once into this table rather than composed on the stack
 * at registration time. 12 x 16 x 16 B is 3 KB of DRAM — paid once, and the
 * app's whole UI is built from these names. */
char s_pname[kMaxNodes][kNodeParams][16];

Model s_model;
Plan s_plan;               /* the live compiled plan */
bool s_bound = false;      /* between bind() and unbind() */
Kind s_registered[kMaxNodes] = {}; /* kind whose params are registered per slot */

/* ---- parameter registration ---- */

void register_slot(int slot, Kind k) {
    const KindDesc& d = kind_desc(k);
    if (d.n_params == 0) return;
    ParamDesc descs[kNodeParams];
    for (int p = 0; p < d.n_params; ++p) {
        const ParamSpec& s = d.params[p];
        snprintf(s_pname[slot][p], sizeof(s_pname[slot][p]), "n%d.%s", slot,
                 s.suffix);
        descs[p] = ParamDesc{node_pid(slot, p),
                             s_pname[slot][p],
                             s.type,
                             s.curve,
                             s.min,
                             s.max,
                             s.def,
                             s.enum_names,
                             s.enum_count};
    }
    const size_t added = ParamStore::instance().add(descs, d.n_params);
    if (added != d.n_params) {
        ESP_LOGE(TAG, "slot %d (%s): registered %u/%u params", slot, d.name,
                 (unsigned)added, (unsigned)d.n_params);
    }
    s_registered[slot] = k;
}

void unregister_slot(int slot) {
    if (s_registered[slot] == Kind::Empty) return;
    ParamStore::instance().removeRange(node_pid(slot, 0),
                                       (uint16_t)(node_pid(slot, 0) + kNodeParams));
    s_registered[slot] = Kind::Empty;
}

/* ---- the one path that changes what the audio task renders ----
 *
 * Ordering is the whole content of this function, and every step of it is
 * there to close a specific hole:
 *
 *  1. Compile the *candidate* first. Validation, cycle detection and the
 *     cost budget all run before anything is mutated, so a rejected edit
 *     costs nothing and leaves no trace — no half-registered slot, no
 *     duck, no revision bump.
 *  2. Retire the live plan before touching the registry. A published plan
 *     holds resolved value pointers into the slots' parameter ranges;
 *     removeRange() while the audio task could still dereference them is
 *     exactly the hazard ParamStore's threading note warns about, and
 *     retire() is the handshake that closes it (the audio task confirms it
 *     has dropped the plan and is rendering silence).
 *  3. Re-register only the slots whose *kind* changed. A cable edit leaves
 *     every parameter registered and every value where the user left it —
 *     re-registering unconditionally would reset the whole patch to
 *     defaults every time a cable moved.
 *  4. Resolve, then publish. The audio task adopts the new plan and ramps
 *     back up.
 */
esp_err_t apply(const Model& candidate, const char* what) {
    Plan next;
    char err[48] = {0};
    const esp_err_t rc = compile(candidate, next, err, sizeof(err));
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "%s rejected: %s", what, err[0] ? err : esp_err_to_name(rc));
        return rc;
    }

    if (!s_bound) { /* editing an unbound graph: model only, no audio side */
        s_model = candidate;
        s_model.revision = candidate.revision + 1;
        return ESP_OK;
    }

    retire();

    for (int i = 0; i < kMaxNodes; ++i) {
        if (s_registered[i] == candidate.nodes[i].kind) continue;
        unregister_slot(i);
        if (candidate.nodes[i].kind != Kind::Empty) {
            register_slot(i, candidate.nodes[i].kind);
        }
    }

    s_model = candidate;
    s_model.revision = candidate.revision + 1;
    next.revision = s_model.revision;
    resolve_params(s_model, next);
    s_plan = next;

    const esp_err_t pub = publish(s_plan);
    if (pub != ESP_OK) {
        ESP_LOGE(TAG, "%s: plan publish failed (%s) — output silent", what,
                 esp_err_to_name(pub));
        return pub;
    }
    ParamStore::instance().set(PID_GRAPH_COST, (float)s_plan.cost,
                               ParamOrigin::Internal);
    ParamStore::instance().set(PID_GRAPH_REV, (float)(s_model.revision & 0xFFFF),
                               ParamOrigin::Internal);
    ESP_LOGI(TAG, "%s: rev %u, %u nodes, %u bufs, cost %u/%u", what,
             (unsigned)s_model.revision, (unsigned)s_plan.n_nodes,
             (unsigned)s_plan.n_bufs, (unsigned)s_plan.cost,
             (unsigned)kCostBudget);
    return ESP_OK;
}

} // namespace

/* ---- kind lookup ---- */

const KindDesc& kind_desc(Kind k) {
    const int i = (int)k;
    return kKinds[(i >= 0 && i < (int)Kind::Count) ? i : 0];
}

const char* kind_name(Kind k) { return kind_desc(k).name; }

/* ---- model access ---- */

const Model& model() { return s_model; }

void reset_model() {
    s_model = Model{};
    s_model.nodes[kOutSlot].kind = Kind::Out;
    s_model.nodes[kOutSlot].ui_x = 480;
    s_model.nodes[kOutSlot].ui_y = 160;
}

/* ---- edits ---- */

esp_err_t set_kind(int slot, Kind kind) {
    if (slot < 0 || slot >= kMaxNodes) return ESP_ERR_INVALID_ARG;
    /* Kind's underlying type is unsigned, so the upper bound is the only
     * check there is — a negative test would always be false. */
    if (kind >= Kind::Count) return ESP_ERR_INVALID_ARG;
    /* The sink is structural: a graph with no Out renders nothing, and a
     * second Out would make "the output" ambiguous. Pinning it to slot 0
     * and refusing to change it removes both cases from every other pass. */
    if (slot == kOutSlot && kind != Kind::Out) return ESP_ERR_INVALID_ARG;
    if (slot != kOutSlot && kind == Kind::Out) return ESP_ERR_INVALID_ARG;

    Model m = s_model;
    m.nodes[slot].kind = kind;
    /* A new kind has different ports; keeping the old cables would silently
     * re-aim them at whatever port index happens to exist now. */
    for (int p = 0; p < kMaxInputs; ++p) m.nodes[slot].in[p] = -1;
    if (kind == Kind::Empty) {
        /* and nothing may still be listening to a slot that is now empty */
        for (auto& n : m.nodes) {
            for (int p = 0; p < kMaxInputs; ++p) {
                if (n.in[p] == slot) n.in[p] = -1;
            }
        }
    }
    return apply(m, "set kind");
}

esp_err_t connect(int dst, int port, int src) {
    if (dst < 0 || dst >= kMaxNodes) return ESP_ERR_INVALID_ARG;
    if (port < 0 || port >= kMaxInputs) return ESP_ERR_INVALID_ARG;
    if (src >= kMaxNodes) return ESP_ERR_INVALID_ARG;
    const Node& dn = s_model.nodes[dst];
    if (port >= kind_desc(dn.kind).n_inputs) return ESP_ERR_INVALID_ARG;
    if (src >= 0 && s_model.nodes[src].kind == Kind::Empty) {
        return ESP_ERR_INVALID_ARG;
    }
    if (src == dst) return ESP_ERR_INVALID_STATE; /* trivial self-loop */

    Model m = s_model;
    m.nodes[dst].in[port] = (int8_t)(src < 0 ? -1 : src);
    return apply(m, "connect");
}

esp_err_t set_ui_pos(int slot, int16_t x, int16_t y) {
    if (slot < 0 || slot >= kMaxNodes) return ESP_ERR_INVALID_ARG;
    /* Canvas-only: no recompile, no duck, no revision bump. The app moves
     * nodes continuously while dragging and this must not cost audio. */
    s_model.nodes[slot].ui_x = x;
    s_model.nodes[slot].ui_y = y;
    return ESP_OK;
}

esp_err_t load_model(const Model& m) {
    Model c = m;
    /* A file cannot be trusted to satisfy the invariants the edit API
     * maintains, and every later pass assumes them. */
    c.nodes[kOutSlot].kind = Kind::Out;
    for (int i = 0; i < kMaxNodes; ++i) {
        if ((int)c.nodes[i].kind >= (int)Kind::Count) c.nodes[i].kind = Kind::Empty;
        if (i != kOutSlot && c.nodes[i].kind == Kind::Out) {
            c.nodes[i].kind = Kind::Empty;
        }
        const int nin = kind_desc(c.nodes[i].kind).n_inputs;
        for (int p = 0; p < kMaxInputs; ++p) {
            int8_t& s = c.nodes[i].in[p];
            if (p >= nin || s < 0 || s >= kMaxNodes || s == i ||
                c.nodes[s].kind == Kind::Empty) {
                s = -1;
            }
        }
    }
    c.revision = s_model.revision;
    return apply(c, "load graph");
}

/* ---- bind / unbind ---- */

esp_err_t bind() {
    esp_err_t rc = render_init();
    if (rc != ESP_OK) return rc;

    static const ParamDesc kGlobals[] = {
        /* Read-only telemetry: what the live patch costs, in the units of
         * graph_compile.h. The app draws it as a budget meter, which is the
         * difference between "the synth refused my cable" and "I can see I
         * am near the limit". */
        {PID_GRAPH_COST, "graph.cost", ParamType::Int, ParamCurve::Linear, 0,
         1000, 0, nullptr, 0},
        /* Wraps rather than saturates: the app compares it for *change*,
         * never for order, so the only property that matters is that a
         * different graph reads differently. */
        {PID_GRAPH_REV, "graph.rev", ParamType::Int, ParamCurve::Linear, 0,
         65535, 0, nullptr, 0},
    };
    ParamStore::instance().add(kGlobals, 2);

    for (int i = 0; i < kMaxNodes; ++i) {
        s_registered[i] = Kind::Empty;
        if (s_model.nodes[i].kind != Kind::Empty) {
            register_slot(i, s_model.nodes[i].kind);
        }
    }
    s_bound = true;

    char err[48] = {0};
    rc = compile(s_model, s_plan, err, sizeof(err));
    if (rc != ESP_OK) {
        /* A model that will not compile must not leave the engine bound to
         * nothing: fall back to the empty patch, which always compiles. */
        ESP_LOGW(TAG, "stored graph rejected (%s), starting empty",
                 err[0] ? err : esp_err_to_name(rc));
        for (int i = 0; i < kMaxNodes; ++i) unregister_slot(i);
        reset_model();
        register_slot(kOutSlot, Kind::Out);
        rc = compile(s_model, s_plan, err, sizeof(err));
        if (rc != ESP_OK) return rc;
    }
    s_plan.revision = s_model.revision;
    resolve_params(s_model, s_plan);
    rc = publish(s_plan);
    if (rc != ESP_OK) return rc;
    ParamStore::instance().set(PID_GRAPH_COST, (float)s_plan.cost,
                               ParamOrigin::Internal);
    ParamStore::instance().set(PID_GRAPH_REV, (float)(s_model.revision & 0xFFFF),
                               ParamOrigin::Internal);
    ESP_LOGI(TAG, "graph bound: rev %u, %u nodes, %u bufs, cost %u/%u",
             (unsigned)s_model.revision, (unsigned)s_plan.n_nodes,
             (unsigned)s_plan.n_bufs, (unsigned)s_plan.cost,
             (unsigned)kCostBudget);
    return ESP_OK;
}

void unbind() {
    /* Order matters exactly as much as it does in apply(): the plan holds
     * pointers into the range about to be dropped. */
    retire();
    s_bound = false;
    for (int i = 0; i < kMaxNodes; ++i) unregister_slot(i);
    ParamStore::instance().removeRange(PID_ENGINE_BASE, PID_FX_BASE);
    render_deinit();
}

/* ---- serialization ---- */

namespace {

inline void put_u16(uint8_t*& p, uint16_t v) {
    *p++ = (uint8_t)(v & 0xFF);
    *p++ = (uint8_t)(v >> 8);
}
inline void put_u32(uint8_t*& p, uint32_t v) {
    put_u16(p, (uint16_t)(v & 0xFFFF));
    put_u16(p, (uint16_t)(v >> 16));
}
inline uint16_t get_u16(const uint8_t*& p) {
    const uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
    p += 2;
    return v;
}
inline uint32_t get_u32(const uint8_t*& p) {
    const uint32_t lo = get_u16(p);
    return lo | ((uint32_t)get_u16(p) << 16);
}

} // namespace

size_t serialize(const Model& m, uint8_t* out, size_t cap) {
    const size_t need = 10 + (size_t)kMaxNodes * 9;
    if (out == nullptr || cap < need) return 0;
    uint8_t* p = out;
    put_u32(p, kSerialMagic);
    *p++ = 1; /* format version */
    *p++ = (uint8_t)kMaxNodes;
    put_u32(p, m.revision);
    for (int i = 0; i < kMaxNodes; ++i) {
        const Node& n = m.nodes[i];
        *p++ = (uint8_t)n.kind;
        for (int k = 0; k < kMaxInputs; ++k) *p++ = (uint8_t)(int8_t)n.in[k];
        put_u16(p, (uint16_t)n.ui_x);
        put_u16(p, (uint16_t)n.ui_y);
    }
    return (size_t)(p - out);
}

bool deserialize(const uint8_t* in, size_t len, Model& out) {
    if (in == nullptr || len < 10) return false;
    const uint8_t* p = in;
    if (get_u32(p) != kSerialMagic) return false;
    const uint8_t version = *p++;
    if (version != 1) return false;
    const uint8_t count = *p++;
    out = Model{};
    out.revision = get_u32(p);
    if (len < (size_t)10 + (size_t)count * 9) return false;
    for (int i = 0; i < count; ++i) {
        const uint8_t kind = *p++;
        int8_t ins[kMaxInputs];
        for (int k = 0; k < kMaxInputs; ++k) ins[k] = (int8_t)*p++;
        const int16_t x = (int16_t)get_u16(p);
        const int16_t y = (int16_t)get_u16(p);
        /* A graph authored on a build with more slots than this one keeps
         * the slots that fit, the way S27 keeps the patterns that fit —
         * refusing the whole file would be the less useful failure. */
        if (i >= kMaxNodes) continue;
        out.nodes[i].kind = (kind < (uint8_t)Kind::Count) ? (Kind)kind : Kind::Empty;
        for (int k = 0; k < kMaxInputs; ++k) out.nodes[i].in[k] = ins[k];
        out.nodes[i].ui_x = x;
        out.nodes[i].ui_y = y;
    }
    return true;
}

} // namespace osynth::graph
