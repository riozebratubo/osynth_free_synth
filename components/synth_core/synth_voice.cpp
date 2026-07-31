/*
 * osynth — voice manager (Session 4; engine render path since Session 5).
 *
 * Threading: producers (USB MIDI on core 0, serial MIDI, later seq/arp/BLE)
 * push events into a fixed ring serialized among themselves by a short
 * critical section; the audio task (core 1) is the only consumer and never
 * takes a lock — it reads the head with acquire semantics and advances the
 * tail. Pitch bend is a single atomic float (only the latest value matters).
 * The sustain pedal travels through the ring so it stays ordered against
 * note events.
 *
 * Session 5: the placeholder sine is gone — voices render through the bound
 * engine (synth_engine.h). Per-voice gain stays 1/SYNTH_VOICES so a
 * full-poly chord cannot clip; unison spreads notes across voices with a
 * balance-law pan (centre voices keep full gain, matching the S4 mono sum).
 * Glide is a per-voice one-pole in semitone space. Engine-common params
 * (0x01xx) are registered here and read via cached pointers at block rate.
 */
#include "synth_voice.h"

#include <atomic>
#include <cmath>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "synth_config.h"
#include "synth_dsp.h"
#include "synth_mod.h"
#include "synth_params.h"

static const char* TAG = "voices";

