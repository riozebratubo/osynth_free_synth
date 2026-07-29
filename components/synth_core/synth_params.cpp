#include "synth_params.h"
#include "synth_params_c.h"

#include <cmath>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static_assert(SYNTH_PID_ENGINE_TYPE == osynth::PID_ENGINE_TYPE);
static_assert(SYNTH_PID_COMMON_GLIDE == osynth::PID_COMMON_GLIDE);
static_assert(SYNTH_PID_COMMON_BEND_RANGE == osynth::PID_COMMON_BEND_RANGE);
static_assert(SYNTH_PID_COMMON_UNISON == osynth::PID_COMMON_UNISON);
static_assert(SYNTH_PID_COMMON_UNI_DETUNE == osynth::PID_COMMON_UNI_DETUNE);
static_assert(SYNTH_PID_COMMON_UNI_SPREAD == osynth::PID_COMMON_UNI_SPREAD);

namespace osynth {

static const char* TAG = "params";

/* Guards registration (entries_/index_/count_) and the listener table.
 * The value hot path (get/set/valuePtr reads) is atomic and lock-free. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

ParamStore& ParamStore::instance() {
    static ParamStore store;
    return store;
}

ParamStore::ParamStore() {
    for (size_t i = 0; i < PID_SPACE_END; ++i) {
        index_[i] = -1;
    }
}

ParamStore::Entry* ParamStore::entryFor(uint16_t id) {
    if (id >= PID_SPACE_END) return nullptr;
    int16_t slot = index_[id];
    return (slot >= 0) ? &entries_[slot] : nullptr;
}

const ParamStore::Entry* ParamStore::entryFor(uint16_t id) const {
    return const_cast<ParamStore*>(this)->entryFor(id);
}

bool ParamStore::add(const ParamDesc& desc) {
    if (desc.id >= PID_SPACE_END || desc.name == nullptr || desc.max < desc.min) {
        ESP_LOGE(TAG, "add: invalid descriptor (id 0x%04x)", desc.id);
        return false;
    }

    bool duplicate = false;
    bool full = true;
    portENTER_CRITICAL(&s_mux);
    if (index_[desc.id] >= 0) {
        duplicate = true;
    } else {
        for (size_t slot = 0; slot < kMaxParams; ++slot) {
            if (!entries_[slot].used) {
                entries_[slot].desc = desc;
                entries_[slot].used = true;
                entries_[slot].value.store(desc.def, std::memory_order_relaxed);
                index_[desc.id] = static_cast<int16_t>(slot);
                ++count_;
                full = false;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_mux);

    if (duplicate) {
        ESP_LOGE(TAG, "add: id 0x%04x (%s) already registered", desc.id, desc.name);
        return false;
    }
    if (full) {
        ESP_LOGE(TAG, "add: store full (%u params), dropped 0x%04x (%s)",
                 (unsigned)kMaxParams, desc.id, desc.name);
        return false;
    }
    generation_.fetch_add(1, std::memory_order_release);
    return true;
}

size_t ParamStore::add(const ParamDesc* descs, size_t count) {
    size_t added = 0;
    for (size_t i = 0; i < count; ++i) {
        if (add(descs[i])) ++added;
    }
    return added;
}

size_t ParamStore::removeRange(uint16_t first, uint16_t last_exclusive) {
    if (last_exclusive > PID_SPACE_END) last_exclusive = PID_SPACE_END;
    size_t removed = 0;
    portENTER_CRITICAL(&s_mux);
    for (uint16_t id = first; id < last_exclusive; ++id) {
        int16_t slot = index_[id];
        if (slot >= 0) {
            entries_[slot].used = false;
            index_[id] = -1;
            --count_;
            ++removed;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    if (removed > 0) generation_.fetch_add(1, std::memory_order_release);
    return removed;
}

uint32_t ParamStore::generation() const {
    return generation_.load(std::memory_order_acquire);
}

bool ParamStore::set(uint16_t id, float value, ParamOrigin origin) {
    Entry* e = entryFor(id);
    if (e == nullptr) return false;

    /* Non-finite values have to be refused before the clamp, because the
     * clamp cannot catch them: every comparison against NaN is false, so
     * `value < min` and `value > max` both fail and the NaN lands in the
     * store — where it latches.
     *
     * master.volume is the worst case. audio_io slews the gain with
     * `d = target - s_gain`, so one NaN block leaves s_gain NaN and every
     * later block computes `target - NaN` — the output never recovers, not
     * even after the parameter is set back to something sane, and persist
     * (S25) writes the NaN to NVS during the silence that follows, so a
     * reboot restores it. An Exp-curve engine parameter poisons its
     * block smoother (synth_smooth.h multiplies through it) until the engine
     * is re-bound, and an Enum one makes the `(int)` casts the render path
     * does on it undefined — engine_wavetable indexes wt_tables with one.
     *
     * This is reachable input, not a theoretical case: BLE SET_PARAM carries
     * four raw bytes straight into a float (ble_ctrl.cpp rdf32), and preset
     * and set files are read back off the filesystem the same way. */
    if (!std::isfinite(value)) {
        ESP_LOGW(TAG, "set: non-finite value for 0x%04x ignored", id);
        return false;
    }

    const ParamDesc& d = e->desc;
    if (d.type != ParamType::Float) value = std::round(value);
    if (value < d.min) value = d.min;
    if (value > d.max) value = d.max;

    e->value.store(value, std::memory_order_relaxed);
    notify(id, value, origin);
    return true;
}

