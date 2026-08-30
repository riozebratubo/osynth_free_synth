/*
 * osynth host port — the host's app_main(). See osynth_host.h.
 */
#include "osynth_host.h"
#include "osynth_host_midi.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "esp_cpu.h"
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
#include "synth_warn.h"

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

/* The status line, and the drain that has to go with it.
 *
 * main.cpp runs this on the task that ran app_main(); here it is a thread of
 * its own, because osynth_host_start() returns to a caller with its own work
 * to do. Same period, same numbers, minus the segments that describe hardware
 * this build has none of -- USB, BLE, the two-core pipeline's stalls.
 *
 * render_warn_drain() is the part that is not optional. The render path cannot
 * log (synth_warn.h explains why: a console write from the audio thread is a
 * dropout), so it queues static strings for a control task to print. Without
 * this loop nothing ever drains that queue, and every warning the DSP raises
 * is discarded unseen. */
void heartbeat_thread(unsigned period_ms) {
    audio_io_stats_t st{};
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
        osynth::dsp::render_warn_drain();
        audio_io_get_stats(&st);

        char sink_seg[64] = "";
        if (st.sink_errors != 0) {
            std::snprintf(sink_seg, sizeof(sink_seg), " | SINK ERR %u (%s)",
                          (unsigned)st.sink_errors,
                          esp_err_to_name((esp_err_t)st.sink_last_err));
        }
        char in_seg[64] = "";
#if SYNTH_ENABLE_AUDIO_IN
        std::snprintf(in_seg, sizeof(in_seg), " | in %.2f/%.2f route %u",
                      (double)st.in_peak_l[0], (double)st.in_peak_r[0],
                      (unsigned)st.in_route);
#endif
        const synth_engine_t* eng = engines_get(engines_active_type());
        ESP_LOGI(TAG,
                 "alive | audio blocks %u, underruns %u, dsp %.1f%% "
                 "(pk %.1f%%) [voi %.1f fx %.1f loop %.1f] | out pk %.2f, "
                 "sat %u | voices %u/%d (+%d drum) | engine %s%s%s",
                 (unsigned)st.blocks_rendered, (unsigned)st.underruns,
                 st.dsp_load_pct, st.dsp_load_peak_pct, st.stage_voices_pct,
                 st.stage_fx_pct, st.stage_loop_pct, st.out_peak,
                 (unsigned)st.soft_clips,
                 (unsigned)voice_manager_active_voices(), SYNTH_VOICES,
                 drums_active_voices(), eng != nullptr ? eng->name : "none",
                 in_seg, sink_seg);
    }
}

/* The render chain. Identical to render_chain() in main/main.cpp, stage for
 * stage and in the same order -- see there for why each stage sits where it
 * does, particularly the looper's record tap and the metronome after it.
 * The three line-in stages compile to nothing without an audio input. */
void SYNTH_RENDER_IRAM render_chain(float* out_l, float* out_r, size_t frames,
                                    void* ctx) {
    /* The three cycle reads are not decoration, and leaving them out is what
     * made the heartbeat report "[voi 0.0 fx 0.0 loop 0.0]" until they were
     * put back. They are what attributes the load to a stage: voices, drums
     * and the input mix behave nothing like the FX bus, which behaves nothing
     * like the looper, and one total number cannot say which of them is over
     * budget. main.cpp's copy carries the full reasoning. */
    const uint32_t c0 = esp_cpu_get_cycle_count();
    voice_manager_render(out_l, out_r, frames, ctx);
    drums_pre_fx(out_l, out_r, frames);
    audio_io_line_in_fx(out_l, out_r, frames);
    const uint32_t c1 = esp_cpu_get_cycle_count();
    fx_process(out_l, out_r, frames);
    const uint32_t c2 = esp_cpu_get_cycle_count();
    drums_post_fx(out_l, out_r, frames);
    audio_io_line_in_dry(out_l, out_r, frames);
    looper_process(out_l, out_r, frames);
    audio_io_line_in_mon(out_l, out_r, frames);
    sampler_capture(out_l, out_r, frames);
    drums_render_click(out_l, out_r, frames);
    const uint32_t c3 = esp_cpu_get_cycle_count();
    audio_io_report_stages(c1 - c0, c2 - c1, c3 - c2);
}

}  // namespace