namespace {

/* ---- note events: control plane -> audio plane ---- */

enum class EvType : uint8_t {
    NoteOn, NoteOff, AllNotesOff, AllSoundOff, SustainOn, SustainOff
};

struct Event {
    EvType type;
    uint8_t note;
    uint8_t velocity;
};

constexpr uint32_t kQueueLen = 64; /* power of two */

Event s_events[kQueueLen];
std::atomic<uint32_t> s_evt_head{0}; /* next write slot (producers) */
std::atomic<uint32_t> s_evt_tail{0}; /* next read slot (audio task) */
portMUX_TYPE s_evt_mux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<uint32_t> s_evt_dropped{0};

void push_event(EvType type, uint8_t note, uint8_t velocity) {
    bool dropped = false;
    portENTER_CRITICAL(&s_evt_mux);
    const uint32_t head = s_evt_head.load(std::memory_order_relaxed);
    if (head - s_evt_tail.load(std::memory_order_acquire) >= kQueueLen) {
        dropped = true;
    } else {
        s_events[head % kQueueLen] = {type, note, velocity};
        s_evt_head.store(head + 1, std::memory_order_release);
    }
    portEXIT_CRITICAL(&s_evt_mux);
    if (dropped) {
        ESP_LOGW(TAG, "event queue full, dropped (%u total)",
                 s_evt_dropped.fetch_add(1, std::memory_order_relaxed) + 1);
    }
}

/* ---- engine binding + voice pool ---- */

std::atomic<const synth_engine_t*> s_engine{nullptr};
uint8_t* s_pool = nullptr;

constexpr int kMaxUnison = 4;

struct Voice {
    bool active = false;
    bool gate = false;
    bool held = false;         /* note-off deferred by the sustain pedal */
    uint8_t note = 0;
    uint32_t order = 0;        /* note-on sequence number, drives stealing */
    float target_semis = 0.0f; /* glide destination (the note) */
    float cur_semis = 0.0f;    /* glide position */
    float uni_cents = 0.0f;    /* unison detune offset */
    float pan = 0.0f;          /* -1..1, unison stereo spread */
    void* estate = nullptr;    /* engine per-voice state */
};

Voice s_voices[SYNTH_VOICES];
uint32_t s_order = 0;
bool s_sustain = false;    /* audio-task view of CC 64 */
float s_last_semis = 0.0f; /* glide starts from the previous note */
bool s_have_last = false;
std::atomic<float> s_bend_norm{0.0f};
std::atomic<uint32_t> s_active{0};

/* ---- engine-switch support (S6) ----
 * s_render_seq ticks at the end of every voice_manager_render() call;
 * the detach handshake waits for two ticks so any block that loaded the old
 * engine pointer has finished before the voice pool is freed. The mute is a
 * post-sum gain the audio task slews toward s_mute_target. */
std::atomic<uint32_t> s_render_seq{0};
std::atomic<float> s_mute_target{1.0f};
std::atomic<float> s_mute_now{1.0f};
float s_mute_gain = 1.0f;               /* audio-task local */
constexpr float kMuteStep = 0.125f;     /* per block: full ramp in 8 (~11 ms) */

/* Blocks rendered with no engine bound (audio-task local). While a switch is
 * in flight the ring is *held*, not discarded, so a note event that lands in
 * that window is delivered to the new engine instead of vanishing — a note-off
 * arriving there used to be dropped silently, which left the key that sent it
 * with no way to stop its note. The hold is bounded because "no engine bound"
 * is not always transient: the switch's restore path can fail outright and
 * leave the synth with none, and in that state events must go back to being
 * discarded rather than filling the ring and warning once per push. Two
 * seconds clears the switch protocol's own ceiling (250 ms mute + 500 ms
 * detach + init) several times over. */
uint32_t s_unbound_blocks = 0;
constexpr uint32_t kUnboundHoldBlocks =
    (uint32_t)(2 * SYNTH_SAMPLE_RATE / SYNTH_BLOCK_SIZE);

constexpr float kVoiceGain = 1.0f / SYNTH_VOICES;

/* Cached engine-common (0x01xx) value pointers, set in init. */
const std::atomic<float>* s_p_glide = nullptr;
const std::atomic<float>* s_p_bend_range = nullptr;
const std::atomic<float>* s_p_unison = nullptr;
const std::atomic<float>* s_p_detune = nullptr;
const std::atomic<float>* s_p_spread = nullptr;

inline float pget(const std::atomic<float>* p) {
    return p ? p->load(std::memory_order_relaxed) : 0.0f;
}

/* ---- event handling (audio task only) ---- */

void do_note_off(const synth_engine_t* eng, Voice& v) {
    eng->note_off(v.estate);
    v.gate = false;
    v.held = false;
}

Voice* pick_voice(const synth_engine_t* eng, Voice* const* taken, int ntaken) {
    auto is_taken = [&](const Voice* v) {
        for (int i = 0; i < ntaken; ++i) {
            if (taken[i] == v) return true;
        }
        return false;
    };
    Voice* idle = nullptr;
    Voice* releasing = nullptr;
    float rel_level = 0.0f;
    Voice* oldest = nullptr;
    for (auto& v : s_voices) {
        if (is_taken(&v)) continue;
        if (!v.active) {
            if (idle == nullptr) idle = &v;
            continue;
        }
        if (!v.gate) {
            const float lv = eng->level(v.estate);
            if (releasing == nullptr || lv < rel_level) {
                releasing = &v;
                rel_level = lv;
            }
        }
        if (oldest == nullptr || v.order < oldest->order) oldest = &v;
    }
    if (idle != nullptr) return idle;
    if (releasing != nullptr) return releasing; /* steal the quietest tail */
    return oldest;
}

void ev_note_on(const synth_engine_t* eng, uint8_t note, uint8_t velocity) {
    int n = (int)pget(s_p_unison);
    if (n < 1) n = 1;
    if (n > kMaxUnison) n = kMaxUnison;
    if (n > SYNTH_VOICES) n = SYNTH_VOICES;

    /* Same note already sounding: retrigger its voices in place (no
     * stacking), then allocate/steal until the unison stack is full. */
    Voice* picked[kMaxUnison];
    int np = 0;
    for (auto& v : s_voices) {
        if (np < n && v.active && v.note == note) picked[np++] = &v;
    }
    while (np < n) {
        picked[np] = pick_voice(eng, picked, np);
        ++np;
    }

    const bool glide_on = pget(s_p_glide) > 0.001f;
    const float detune = pget(s_p_detune);
    const float spread = pget(s_p_spread);
    const float vel01 = (float)velocity * (1.0f / 127.0f);

    for (int k = 0; k < np; ++k) {
        Voice& v = *picked[k];
        /* symmetric unison layout: -1 .. +1 across the stack, 0 for n = 1 */
        const float pos =
            (n > 1) ? 2.0f * (float)k / (float)(n - 1) - 1.0f : 0.0f;
        const bool was_sounding = v.active;
        if (!was_sounding) {
            v.cur_semis = (glide_on && s_have_last) ? s_last_semis : (float)note;
        } /* live voices (retrigger/steal) glide from where they are */
        v.target_semis = (float)note;
        v.note = note;
        v.order = ++s_order;
        v.uni_cents = detune * pos;
        v.pan = spread * pos;
        v.active = true;
        v.gate = true;
        v.held = false;
        eng->note_on(v.estate, note, vel01, was_sounding);
    }
    s_last_semis = (float)note;
    s_have_last = true;
}

void drain_events(const synth_engine_t* eng) {
    uint32_t tail = s_evt_tail.load(std::memory_order_relaxed);
    const uint32_t head = s_evt_head.load(std::memory_order_acquire);
    while (tail != head) {
        const Event& e = s_events[tail % kQueueLen];
        switch (e.type) {
            case EvType::NoteOn:
                ev_note_on(eng, e.note, e.velocity);
                break;
            case EvType::NoteOff:
                for (auto& v : s_voices) {
                    if (v.active && v.gate && v.note == e.note) {
                        if (s_sustain) {
                            v.held = true;
                        } else {
                            do_note_off(eng, v);
                        }
                    }
                }
                break;
            case EvType::AllNotesOff:
                /* per MIDI: acts like note-off on every key — notes under
                 * the sustain pedal keep ringing until the pedal lifts */
                for (auto& v : s_voices) {
                    if (v.active && v.gate) {
                        if (s_sustain) {
                            v.held = true;
                        } else {
                            do_note_off(eng, v);
                        }
                    }
                }
                break;
            case EvType::AllSoundOff:
                for (auto& v : s_voices) {
                    if (v.estate != nullptr) eng->voice_reset(v.estate);
                    v.active = false;
                    v.gate = false;
                    v.held = false;
                }
                break;
            case EvType::SustainOn:
                s_sustain = true;
                break;
            case EvType::SustainOff:
                s_sustain = false;
                for (auto& v : s_voices) {
                    if (v.active && v.held) do_note_off(eng, v);
                }
                break;
        }
        ++tail;
    }
    s_evt_tail.store(tail, std::memory_order_release);
}

} // namespace

