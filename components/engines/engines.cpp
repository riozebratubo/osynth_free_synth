/*
 * osynth — engine registry + switch machinery (Session 5; switching S6).
 *
 * Boot: engines_init() binds the engine named by engine.type (default
 * subtractive), then registers a ParamStore listener and starts the switch
 * task. Any engine.type write (MIDI program change today, BLE/presets
 * later) notifies the task, which runs the switch protocol on core 0:
 *
 *   mute (~10 ms ramp) -> voice_manager_detach_engine() (waits for the
 *   audio task to leave the old engine, frees the voice pool) ->
 *   old->deinit() (drops its 0x02xx params) -> new->init() (registers its
 *   set) -> voice_manager_set_engine(new) -> unmute.
 *
 * Failure at any step rolls back to the old engine and reverts the
 * parameter. Requests for an engine id this build does not have warn and
 * revert — since S38 that is a live path rather than a defensive one, since
 * the enum reserves the modular engine's index whether or not it is compiled
 * in (engines.h). The listener itself only stores the request and notifies —
 * it may run on the TinyUSB task.
 */
#include "engines.h"

#include <atomic>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "engine_additive.h"
#include "engine_fm.h"
#include "engine_granular.h"
#include "engine_sampler.h"
#include "engine_subtractive.h"
#include "engine_wavetable.h"
#include "synth_config.h"
#include "synth_params.h"
#include "synth_voice.h"

#if SYNTH_ENABLE_MODULAR
#include "graph_engine.h"
#endif

static const char* TAG = "engines";

using osynth::ParamOrigin;
using osynth::ParamStore;
using osynth::PID_ENGINE_TYPE;

namespace {

/* Positional, and the null is the point: since S38 the enum reserves index 4
 * for the modular engine whether or not this build has one, so the slot has
 * to be occupied by something. engines_get() hands the null straight back,
 * which is the "engine %d not available" path below. */
const synth_engine_t* const s_engines[SYNTH_ENGINE_COUNT] = {
    &g_engine_subtractive, /* SYNTH_ENGINE_SUBTRACTIVE */
    &g_engine_additive,    /* SYNTH_ENGINE_ADDITIVE */
    &g_engine_fm,          /* SYNTH_ENGINE_FM */
    &g_engine_wavetable,   /* SYNTH_ENGINE_WAVETABLE */
#if SYNTH_ENABLE_MODULAR
    &g_engine_modular,     /* SYNTH_ENGINE_MODULAR (S28) */
#else
    nullptr,               /* SYNTH_ENGINE_MODULAR, not built */
#endif
    &g_engine_granular,    /* SYNTH_ENGINE_GRANULAR (S38) */
    &g_engine_sampler,     /* SYNTH_ENGINE_SAMPLER (S44) */
};

std::atomic<int> s_active_type{-1};
std::atomic<int> s_requested{-1};
TaskHandle_t s_switch_task = nullptr;

constexpr int kSwitchTaskPrio = 5;      /* control plane, core 0 */
constexpr int kSwitchTaskStack = 4096;

void revert_param(int to) {
    /* re-fires the listener; the task then sees requested == active and
     * idles, so this cannot loop */
    ParamStore::instance().set(PID_ENGINE_TYPE, (float)to,
                               ParamOrigin::Internal);
}

/* Waits for the mute ramp to settle. Cosmetic: on timeout the switch goes
 * ahead anyway — the detach handshake is what guarantees memory safety. */
void mute_and_wait() {
    voice_manager_set_muted(true);
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(250);
    while (!voice_manager_muted()) {
        if (xTaskGetTickCount() > deadline) {
            ESP_LOGW(TAG, "mute ramp never settled, switching anyway");
            break;
        }
        vTaskDelay(1);
    }
}

void switch_to(int want) {
    const int cur = s_active_type.load(std::memory_order_relaxed);
    const synth_engine_t* next = engines_get((synth_engine_type_t)want);
    if (next == nullptr) {
        ESP_LOGW(TAG, "engine %d not available", want);
        revert_param(cur);
        return;
    }
    const synth_engine_t* old = engines_get((synth_engine_type_t)cur);

    mute_and_wait();
    if (voice_manager_detach_engine() != ESP_OK) {
        ESP_LOGE(TAG, "switch aborted: could not detach %s", old->name);
        revert_param(cur);
        voice_manager_set_muted(false);
        return;
    }
    old->deinit();

    esp_err_t err = next->init();
    if (err == ESP_OK) err = voice_manager_set_engine(next);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "switch to %s failed (%s), restoring %s", next->name,
                 esp_err_to_name(err), old->name);
        next->deinit(); /* scrub any partially registered params */
        if (old->init() != ESP_OK || voice_manager_set_engine(old) != ESP_OK) {
            ESP_LOGE(TAG, "restore failed — no engine bound, output silent");
        }
        revert_param(cur);
        voice_manager_set_muted(false);
        return;
    }

    s_active_type.store(want, std::memory_order_relaxed);
    voice_manager_set_muted(false);
    ESP_LOGI(TAG, "engine switched: %s (caps 0x%02x, %u params)", next->name,
             (unsigned)next->caps,
             (unsigned)(ParamStore::instance().count()));
}

void switch_task(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const int want = s_requested.load(std::memory_order_relaxed);
        if (want != s_active_type.load(std::memory_order_relaxed)) {
            switch_to(want);
        }
    }
}

void engine_type_listener(uint16_t id, float value, ParamOrigin origin,
                          void*) {
    (void)origin;
    if (id != PID_ENGINE_TYPE || s_switch_task == nullptr) return;
    s_requested.store((int)value, std::memory_order_relaxed);
    xTaskNotifyGive(s_switch_task);
}

} // namespace

extern "C" const synth_engine_t* engines_get(synth_engine_type_t type) {
    const int t = (int)type;
    return (t >= 0 && t < SYNTH_ENGINE_COUNT) ? s_engines[t] : nullptr;
}

extern "C" synth_engine_type_t engines_active_type(void) {
    return (synth_engine_type_t)s_active_type.load(std::memory_order_relaxed);
}

extern "C" esp_err_t engines_init(void) {
    ParamStore& ps = ParamStore::instance();

    int type = (int)ps.get(PID_ENGINE_TYPE);
    const synth_engine_t* eng = engines_get((synth_engine_type_t)type);
    if (eng == nullptr) {
        ESP_LOGW(TAG, "engine %d unavailable at boot, using subtractive", type);
        type = SYNTH_ENGINE_SUBTRACTIVE;
        eng = engines_get(SYNTH_ENGINE_SUBTRACTIVE);
        ps.set(PID_ENGINE_TYPE, (float)type, ParamOrigin::Internal);
    }
    esp_err_t err = eng->init();
    if (err != ESP_OK) return err;
    err = voice_manager_set_engine(eng);
    if (err != ESP_OK) return err;
    s_active_type.store(type, std::memory_order_relaxed);

    if (xTaskCreatePinnedToCore(switch_task, "eng_switch", kSwitchTaskStack,
                                nullptr, kSwitchTaskPrio, &s_switch_task,
                                0) != pdPASS) {
        return ESP_FAIL;
    }
    if (ps.addListener(engine_type_listener, nullptr) < 0) return ESP_FAIL;

    ESP_LOGI(TAG, "active engine: %s (switch via engine.type / program change)",
             eng->name);
    return ESP_OK;
}
