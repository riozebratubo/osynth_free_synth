/*
 * osynth — persisted settings (Session 25). Contract and rationale in
 * persist.h.
 */
#include "persist.h"

#include <atomic>
#include <cstdio>
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
 * setting entirely.
 *
 * Two minutes until S47, when the quiet path turned out to be unreachable on
 * any rig with a live input rather than merely rare. kQuietPeak is ~-78 dBFS
 * and a monitored microphone sits three orders of magnitude above it, so the
 * output peak never falls below the threshold, `quiet` is never true, and
 * *every* write on such a rig comes out of this timer. The measured symptom
 * was a player setting in.route to off, seeing nothing in the log, reflashing
 * within the window and getting the old value back: `saved 8 value(s)
 * [overdue]` at t+120 s was the first and only sign the write had happened.
 *
 * So this is the normal path, not the backstop, and it is timed as one. Ten
 * seconds is well clear of kSettleMs — a slider sweep still coalesces into a
 * single write — and short enough that changing a setting and reaching for the
 * power switch is not a race. What it costs is that on those rigs the write
 * lands while sound is playing; that was already true at 120 s, and it is the
 * flash-stall question the XiP note in sdkconfig.defaults.esp32p4 leaves open,
 * not a new exposure.
 *
 * The quiet gate is deliberately left in place rather than removed: on a rig
 * with nothing plugged in it still works, and it still costs one compare per
 * block that was computed anyway. */
constexpr uint32_t kMaxDeferMs = 10000;
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

/* What the *player* last set, which is not the same thing as what the store
 * holds when the write finally happens (S47).
 *
 * flush() used to call ps.get() at write time, and write time is always some
 * seconds after the edit was marked — kMaxDeferMs, two minutes when this was
 * found. Anything that moved the parameter in between won a race it was never
 * meant to enter: sampler.cpp's
 * monitor_hold() borrows in.route from `off` to `mon` while a pad is armed,
 * with ParamOrigin::Internal so that the borrow is not itself stored, and the
 * snapshot then picked up `mon` anyway and stored *that*. The player's `off`
 * was dropped, s_dirty was cleared as though it had been saved, and — because
 * the resulting blob matched what was already in NVS — flush() returned through
 * its same_as_stored() path without logging a thing. "Set it to off, reflash,
 * comes back monitoring, and nothing in the log" is that bug end to end.
 *
 * So the value is captured in the listener, at the moment of the edit, by the
 * same test that decides the edit is worth saving. Internal writes update
 * neither the dirty flag nor this, which is now one rule instead of two.
 * Atomic because control tasks write it and the persist task reads it. */
std::atomic<float> s_shadow[PERSIST_MAX_PARAMS];

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
 * a persisted setting should survive a reboot like any other edit.
 *
 * ParamOrigin::Internal is not, and that exclusion is the whole of S45c. It is
 * the firmware talking to itself, and what it says is usually *temporary* — the
 * case that found this is sampler.cpp's monitor_hold(), which borrows in.route
 * from `off` to `mon` while a pad is armed so the player can hear what they are
 * about to record, and puts back what was there when the recorder goes idle.
 * Storing the borrowed value defeats the borrow: a reset taken while the pad
 * was still armed, or simply before the restore's own write window came round,
 * left `mon` in NVS for good. The player turns the input monitor off, reflashes,
 * and the box comes up monitoring.
 *
 * presets.cpp reached the same conclusion first, for the same reason, and
 * state_tracks() there is worth reading beside this: parameter locks rewrite a
 * patch value on every step and put it back when the track stops, and counting
 * those as edits would store the lock's momentary value rather than the one the
 * player set. Identical shape, different owner.
 *
 * Nothing is lost by it. The two other Internal writers that can reach a
 * persisted id are usb_mode_resolve()'s corrective clamp — recomputed from the
 * stored value at every boot, so persisting it only ever overwrote the choice
 * the player is still entitled to — and ParamStore::resetRange(), which has no
 * caller outside synth_params.cpp. A genuine reset-to-defaults arriving from
 * the app, MIDI or the local UI carries that origin and is stored as always. */
void param_listener(uint16_t id, float value, ParamOrigin origin, void*) {
    if (origin == ParamOrigin::Internal) return;
    if (!s_ready || !is_persisted(id)) return;
    /* The value as the player left it, recorded here rather than re-read at
     * write time — see s_shadow. `value` is what ParamStore has just clamped
     * and stored, so this is the same number ps.get() would return now; what
     * it is not is whatever some later Internal write leaves behind. */
    for (size_t i = 0; i < s_count; ++i) {
        if (s_ids[i] == id) {
            s_shadow[i].store(value, std::memory_order_relaxed);
            break;
        }
    }
    s_dirty.store(true, std::memory_order_relaxed);
    s_dirty_seq.fetch_add(1, std::memory_order_relaxed);
}