void osynth_host_config_default(osynth_host_config_t* out) {
    if (out == nullptr) return;
    out->data_dir = nullptr;
    out->spiram_budget = OSYNTH_HOST_SPIRAM_BUDGET_BYTES;
    out->internal_budget = OSYNTH_HOST_INTERNAL_BUDGET_BYTES;
    out->start_audio = true;
    out->start_midi_in = true;
    out->heartbeat_ms = SYNTH_HEARTBEAT_MS;
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
        /* The working state -- the patch that was playing when the app last
         * closed. main.cpp restores it at the same point and for the same
         * reason: applying it can switch engines, and the S6 switch protocol
         * hands the voice pool over on two render boundaries, so before the
         * audio task exists there is no boundary to hand over on and the
         * switch is refused.
         *
         * Not fatal if it fails. A synth that refuses to start because it
         * could not restore a patch is worse than one that starts at its
         * defaults -- the same rule persist_init() follows.
         *
         * Auto-saving begins when this completes and not before, so nothing
         * has to be scheduled here: from now on the engine writes the state
         * out by itself once edits settle and the output falls quiet. */
        const esp_err_t rerr = presets_state_restore();
        if (rerr != ESP_OK) {
            ESP_LOGW(TAG, "working state not restored: %s (starting at "
                          "defaults)", esp_err_to_name(rerr));
        } else {
            /* Waited for, unlike the firmware, and the difference is what the
             * caller does next. main.cpp queues this and then brings up MIDI
             * and BLE, so the synth has settled long before a client connects.
             * An embedding app calls start() and immediately begins reading --
             * or worse, writing -- parameters, and the restore lands
             * underneath it: a read sees the whole set move, and a write made
             * before the restore finishes is folded into its change-detection
             * baseline and then never auto-saved.
             *
             * So start() returns a settled synth. The wait is one small file
             * read; the timeout is only so that a wedged preset task cannot
             * stop an app from opening. */
            const esp_err_t werr = presets_state_wait_restored(3000);
            if (werr != ESP_OK) {
                ESP_LOGW(TAG, "working state still restoring after 3 s — "
                              "continuing, early edits may not be saved");
            }
        }

        if (c.heartbeat_ms > 0) {
            /* Detached: it runs for the life of the process, exactly as the
             * firmware's does, and there is no shutdown it has to participate
             * in -- osynth_host_stop() takes the device away and this loop
             * simply reports a synth that has stopped rendering. */
            std::thread(heartbeat_thread, c.heartbeat_ms).detach();
        }

        ESP_LOGI(TAG, "up: %u parameters, sink %s", (unsigned)ps.count(),
                 audio_io_sink_name());
    } else {
        /* No restore without the audio task: the contract in presets.h is
         * explicit that it has to come after audio_io_start(), because an
         * engine switch cannot be handed over without render boundaries. A
         * harness driving the protocol wants a known starting state anyway. */
        ESP_LOGI(TAG, "up: %u parameters, audio not started (working state "
                      "not restored)", (unsigned)ps.count());
    }
    return ESP_OK;
}

void osynth_host_stop(void) {
    if (!g_started.load()) return;

    /* Write the working state before anything else goes.
     *
     * The firmware never reaches this: an instrument runs until power is
     * pulled, which is why the auto-save is built to find a quiet moment
     * rather than to be asked. An app is *closed*, and that is a moment the
     * synth never gets -- so the last edits before a close would otherwise be
     * the ones the settle timer had not yet committed.
     *
     * Blocking, and that is acceptable here: the caller is shutting down, and
     * a stalled render chain has nothing left to disturb. */
    const esp_err_t serr = presets_state_save_now();
    if (serr != ESP_OK) {
        ESP_LOGW(TAG, "working state not saved on shutdown: %s",
                 esp_err_to_name(serr));
    } else {
        /* Logged on success too, which the firmware has no reason to do and
         * an app does: this is the only evidence that the shutdown path ran
         * at all. A close that skipped it looks identical from the outside to
         * one that ran and found nothing changed -- and only one of those is
         * a bug. (It says "checked", not "written", because the writer skips
         * the file when nothing has moved since the restore.) */
        ESP_LOGI(TAG, "working state checked and committed");
    }

    osynth_host_midi_in_stop();
    /* The device only. The control tasks own state the app may still be
     * reading, and the firmware never stops them either -- see the header. */
    audio_sink_host_stop();
}

bool osynth_host_running(void) { return g_started.load(); }