extern "C" esp_err_t voice_manager_init(void) {
    using namespace osynth;
    static const ParamDesc kCommon[] = {
        {PID_COMMON_GLIDE, "common.glide", ParamType::Float, ParamCurve::Linear,
         0.0f, 2.0f, 0.0f, nullptr, 0}, /* seconds; 0 = off */
        {PID_COMMON_BEND_RANGE, "common.bend.range", ParamType::Int,
         ParamCurve::Linear, 0.0f, 24.0f, 2.0f, nullptr, 0}, /* semitones */
        {PID_COMMON_UNISON, "common.unison", ParamType::Int, ParamCurve::Linear,
         1.0f, (float)kMaxUnison, 1.0f, nullptr, 0}, /* voices per note */
        {PID_COMMON_UNI_DETUNE, "common.uni.detune", ParamType::Float,
         ParamCurve::Linear, 0.0f, 50.0f, 12.0f, nullptr, 0}, /* cents */
        {PID_COMMON_UNI_SPREAD, "common.uni.spread", ParamType::Float,
         ParamCurve::Linear, 0.0f, 1.0f, 0.5f, nullptr, 0},
    };
    ParamStore& ps = ParamStore::instance();
    ps.add(kCommon, sizeof(kCommon) / sizeof(kCommon[0]));
    s_p_glide = ps.valuePtr(PID_COMMON_GLIDE);
    s_p_bend_range = ps.valuePtr(PID_COMMON_BEND_RANGE);
    s_p_unison = ps.valuePtr(PID_COMMON_UNISON);
    s_p_detune = ps.valuePtr(PID_COMMON_UNI_DETUNE);
    s_p_spread = ps.valuePtr(PID_COMMON_UNI_SPREAD);
    ESP_LOGI(TAG, "voice manager up: %d voices, engine render path",
             SYNTH_VOICES);
    return ESP_OK;
}

extern "C" esp_err_t voice_manager_set_engine(const synth_engine_t* engine) {
    /* Only while no engine is bound: at boot before the audio task starts,
     * or after voice_manager_detach_engine() (see synth_voice.h). */
    s_engine.store(nullptr, std::memory_order_release);
    if (s_pool != nullptr) {
        heap_caps_free(s_pool);
        s_pool = nullptr;
    }
    if (engine == nullptr) return ESP_OK;

    s_pool = (uint8_t*)heap_caps_calloc(SYNTH_VOICES, engine->voice_size,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_pool == nullptr) {
        ESP_LOGE(TAG, "no memory for %d x %u B voice state", SYNTH_VOICES,
                 (unsigned)engine->voice_size);
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < SYNTH_VOICES; ++i) {
        Voice& v = s_voices[i];
        v = Voice{};
        v.estate = s_pool + (size_t)i * engine->voice_size;
        engine->voice_reset(v.estate);
    }
    s_sustain = false;
    s_have_last = false;
    s_order = 0;
    s_engine.store(engine, std::memory_order_release);
    ESP_LOGI(TAG, "engine bound: %s (caps 0x%02x, %u B/voice)", engine->name,
             (unsigned)engine->caps, (unsigned)engine->voice_size);
    return ESP_OK;
}

