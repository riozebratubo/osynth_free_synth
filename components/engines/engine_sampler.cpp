/*
 * osynth — sampler engine (Session 44). Contract in engine_sampler.h.
 *
 * Per voice: one pad of the bound kit, resampled, through an amplitude ADSR.
 * That is the entire signal path, and the file is short because of it — the
 * interesting parts are not the DSP but the two things a sampler has that a
 * synthesiser does not.
 *
 * The first is that its oscillator can be taken away. A pad is a pointer into
 * a block the drum bus owns, and that block can be freed while a voice is
 * sounding on it: someone recorded over the pad, erased it, or hit undo. So a
 * voice latches drums_kit_generation() next to the pointer and compares it at
 * the top of every render; a mismatch means the pad it holds is no longer the
 * pad that is there, and the voice ends. It ends with a two-millisecond fade
 * rather than a hard stop, because "ends" here happens under a player's hands
 * and a click is not an acceptable way to report a kit edit.
 *
 * The second is that the sample runs out. An envelope release that outlives
 * the audio is the normal case for a one-shot pad, so `busy()` has to be true
 * only while there is *both* an envelope and something left to read — a voice
 * that has run off the end of its sample is finished no matter what the
 * envelope thinks, and holding it would spend polyphony on silence.
 */
#include "engine_sampler.h"

#include <atomic>
#include <cmath>
#include <cstring>

#include "esp_log.h"

#include "drums.h"
#include "synth_config.h"
#include "synth_dsp.h"
#include "synth_params.h"
#include "synth_smooth.h"

static const char* TAG = "eng_smp";

namespace dsp = osynth::dsp;
using osynth::ParamCurve;
using osynth::ParamDesc;
using osynth::ParamStore;
using osynth::ParamType;

