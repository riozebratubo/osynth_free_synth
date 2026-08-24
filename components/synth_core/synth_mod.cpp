/*
 * osynth — mod matrix implementation (Session 9).
 *
 * The per-block plan is a compact array of the active slots (src != off,
 * amount != 0, dest registered): destination id, per-voice source index,
 * amount, plus the destination's mapping data (range/curve/snap) so
 * synth_mod_apply() never touches the ParamStore. Global sources (bend,
 * wheel) are folded into a per-slot offset at plan-build time, so apply()
 * only evaluates per-voice sources. Both run on the audio task; the
 * registry lookups in begin_block() are safe because it is only reached
 * while an engine is bound and every add()/removeRange() happens either
 * before the audio task starts or inside the engine-switch protocol, which
 * detaches the engine first (see engines.cpp).
 */
#include "synth_mod.h"

#include <atomic>
#include <cmath>

#include "esp_log.h"

#include "synth_config.h"
#include "synth_params.h"

static const char* TAG = "modmat";

static_assert(SYNTH_PID_MOD_BASE == osynth::PID_MODMATRIX_BASE);
static_assert(SYNTH_PID_MOD_AMOUNT(SYNTH_MOD_SLOTS - 1) < osynth::PID_SPACE_END);

using osynth::ParamCurve;
using osynth::ParamDesc;
using osynth::ParamStore;
using osynth::ParamType;

namespace {

const char* const kSrcNames[SYNTH_MOD_SRC_COUNT] = {
    "off", "env2", "lfo1", "lfo2", "vel", "note", "bend", "wheel"};

std::atomic<float> s_wheel{0.0f};

/* Cached slot-param value pointers: [slot][src, dest, amount]. */
const std::atomic<float>* s_pv[SYNTH_MOD_SLOTS][3];

/* False until every one of those resolved. The block path below dereferences
 * them unconditionally, so a partial registration — the parameter store
 * running out of room — would fault the audio task on its first block rather
 * than costing a control. One compare per block buys the matrix the same
 * "disabled, not fatal" degradation the FX bus takes. */
bool s_ready = false;

struct PlanSlot {
    uint16_t dest;
    uint8_t src;      /* per-voice source; SYNTH_MOD_SRC_OFF when folded */
    bool is_exp;
    bool snap;        /* Int/Enum/Bool destination: round the result */
    float amount;
    float min, max;
    float span;       /* max - min (linear mapping) */
    float log_ratio;  /* logf(max / min) (exp mapping) */
    float global_off; /* bend/wheel contribution, folded at block rate */
};

PlanSlot s_plan[SYNTH_MOD_SLOTS];
int s_nplan = 0;

} // namespace

extern "C" esp_err_t synth_mod_init(void) {
/* three params per slot; the n-1 keeps ids 0-based while names read mod1.. */
#define MOD_SLOT(n)                                                          \
    {SYNTH_PID_MOD_SRC(n - 1), "mod" #n ".src", ParamType::Enum,             \
     ParamCurve::Linear, 0.0f, (float)(SYNTH_MOD_SRC_COUNT - 1), 0.0f,       \
     kSrcNames, SYNTH_MOD_SRC_COUNT},                                        \
    {SYNTH_PID_MOD_DEST(n - 1), "mod" #n ".dest", ParamType::Int,            \
     ParamCurve::Linear, 0.0f, (float)(osynth::PID_SPACE_END - 1), 0.0f,     \
     nullptr, 0}, /* a param id; 0 = none */                                 \
    {SYNTH_PID_MOD_AMOUNT(n - 1), "mod" #n ".amount", ParamType::Float,      \
     ParamCurve::Linear, -1.0f, 1.0f, 0.0f, nullptr, 0}
    static const ParamDesc kParams[] = {
        MOD_SLOT(1), MOD_SLOT(2), MOD_SLOT(3), MOD_SLOT(4),
        MOD_SLOT(5), MOD_SLOT(6), MOD_SLOT(7), MOD_SLOT(8),
    };
#undef MOD_SLOT
    constexpr size_t kCount = sizeof(kParams) / sizeof(kParams[0]);
    static_assert(kCount == SYNTH_MOD_SLOTS * 3);

    ParamStore& ps = ParamStore::instance();
    const size_t added = ps.add(kParams, kCount);
    if (added != kCount) {
        /* ESP_OK with the matrix down, for the same reason fx_init() does it:
         * main.cpp ESP_ERROR_CHECKs this, and a full parameter store should
         * not be a bootloop on an instrument that would otherwise play. */
        ESP_LOGE(TAG,
                 "registered %u/%u params — mod matrix disabled for this boot "
                 "(the parameter store is full)",
                 (unsigned)added, (unsigned)kCount);
        ps.removeRange(SYNTH_PID_MOD_BASE, osynth::PID_LOOPER_BASE);
        return ESP_OK;
    }
    for (int k = 0; k < SYNTH_MOD_SLOTS; ++k) {
        s_pv[k][0] = ps.valuePtr(SYNTH_PID_MOD_SRC(k));
        s_pv[k][1] = ps.valuePtr(SYNTH_PID_MOD_DEST(k));
        s_pv[k][2] = ps.valuePtr(SYNTH_PID_MOD_AMOUNT(k));
    }
    s_ready = true;
    ESP_LOGI(TAG,
             "mod matrix up: %d slots (src/dest/amount), sources "
             "env2/lfo1/lfo2/vel/note/bend/wheel",
             SYNTH_MOD_SLOTS);
    return ESP_OK;
}