extern "C" esp_err_t voice_manager_detach_engine(void) {
    const synth_engine_t* old =
        s_engine.exchange(nullptr, std::memory_order_acq_rel);
    if (s_pool == nullptr) return ESP_OK; /* nothing bound */

    /* Two render boundaries: the first may belong to a block that loaded
     * the old engine pointer before the exchange; the second block started
     * after that, so it saw nullptr and never touched the pool. */
    const uint32_t start = s_render_seq.load(std::memory_order_acquire);
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(500);
    while (s_render_seq.load(std::memory_order_acquire) - start < 2) {
        if (xTaskGetTickCount() > deadline) {
            ESP_LOGE(TAG, "detach: audio task unresponsive, engine kept");
            s_engine.store(old, std::memory_order_release);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }

    heap_caps_free(s_pool);
    s_pool = nullptr;
    for (auto& v : s_voices) v = Voice{};
    ESP_LOGI(TAG, "engine detached");
    return ESP_OK;
}

extern "C" void voice_manager_set_muted(bool muted) {
    s_mute_target.store(muted ? 0.0f : 1.0f, std::memory_order_relaxed);
}

extern "C" bool voice_manager_muted(void) {
    return s_mute_now.load(std::memory_order_relaxed) == 0.0f;
}

extern "C" void voice_manager_note_on(uint8_t note, uint8_t velocity) {
    push_event(EvType::NoteOn, note, velocity);
}

extern "C" void voice_manager_note_off(uint8_t note) {
    push_event(EvType::NoteOff, note, 0);
}

extern "C" void voice_manager_all_notes_off(void) {
    push_event(EvType::AllNotesOff, 0, 0);
}

extern "C" void voice_manager_all_sound_off(void) {
    push_event(EvType::AllSoundOff, 0, 0);
}

extern "C" void voice_manager_set_sustain(bool down) {
    push_event(down ? EvType::SustainOn : EvType::SustainOff, 0, 0);
}

extern "C" void voice_manager_set_pitch_bend(float bend_norm) {
    s_bend_norm.store(bend_norm, std::memory_order_relaxed);
}

extern "C" float voice_manager_pitch_bend(void) {
    return s_bend_norm.load(std::memory_order_relaxed);
}

extern "C" size_t voice_manager_active_voices(void) {
    return s_active.load(std::memory_order_relaxed);
}

namespace {

/* Post-sum mute: slew the gain one kMuteStep toward the target per block,
 * ramping linearly inside the block so the step never clicks. */
void SYNTH_RENDER_IRAM apply_mute(float* __restrict__ out_l,
                                  float* __restrict__ out_r, size_t frames) {
    const float target = s_mute_target.load(std::memory_order_relaxed);
    if (s_mute_gain == target) {
        if (target == 0.0f) { /* settled silent: keep the block muted */
            for (size_t i = 0; i < frames; ++i) out_l[i] = out_r[i] = 0.0f;
        }
        return;
    }
    float g1 = s_mute_gain;
    const float d = target - g1;
    g1 += (d > kMuteStep) ? kMuteStep : (d < -kMuteStep) ? -kMuteStep : d;
    const float dg = (g1 - s_mute_gain) / (float)frames;
    float g = s_mute_gain;
    for (size_t i = 0; i < frames; ++i) {
        g += dg;
        out_l[i] *= g;
        out_r[i] *= g;
    }
    s_mute_gain = g1;
    s_mute_now.store(g1, std::memory_order_relaxed);
}

} // namespace

extern "C" void SYNTH_RENDER_IRAM voice_manager_render(float* out_l,
                                                       float* out_r,
                                                       size_t frames,
                                                       void* ctx) {
    (void)ctx;
    const synth_engine_t* eng = s_engine.load(std::memory_order_acquire);
    if (eng == nullptr) {
        /* No engine bound. Stay silent, and hold the ring for the new engine
         * for as long as this could still be a switch in flight; past that,
         * discard so it can't fill (see kUnboundHoldBlocks). */
        if (s_unbound_blocks < kUnboundHoldBlocks) {
            ++s_unbound_blocks;
        } else {
            s_evt_tail.store(s_evt_head.load(std::memory_order_acquire),
                             std::memory_order_release);
        }
        s_active.store(0, std::memory_order_relaxed);
        apply_mute(out_l, out_r, frames);
        s_render_seq.fetch_add(1, std::memory_order_release);
        return;
    }
    s_unbound_blocks = 0;

    drain_events(eng);

    const float bend =
        s_bend_norm.load(std::memory_order_relaxed) * pget(s_p_bend_range);

    /* one-pole glide in semitone space; same coefficient for every voice */
    const float glide_s = pget(s_p_glide);
    const float kblk =
        (glide_s > 0.001f)
            ? 1.0f - expf(-(float)frames / (glide_s * (float)SYNTH_SAMPLE_RATE))
            : 1.0f; /* coefficient 1: snap to the target */

    bool any_voice = false;
    for (const auto& v : s_voices) {
        if (v.active) {
            any_voice = true;
            break;
        }
    }

    /* begin_block() runs every block, not lazily before the first sounding
     * voice: since S21 the engines smooth their parameters in there
     * (synth_smooth.h), and a smoother that only advanced while a note was
     * held would still be catching up when the *next* note starts — you would
     * hear the cutoff you just dialled in slide into place after the attack.
     * The cost of an idle call is a few dozen float ops plus the envelope
     * coefficient builders, well under 1% of a block. The mod-matrix plan
     * stays gated: only a rendering voice ever consumes it. */
    if (any_voice && (eng->caps & SYNTH_CAP_MODMATRIX)) {
        /* build the mod-matrix plan (raw bend, before bend range) */
        synth_mod_begin_block(s_bend_norm.load(std::memory_order_relaxed));
    }
    eng->begin_block(frames);

    /* Per-voice pitch/gain is computed the same way for both render
     * contracts; what differs is only whether the engine is entered once per
     * voice or once per block (synth_engine.h render_block, S28). */
    void* batch_states[SYNTH_VOICES];
    synth_voice_frame_t batch_frames[SYNTH_VOICES];
    size_t nbatch = 0;
    const bool batched = (eng->render_block != nullptr);

    for (auto& v : s_voices) {
        if (!v.active) continue;
        v.cur_semis += (v.target_semis - v.cur_semis) * kblk;

        synth_voice_frame_t f;
        f.freq_hz = osynth::dsp::midi_to_freq(v.cur_semis + bend +
                                              v.uni_cents * 0.01f);
        /* balance-law pan: centre voices keep full gain on both channels */
        f.gain_l = kVoiceGain * (v.pan > 0.0f ? 1.0f - v.pan : 1.0f);
        f.gain_r = kVoiceGain * (v.pan < 0.0f ? 1.0f + v.pan : 1.0f);

        if (batched) {
            batch_states[nbatch] = v.estate;
            batch_frames[nbatch] = f;
            ++nbatch;
        } else {
            eng->render(v.estate, &f, out_l, out_r, frames);
        }
    }
    /* Called even with no sounding voice: a batched engine keeps per-block
     * bookkeeping in here (the graph's plan-swap handshake), and skipping it
     * on idle blocks would stall an edit until its timeout — the same reason
     * begin_block() runs unconditionally. */
    if (batched) {
        eng->render_block(batch_states, batch_frames, nbatch, out_l, out_r,
                          frames);
    }

    uint32_t active = 0;
    for (auto& v : s_voices) {
        if (!v.active) continue;
        if (eng->busy(v.estate)) {
            ++active;
        } else {
            v.active = false;
        }
    }
    s_active.store(active, std::memory_order_relaxed);
    apply_mute(out_l, out_r, frames);

    /* Bound the voice bus before it reaches the FX (S21). The FX delay lines
     * are int16 and used to hard-clamp whatever arrived above ±1, so an
     * overloaded chord clicked its way into the chorus/delay/reverb tails as
     * well as the output. Nominal full polyphony sums to 1.0 by the
     * kVoiceGain staging, so this is inaudible on anything that was already
     * in range — it engages on resonance boost, cranked mix levels and
     * matrix-driven overshoot. The master gets its own pass in audio_io.
     * Skipped when no voice rendered: the bus is then untouched silence. */
    if (any_voice) {
        for (size_t i = 0; i < frames; ++i) {
            out_l[i] = osynth::dsp::soft_clip(out_l[i]);
            out_r[i] = osynth::dsp::soft_clip(out_r[i]);
        }
    }

    s_render_seq.fetch_add(1, std::memory_order_release);
}