/* One line naming every pair, for the two moments worth seeing: what came out
 * of NVS at boot, and what is going into it on a write (S47).
 *
 * The boot line used to report a *count* — "8 restored" — which is exactly as
 * much as you need to know that persistence is alive and not nearly enough to
 * see that it restored the wrong value. Two rounds of this session were spent
 * unable to tell "NVS holds the wrong value" from "NVS is right and something
 * overwrites it after the restore", a question one printed value answers.
 *
 * INFO on purpose: it is two lines per boot and one per write, and the writes
 * are already rate-limited by kSettleMs. Enum values print as the number, not
 * the label — the labels live in the param table and this component
 * deliberately knows nothing about any particular id. */
void log_pairs(const char* what, const Pair* pairs, size_t n) {
    ParamStore& ps = ParamStore::instance();
    char line[224];
    size_t used = 0;
    for (size_t i = 0; i < n && used + 1 < sizeof(line); ++i) {
        const ParamDesc* d = ps.describe(pairs[i].id);
        const int wrote = snprintf(line + used, sizeof(line) - used, "%s%s=%g",
                                   used ? " " : "",
                                   d != nullptr ? d->name : "?",
                                   (double)pairs[i].value);
        if (wrote < 0 || (size_t)wrote >= sizeof(line) - used) {
            /* Out of line: say so rather than print a value cut in half. */
            snprintf(line + used, sizeof(line) - used, " ...");
            break;
        }
        used += (size_t)wrote;
    }
    if (n == 0) line[0] = '\0';
    ESP_LOGI(TAG, "%s: %s", what, line);
}

/* Builds the blob from the shadow. Returns the pair count.
 *
 * From s_shadow, not from ParamStore, so a value borrowed after the edit was
 * marked cannot be written in place of the edit itself. At init the shadow is
 * seeded from the store (nothing has been edited yet, and the restored blob is
 * the truth); after that the listener is its only writer. */
size_t snapshot(Pair* out) {
    ParamStore& ps = ParamStore::instance();
    size_t n = 0;
    for (size_t i = 0; i < s_count; ++i) {
        if (ps.describe(s_ids[i]) == nullptr) continue; /* unregistered now */
        out[n].id = s_ids[i];
        out[n].value = s_shadow[i].load(std::memory_order_relaxed);
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
    log_pairs("wrote", pairs, n);
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
            Pair from_nvs[PERSIST_MAX_PARAMS];
            size_t seen = 0;
            for (size_t i = 0; i < n && i < PERSIST_MAX_PARAMS; ++i) {
                Pair p;
                memcpy(&p, buf + sizeof(Header) + i * sizeof(Pair), sizeof(p));
                from_nvs[seen++] = p; /* logged below, applied or not */
                /* Only apply what is still both registered and wanted: a
                 * firmware that stopped persisting an id must not have it
                 * pushed back in, and the store clamps the value anyway. */
                if (!is_persisted(p.id) || ps.describe(p.id) == nullptr) continue;
                ps.set(p.id, p.value, ParamOrigin::Preset);
                ++applied;  /* s_stored is rebuilt by snapshot() below */
            }
            /* What the flash actually held, before this build had any say in
             * it. Read this against the "restored" line below: the two differ
             * only where an id was dropped from the persisted set or the store
             * clamped the value, and if they agree while the instrument still
             * comes up wrong, the overwrite is downstream of persist and no
             * amount of work in this file will fix it. */
            log_pairs("from nvs", from_nvs, seen);
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored settings yet (first boot)");
    }

    /* Seed the shadow from the live store, which at this point holds the
     * restored blob (or the defaults on a first boot). This has to happen
     * before the snapshot below and before the listener is added: from here on
     * the listener is the shadow's only writer, so a parameter that is never
     * touched this session must already carry its restored value or the first
     * unrelated edit would write zeroes over all of them. */
    for (size_t i = 0; i < s_count; ++i) {
        const ParamDesc* d = ps.describe(s_ids[i]);
        s_shadow[i].store(d != nullptr ? ps.get(s_ids[i]) : 0.0f,
                          std::memory_order_relaxed);
    }

    /* Canonical order for the change test: snapshot() walks s_ids, so compare
     * against a blob built the same way rather than against the stored order,
     * which need not match after the persisted set changes. */
    s_stored_count = snapshot(s_stored);
    /* The live values persist is leaving behind. Anything that differs from
     * here on was written by something else, which is the fact the "from nvs"
     * line above exists to pin down. */
    log_pairs("restored", s_stored, s_stored_count);

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
