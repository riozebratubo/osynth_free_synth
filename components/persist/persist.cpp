/*
 * osynth — persisted settings (Session 25). Contract and rationale in
 * persist.h.
 */
#include "persist.h"

#include <atomic>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "audio_io.h"
#include "synth_params.h"

static const char* TAG = "persist";

using osynth::ParamDesc;
using osynth::ParamOrigin;
using osynth::ParamStore;

namespace {

constexpr char kNamespace[] = "osynth";
constexpr char kKey[] = "globals";
constexpr uint32_t kMagic = 0x314E4750u; /* "PGN1" */
constexpr uint16_t kVersion = 1;

/* Nothing is written until the dirty set has been still this long. */
constexpr uint32_t kSettleMs = 3000;
/* …and then not until the output has been inaudible for this long. */
constexpr uint32_t kQuietMs = 200;
/* …but never deferred past this, or a drone left running would cost the
 * setting entirely. */
constexpr uint32_t kMaxDeferMs = 120000;
/* Poll period of the writer task. Slow on purpose: this is a background
 * chore, and it wakes ~2x/s to do nothing at all in the common case. */
constexpr uint32_t kPollMs = 500;

constexpr int kTaskPrio = 2; /* below every other control task */
constexpr int kTaskStack = 3584;

struct __attribute__((packed)) Header {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
};
struct __attribute__((packed)) Pair {
    uint16_t id;
    float value;
};
static_assert(sizeof(Header) == 8, "on-disk layout");
static_assert(sizeof(Pair) == 6, "on-disk layout");

uint16_t s_ids[PERSIST_MAX_PARAMS];
size_t s_count = 0;

/* What is currently in flash, so an unchanged blob never causes a write. */
Pair s_stored[PERSIST_MAX_PARAMS];
size_t s_stored_count = 0;

std::atomic<bool> s_dirty{false};
std::atomic<uint32_t> s_dirty_seq{0}; /* bumped on every change: settle timer */
TaskHandle_t s_task = nullptr;
bool s_ready = false;

bool is_persisted(uint16_t id) {
    for (size_t i = 0; i < s_count; ++i) {
        if (s_ids[i] == id) return true;
    }
    return false;
}

/* Any control task; must stay short. Marking is all it does — the writer task
 * decides when. Preset loads are included deliberately: a preset that changes
 * a persisted setting should survive a reboot like any other edit. */
void param_listener(uint16_t id, float value, ParamOrigin origin, void*) {
    (void)value;
    (void)origin;
    if (!s_ready || !is_persisted(id)) return;
    s_dirty.store(true, std::memory_order_relaxed);
    s_dirty_seq.fetch_add(1, std::memory_order_relaxed);
}

/* Builds the blob from the live ParamStore. Returns the pair count. */
size_t snapshot(Pair* out) {
    ParamStore& ps = ParamStore::instance();
    size_t n = 0;
    for (size_t i = 0; i < s_count; ++i) {
        if (ps.describe(s_ids[i]) == nullptr) continue; /* unregistered now */
        out[n].id = s_ids[i];
        out[n].value = ps.get(s_ids[i]);
        ++n;
    }
    return n;
}

bool same_as_stored(const Pair* pairs, size_t n) {
    if (n != s_stored_count) return false;
    return memcmp(pairs, s_stored, n * sizeof(Pair)) == 0;
}

esp_err_t write_blob(const Pair* pairs, size_t n) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    uint8_t buf[sizeof(Header) + PERSIST_MAX_PARAMS * sizeof(Pair)];
    Header hdr = {kMagic, kVersion, (uint16_t)n};
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), pairs, n * sizeof(Pair));

    err = nvs_set_blob(h, kKey, buf, sizeof(hdr) + n * sizeof(Pair));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        memcpy(s_stored, pairs, n * sizeof(Pair));
        s_stored_count = n;
    }
    return err;
}

/* Writes if anything is dirty. `why` only labels the log line. */
esp_err_t flush(const char* why) {
    if (!s_dirty.load(std::memory_order_relaxed)) return ESP_OK;

    Pair pairs[PERSIST_MAX_PARAMS];
    const size_t n = snapshot(pairs);

    /* Clear the flag before writing: a change that lands during the write
     * must leave the set dirty again rather than be swallowed. */
    s_dirty.store(false, std::memory_order_relaxed);

    if (same_as_stored(pairs, n)) return ESP_OK; /* nothing actually moved */

    const esp_err_t err = write_blob(pairs, n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save failed (%s) — will retry", esp_err_to_name(err));
        s_dirty.store(true, std::memory_order_relaxed);
        return err;
    }
    ESP_LOGI(TAG, "saved %u value(s) [%s]", (unsigned)n, why);
    return ESP_OK;
}