extern "C" void synth_mod_set_wheel(float wheel01) {
    if (wheel01 < 0.0f) wheel01 = 0.0f;
    if (wheel01 > 1.0f) wheel01 = 1.0f;
    s_wheel.store(wheel01, std::memory_order_relaxed);
}

extern "C" float synth_mod_wheel(void) {
    return s_wheel.load(std::memory_order_relaxed);
}

extern "C" void SYNTH_RENDER_IRAM synth_mod_begin_block(float bend_norm) {
    if (!s_ready) { /* registration failed at boot; no slots to resolve */
        s_nplan = 0;
        return;
    }
    const float wheel = s_wheel.load(std::memory_order_relaxed);
    int n = 0;
    for (int k = 0; k < SYNTH_MOD_SLOTS; ++k) {
        const int src = (int)s_pv[k][0]->load(std::memory_order_relaxed);
        if (src <= SYNTH_MOD_SRC_OFF || src >= SYNTH_MOD_SRC_COUNT) continue;
        const float amount = s_pv[k][2]->load(std::memory_order_relaxed);
        if (amount == 0.0f) continue;
        const int dest = (int)s_pv[k][1]->load(std::memory_order_relaxed);
        /* dest 0 = none; the matrix's own range is off-limits (no self-mod) */
        if (dest <= 0 || dest >= SYNTH_PID_MOD_BASE) continue;
        const ParamDesc* d = ParamStore::instance().describe((uint16_t)dest);
        if (d == nullptr) continue; /* e.g. a param of an inactive engine */

        PlanSlot& p = s_plan[n++];
        p.dest = (uint16_t)dest;
        p.amount = amount;
        p.is_exp = d->curve == ParamCurve::Exp && d->min > 0.0f;
        p.snap = d->type != ParamType::Float;
        p.min = d->min;
        p.max = d->max;
        p.span = d->max - d->min;
        p.log_ratio = p.is_exp ? logf(d->max / d->min) : 0.0f;
        if (src == SYNTH_MOD_SRC_BEND) {
            p.src = SYNTH_MOD_SRC_OFF;
            p.global_off = amount * bend_norm;
        } else if (src == SYNTH_MOD_SRC_WHEEL) {
            p.src = SYNTH_MOD_SRC_OFF;
            p.global_off = amount * wheel;
        } else {
            p.src = (uint8_t)src;
            p.global_off = 0.0f;
        }
    }
    s_nplan = n;
}

extern "C" float SYNTH_RENDER_IRAM synth_mod_apply(
    uint16_t pid, float base, const synth_mod_voice_src_t* s) {
    const PlanSlot* m = nullptr;
    float off = 0.0f;
    for (int i = 0; i < s_nplan; ++i) {
        const PlanSlot& p = s_plan[i];
        if (p.dest != pid) continue;
        float sv;
        switch (p.src) {
            case SYNTH_MOD_SRC_ENV2: sv = s->env2; break;
            case SYNTH_MOD_SRC_LFO1: sv = s->lfo1; break;
            case SYNTH_MOD_SRC_LFO2: sv = s->lfo2; break;
            case SYNTH_MOD_SRC_VEL:  sv = s->vel; break;
            case SYNTH_MOD_SRC_NOTE:
                sv = (s->note - 60.0f) * (1.0f / 60.0f);
                break;
            default: sv = 0.0f; break; /* bend/wheel: folded in global_off */
        }
        off += p.global_off + p.amount * sv;
        m = &p; /* slots sharing a dest share its mapping data */
    }
    if (m == nullptr) return base;

    float v = m->is_exp ? base * expf(off * m->log_ratio)
                        : base + off * m->span;
    if (v < m->min) v = m->min;
    if (v > m->max) v = m->max;
    if (m->snap) v = roundf(v);
    return v;
}
