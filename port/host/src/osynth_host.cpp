/*
 * osynth host port — the host's app_main(). See osynth_host.h.
 */
#include "osynth_host.h"
#include "osynth_host_midi.h"

#include <atomic>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "audio_io.h"
#include "audio_sink.h"
#include "chord.h"
#include "ctrl_proto.h"
#include "drums.h"
#include "engines.h"
#include "fx.h"
#include "host_paths.h"
#include "looper.h"
#include "midi.h"
#include "persist.h"
#include "presets.h"
#include "sampler.h"
#include "seqarp.h"
#include "synth_config.h"
#include "synth_mod.h"
#include "synth_params.h"
#include "synth_voice.h"

using osynth::ParamCurve;
using osynth::ParamDesc;
using osynth::ParamStore;
using osynth::ParamType;

namespace {

const char* TAG = "osynth";

std::atomic<bool> g_started{false};

/* Order and count must track synth_engine_type_t, exactly as in main.cpp --
 * "modular" is named even on a build without it, because its index is
 * reserved either way. */
const char* const kEngineNames[] = {"subtractive", "additive", "fm",
                                    "wavetable",   "modular",  "granular",
                                    "sampler"};
static_assert(sizeof(kEngineNames) / sizeof(kEngineNames[0]) ==
                  SYNTH_ENGINE_COUNT,
              "engine.type names must track synth_engine_type_t");

#if SYNTH_ENABLE_AUDIO_IN
/* Where the input joins the render chain, and therefore what it records:
 * `mon` is mixed after the looper's record tap, so it is heard but never
 * printed into a take; `fx` and `dry` are mixed before it. */
const char* const kInRouteNames[] = {"off", "mon", "fx", "dry"};
#endif

/* The 0x00xx globals, from main.cpp's register_global_params().
 *
 * The entries absent here are absent for one reason: each is gated on hardware
 * this build does not have -- an ES8388's analogue stages, a USB OTG port.
 * Registering them anyway would put controls in the app's hands that answer to
 * nothing. The audio input is present, because miniaudio's capture side makes
 * it real (see SYNTH_ENABLE_LINE_IN in synth_config.h).
 *
 * The defaults match main.cpp's, including master.volume at 0.8 rather than
 * something quieter, because a preset saves sparsely: a value absent from a
 * file is read back as whatever default sits here, so a disagreement between
 * the two builds would make the same patch sound different on each. */
const ParamDesc kGlobals[] = {
    {osynth::PID_MASTER_VOLUME, "master.volume", ParamType::Float,
     ParamCurve::Linear, 0.0f, 1.0f, 0.8f, nullptr, 0},
    {osynth::PID_ENGINE_TYPE, "engine.type", ParamType::Enum,
     ParamCurve::Linear, 0.0f, (float)(SYNTH_ENGINE_COUNT - 1),
     (float)SYNTH_ENGINE_SUBTRACTIVE, kEngineNames, SYNTH_ENGINE_COUNT},
#if SYNTH_ENABLE_AUDIO_IN
    /* Defaults to `off`, and that default is deliberate here in a way it is
     * not on the instrument: a desktop's default capture device is usually a
     * microphone a foot from a loudspeaker, and coming up routed would howl.
     * The player turns it on when something is plugged in. */
    {osynth::PID_LINE_IN_ROUTE, "in.route", ParamType::Enum, ParamCurve::Linear,
     0.0f, 3.0f, 0.0f, kInRouteNames, 4},
    /* Linear, not Exp: smooth_exp() divides by its running value, so an
     * exponential curve needs min > 0 -- and 0 here has to mean silent. */
    {osynth::PID_LINE_IN_GAIN, "in.gain", ParamType::Float, ParamCurve::Linear,
     0.0f, 4.0f, 1.0f, nullptr, 0},
#endif
};

/* Settings that survive a restart. Same reasoning as main.cpp's list: these
 * describe the rig rather than the patch, so nothing else would remember
 * them. Its remaining entries belong to a codec or a USB port not here. */
const uint16_t kPersisted[] = {
    osynth::PID_MASTER_VOLUME,
#if SYNTH_ENABLE_AUDIO_IN
    osynth::PID_LINE_IN_ROUTE,
    osynth::PID_LINE_IN_GAIN,
#endif
};

/* The render chain. Identical to render_chain() in main/main.cpp, stage for
 * stage and in the same order -- see there for why each stage sits where it
 * does, particularly the looper's record tap and the metronome after it.
 * The three line-in stages compile to nothing without an audio input. */
void SYNTH_RENDER_IRAM render_chain(float* out_l, float* out_r, size_t frames,
                                    void* ctx) {
    voice_manager_render(out_l, out_r, frames, ctx);
    drums_pre_fx(out_l, out_r, frames);
    audio_io_line_in_fx(out_l, out_r, frames);
    fx_process(out_l, out_r, frames);
    drums_post_fx(out_l, out_r, frames);
    audio_io_line_in_dry(out_l, out_r, frames);
    looper_process(out_l, out_r, frames);
    audio_io_line_in_mon(out_l, out_r, frames);
    sampler_capture(out_l, out_r, frames);
    drums_render_click(out_l, out_r, frames);
}

}  // namespace