void persist_task(void*) {
    uint32_t last_seq = s_dirty_seq.load(std::memory_order_relaxed);
    uint32_t settled_ms = 0;
    uint32_t waiting_ms = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kPollMs));

        if (!s_dirty.load(std::memory_order_relaxed)) {
            settled_ms = 0;
            waiting_ms = 0;
            continue;
        }

        /* Still moving? Restart the settle timer — but not the overall defer
         * budget, or a slider nudged every few seconds would never save. */
        const uint32_t seq = s_dirty_seq.load(std::memory_order_relaxed);
        if (seq != last_seq) {
            last_seq = seq;
            settled_ms = 0;
        } else {
            settled_ms += kPollMs;
        }
        waiting_ms += kPollMs;

        if (settled_ms < kSettleMs) continue;

        const bool quiet = audio_io_quiet_ms() >= kQuietMs;
        const bool overdue = waiting_ms >= kMaxDeferMs;
        if (!quiet && !overdue) continue;

        flush(quiet ? "quiet" : "overdue");
        settled_ms = 0;
        waiting_ms = 0;
    }
}

} // namespace

size_t persist_add(const uint16_t* ids, size_t count) {
    if (ids == nullptr) return 0;
    size_t added = 0;
    for (size_t i = 0; i < count; ++i) {
        if (is_persisted(ids[i])) continue; /* idempotent */
        if (s_count >= PERSIST_MAX_PARAMS) {
            ESP_LOGE(TAG, "persisted set full (%d) — 0x%04x dropped",
                     PERSIST_MAX_PARAMS, ids[i]);
            break;
        }
        s_ids[s_count++] = ids[i];
        ++added;
    }
    return added;
}

esp_err_t persist_init(void) {
    ParamStore& ps = ParamStore::instance();

    /* Drop anything that never got registered: persisting an id nothing owns
     * would quietly resurrect it as a stored value forever. */
    size_t w = 0;
    for (size_t i = 0; i < s_count; ++i) {
        if (ps.describe(s_ids[i]) != nullptr) {
            s_ids[w++] = s_ids[i];
        } else {
            ESP_LOGW(TAG, "0x%04x is not registered — not persisted", s_ids[i]);
        }
    }
    s_count = w;

    /* ---- load ---- */
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable (%s) — settings will not persist",
                 esp_err_to_name(err));
        return ESP_OK; /* the sink-fallback rule: never fail to boot for this */
    }

    uint8_t buf[sizeof(Header) + PERSIST_MAX_PARAMS * sizeof(Pair)];
    size_t len = sizeof(buf);
    err = nvs_get_blob(h, kKey, buf, &len);
    nvs_close(h);

    int applied = 0;
    if (err == ESP_OK && len >= sizeof(Header)) {
        Header hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        const size_t have = (len - sizeof(Header)) / sizeof(Pair);
        if (hdr.magic != kMagic || hdr.version != kVersion) {
            ESP_LOGW(TAG, "stored settings are v%u/%08x — ignoring",
                     hdr.version, (unsigned)hdr.magic);
        } else {
            const size_t n = hdr.count < have ? hdr.count : have;
            for (size_t i = 0; i < n && i < PERSIST_MAX_PARAMS; ++i) {
                Pair p;
                memcpy(&p, buf + sizeof(Header) + i * sizeof(Pair), sizeof(p));
                /* Only apply what is still both registered and wanted: a
                 * firmware that stopped persisting an id must not have it
                 * pushed back in, and the store clamps the value anyway. */
                if (!is_persisted(p.id) || ps.describe(p.id) == nullptr) continue;
                ps.set(p.id, p.value, ParamOrigin::Preset);
                ++applied;  /* s_stored is rebuilt by snapshot() below */
            }
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored settings yet (first boot)");
    }

    /* Canonical order for the change test: snapshot() walks s_ids, so compare
     * against a blob built the same way rather than against the stored order,
     * which need not match after the persisted set changes. */
    s_stored_count = snapshot(s_stored);

    s_ready = true;

    /* Neither failure below is fatal. Settings that stop being saved is a
     * degraded synth; a synth that refuses to boot because it could not
     * arrange to save them is a broken one (the sink-fallback rule). */
    if (ps.addListener(param_listener, nullptr) < 0) {
        ESP_LOGE(TAG, "listener table full — changes will not be saved");
        return ESP_OK;
    }
    if (xTaskCreatePinnedToCore(persist_task, "persist", kTaskStack, nullptr,
                                kTaskPrio, &s_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "writer task not started — use persist_save_now()");
        return ESP_OK;
    }

    ESP_LOGI(TAG,
             "up: %u param(s) tracked, %d restored; writes settle %u ms then "
             "wait for silence (forced after %u s)",
             (unsigned)s_count, applied, (unsigned)kSettleMs,
             (unsigned)(kMaxDeferMs / 1000));
    return ESP_OK;
}

bool persist_owns(uint16_t id) { return is_persisted(id); }

esp_err_t persist_save_now(void) { return flush("forced"); }

esp_err_t persist_reset(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, kKey);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    s_stored_count = 0;
    s_dirty.store(false, std::memory_order_relaxed);

    ParamStore& ps = ParamStore::instance();
    for (size_t i = 0; i < s_count; ++i) {
        const ParamDesc* d = ps.describe(s_ids[i]);
        if (d != nullptr) ps.set(s_ids[i], d->def, ParamOrigin::Preset);
    }
    ESP_LOGI(TAG, "stored settings cleared; defaults restored");
    return ESP_OK;
}