namespace {

constexpr float kSr = (float)SYNTH_SAMPLE_RATE;
/* A voice dropped because its pad was republished fades over this long. Same
 * value and same reasoning as the drum bus's steal declick. */
constexpr float kDropMs = 2.0f;
/* Ceiling on the playback rate, so a pitched pad two octaves up does not walk
 * the buffer faster than the interpolator can say anything useful about. */
constexpr float kMaxRate = 16.0f;

enum : int { MODE_PADS = 0, MODE_PITCHED = 1 };
const char* const kModeNames[] = {"pads", "pitched"};

enum PIdx {
    P_MODE, P_PAD, P_ROOT, P_START, P_VELDEPTH, P_LEVEL, P_SPREAD,
    P_ENV_A, P_ENV_D, P_ENV_S, P_ENV_R,
    P_COUNT
};

const std::atomic<float>* s_p[P_COUNT] = {};

inline float pv(int i) {
    return s_p[i] != nullptr ? s_p[i]->load(std::memory_order_relaxed) : 0.0f;
}

struct SmpVoice {
    const uint8_t* data = nullptr;
    uint32_t frames = 0;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    float pos = 0.0f;
    float step = 0.0f;
    /* Playback bounds, one of which is always out of reach — the same trick
     * drums.cpp uses so the inner loop never branches on direction. */
    float lo = -1.0f;
    float hi = 1.0f;
    float gl = 0.0f, gr = 0.0f;
    /* The pad's stored rate against the render rate, kept apart from `step`
     * so pitched mode can rebuild the step from a live freq_hz every block
     * without having to divide the old one back out. */
    float src_rate = 1.0f;
    float drop = 0.0f;  /* fade-out multiplier while a dropped voice rings out */
    float dropc = 0.0f; /* its per-sample coefficient */
    dsp::Adsr env;
    uint32_t gen = 0;
    uint8_t format = 0;
    bool ran_out = false; /* the sample ended; only the release is left */
    float vel = 0.0f;
};

/* Block-shared state, rebuilt once per block by smp_begin_block(). */
struct BlockCache {
    int mode = MODE_PADS;
    int pad = 0;
    float root_hz = 261.6256f;
    float start = 0.0f;
    float veldepth = 1.0f;
    float level = 1.0f;
    float spread = 1.0f;
    dsp::AdsrCoef amp;
    uint32_t gen = 0;
    const int16_t* ulaw = nullptr;
    float dropc = 0.0f;
};
BlockCache s_bc;

struct Smoothers {
    dsp::Smooth level, spread, start;
};
Smoothers s_sm;

/* One decoded sample, either storage format. Mirrors drums.cpp's sample_at();
 * the table itself is shared rather than rebuilt — see drums_ulaw_table(). */
inline float SYNTH_RENDER_IRAM sample_at(const uint8_t* data, uint8_t fmt,
                                         const int16_t* ulaw, uint32_t i) {
    if (fmt == DRUM_FMT_PCM16) {
        int16_t v;
        memcpy(&v, data + (size_t)i * 2, 2);
        return (float)v * (1.0f / 32768.0f);
    }
    return (float)ulaw[data[i]] * (1.0f / 32768.0f);
}

/* Aims a voice at a pad. Audio task, from note_on.
 *
 * Not IRAM, and neither is its only caller — see smp_note_on(). */
void arm_voice(SmpVoice& v, const drums_pad_t& p, float rate,
                                 float src_rate, float extra_start,
                                 float spread) {
    v.data = p.data;
    v.src_rate = src_rate;
    v.frames = p.frames;
    v.format = p.format;
    v.gen = drums_kit_generation();
    v.ran_out = false;

    v.loop_start = p.loop_start;
    v.loop_end = p.loop_end;
    if (p.play_mode == DRUM_PLAY_LOOP && p.loop_end == 0) {
        v.loop_start = 0;
        v.loop_end = p.frames;
    } else if (p.play_mode == DRUM_PLAY_ONESHOT) {
        /* A one-shot ignores loop points, exactly as it does on the drum bus,
         * so the two surfaces cannot disagree about what a pad does. */
        v.loop_end = 0;
    }

    float ofs = p.start_ofs + extra_start;
    if (ofs < 0.0f) ofs = 0.0f;
    if (ofs > 0.999f) ofs = 0.999f;
    const float last = (float)(p.frames > 0 ? p.frames - 1 : 0);
    if (p.reverse != 0) {
        v.step = -rate;
        v.pos = fminf(last - ofs * last, last - 0.001f);
        v.lo = 0.0f;
        v.hi = (float)p.frames + 1.0f;
    } else {
        v.step = rate;
        v.pos = ofs * last;
        v.lo = -1.0f;
        v.hi = last;
    }

    /* Equal-power pan from the pad's own placement, scaled by smp.spread — a
     * kit panned for a drum bus is often wider than you want under a melodic
     * part, and this is one control instead of sixteen. */
    const float theta = (p.pan * spread * 0.5f + 0.5f) * 1.57079633f;
    v.gl = cosf(theta) * p.gain;
    v.gr = sinf(theta) * p.gain;
    v.drop = 0.0f;
}

/* ---- vtable entries ---- */

esp_err_t smp_init(void) {
    static const ParamDesc kParams[P_COUNT] = {
        {SMPE_PID_MODE, "smp.mode", ParamType::Enum, ParamCurve::Linear,
         0.0f, 1.0f, 0.0f /* pads */, kModeNames, 2},
        {SMPE_PID_PAD, "smp.pad", ParamType::Int, ParamCurve::Linear,
         0.0f, (float)(DRUM_SLOTS - 1), 0.0f, nullptr, 0},
        {SMPE_PID_ROOT, "smp.root", ParamType::Int, ParamCurve::Linear,
         0.0f, 127.0f, 60.0f, nullptr, 0},
        {SMPE_PID_START, "smp.start", ParamType::Float, ParamCurve::Linear,
         0.0f, 0.999f, 0.0f, nullptr, 0},
        {SMPE_PID_VELDEPTH, "smp.veldepth", ParamType::Float,
         ParamCurve::Linear, 0.0f, 1.0f, 1.0f, nullptr, 0},
        {SMPE_PID_LEVEL, "smp.level", ParamType::Float, ParamCurve::Linear,
         0.0f, 2.0f, 1.0f, nullptr, 0},
        {SMPE_PID_SPREAD, "smp.spread", ParamType::Float, ParamCurve::Linear,
         0.0f, 1.0f, 1.0f, nullptr, 0},
        /* A sampler's default envelope has to be "play the sample": instant
         * attack, full sustain, and a release short enough to feel like a key
         * lift without cutting a one-shot's tail — which it cannot do anyway,
         * since a one-shot that has run out is already finished. */
        {SMPE_PID_ENV1_ATTACK, "env1.attack", ParamType::Float, ParamCurve::Exp,
         0.001f, 10.0f, 0.001f, nullptr, 0},
        {SMPE_PID_ENV1_DECAY, "env1.decay", ParamType::Float, ParamCurve::Exp,
         0.001f, 10.0f, 0.5f, nullptr, 0},
        {SMPE_PID_ENV1_SUSTAIN, "env1.sustain", ParamType::Float,
         ParamCurve::Linear, 0.0f, 1.0f, 1.0f, nullptr, 0},
        {SMPE_PID_ENV1_RELEASE, "env1.release", ParamType::Float,
         ParamCurve::Exp, 0.001f, 10.0f, 0.08f, nullptr, 0},
    };

    ParamStore& ps = ParamStore::instance();
    const size_t added = ps.add(kParams, P_COUNT);
    if (added != P_COUNT) {
        ESP_LOGE(TAG, "registered %u/%u params", (unsigned)added,
                 (unsigned)P_COUNT);
        return ESP_FAIL;
    }
    for (size_t i = 0; i < P_COUNT; ++i) s_p[i] = ps.valuePtr(kParams[i].id);
    s_sm = Smoothers{};
    s_bc.ulaw = drums_ulaw_table();
    ESP_LOGI(TAG, "sampler engine up: %u params, kit '%s' (%d pads)",
             (unsigned)P_COUNT, drums_kit_name(), drums_slot_count());
    return ESP_OK;
}

void smp_deinit(void) {
    ParamStore::instance().removeRange(osynth::PID_ENGINE_BASE,
                                       osynth::PID_FX_BASE);
}

void SYNTH_RENDER_IRAM smp_begin_block(size_t frames) {
    BlockCache& b = s_bc;
    b.mode = (int)pv(P_MODE);
    b.pad = (int)pv(P_PAD);
    b.root_hz = 440.0f * exp2f((pv(P_ROOT) - 69.0f) * (1.0f / 12.0f));
    b.start = dsp::smooth_lin(s_sm.start, pv(P_START));
    b.veldepth = pv(P_VELDEPTH);
    b.level = dsp::smooth_lin(s_sm.level, pv(P_LEVEL));
    b.spread = dsp::smooth_lin(s_sm.spread, pv(P_SPREAD));
    b.amp = dsp::adsr_coef_block(pv(P_ENV_A), pv(P_ENV_D), pv(P_ENV_S),
                                 pv(P_ENV_R), kSr, (uint32_t)frames);
    b.gen = drums_kit_generation();
    if (b.ulaw == nullptr) b.ulaw = drums_ulaw_table();
    b.dropc = expf(-1.0f / (kDropMs * 1e-3f * kSr));
}

void smp_voice_reset(void* vs) { *(SmpVoice*)vs = SmpVoice{}; }

/* Note events are deliberately *not* IRAM, unlike the two functions below
 * them that run every block.
 *
 * SYNTH_RENDER_IRAM exists to keep flash-cache misses out of the audio
 * deadline, and the cost of a miss is paid per call: smp_render() runs 750
 * times a second per sounding voice and smp_begin_block() once per block, so
 * both earn their place. A note-on happens a few times a second at most, on a
 * path that already pays for the voice manager's own allocation and stealing.
 * Together these two are 818 bytes of the P4's sram_low, which was the region
 * that ran out when this engine was added — the cheapest 818 bytes in the
 * build to give back, and the reason the render loop kept its own. */
void smp_note_on(void* vs, uint8_t note, float vel01, bool was_sounding) {
    SmpVoice& v = *(SmpVoice*)vs;
    const float vel = fmaxf(vel01, 1.0f / 127.0f);
    const BlockCache& b = s_bc;

    int slot;
    float rate;
    if (b.mode == MODE_PITCHED) {
        slot = b.pad;
        /* The voice manager has already folded glide, bend and unison detune
         * into freq_hz, but note_on does not see a frame — and it must not
         * guess, because the first block's render will apply the real one. So
         * the rate is set from the note here and corrected per block below. */
        const float hz = 440.0f * exp2f(((float)note - 69.0f) * (1.0f / 12.0f));
        rate = hz / b.root_hz;
    } else {
        /* Follow the kit's own note map first, so a pad sits under the same
         * key here as it does for the MIDI router and the sequencer. The
         * chromatic-from-C2 fallback is the layout the WAV loader assigns to
         * a folder of samples, which is what an unlabelled recorded kit has. */
        slot = drums_slot_for_note(note);
        if (slot < 0) {
            const int chromatic = (int)note - 36;
            slot = (chromatic >= 0 && chromatic < DRUM_SLOTS) ? chromatic : -1;
        }
        rate = 1.0f;
    }

    drums_pad_t p;
    if (slot < 0 || !drums_pad_get(slot, &p)) {
        /* No pad under this key. Silence rather than a fallback sound: an
         * empty pad on a kit being built is the normal state, and filling it
         * with something else would teach the player the wrong layout. */
        v.data = nullptr;
        v.frames = 0;
        v.ran_out = true;
        dsp::adsr_gate_on(v.env);
        v.vel = vel;
        return;
    }

    /* Stored rate against the render rate is the resampling; the pitch ratio
     * rides on top, exactly as it does on the drum bus. */
    const float src_rate = (float)p.rate / kSr;
    rate *= src_rate;
    if (rate > kMaxRate) rate = kMaxRate;

    if (was_sounding && v.env.level > 0.0f && v.vel > 0.0f) {
        /* Retrigger or steal: rescale so vel * env stays continuous, the same
         * correction the other engines make. */
        v.env.level = fminf(1.0f, v.env.level * v.vel / vel);
    }
    arm_voice(v, p, rate, src_rate, b.start, b.spread);
    v.vel = vel;
    dsp::adsr_gate_on(v.env);
}

void smp_note_off(void* vs) {
    dsp::adsr_gate_off(((SmpVoice*)vs)->env);
}

void SYNTH_RENDER_IRAM smp_render(void* vs, const synth_voice_frame_t* f,
                                  float* out_l, float* out_r, size_t frames) {
    SmpVoice& v = *(SmpVoice*)vs;
    const BlockCache& b = s_bc;

    /* The pad was republished under this voice — recorded over, erased, or
     * undone. The pointer may already be pointing at freed memory by the time
     * the *next* block runs (drums_slot_replace waits exactly two render
     * boundaries), so it has to stop being read now; the fade below is the
     * only thing that still touches the buffer, and it is a captured value
     * rather than a read. */
    if (SYNTH_UNLIKELY(v.data != nullptr && v.gen != b.gen)) {
        const uint32_t i0 = (uint32_t)v.pos;
        float tail = 0.0f;
        if (i0 + 1 < v.frames) {
            tail = sample_at(v.data, v.format, b.ulaw, i0) * v.env.level;
        }
        v.drop = tail;
        v.dropc = b.dropc;
        v.data = nullptr;
        v.frames = 0;
        v.ran_out = true;
    }

    /* Whatever is left of a dropped voice. Two multiplies a sample and only
     * for the handful of blocks it takes to reach the 16-bit floor. */
    if (SYNTH_UNLIKELY(v.drop != 0.0f)) {
        float d = v.drop;
        const float c = v.dropc;
        for (size_t i = 0; i < frames; ++i) {
            out_l[i] += d * v.gl;
            out_r[i] += d * v.gr;
            d *= c;
        }
        v.drop = (fabsf(d) < 1.0f / 4096.0f) ? 0.0f : d;
    }

    const dsp::AdsrRamp ar = dsp::adsr_block(v.env, b.amp, (uint32_t)frames);
    if (v.data == nullptr || v.frames < 2) return;
    if (SYNTH_UNLIKELY(dsp::adsr_ramp_silent(ar))) return;

    /* Pitched mode tracks the voice frame, so bend and glide are live rather
     * than latched at note_on. Pads mode ignores it by construction: a pad is
     * a pad, and pitch-bending a hi-hat because the wheel moved is not what
     * anyone means by "the keyboard acts as the pads". */
    float step = v.step;
    if (b.mode == MODE_PITCHED) {
        const float mag =
            fminf((f->freq_hz / b.root_hz) * v.src_rate, kMaxRate);
        step = (v.step < 0.0f) ? -mag : mag;
    }

    const uint8_t* data = v.data;
    const uint8_t fmt = v.format;
    const int16_t* ulaw = b.ulaw;
    const float last = (float)(v.frames - 1);
    const uint32_t loop_end = v.loop_end;
    const uint32_t loop_start = v.loop_start;
    const float lo = v.lo;
    const float hi = v.hi;
    float pos = v.pos;
    const float amp = v.vel * (1.0f - b.veldepth) + v.vel * v.vel * b.veldepth;
    const float gl = f->gain_l * v.gl * amp * b.level;
    const float gr = f->gain_r * v.gr * amp * b.level;

    float a = ar.base;
    for (size_t i = 0; i < frames; ++i) {
        a += ar.step;
        if (SYNTH_UNLIKELY(pos < lo || pos >= hi)) {
            if (loop_end != 0) {
                const float span = (float)(loop_end - loop_start);
                if (span <= 1.0f) {
                    v.ran_out = true;
                    break;
                }
                pos = (step >= 0.0f)
                          ? (float)loop_start + fmodf(pos - (float)loop_start,
                                                      span)
                          : (float)loop_end - fmodf((float)loop_end - pos, span);
                if (pos < 0.0f || pos >= last) {
                    v.ran_out = true;
                    break;
                }
            } else {
                v.ran_out = true;
                break;
            }
        }
        const uint32_t i0 = (uint32_t)pos;
        const float frac = pos - (float)i0;
        const float s0 = sample_at(data, fmt, ulaw, i0);
        const float s1 = sample_at(data, fmt, ulaw, i0 + 1);
        const float y = (s0 + (s1 - s0) * frac) * a;
        out_l[i] += y * gl;
        out_r[i] += y * gr;
        pos += step;
    }
    v.pos = pos;
    if (v.ran_out) {
        v.data = nullptr;
        v.frames = 0;
    }
}

/* A voice is audible while the envelope is running *and* there is still
 * sample under it, or while a dropped voice's tail is decaying. Reporting a
 * ran-out one-shot as busy would hold a voice for the length of its release
 * doing nothing, which at eight voices is a third of the polyphony on a fast
 * part. */
bool smp_busy(const void* vs) {
    const SmpVoice& v = *(const SmpVoice*)vs;
    if (v.drop != 0.0f) return true;
    if (v.data == nullptr) return false;
    return dsp::adsr_active(v.env);
}

float smp_level(const void* vs) {
    const SmpVoice& v = *(const SmpVoice*)vs;
    return v.data != nullptr ? v.env.level : 0.0f;
}

} // namespace

extern "C" const synth_engine_t g_engine_sampler = {
    "sampler",
    /* No caps: no filter, no second envelope, no LFOs, no matrix — see the
     * header on why, and what to add first if the IRAM budget ever allows it.
     * Declaring none also means the voice manager allocates none of that
     * per-voice state, so a sampler voice is the smallest in the synth. */
    0,
    sizeof(SmpVoice),
    smp_init,
    smp_deinit,
    smp_begin_block,
    smp_voice_reset,
    smp_note_on,
    smp_note_off,
    smp_render,
    smp_busy,
    smp_level,
    nullptr, /* render_block (S28): fixed engines render per voice */
};