void osynth_host_config_default(osynth_host_config_t* out) {
    if (out == nullptr) return;
    out->data_dir = nullptr;
    out->spiram_budget = OSYNTH_HOST_SPIRAM_BUDGET_BYTES;
    out->internal_budget = OSYNTH_HOST_INTERNAL_BUDGET_BYTES;
    out->start_audio = true;
    out->start_midi_in = true;
}

esp_err_t osynth_host_start(const osynth_host_config_t* cfg) {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) {
        return ESP_ERR_INVALID_STATE;
    }

    osynth_host_config_t c;
    osynth_host_config_default(&c);
    if (cfg != nullptr) {
        c = *cfg;
        if (c.spiram_budget == 0) c.spiram_budget = OSYNTH_HOST_SPIRAM_BUDGET_BYTES;
        if (c.internal_budget == 0) {
            c.internal_budget = OSYNTH_HOST_INTERNAL_BUDGET_BYTES;
        }
    }

    /* Both before anything allocates or resolves a path. */
    if (c.data_dir != nullptr) osynth_host_set_data_dir(c.data_dir);
    heap_caps_host_set_budgets(c.spiram_budget, c.internal_budget);

    ESP_LOGI(TAG, "osynth (host build) — %d Hz, block %d, %d voices",
             SYNTH_SAMPLE_RATE, SYNTH_BLOCK_SIZE, SYNTH_VOICES);
    ESP_LOGI(TAG, "data: %s", osynth_host_data_dir());

    ParamStore& ps = ParamStore::instance();
    ps.add(kGlobals, sizeof(kGlobals) / sizeof(kGlobals[0]));
    persist_add(kPersisted, sizeof(kPersisted) / sizeof(kPersisted[0]));

    /* main.cpp's order. The reasons are documented at each call site there;
     * the header above says why they are copied rather than re-derived. */
    esp_err_t err;
    if ((err = voice_manager_init()) != ESP_OK) return err;
    if ((err = synth_mod_init()) != ESP_OK) return err;
    if ((err = drums_init()) != ESP_OK) return err;
    if ((err = engines_init()) != ESP_OK) return err;
    if ((err = fx_init()) != ESP_OK) return err;
    if ((err = seqarp_init()) != ESP_OK) return err;
    if ((err = chord_init()) != ESP_OK) return err;
    if ((err = presets_init()) != ESP_OK) return err;
    drums_kits_load();
    if ((err = looper_init()) != ESP_OK) return err;
    if ((err = midi_init()) != ESP_OK) return err;
    if ((err = persist_init()) != ESP_OK) return err;

    /* The protocol's ParamStore listener, so external changes reach whatever
     * transport is installed. The transport itself is not this file's business
     * -- an embedding app installs its own. */
    if ((err = ctrl_proto_init()) != ESP_OK) return err;

    ps.dump();

    /* After the router exists and before the audio device, so a key pressed
     * the instant the device opens already has somewhere to go. Its return is
     * ignored on purpose: no ports, no MIDI stack and no backend at all are
     * ordinary states, and none of them should stop a synth coming up. */
    if (c.start_midi_in) (void)osynth_host_midi_in_start();

    if (c.start_audio) {
        if ((err = audio_io_start(render_chain, nullptr)) != ESP_OK) {
            ESP_LOGE(TAG, "audio failed to start: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "up: %u parameters, sink %s", (unsigned)ps.count(),
                 audio_io_sink_name());
    } else {
        ESP_LOGI(TAG, "up: %u parameters, audio not started",
                 (unsigned)ps.count());
    }
    return ESP_OK;
}

void osynth_host_stop(void) {
    if (!g_started.load()) return;
    osynth_host_midi_in_stop();
    /* The device only. The control tasks own state the app may still be
     * reading, and the firmware never stops them either -- see the header. */
    audio_sink_host_stop();
}

bool osynth_host_running(void) { return g_started.load(); }