float ParamStore::get(uint16_t id) const {
    const Entry* e = entryFor(id);
    return e ? e->value.load(std::memory_order_relaxed) : 0.0f;
}

const std::atomic<float>* ParamStore::valuePtr(uint16_t id) const {
    const Entry* e = entryFor(id);
    return e ? &e->value : nullptr;
}

const ParamDesc* ParamStore::describe(uint16_t id) const {
    const Entry* e = entryFor(id);
    return e ? &e->desc : nullptr;
}

size_t ParamStore::count() const {
    return count_;
}

size_t ParamStore::listIds(uint16_t* out, size_t max) const {
    size_t n = 0;
    for (uint16_t id = 0; id < PID_SPACE_END && n < max; ++id) {
        if (index_[id] >= 0) out[n++] = id;
    }
    return n;
}

void ParamStore::resetRange(uint16_t first, uint16_t last_exclusive) {
    if (last_exclusive > PID_SPACE_END) last_exclusive = PID_SPACE_END;
    for (uint16_t id = first; id < last_exclusive; ++id) {
        const Entry* e = entryFor(id);
        if (e != nullptr) {
            set(id, e->desc.def, ParamOrigin::Internal);
        }
    }
}

int ParamStore::addListener(ParamListener fn, void* ctx) {
    if (fn == nullptr) return -1;
    int handle = -1;
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < kMaxListeners; ++i) {
        if (listeners_[i].fn == nullptr) {
            listeners_[i].fn = fn;
            listeners_[i].ctx = ctx;
            handle = static_cast<int>(i);
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    if (handle < 0) {
        ESP_LOGE(TAG, "addListener: table full (%u)", (unsigned)kMaxListeners);
    }
    return handle;
}

void ParamStore::removeListener(int handle) {
    if (handle < 0 || handle >= (int)kMaxListeners) return;
    portENTER_CRITICAL(&s_mux);
    listeners_[handle].fn = nullptr;
    listeners_[handle].ctx = nullptr;
    portEXIT_CRITICAL(&s_mux);
}

void ParamStore::notify(uint16_t id, float value, ParamOrigin origin) {
    ListenerSlot snapshot[kMaxListeners];
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < kMaxListeners; ++i) snapshot[i] = listeners_[i];
    portEXIT_CRITICAL(&s_mux);

    for (size_t i = 0; i < kMaxListeners; ++i) {
        if (snapshot[i].fn != nullptr) {
            snapshot[i].fn(id, value, origin, snapshot[i].ctx);
        }
    }
}

void ParamStore::dump() const {
    static const char* kTypeNames[] = {"float", "int", "enum", "bool"};
    ESP_LOGI(TAG, "registry: %u parameter(s)", (unsigned)count_);
    for (uint16_t id = 0; id < PID_SPACE_END; ++id) {
        const Entry* e = entryFor(id);
        if (e == nullptr) continue;
        const ParamDesc& d = e->desc;
        float v = e->value.load(std::memory_order_relaxed);
        if (d.type == ParamType::Enum && d.enum_names != nullptr) {
            int iv = (int)v;
            const char* label =
                (iv >= 0 && iv < d.enum_count) ? d.enum_names[iv] : "?";
            ESP_LOGI(TAG, "  0x%04x %-24s = %s (%d) [enum 0..%d]",
                     id, d.name, label, iv, d.enum_count - 1);
        } else {
            ESP_LOGI(TAG, "  0x%04x %-24s = %.3f [%s %.2f..%.2f]",
                     id, d.name, v, kTypeNames[(int)d.type], d.min, d.max);
        }
    }
}

} // namespace osynth

extern "C" bool synth_param_set_norm_midi(uint16_t id, float norm01) {
    using namespace osynth;
    ParamStore& ps = ParamStore::instance();
    const ParamDesc* d = ps.describe(id);
    if (d == nullptr) return false;
    if (norm01 < 0.0f) norm01 = 0.0f;
    if (norm01 > 1.0f) norm01 = 1.0f;
    float v;
    if (d->curve == ParamCurve::Exp && d->min > 0.0f) {
        v = d->min * powf(d->max / d->min, norm01);
    } else {
        v = d->min + (d->max - d->min) * norm01;
    }
    return ps.set(id, v, ParamOrigin::Midi);
}

extern "C" bool synth_param_set_midi(uint16_t id, float value) {
    using namespace osynth;
    return ParamStore::instance().set(id, value, ParamOrigin::Midi);
}

extern "C" bool synth_param_set_nrpn_midi(uint16_t id, uint16_t value14) {
    using namespace osynth;
    ParamStore& ps = ParamStore::instance();
    const ParamDesc* d = ps.describe(id);
    if (d == nullptr) return false;
    if (value14 > 0x3FFF) value14 = 0x3FFF;
    if (d->type != ParamType::Float) {
        /* Int/Enum/Bool: min + data (min is 0 for enums and mod dest ids,
         * so the data value is the value; set() clamps and snaps) */
        return ps.set(id, d->min + (float)value14, ParamOrigin::Midi);
    }
    const float norm = (float)value14 * (1.0f / 16383.0f);
    float v;
    if (d->curve == ParamCurve::Exp && d->min > 0.0f) {
        v = d->min * powf(d->max / d->min, norm);
    } else {
        v = d->min + (d->max - d->min) * norm;
    }
    return ps.set(id, v, ParamOrigin::Midi);
}
