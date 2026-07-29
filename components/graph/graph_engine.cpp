/*
 * osynth — modular patch graph: the engine binding (Session 28).
 *
 * Presents the graph to the voice manager as a fifth engine. Everything a
 * fixed engine does inline — parameter registration, per-block coefficient
 * work, the per-voice render — is delegated to the model, the compiler and
 * the render path respectively; what is left here is the vtable and the
 * factory patch.
 *
 * Two things are deliberately different from the S5-S8 engines:
 *
 *  - **caps is 0.** The capability mask tells a control surface which of the
 *    *fixed* shared-block pages to show (filter page, LFO page, …). A graph
 *    has none of those: its filter is whichever slots hold a Filter node,
 *    and the app draws the patch instead. Declaring caps it does not have
 *    would put dead pages in front of the user.
 *
 *  - **begin_block() is empty and render_block() does everything.** The
 *    per-block work a fixed engine hoists into begin_block() is per *node*
 *    here, so it belongs at the node's visit inside the plan walk — hoisting
 *    it any further would mean walking the plan twice per block for nothing.
 */
#include <cstddef>

#include "esp_log.h"

#include "graph_model.h"
#include "graph_render.h"
#include "synth_engine.h"

static const char* TAG = "eng_mod";

namespace {

using namespace osynth::graph;

/* The graph survives an engine switch: the model is file-static in
 * graph_model.cpp, so leaving the modular engine and coming back finds the
 * patch as it was. This flag only distinguishes "first bind of this boot"
 * (build the factory patch) from every later one (keep what is there). */
bool s_have_model = false;

/* ---- factory patch ----
 *
 * A complete subtractive voice, because an empty canvas is a bad first
 * impression and because this one doubles as the reference for the cost
 * model: it compiles to seven nodes and — thanks to in-place reuse — a
 * single audio buffer, whatever the chain length suggests.
 *
 *   osc(1) -> filter(2) -> vca(3) -> out(0)
 *              cut: env(5)  gain: env(4)   amp: vel(6)
 */
void build_factory_patch() {
    Model m{};
    m.nodes[0] = {Kind::Out, {3, 6, -1, -1}, 560, 200};
    m.nodes[1] = {Kind::Osc, {-1, -1, -1, -1}, 80, 200};
    m.nodes[2] = {Kind::Filter, {1, 5, -1, -1}, 240, 200};
    m.nodes[3] = {Kind::Vca, {2, 4, -1, -1}, 400, 200};
    m.nodes[4] = {Kind::Env, {-1, -1, -1, -1}, 400, 360};
    m.nodes[5] = {Kind::Env, {-1, -1, -1, -1}, 240, 360};
    m.nodes[6] = {Kind::MidiSrc, {-1, -1, -1, -1}, 560, 360};
    const esp_err_t rc = load_model(m);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "factory patch rejected (%s) — starting empty",
                 esp_err_to_name(rc));
        reset_model();
    }
}

/* ---- vtable ---- */

esp_err_t mod_init(void) {
    if (!s_have_model) {
        reset_model();
        build_factory_patch();
        s_have_model = true;
    }
    const esp_err_t rc = bind();
    if (rc != ESP_OK) return rc;
    ESP_LOGI(TAG, "modular engine up: %d slots, %d params/slot, %u B/voice",
             kMaxNodes, kNodeParams, (unsigned)sizeof(VoiceState));
    return ESP_OK;
}

void mod_deinit(void) { unbind(); }

void mod_begin_block(size_t frames) { (void)frames; }

void mod_voice_reset(void* vs) { voice_reset(*(VoiceState*)vs); }

void mod_note_on(void* vs, uint8_t note, float vel01, bool was_sounding) {
    note_on(*(VoiceState*)vs, note, vel01, was_sounding);
}

void mod_note_off(void* vs) { note_off(*(VoiceState*)vs); }

/* Never reached — the voice manager prefers render_block when it is set —
 * but a vtable with a null render() would be a trap for any later caller
 * that does not know about the batched contract. */
void mod_render(void* vs, const synth_voice_frame_t* f, float* out_l,
                float* out_r, size_t frames) {
    (void)vs;
    (void)f;
    (void)out_l;
    (void)out_r;
    (void)frames;
}

void SYNTH_RENDER_IRAM mod_render_block(void* const* states,
                                        const synth_voice_frame_t* frames,
                                        size_t n_voices, float* out_l,
                                        float* out_r, size_t n) {
    render_block(states, frames, n_voices, out_l, out_r, n);
}

bool mod_busy(const void* vs) { return voice_busy(*(const VoiceState*)vs); }

float mod_level(const void* vs) { return voice_level(*(const VoiceState*)vs); }

} // namespace

extern "C" const synth_engine_t g_engine_modular = {
    "modular",
    0, /* see the header comment: a graph has no fixed-block pages */
    sizeof(VoiceState),
    mod_init,
    mod_deinit,
    mod_begin_block,
    mod_voice_reset,
    mod_note_on,
    mod_note_off,
    mod_render,
    mod_busy,
    mod_level,
    mod_render_block,
};
