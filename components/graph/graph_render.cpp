/*
 * osynth — modular patch graph: the render path (Session 28).
 *
 * One IRAM function walks the compiled plan once per block. See
 * graph_render.h for the three shape decisions (node-major over voices,
 * switch dispatch, ducked plan swaps); this file is their consequence.
 *
 * Two conventions run through every kernel:
 *
 *  - **An unpatched input is never an error.** Audio inputs read a shared
 *    zero row, modulation inputs fall back to the node's own parameter. A
 *    half-built patch makes sound instead of silence, which is the
 *    difference between a modular you can explore and one you have to be
 *    right about first.
 *
 *  - **Control signals reach audio consumers as a per-block ramp, not a
 *    step.** A control node produces one value per voice per block; a VCA
 *    or the output stage interpolates linearly from the previous block's
 *    value across this one. That is one add per sample, and it is what
 *    stops a 5 ms attack from arriving as ~4 audible stair steps. It is the
 *    same trick adsr_block() (S17) plays inside the fixed engines, applied
 *    across a cable instead of inside a voice.
 */
#include "graph_render.h"

#include <atomic>
#include <cmath>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "synth_config.h"
#include "synth_mod.h"
#include "synth_params.h"
#include "synth_smooth.h"
#include "synth_voice.h"

static const char* TAG = "graph_r";

