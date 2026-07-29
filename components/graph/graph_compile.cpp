/*
 * osynth — modular patch graph: the compiler (Session 28).
 *
 * Turns an edited Model into the flat Plan the audio task walks. Pure: it
 * reads the model, writes the plan, and touches nothing else — which is what
 * lets apply() compile a candidate the user may never adopt and throw it
 * away for free.
 *
 * See graph_compile.h for why each pass exists. The order below is forced:
 * reachability needs the graph, the topological order needs to know there is
 * no cycle, live ranges need the order, and buffer assignment needs the live
 * ranges.
 */
#include "graph_compile.h"

#include <cstdio>
#include <cstring>

#include "synth_params.h"

namespace osynth::graph {
namespace {

/* DFS colours for the combined cycle check and topological sort. */
enum class Mark : uint8_t { White = 0, Grey, Black };

struct Ctx {
    const Model* m;
    Mark mark[kMaxNodes];
    int8_t order[kMaxNodes]; /* post-order: every input before its consumer */
    int n_order;
    bool cycle;
};

/* Post-order DFS over incoming edges. Emitting a node only after all of its
 * inputs have been emitted *is* the topological order — no separate sorting
 * pass, and the same traversal detects the cycle: meeting a Grey node means
 * we reached a node that is still on the current DFS stack, i.e. a node that
 * (transitively) feeds itself.
 *
 * Only nodes reachable from Out are ever visited, so an orphan — a node the
 * user left on the canvas with nothing plugged into the output path — never
 * enters the plan and costs nothing per block. */
void visit(Ctx& c, int slot) {
    if (slot < 0 || slot >= kMaxNodes) return;
    if (c.mark[slot] == Mark::Black) return;
    if (c.mark[slot] == Mark::Grey) {
        c.cycle = true;
        return;
    }
    const Node& n = c.m->nodes[slot];
    if (n.kind == Kind::Empty) return;

    c.mark[slot] = Mark::Grey;
    const int nin = kind_desc(n.kind).n_inputs;
    for (int p = 0; p < nin; ++p) {
        const int src = n.in[p];
        if (src < 0 || src >= kMaxNodes) continue;
        if (c.m->nodes[src].kind == Kind::Empty) continue;
        visit(c, src);
        if (c.cycle) return;
    }
    c.mark[slot] = Mark::Black;
    c.order[c.n_order++] = (int8_t)slot;
}

} // namespace

esp_err_t compile(const Model& m, Plan& out, char* err, size_t err_len) {
    auto fail = [&](esp_err_t rc, const char* msg) {
        if (err != nullptr && err_len > 0) snprintf(err, err_len, "%s", msg);
        return rc;
    };

    out = Plan{};

    /* ---- 1. reachability + cycle check + topological order ---- */

    Ctx c{};
    c.m = &m;
    visit(c, kOutSlot);
    if (c.cycle) {
        return fail(ESP_ERR_INVALID_STATE, "cycle: a node feeds itself");
    }
    if (c.n_order == 0) {
        /* Cannot happen through the edit API (slot 0 is pinned to Out), but
         * a deserialized model reaches here before load_model's repair. */
        return fail(ESP_ERR_INVALID_ARG, "no output node");
    }

    /* ---- 2. fan-out and live ranges ----
     *
     * fanout[j] counts the input ports that read node j; last[j] is the
     * latest position in the evaluation order that reads it. Together they
     * are exactly the two facts buffer reuse needs: last[j] says when j's
     * buffer may be recycled, fanout[j] == 1 says nobody else can observe
     * it being overwritten in place. */
    uint8_t fanout[kMaxNodes] = {};
    int8_t last[kMaxNodes];
    for (int i = 0; i < kMaxNodes; ++i) last[i] = -1;

    for (int t = 0; t < c.n_order; ++t) {
        const int slot = c.order[t];
        const Node& n = m.nodes[slot];
        const int nin = kind_desc(n.kind).n_inputs;
        for (int p = 0; p < nin; ++p) {
            const int src = n.in[p];
            if (src < 0 || m.nodes[src].kind == Kind::Empty) continue;
            if (c.mark[src] != Mark::Black) continue; /* unreachable, pruned */
            ++fanout[src];
            last[src] = (int8_t)t;
        }
    }

    /* ---- 3. buffer assignment (linear scan over the evaluation order) ---- */

    bool busy[kMaxBufs] = {};
    int8_t bufof[kMaxNodes]; /* audio buffer holding each node's output */
    for (int i = 0; i < kMaxNodes; ++i) bufof[i] = -1;
    int high_water = 0;
    uint32_t cost = 0;

    for (int t = 0; t < c.n_order; ++t) {
        const int slot = c.order[t];
        const Node& n = m.nodes[slot];
        const KindDesc& d = kind_desc(n.kind);

        PlanNode& pn = out.nodes[t];
        pn.kind = n.kind;
        pn.slot = (uint8_t)slot;
        pn.rate = d.rate;
        pn.out_buf = -1;
        for (int p = 0; p < kMaxInputs; ++p) {
            pn.in_buf[p] = -1;
            pn.in_ctl[p] = -1;
        }

        /* Inputs: an edge is routed by the *source's* rate, and the consumer
         * coerces. That is why an LFO can be plugged into an audio input and
         * an oscillator into a modulation input without either being an
         * error — in a patchable instrument, refusing a cable because of a
         * rate mismatch would be the wrong answer far more often than not. */
        const int nin = d.n_inputs;
        for (int p = 0; p < nin; ++p) {
            const int src = n.in[p];
            if (src < 0 || m.nodes[src].kind == Kind::Empty) continue;
            if (c.mark[src] != Mark::Black) continue;
            if (kind_desc(m.nodes[src].kind).rate == Rate::Audio) {
                pn.in_buf[p] = bufof[src];
            } else {
                pn.in_ctl[p] = (int8_t)src;
            }
        }

        if (d.rate == Rate::Audio && n.kind != Kind::Out) {
            /* In-place: write the output over an input buffer whose only
             * consumer is this node and whose life ends here. Every audio
             * kernel in graph_render.cpp is strictly per-sample with no
             * lookahead, so reading and writing the same address in lockstep
             * is well-defined; the saving is one buffer and, more usefully,
             * half the memory traffic of a long chain. */
            int reuse = -1;
            for (int p = 0; p < nin; ++p) {
                const int src = n.in[p];
                if (src < 0 || pn.in_buf[p] < 0) continue;
                if (fanout[src] == 1 && last[src] == (int8_t)t) {
                    reuse = pn.in_buf[p];
                    break;
                }
            }
            if (reuse >= 0) {
                pn.out_buf = (int8_t)reuse;
            } else {
                int b = -1;
                for (int i = 0; i < kMaxBufs; ++i) {
                    if (!busy[i]) {
                        b = i;
                        break;
                    }
                }
                if (b < 0) {
                    return fail(ESP_ERR_NO_MEM, "patch too wide (buffers)");
                }
                busy[b] = true;
                pn.out_buf = (int8_t)b;
                if (b + 1 > high_water) high_water = b + 1;
            }
            bufof[slot] = pn.out_buf;
        }

        /* Release inputs that die here — after the output is placed, so a
         * freed buffer can never be handed out to this same node and alias
         * an input by accident. The one intentional alias is the in-place
         * case above, which keeps its buffer busy by construction. */
        for (int p = 0; p < nin; ++p) {
            const int src = n.in[p];
            if (src < 0 || pn.in_buf[p] < 0) continue;
            if (last[src] != (int8_t)t) continue;
            if (pn.in_buf[p] == pn.out_buf) continue; /* became the output */
            busy[pn.in_buf[p]] = false;
        }

        cost += d.cost;
    }

    out.n_nodes = (uint8_t)c.n_order;
    out.n_bufs = (uint8_t)high_water;
    out.cost = (uint16_t)(cost > 0xFFFF ? 0xFFFF : cost);
    out.revision = m.revision;

    /* ---- 4. budget ----
     *
     * The last pass, deliberately: a patch is rejected on what it actually
     * compiled to, after orphan pruning, not on what is drawn on the canvas.
     * Refusing here is the whole reason free patching is safe on this chip —
     * the alternative is accepting the edit and letting the user diagnose an
     * underrun by ear. */
    if (out.cost > kCostBudget) {
        char msg[48];
        snprintf(msg, sizeof(msg), "too expensive: %u/%u", (unsigned)out.cost,
                 (unsigned)kCostBudget);
        return fail(ESP_ERR_NOT_SUPPORTED, msg);
    }
    return ESP_OK;
}

void resolve_params(const Model& m, Plan& out) {
    const ParamStore& ps = ParamStore::instance();
    for (int slot = 0; slot < kMaxNodes; ++slot) {
        const KindDesc& d = kind_desc(m.nodes[slot].kind);
        for (int p = 0; p < kNodeParams; ++p) {
            out.pp[slot][p] =
                (p < d.n_params) ? ps.valuePtr(node_pid(slot, p)) : nullptr;
        }
    }
}

} // namespace osynth::graph
