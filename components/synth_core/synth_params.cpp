#include "synth_params.h"
#include "synth_params_c.h"

#include <cmath>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static_assert(SYNTH_PID_ENGINE_TYPE == osynth::PID_ENGINE_TYPE);
static_assert(SYNTH_PID_USB_MODE == osynth::PID_USB_MODE);
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

/* index_ is read on the audio task (synth_mod_begin_block -> describe), so it
 * has to lower to an inline load, not a libatomic call that could take a lock
 * inside the render deadline. Both toolchains define
 * __GCC_ATOMIC_SHORT_LOCK_FREE as 2 today — Xtensa has L16UI/S16I and the
 * RISC-V build has the A extension — and this is what says so out loud, so a
 * target or toolchain where it stops being true fails the build instead of
 * quietly putting a function call in the audio path. Widening index_ to
 * int32_t is the fix if it ever fires; it costs 4 KB of DRAM. */
static_assert(std::atomic<int16_t>::is_always_lock_free,
              "ParamStore::index_ must be lock-free: the audio task reads it");

ParamStore& ParamStore::instance() {
    static ParamStore store;
    return store;
}

ParamStore::ParamStore() {
    for (size_t i = 0; i < PID_SPACE_END; ++i) {
        index_[i].store(-1, std::memory_order_relaxed);
    }
}

ParamStore::Entry* ParamStore::entryFor(uint16_t id) {
    if (id >= PID_SPACE_END) return nullptr;
    /* Acquire, pairing with the release in add(): a reader on another task
     * that sees this slot published sees the descriptor stored into it first,
     * rather than one whose `name` pointer is still half written. Free at
     * run time on both targets — it is the compiler's reordering this stops,
     * not the core's. */
    const int16_t slot = index_[id].load(std::memory_order_acquire);
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
    if (index_[desc.id].load(std::memory_order_relaxed) >= 0) {
        duplicate = true;
    } else {
        for (size_t slot = 0; slot < kMaxParams; ++slot) {
            if (!entries_[slot].used) {
                entries_[slot].desc = desc;
                entries_[slot].used = true;
                entries_[slot].value.store(desc.def, std::memory_order_relaxed);
                /* Last, and a release: this store is what makes the id
                 * findable, so everything above it has to be visible to a
                 * reader that follows it. */
                index_[desc.id].store(static_cast<int16_t>(slot),
                                      std::memory_order_release);
                count_.fetch_add(1, std::memory_order_relaxed);
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
        const int16_t slot = index_[id].load(std::memory_order_relaxed);
        if (slot >= 0) {
            /* Unpublish first, so no reader can start following this id into
             * a slot add() is about to hand to a different parameter. One
             * already inside entryFor() keeps a valid pointer to a valid
             * descriptor either way — see the threading note in the header. */
            index_[id].store(-1, std::memory_order_release);
            entries_[slot].used = false;
            count_.fetch_sub(1, std::memory_order_relaxed);
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
    return count_.load(std::memory_order_relaxed);
}

size_t ParamStore::listIds(uint16_t* out, size_t max) const {
    size_t n = 0;
    for (uint16_t id = 0; id < PID_SPACE_END && n < max; ++id) {
        if (index_[id].load(std::memory_order_acquire) >= 0) out[n++] = id;
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
    /* Headroom, not just the count. Overflow is per-parameter and partial —
     * add() drops the one that did not fit and says so — and the components
     * that treat a partial registration as fatal (the engines, the drum bus,
     * seqarp) are ESP_ERROR_CHECKed at boot, so the store filling up presents
     * as a bootloop rather than as a missing control. This line is what makes
     * the margin visible before that happens: it is printed at the peak of
     * boot registration, and the peak that matters at run time is a full
     * modular graph, which is larger. Raise kMaxParams if it gets close. */
    const size_t n = count();
    ESP_LOGI(TAG, "registry: %u parameter(s), %u of %u slots free",
             (unsigned)n, (unsigned)(kMaxParams - n), (unsigned)kMaxParams);
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