namespace osynth::graph {
namespace {

constexpr size_t kStride = SYNTH_BLOCK_SIZE;
constexpr float kSr = (float)SYNTH_SAMPLE_RATE;
constexpr float kInvSr = 1.0f / (float)SYNTH_SAMPLE_RATE;
constexpr float kMaxStep = 0.49f; /* keep phase increments below Nyquist */

/* Audio buffer pool: [buffer][voice][frame]. Internal RAM, never PSRAM —
 * this is the per-sample working set and a PSRAM round trip in the inner
 * loop would cost more than the whole graph does. */
float* s_buf[kMaxBufs] = {};
float* s_zero = nullptr; /* one row of silence, shared by every unpatched
                          * audio input of every voice (read-only) */

/* Control-rate outputs, one float per voice per slot, plus the previous
 * block's value for the ramp described in the file header. 12 x 8 x 4 B x 2
 * is under 800 bytes, which is why control-rate routing is effectively
 * free and audio-rate routing is not. */
float s_ctl[kMaxNodes][SYNTH_VOICES];
float s_ctl_prev[kMaxNodes][SYNTH_VOICES];

/* Per-slot parameter smoothers (S21). Keyed by slot rather than stored in
 * the plan so that a cable edit — which rebuilds the plan but leaves every
 * parameter where the user left it — does not restart every ramp. They are
 * reset only when a slot's *kind* changes, because then the parameter at
 * that index means something else. */
dsp::Smooth s_sm[kMaxNodes][kNodeParams];
Kind s_sm_kind[kMaxNodes] = {};

/* ---- plan publication ----
 *
 * Two staging slots plus a permanently empty plan. The audio task reads
 * s_live once per block; the control task stages into whichever slot is not
 * live and hands it over through s_pending. */
Plan s_plans[2];
Plan s_empty;
std::atomic<const Plan*> s_live{&s_empty};
std::atomic<const Plan*> s_pending{nullptr};
std::atomic<uint32_t> s_render_seq{0};
std::atomic<uint16_t> s_live_cost{0};

/* Swap duck, audio-task local: full ramp in 8 blocks (~11 ms at 64/48k),
 * the same rate the voice manager's engine-switch mute uses. */
float s_gain = 1.0f;
constexpr float kGainStep = 0.125f;

inline float* buf_row(int b, int v) {
    return s_buf[b] + (size_t)v * kStride;
}

inline float pval(const Plan& pl, int slot, int p, float dflt) {
    const std::atomic<float>* a = pl.pp[slot][p];
    return (a != nullptr) ? a->load(std::memory_order_relaxed) : dflt;
}

inline float psm(const Plan& pl, int slot, int p, float dflt) {
    return dsp::smooth_lin(s_sm[slot][p], pval(pl, slot, p, dflt));
}

inline float psm_exp(const Plan& pl, int slot, int p, float dflt) {
    return dsp::smooth_exp(s_sm[slot][p], pval(pl, slot, p, dflt));
}

/* Audio input row for one voice: the real buffer, or shared silence. */
inline const float* audio_in(const PlanNode& n, int port, int v) {
    return (n.in_buf[port] >= 0) ? buf_row(n.in_buf[port], v) : s_zero;
}

/* Modulation input for one voice, as a scalar. A control source reads its
 * per-voice value; an *audio* source is coerced by taking the block's last
 * sample. Coercing rather than refusing is deliberate — see the rate note
 * in graph_compile.cpp. */
inline float mod_in(const PlanNode& n, int port, int v, size_t frames,
                    float dflt) {
    if (n.in_ctl[port] >= 0) return s_ctl[n.in_ctl[port]][v];
    if (n.in_buf[port] >= 0) return buf_row(n.in_buf[port], v)[frames - 1];
    return dflt;
}

/* Previous-block value of a modulation input, for the linear ramp. Matches
 * mod_in()'s fallbacks so an unpatched input ramps from and to the same
 * constant (i.e. does not ramp at all). */
inline float mod_in_prev(const PlanNode& n, int port, int v, size_t frames,
                         float dflt) {
    if (n.in_ctl[port] >= 0) return s_ctl_prev[n.in_ctl[port]][v];
    if (n.in_buf[port] >= 0) return buf_row(n.in_buf[port], v)[frames - 1];
    return dflt;
}

/* ---- small shared math ----
 *
 * Rational tanh (Padé): within ~0.3% over |x| < 3 and monotone beyond it,
 * for four flops and no libm call in the sample loop. */
inline float fast_tanh(float x) {
    if (x < -3.0f) return -1.0f;
    if (x > 3.0f) return 1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Triangle wavefolder: reflects at ±1 instead of clipping, so overdrive
 * adds harmonics rather than removing them. */
inline float fold(float x) {
    while (x > 1.0f || x < -1.0f) {
        if (x > 1.0f) x = 2.0f - x;
        if (x < -1.0f) x = -2.0f - x;
    }
    return x;
}

/* ---- per-kind block state ----
 *
 * Everything a node can compute once for all voices lands here, in the
 * visit to that node, before the voice loop. This is the whole point of
 * node-major ordering: at 8 voices these reads and this smoothing happen
 * once instead of eight times. */
struct OscBlk {
    dsp::OscWave wave;
    float mul, pw, fm, level;
};
struct FiltBlk {
    dsp::SvfMode mode;
    float cutoff, reso, kbd, cutamt, drive;
    bool on;
};
/* Ladder and Dual read the same shape; Vowel swaps cutoff for a morph and
 * its mod input drives that instead. */
struct VowelBlk {
    float vowel, reso, shift, drive, kbd, modamt;
    bool on;
};
struct EnvBlk {
    dsp::AdsrCoef coef;
};
struct LfoBlk {
    dsp::LfoWave wave;
    float inc, depth, rateamt;
    bool uni;
};

} // namespace

/* ---- lifecycle ---- */

esp_err_t render_init() {
    if (s_zero != nullptr) return ESP_OK; /* idempotent */
    s_zero = (float*)heap_caps_calloc(kStride, sizeof(float),
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_zero == nullptr) return ESP_ERR_NO_MEM;
    for (int i = 0; i < kMaxBufs; ++i) {
        s_buf[i] = (float*)heap_caps_calloc(kStride * SYNTH_VOICES,
                                            sizeof(float),
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (s_buf[i] == nullptr) {
            render_deinit();
            return ESP_ERR_NO_MEM;
        }
    }
    memset(s_ctl, 0, sizeof(s_ctl));
    memset(s_ctl_prev, 0, sizeof(s_ctl_prev));
    s_empty = Plan{};
    s_live.store(&s_empty, std::memory_order_release);
    s_pending.store(nullptr, std::memory_order_release);
    s_gain = 1.0f;
    ESP_LOGI(TAG, "buffers: %d x %u voices x %u frames (%u B internal)",
             kMaxBufs, (unsigned)SYNTH_VOICES, (unsigned)kStride,
             (unsigned)(kMaxBufs * kStride * SYNTH_VOICES * sizeof(float)));
    return ESP_OK;
}

void render_deinit() {
    s_live.store(&s_empty, std::memory_order_release);
    s_pending.store(nullptr, std::memory_order_release);
    for (int i = 0; i < kMaxBufs; ++i) {
        heap_caps_free(s_buf[i]);
        s_buf[i] = nullptr;
    }
    heap_caps_free(s_zero);
    s_zero = nullptr;
}

namespace {

/* Hands `staged` to the audio task and waits for it to be adopted.
 *
 * The wait cannot simply block until the audio task acts, because there are
 * two legitimate situations here and they look different: during a live
 * edit the audio task is calling render_block() every 1.33 ms and will
 * adopt within the duck ramp; during an engine bind the voice manager has
 * detached the engine, so render_block() is not being called at all and
 * waiting would hang until the timeout. The render sequence counter tells
 * them apart — if it has not moved, nobody is rendering and the control
 * task adopts the plan itself. The claim is a compare_exchange so that the
 * two paths can never both adopt. */
esp_err_t hand_over(const Plan* staged) {
    s_pending.store(staged, std::memory_order_release);
    const uint32_t start = s_render_seq.load(std::memory_order_acquire);
    const TickType_t t0 = xTaskGetTickCount();
    const TickType_t idle_after = t0 + pdMS_TO_TICKS(30);
    const TickType_t deadline = t0 + pdMS_TO_TICKS(500);

    for (;;) {
        if (s_pending.load(std::memory_order_acquire) == nullptr) return ESP_OK;
        const TickType_t now = xTaskGetTickCount();
        const bool idle = (s_render_seq.load(std::memory_order_acquire) == start &&
                           now > idle_after);
        if (idle || now > deadline) {
            const Plan* claim = staged;
            if (s_pending.compare_exchange_strong(claim, nullptr,
                                                  std::memory_order_acq_rel)) {
                s_live.store(staged, std::memory_order_release);
                s_gain = 1.0f;
                if (!idle) {
                    ESP_LOGW(TAG, "plan swap timed out, adopted directly");
                }
            }
            return ESP_OK;
        }
        vTaskDelay(1);
    }
}

} // namespace

esp_err_t publish(const Plan& p) {
    const Plan* live = s_live.load(std::memory_order_acquire);
    Plan* spare = (live == &s_plans[0]) ? &s_plans[1] : &s_plans[0];
    *spare = p;
    s_live_cost.store(p.cost, std::memory_order_relaxed);
    return hand_over(spare);
}

esp_err_t retire() {
    s_empty = Plan{};
    s_live_cost.store(0, std::memory_order_relaxed);
    return hand_over(&s_empty);
}

uint16_t live_cost() { return s_live_cost.load(std::memory_order_relaxed); }

/* ---- per-voice hooks ---- */

/* Clears one slot's state and re-seeds it. Zeroing alone is not enough: the
 * union overlaps the xorshift word, and xorshift(0) is 0 forever — a slot
 * left with a zero seed would be a silent Noise node. Distinct per slot and
 * per voice, so a Noise and an S&H in the same patch cannot correlate
 * audibly. Shared with the kind-change path in render_block() (S33) so the
 * two can never drift apart. */
void slot_reset(VoiceState& v, int slot) {
    memset(&v.n[slot], 0, sizeof(v.n[slot]));
    v.n[slot].noise.s = 0x9E3779B9u ^ (uint32_t)(slot * 2654435761u) ^
                        (uint32_t)(uintptr_t)&v;
    if (v.n[slot].noise.s == 0) v.n[slot].noise.s = 0x9E3779B9u;
}

void voice_reset(VoiceState& v) {
    v.note = 60;
    v.vel = 0.0f;
    v.gate = 0.0f;
    v.rnd = 0.0f;
    for (int i = 0; i < kMaxNodes; ++i) slot_reset(v, i);
}

void note_on(VoiceState& v, uint8_t note, float vel01, bool was_sounding) {
    const float vel = fmaxf(vel01, 1.0f / 127.0f);
    const Plan* pl = s_live.load(std::memory_order_acquire);
    v.note = note;
    v.vel = vel;
    v.gate = 1.0f;
    /* One random value per note-on — the MidiSrc "rand" source, and the
     * reason a graph can do per-note variation without an extra node. */
    dsp::Noise seed{(uint32_t)(note + 1) * 2654435761u ^ v.n[0].noise.s};
    v.rnd = dsp::noise_next(seed) * 0.5f + 0.5f;

    for (int t = 0; t < pl->n_nodes; ++t) {
        const PlanNode& n = pl->nodes[t];
        switch (n.kind) {
            case Kind::Env:
                dsp::adsr_gate_on(v.n[n.slot].adsr);
                break;
            case Kind::Lfo:
                /* Retrigger is per node: a vibrato LFO usually wants to
                 * restart with the note, a slow filter sweep usually does
                 * not, and in a patchable graph that has to be per node
                 * rather than a global policy. */
                if (!was_sounding &&
                    pval(*pl, n.slot, pidx::LFO_RETRIG, 1.0f) >= 0.5f) {
                    dsp::lfo_retrig(v.n[n.slot].lfo);
                }
                break;
            case Kind::Osc:
                if (!was_sounding) {
                    /* Stagger phases across slots: identically tuned
                     * oscillators starting at phase 0 either cancel or
                     * double, and both are surprises. */
                    v.n[n.slot].osc.phase = 0.25f * (float)(n.slot & 3);
                }
                break;
            default:
                break;
        }
    }
}

void note_off(VoiceState& v) {
    const Plan* pl = s_live.load(std::memory_order_acquire);
    v.gate = 0.0f;
    for (int t = 0; t < pl->n_nodes; ++t) {
        if (pl->nodes[t].kind == Kind::Env) {
            dsp::adsr_gate_off(v.n[pl->nodes[t].slot].adsr);
        }
    }
}

/* A graph has no single canonical amp envelope, so "still audible" is the
 * disjunction of the honest signals available: the key is down, or some
 * envelope in the patch has not finished. A patch with no envelope at all
 * therefore stops exactly at note-off — which is what a patch with no
 * envelope should do, and is visible enough that the fix is obvious. */
bool voice_busy(const VoiceState& v) {
    if (v.gate > 0.0f) return true;
    const Plan* pl = s_live.load(std::memory_order_acquire);
    for (int t = 0; t < pl->n_nodes; ++t) {
        if (pl->nodes[t].kind == Kind::Env &&
            dsp::adsr_active(v.n[pl->nodes[t].slot].adsr)) {
            return true;
        }
    }
    return false;
}

float voice_level(const VoiceState& v) {
    const Plan* pl = s_live.load(std::memory_order_acquire);
    float best = 0.0f;
    for (int t = 0; t < pl->n_nodes; ++t) {
        if (pl->nodes[t].kind != Kind::Env) continue;
        const float l = v.n[pl->nodes[t].slot].adsr.level;
        if (l > best) best = l;
    }
    /* No envelope in the patch: a held key ranks above a released one, so
     * voice stealing still prefers the note the player let go of. */
    if (best == 0.0f && v.gate > 0.0f) best = 1.0f;
    return best;
}

/* ---- the render ---- */

void SYNTH_RENDER_IRAM render_block(void* const* states,
                                    const synth_voice_frame_t* frames,
                                    size_t n_voices, float* out_l,
                                    float* out_r, size_t n) {
    /* Adopt a staged plan at the bottom of the duck; ramp back up after.
     * Reading s_pending once per block is the only synchronisation in the
     * render path. */
    const Plan* pending = s_pending.load(std::memory_order_acquire);
    float g0 = s_gain;
    if (pending != nullptr) {
        s_gain = (s_gain > kGainStep) ? s_gain - kGainStep : 0.0f;
        if (s_gain == 0.0f) {
            const Plan* claim = pending;
            if (s_pending.compare_exchange_strong(claim, nullptr,
                                                  std::memory_order_acq_rel)) {
                s_live.store(pending, std::memory_order_release);
            }
        }
    } else if (s_gain < 1.0f) {
        s_gain = (s_gain + kGainStep > 1.0f) ? 1.0f : s_gain + kGainStep;
    }
    const float g1 = s_gain;

    const Plan* pl = s_live.load(std::memory_order_acquire);
    s_render_seq.fetch_add(1, std::memory_order_release);
    if (pl->n_nodes == 0 || n_voices == 0 || (g0 == 0.0f && g1 == 0.0f)) return;

    const int nv = (int)n_voices;
    const size_t nf = n;
    const float inv_nf = 1.0f / (float)nf;
    const float blk_rate = kSr * inv_nf; /* one update per block */

    for (int t = 0; t < pl->n_nodes; ++t) {
        const PlanNode& node = pl->nodes[t];
        const int slot = node.slot;

        /* A slot that changed kind must not inherit the previous kind's
         * smoothers: parameter index 1 might have been a cutoff in hertz
         * and be a decay time in seconds now. */
        if (s_sm_kind[slot] != node.kind) {
            for (int p = 0; p < kNodeParams; ++p) s_sm[slot][p] = dsp::Smooth{};
            /* And the node state, which is a union: the incoming kind would
             * otherwise read the outgoing kind's bytes as its own. Mostly
             * that just means an odd first block, but not always — a Noise
             * node leaves a raw xorshift word where a filter expects an
             * integrator, and roughly one word in 250 is a NaN bit pattern.
             * A NaN in a recursive filter never decays: the slot would stay
             * silent until something reset it. Costs one slot per voice on a
             * kind change and nothing at all in steady state. (S33 — the
             * hazard predates it, but four filter kinds aliasing each other
             * is what made it worth closing.) */
            for (int v = 0; v < nv; ++v) {
                slot_reset(*(VoiceState*)states[v], slot);
            }
            s_sm_kind[slot] = node.kind;
        }

        switch (node.kind) {

        /* ================= audio-rate ================= */

        case Kind::Osc: {
            OscBlk b;
            b.wave = (dsp::OscWave)(int)pval(*pl, slot, pidx::OSC_WAVE, 2.0f);
            b.mul = dsp::smooth_exp(
                s_sm[slot][pidx::OSC_SEMI],
                exp2f((pval(*pl, slot, pidx::OSC_SEMI, 0.0f) +
                       pval(*pl, slot, pidx::OSC_FINE, 0.0f) * 0.01f) *
                      (1.0f / 12.0f)));
            b.pw = psm(*pl, slot, pidx::OSC_PW, 0.5f);
            b.fm = psm(*pl, slot, pidx::OSC_FM, 0.0f);
            b.level = psm(*pl, slot, pidx::OSC_LEVEL, 1.0f);

            for (int v = 0; v < nv; ++v) {
                VoiceState& vs = *(VoiceState*)states[v];
                dsp::Osc& o = vs.n[slot].osc;
                float* dst = buf_row(node.out_buf, v);

                const float semis = mod_in(node, 1, v, nf, 0.0f) * 12.0f;
                const float hz = frames[v].freq_hz * b.mul *
                                 ((semis != 0.0f) ? exp2f(semis * (1.0f / 12.0f))
                                                  : 1.0f);
                const float step = fminf(hz * kInvSr, kMaxStep);

                if (node.in_buf[0] >= 0 && b.fm != 0.0f) {
                    /* Phase modulation. The band-limiting residual is
                     * evaluated at the modulated phase, which is an
                     * approximation — as it is in every PM oscillator that
                     * is not oversampled — so a high index trades some
                     * aliasing for the sound you asked for. */
                    const float* fmv = buf_row(node.in_buf[0], v);
                    float ph = o.phase;
                    for (size_t i = 0; i < nf; ++i) {
                        float p = ph + fmv[i] * b.fm;
                        p -= floorf(p);
                        dst[i] = b.level * dsp::osc_at(b.wave, p, step, b.pw);
                        ph += step;
                        if (ph >= 1.0f) ph -= 1.0f;
                    }
                    o.phase = ph;
                } else {
                    for (size_t i = 0; i < nf; ++i) {
                        dst[i] = b.level * dsp::osc_next(o, b.wave, step, b.pw);
                    }
                }
            }
            break;
        }

        case Kind::Noise: {
            const float level = psm(*pl, slot, pidx::NOI_LEVEL, 1.0f);
            for (int v = 0; v < nv; ++v) {
                VoiceState& vs = *(VoiceState*)states[v];
                dsp::Noise& rng = vs.n[slot].noise;
                float* dst = buf_row(node.out_buf, v);
                for (size_t i = 0; i < nf; ++i) {
                    dst[i] = level * dsp::noise_next(rng);
                }
            }
            break;
        }

        /* The four filter kinds share this shape: read the block once, then
         * per voice resolve the cutoff, build coefficients, run the loop.
         * Cutoff modulation is resolved once per voice per block — the
         * coefficient build is a tanf, which is exactly why the fixed
         * engines do the same thing (S5). Per-sample cutoff would cost more
         * than the filter.
         *
         * `on` is checked outside the sample loop, where a bypass can copy
         * the input through and cost nothing. (The fixed engines fold bypass
         * into a filter type instead — they have no buffer to copy, only a
         * sample in hand.) */

        case Kind::Filter:
        case Kind::Filter24: {
            const bool wide = node.kind == Kind::Filter24;
            FiltBlk b;
            b.mode = (dsp::SvfMode)(int)pval(*pl, slot, pidx::FLT_MODE, 0.0f);
            b.cutoff = psm_exp(*pl, slot, pidx::FLT_CUTOFF, 1200.0f);
            b.reso = psm(*pl, slot, pidx::FLT_RESO, 0.15f);
            b.kbd = psm(*pl, slot, pidx::FLT_KBD, 0.5f);
            b.cutamt = psm(*pl, slot, pidx::FLT_CUTAMT, 2.5f);
            b.drive = psm(*pl, slot, pidx::FLT_DRIVE, 0.0f);
            b.on = pval(*pl, slot, pidx::FLT_ON, 1.0f) >= 0.5f;

            for (int v = 0; v < nv; ++v) {
                VoiceState& vs = *(VoiceState*)states[v];
                const float* src = audio_in(node, 0, v);
                float* dst = buf_row(node.out_buf, v);
                if (!b.on) {
                    memcpy(dst, src, nf * sizeof(float));
                    continue;
                }
                const float oct = b.cutamt * mod_in(node, 1, v, nf, 0.0f) +
                                  b.kbd * ((float)vs.note - 60.0f) * (1.0f / 12.0f);
                const float hz = b.cutoff * exp2f(oct);
                if (wide) {
                    const dsp::Svf2Coef fc =
                        dsp::svf2_coef(hz, b.reso, b.drive, kSr);
                    dsp::Svf2& f = vs.n[slot].svf2;
                    if (b.drive > 0.0f) {
                        for (size_t i = 0; i < nf; ++i) {
                            dst[i] = dsp::svf2_next_drive(f, fc, b.mode, src[i]);
                        }
                    } else {
                        for (size_t i = 0; i < nf; ++i) {
                            dst[i] = dsp::svf2_next(f, fc, b.mode, src[i]);
                        }
                    }
                } else {
                    dsp::Svf& f = vs.n[slot].svf;
                    if (b.drive > 0.0f) {
                        const dsp::SvfCoef fc =
                            dsp::svf_coef_drive(hz, b.reso, b.drive, kSr);
                        for (size_t i = 0; i < nf; ++i) {
                            dst[i] = dsp::svf_next_drive(f, fc, b.mode, src[i]);
                        }
                    } else {
                        const dsp::SvfCoef fc = dsp::svf_coef(hz, b.reso, kSr);
                        for (size_t i = 0; i < nf; ++i) {
                            dst[i] = dsp::svf_next(f, fc, b.mode, src[i]);
                        }
                    }
                }
            }
            break;
        }

        case Kind::Ladder: {
            FiltBlk b;
            b.cutoff = psm_exp(*pl, slot, pidx::LAD_CUTOFF, 1200.0f);
            b.reso = psm(*pl, slot, pidx::LAD_RESO, 0.3f);
            b.drive = psm(*pl, slot, pidx::LAD_DRIVE, 0.0f);
            b.kbd = psm(*pl, slot, pidx::LAD_KBD, 0.5f);
            b.cutamt = psm(*pl, slot, pidx::LAD_CUTAMT, 2.5f);
            b.on = pval(*pl, slot, pidx::LAD_ON, 1.0f) >= 0.5f;

            for (int v = 0; v < nv; ++v) {
                VoiceState& vs = *(VoiceState*)states[v];
                const float* src = audio_in(node, 0, v);
                float* dst = buf_row(node.out_buf, v);
                if (!b.on) {
                    memcpy(dst, src, nf * sizeof(float));
                    continue;
                }
                const float oct = b.cutamt * mod_in(node, 1, v, nf, 0.0f) +
                                  b.kbd * ((float)vs.note - 60.0f) * (1.0f / 12.0f);
                const dsp::LadderCoef fc = dsp::ladder_coef(
                    b.cutoff * exp2f(oct), b.reso, b.drive, kSr);
                dsp::Ladder& f = vs.n[slot].ladder;
                for (size_t i = 0; i < nf; ++i) {
                    dst[i] = dsp::ladder_next(f, fc, src[i]);
                }
            }
            break;
        }

        case Kind::Dual: {
            FiltBlk b;
            b.cutoff = psm_exp(*pl, slot, pidx::DUA_CUTOFF, 1200.0f);
            b.reso = psm(*pl, slot, pidx::DUA_RESO, 0.15f);
            b.drive = psm(*pl, slot, pidx::DUA_DRIVE, 0.0f);
            b.kbd = psm(*pl, slot, pidx::DUA_KBD, 0.5f);
            b.cutamt = psm(*pl, slot, pidx::DUA_CUTAMT, 2.5f);
            b.on = pval(*pl, slot, pidx::DUA_ON, 1.0f) >= 0.5f;
            const float spread = psm(*pl, slot, pidx::DUA_SPREAD, 2.0f);

            for (int v = 0; v < nv; ++v) {
                VoiceState& vs = *(VoiceState*)states[v];
                const float* src = audio_in(node, 0, v);
                float* dst = buf_row(node.out_buf, v);
                if (!b.on) {
                    memcpy(dst, src, nf * sizeof(float));
                    continue;
                }
                const float oct = b.cutamt * mod_in(node, 1, v, nf, 0.0f) +
                                  b.kbd * ((float)vs.note - 60.0f) * (1.0f / 12.0f);
                const dsp::Svf2Coef fc = dsp::dual_coef(
                    b.cutoff * exp2f(oct), b.reso, spread, b.drive, kSr);
                dsp::Svf2& f = vs.n[slot].svf2;
                if (b.drive > 0.0f) {
                    for (size_t i = 0; i < nf; ++i) {
                        dst[i] = dsp::dual_next_drive(f, fc, src[i]);
                    }
                } else {
                    for (size_t i = 0; i < nf; ++i) {
                        dst[i] = dsp::dual_next(f, fc, src[i]);
                    }
                }
            }
            break;
        }

        case Kind::Vowel: {
            VowelBlk b;
            b.vowel = psm(*pl, slot, pidx::VOW_VOWEL, 0.0f);
            b.reso = psm(*pl, slot, pidx::VOW_RESO, 0.5f);
            b.shift = psm_exp(*pl, slot, pidx::VOW_SHIFT, 1000.0f);
            b.drive = psm(*pl, slot, pidx::VOW_DRIVE, 0.0f);
            b.kbd = psm(*pl, slot, pidx::VOW_KBD, 0.0f);
            b.modamt = psm(*pl, slot, pidx::VOW_MODAMT, 1.0f);
            b.on = pval(*pl, slot, pidx::VOW_ON, 1.0f) >= 0.5f;

            for (int v = 0; v < nv; ++v) {
                VoiceState& vs = *(VoiceState*)states[v];
                const float* src = audio_in(node, 0, v);
                float* dst = buf_row(node.out_buf, v);
                if (!b.on) {
                    memcpy(dst, src, nf * sizeof(float));
                    continue;
                }
                /* The cable moves the morph, not the cutoff — vowel_coef()
                 * clamps it, so an over-driven cable parks on "u" instead of
                 * wrapping round to "a". */
                const float morph = b.vowel + b.modamt * mod_in(node, 1, v, nf, 0.0f);
                const float oct = b.kbd * ((float)vs.note - 60.0f) * (1.0f / 12.0f);
                const dsp::VowelCoef fc = dsp::vowel_coef(
                    morph, b.shift * exp2f(oct), b.reso, b.drive, kSr);
                dsp::Vowel& f = vs.n[slot].vowel;
                if (b.drive > 0.0f) {
                    for (size_t i = 0; i < nf; ++i) {
                        dst[i] = dsp::vowel_next_drive(f, fc, src[i]);
                    }
                } else {
                    for (size_t i = 0; i < nf; ++i) {
                        dst[i] = dsp::vowel_next(f, fc, src[i]);
                    }
                }
            }
            break;
        }

        case Kind::Vca: {
            const float gain = psm(*pl, slot, pidx::VCA_GAIN, 1.0f);
            const float depth = psm(*pl, slot, pidx::VCA_DEPTH, 1.0f);
            for (int v = 0; v < nv; ++v) {
                const float* src = audio_in(node, 0, v);
                float* dst = buf_row(node.out_buf, v);
                if (node.in_buf[1] >= 0) {
                    /* audio-rate gain: ring-modulation territory, so no
                     * ramp — the modulator is already per-sample */
                    const float* m = buf_row(node.in_buf[1], v);
                    for (size_t i = 0; i < nf; ++i) {
                        float g = gain * (1.0f - depth + depth * m[i]);
                        if (g < 0.0f) g = 0.0f;
                        dst[i] = src[i] * g;
                    }
                } else {
                    /* control gain: ramp across the block (file header) */
                    const float c1 = mod_in(node, 1, v, nf, 1.0f);
                    const float c0 = mod_in_prev(node, 1, v, nf, 1.0f);
                    float ga = gain * (1.0f - depth + depth * c0);
                    const float gb = gain * (1.0f - depth + depth * c1);
                    if (ga < 0.0f) ga = 0.0f;
                    const float dg = ((gb < 0.0f ? 0.0f : gb) - ga) * inv_nf;
                    for (size_t i = 0; i < nf; ++i) {
                        ga += dg;
                        dst[i] = src[i] * ga;
                    }
                }
            }
            break;
        }

        case Kind::Mix: {
            const float l0 = psm(*pl, slot, pidx::MIX_L0, 1.0f);
            const float l1 = psm(*pl, slot, pidx::MIX_L1, 1.0f);
            const float l2 = psm(*pl, slot, pidx::MIX_L2, 1.0f);
            const float l3 = psm(*pl, slot, pidx::MIX_L3, 1.0f);
            for (int v = 0; v < nv; ++v) {
                const float* a = audio_in(node, 0, v);
                const float* b2 = audio_in(node, 1, v);
                const float* c2 = audio_in(node, 2, v);
                const float* d2 = audio_in(node, 3, v);
                float* dst = buf_row(node.out_buf, v);
                for (size_t i = 0; i < nf; ++i) {
                    dst[i] = l0 * a[i] + l1 * b2[i] + l2 * c2[i] + l3 * d2[i];
                }
            }
            break;
        }

        case Kind::Shaper: {
            const int mode = (int)pval(*pl, slot, pidx::SHP_MODE, 0.0f);
            const float drive = psm_exp(*pl, slot, pidx::SHP_DRIVE, 1.0f);
            const float amt = psm(*pl, slot, pidx::SHP_AMT, 0.0f);
            for (int v = 0; v < nv; ++v) {
                const float* src = audio_in(node, 0, v);
                float* dst = buf_row(node.out_buf, v);
                const float d = drive * (1.0f + amt * mod_in(node, 1, v, nf, 0.0f));
                const float dd = (d < 0.01f) ? 0.01f : d;
                /* Compensate so raising drive changes timbre, not level —
                 * otherwise the control doubles as a volume knob and the
                 * patch has to be re-gained every time it moves. */
                const float comp = 1.0f / fast_tanh(dd);
                switch (mode) {
                    case 1:
                        for (size_t i = 0; i < nf; ++i) dst[i] = fold(src[i] * dd);
                        break;
                    case 2:
                        for (size_t i = 0; i < nf; ++i) {
                            dst[i] = dsp::soft_clip(src[i] * dd);
                        }
                        break;
                    default:
                        for (size_t i = 0; i < nf; ++i) {
                            dst[i] = fast_tanh(src[i] * dd) * comp;
                        }
                        break;
                }
            }
            break;
        }

        case Kind::RingMod: {
            const float amount = psm(*pl, slot, pidx::RNG_AMOUNT, 1.0f);
            for (int v = 0; v < nv; ++v) {
                const float* a = audio_in(node, 0, v);
                float* dst = buf_row(node.out_buf, v);
                if (node.in_buf[1] >= 0) {
                    const float* b2 = buf_row(node.in_buf[1], v);
                    for (size_t i = 0; i < nf; ++i) {
                        dst[i] = a[i] * (1.0f - amount) + a[i] * b2[i] * amount;
                    }
                } else {
                    /* nothing in the second jack: a ring modulator with one
                     * input is a wire */
                    const float mb = mod_in(node, 1, v, nf, 1.0f);
                    const float g = (1.0f - amount) + mb * amount;
                    for (size_t i = 0; i < nf; ++i) dst[i] = a[i] * g;
                }
            }
            break;
        }

        /* ================= control-rate ================= */

        case Kind::Env: {
            EnvBlk b;
            b.coef = dsp::adsr_coef(pval(*pl, slot, pidx::ENV_A, 0.005f),
                                    pval(*pl, slot, pidx::ENV_D, 0.25f),
                                    pval(*pl, slot, pidx::ENV_S, 0.7f),
                                    pval(*pl, slot, pidx::ENV_R, 0.25f),
                                    blk_rate);
            for (int v = 0; v < nv; ++v) {
                VoiceState& vs = *(VoiceState*)states[v];
                s_ctl_prev[slot][v] = s_ctl[slot][v];
                s_ctl[slot][v] = dsp::adsr_next(vs.n[slot].adsr, b.coef);
            }
            break;
        }

        case Kind::Lfo: {
            LfoBlk b;
            b.wave = (dsp::LfoWave)(int)pval(*pl, slot, pidx::LFO_WAVE, 0.0f);
            b.depth = psm(*pl, slot, pidx::LFO_DEPTH, 1.0f);
            b.rateamt = psm(*pl, slot, pidx::LFO_RATEAMT, 0.0f);
            b.uni = pval(*pl, slot, pidx::LFO_UNI, 0.0f) >= 0.5f;
            const float rate = psm_exp(*pl, slot, pidx::LFO_RATE, 5.0f);
            b.inc = rate * (float)nf * kInvSr;
            for (int v = 0; v < nv; ++v) {
                VoiceState& vs = *(VoiceState*)states[v];
                float inc = b.inc;
                if (b.rateamt != 0.0f) {
                    inc *= exp2f(b.rateamt * mod_in(node, 0, v, nf, 0.0f));
                }
                float x = dsp::lfo_next(vs.n[slot].lfo, b.wave, inc) * b.depth;
                if (b.uni) x = x * 0.5f + 0.5f;
                s_ctl_prev[slot][v] = s_ctl[slot][v];
                s_ctl[slot][v] = x;
            }
            break;
        }

        case Kind::SampleHold: {
            const float rate = psm_exp(*pl, slot, pidx::SAH_RATE, 8.0f);
            const float inc = rate * (float)nf * kInvSr;
            for (int v = 0; v < nv; ++v) {
                VoiceState& vs = *(VoiceState*)states[v];
                auto& sh = vs.n[slot].sah;
                sh.phase += inc;
                if (sh.phase >= 1.0f) {
                    sh.phase -= (float)(int)sh.phase;
                    /* Unpatched: sample internal noise — the classic random
                     * stepped modulator, which is what an S&H with an empty
                     * input jack does on hardware too. */
                    sh.held = (node.in_ctl[0] >= 0 || node.in_buf[0] >= 0)
                                  ? mod_in(node, 0, v, nf, 0.0f)
                                  : dsp::noise_next(sh.rng);
                }
                s_ctl_prev[slot][v] = s_ctl[slot][v];
                s_ctl[slot][v] = sh.held;
            }
            break;
        }

        case Kind::ModMap: {
            const float scale = psm(*pl, slot, pidx::MM_SCALE, 1.0f);
            const float offset = psm(*pl, slot, pidx::MM_OFFSET, 0.0f);
            const int quant = (int)pval(*pl, slot, pidx::MM_QUANT, 0.0f);
            for (int v = 0; v < nv; ++v) {
                float x = mod_in(node, 0, v, nf, 0.0f) * scale + offset;
                if (quant > 0) {
                    const float q = (float)quant;
                    x = floorf(x * q + 0.5f) / q;
                }
                s_ctl_prev[slot][v] = s_ctl[slot][v];
                s_ctl[slot][v] = x;
            }
            break;
        }

        case Kind::MidiSrc: {
            const MidiSource src =
                (MidiSource)(int)pval(*pl, slot, pidx::MS_SRC, 0.0f);
            const float bend = voice_manager_pitch_bend();
            const float wheel = synth_mod_wheel();
            for (int v = 0; v < nv; ++v) {
                const VoiceState& vs = *(const VoiceState*)states[v];
                float x;
                switch (src) {
                    case MidiSource::Note:
                        x = ((float)vs.note - 60.0f) * (1.0f / 60.0f);
                        break;
                    case MidiSource::Gate:  x = vs.gate; break;
                    case MidiSource::Bend:  x = bend; break;
                    case MidiSource::Wheel: x = wheel; break;
                    case MidiSource::Rand:  x = vs.rnd; break;
                    default:                x = vs.vel; break;
                }
                s_ctl_prev[slot][v] = s_ctl[slot][v];
                s_ctl[slot][v] = x;
            }
            break;
        }

        /* ================= sink ================= */

        case Kind::Out: {
            const float level = psm(*pl, slot, pidx::OUT_LEVEL, 1.0f);
            const float pan = psm(*pl, slot, pidx::OUT_PAN, 0.0f);
            /* Balance-law pan, matching the voice manager's unison spread so
             * a centred node is unity on both channels. */
            const float pl_g = (pan > 0.0f) ? 1.0f - pan : 1.0f;
            const float pr_g = (pan < 0.0f) ? 1.0f + pan : 1.0f;
            const float dgs = (g1 - g0) * inv_nf; /* the swap duck */

            for (int v = 0; v < nv; ++v) {
                const float* src = audio_in(node, 0, v);
                const float a1 = mod_in(node, 1, v, nf, 1.0f);
                const float a0 = mod_in_prev(node, 1, v, nf, 1.0f);
                float amp = a0 * level;
                const float damp = (a1 * level - amp) * inv_nf;
                const float gl = frames[v].gain_l * pl_g;
                const float gr = frames[v].gain_r * pr_g;
                float sw = g0;
                for (size_t i = 0; i < nf; ++i) {
                    amp += damp;
                    sw += dgs;
                    const float y = src[i] * amp * sw;
                    out_l[i] += y * gl;
                    out_r[i] += y * gr;
                }
            }
            break;
        }

        default:
            break;
        }
    }
}

} // namespace osynth::graph
