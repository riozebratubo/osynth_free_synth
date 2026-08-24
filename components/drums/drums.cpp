/*
 * osynth — drum / sample bus (Session 22). Contract in drums.h.
 *
 * A pool of one-shot sample voices rendered into an internal stereo scratch,
 * then split across the FX bus by `drums.send`. Everything the per-sample
 * loop needs is precomputed: mu-law decodes through a 256-entry LUT, the
 * per-slot tune/decay/pan derivations are refreshed once per block and only
 * when their parameter actually moved, and a voice's gains are latched at
 * trigger time. What is left in the inner loop is two table reads, a lerp,
 * a multiply-accumulate per channel and a decay multiply.
 *
 * Kit swapping is the same problem as an engine switch (S6), with one extra
 * twist: the audio task holds raw pointers into the kit's sample data, and a
 * *voice* keeps its copy for the whole of its decay rather than re-reading it
 * each block. So freeing a kit under it would be a use-after-free that two
 * render boundaries alone do not cover. Solution — silence the voices, publish
 * the new kit, wait for two render boundaries (the first block may still have
 * loaded the old pointer, the second provably saw the new one), then free;
 * and the silencing itself triggers on the kit pointer changing, so the first
 * block to see the new kit drops every voice still holding the old one.
 *
 * Triggers arrive from control tasks (sequencer, MIDI, BLE, the audition
 * parameter) through a lock-free ring the audio task drains at block start:
 * producers serialise with a short critical section, the audio task never
 * locks. `delay_frames` lets the sequencer place a hit anywhere inside a
 * block, which is what makes sub-tick micro-timing audible.
 */
#include "drums.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "drum_kit.h"
#include "synth_config.h"
#include "synth_params.h"
#include "synth_smooth.h"

static const char* TAG = "drums";

using osynth::ParamCurve;
using osynth::ParamDesc;
using osynth::ParamOrigin;
using osynth::ParamStore;
using osynth::ParamType;
using osynth::dsp::Smooth;
using osynth::dsp::smooth_lin;

namespace {

/* Voices are cheap here (no filter, no envelope generator — a one-pole decay
 * multiply), so the count is set by how many drums can plausibly overlap: a
 * flam plus a ringing crash plus a ride plus hats. */
constexpr int kVoices = 8;
constexpr int kTrigRing = 32;
/* A choke does not cut dead — that clicks. It ramps out over this long. */
constexpr float kChokeMs = 2.5f;
/* Nor does a steal, or a kit swap. Those cannot ramp the voice — something
 * else is taking it over this instant — so they leave a decaying copy of its
 * last output behind instead. A time constant, not a length: at 1.5 ms the
 * tail is 29 dB down after 5 ms and past the 16-bit floor after about 12,
 * which is short enough never to read as a second sound and long enough to
 * have no edge of its own. */
constexpr float kDeclickMs = 1.5f;
/* Below this the decay envelope has nothing left to say; free the voice. */
constexpr float kSilence = 1.0f / 4096.0f;
constexpr int kMaxKits = 9; /* factory + 8 from the SD card */

constexpr float kSampleRate = (float)SYNTH_SAMPLE_RATE;

/* ---- mu-law decode table (G.711) ---- */
int16_t s_ulaw[256];

void build_ulaw_table() {
    for (int i = 0; i < 256; ++i) {
        const int u = ~i & 0xFF;
        const int exponent = (u >> 4) & 0x07;
        const int mantissa = u & 0x0F;
        int mag = (((mantissa << 3) + 0x84) << exponent) - 0x84;
        s_ulaw[i] = (int16_t)((u & 0x80) ? -mag : mag);
    }
}

/* ---- parameters ---- */

/* Slot parameter names are built once into static storage: ParamDesc holds
 * the pointer, so a stack buffer or a std::string would dangle. */
char s_slot_names[DRUM_SLOTS * 4][16];
ParamDesc s_descs[DRUM_SLOTS * 4 + 7];

const std::atomic<float>* s_p_level = nullptr;
const std::atomic<float>* s_p_send = nullptr;
const std::atomic<float>* s_p_choke = nullptr;
const std::atomic<float>* s_p_midich = nullptr;
const std::atomic<float>* s_p_slot[DRUM_SLOTS][4];

/* ---- per-slot derived state (audio task, refreshed at block start) ---- */
struct SlotDerived {
    float level = 1.0f;   /* raw param */
    float pan = 0.0f;     /* raw param */
    float tune = 0.0f;    /* raw param, semitones */
    float decay = 1.0f;   /* raw param */
    float gain_l = 0.7f;  /* derived from level*kit gain and pan */
    float gain_r = 0.7f;
    float rate_mul = 1.0f;  /* derived from tune */
    float decay_coef = 1.0f; /* derived from decay; 1.0 = natural length */
    bool primed = false;
};
SlotDerived s_slot[DRUM_SLOTS];

/* ---- voices (audio task only) ---- */
struct Voice {
    const uint8_t* data = nullptr;
    uint32_t frames = 0;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    float pos = 0.0f;
    float step = 1.0f;
    float gain_l = 0.0f;
    float gain_r = 0.0f;
    float env = 0.0f;
    float env_coef = 1.0f;
    uint32_t delay = 0; /* frames still to wait before the first sample */
    /* Declick tail, held across the voice being taken away from whatever was
     * playing on it. See steal_declick(). Zero when there is nothing to
     * decay, which is almost always. */
    float fade_l = 0.0f, fade_r = 0.0f;
    uint8_t format = DRUM_FMT_ULAW;
    uint8_t slot = 0;
    uint8_t choke = 0;
    bool active = false;
};
Voice s_voice[kVoices];

/* One-pole coefficient for the steal/kit-swap declick, ~1.5 ms. Filled once
 * in drums_init() rather than derived per steal: this is a constant, and the
 * audio task is the wrong place to reach into flash for expf() to say so. */
float s_declick_coef = 0.0f;

/* ---- trigger ring (producers: any control task) ---- */
struct Trig {
    uint8_t slot;
    uint8_t vel;
    uint16_t delay;
};
Trig s_ring[kTrigRing];
std::atomic<uint32_t> s_ring_head{0}; /* written by producers */
std::atomic<uint32_t> s_ring_tail{0}; /* written by the audio task */
portMUX_TYPE s_ring_lock = portMUX_INITIALIZER_UNLOCKED;

/* ---- per-block hit tap (S34), for the FX bus sidechain key ----
 *
 * Deliberately *not* fed from drums_trigger(): a queued hit is not a sounding
 * hit. These are written by start_voice(), so a trigger dropped by a full
 * ring, aimed at an empty slot, or arriving while a kit swap is in flight
 * never keys the ducker — and the velocity recorded is the one the voice
 * actually plays at.
 *
 * Plain statics, no atomics: the only producer (start_voice, from
 * drums_pre_fx) and the only consumer (fx_process) both run on the audio
 * task, in that order, inside one render callback. Cleared at the top of
 * every drums_pre_fx() so a stale hit cannot key a second block. */
uint8_t s_hit_vel[DRUM_SLOTS];
uint16_t s_hit_delay[DRUM_SLOTS];

/* ---- kits ---- */
drum_kit_t s_kits[2];              /* double-buffered for the swap protocol */
std::atomic<drum_kit_t*> s_kit{nullptr}; /* what the audio task plays */
int s_kit_back = 1;                /* index of the buffer not in use */
std::atomic<uint32_t> s_render_seq{0};
std::atomic<bool> s_kill_voices{false};
/* The kit the audio task last rendered with — audio task only, never read by
 * a control task. A voice latches raw sample pointers at trigger time and
 * holds them for its whole decay, so "silence the voices" has to key off the
 * pointer the block actually loaded and not off a flag some earlier block
 * happened to consume. See the comparison in drums_pre_fx(). */
const drum_kit_t* s_last_kit = nullptr;
char s_kit_names[kMaxKits][DRUM_KIT_NAME_MAX];
int s_kit_count = 1;
int s_kit_current = 0;
std::atomic<int> s_active_voices{0};

/* ---- metronome click (count-in) ----
 * A decaying sine, not a sample: the count-in has to work before a kit is
 * loaded, on a build with no kit at all, and while a kit is being swapped.
 * Two atomics are the whole handshake — control tasks arm it, the audio task
 * consumes the arm at the top of a block. */
constexpr float kClickHz = 1000.0f;      /* beat */
constexpr float kClickAccentHz = 1500.0f; /* beat 1 of the bar */
constexpr float kClickDecayMs = 45.0f;
std::atomic<int> s_click_pending{0}; /* 0 none, 1 beat, 2 accent */
const std::atomic<float>* s_p_click = nullptr;
float s_click_phase = 0.0f;
float s_click_step = 0.0f;
float s_click_env = 0.0f;
float s_click_coef = 0.0f;

/* Scratch for the drum bus, split around the FX bus by pre/post. */
float s_dl[SYNTH_BLOCK_SIZE];
float s_dr[SYNTH_BLOCK_SIZE];
size_t s_scratch_frames = 0;
float s_send_now = 0.0f;
Smooth s_sm_level, s_sm_send;

inline float pv(const std::atomic<float>* p) {
    return p != nullptr ? p->load(std::memory_order_relaxed) : 0.0f;
}

/* One decoded sample, whichever storage format the slot uses. */
inline float SYNTH_RENDER_IRAM sample_at(const uint8_t* data, uint8_t fmt,
                                         uint32_t i) {
    if (fmt == DRUM_FMT_PCM16) {
        int16_t v;
        memcpy(&v, data + (size_t)i * 2, 2);
        return (float)v * (1.0f / 32768.0f);
    }
    return (float)s_ulaw[data[i]] * (1.0f / 32768.0f);
}

/* Refresh the per-slot derivations. Each is guarded on its raw parameter so
 * the common case (nothing moved) is four float compares per slot. */
void SYNTH_RENDER_IRAM refresh_slots(const drum_kit_t* kit) {
    for (int i = 0; i < DRUM_SLOTS; ++i) {
        SlotDerived& d = s_slot[i];
        const float level = pv(s_p_slot[i][0]);
        const float pan = pv(s_p_slot[i][1]);
        const float tune = pv(s_p_slot[i][2]);
        const float decay = pv(s_p_slot[i][3]);

        if (!d.primed || level != d.level || pan != d.pan) {
            d.level = level;
            d.pan = pan;
            const float kit_gain =
                (kit != nullptr && i < kit->slot_count) ? kit->slots[i].gain
                                                        : 1.0f;
            const float g = level * kit_gain;
            /* Equal-power pan: constant perceived loudness across the sweep. */
            const float theta = (pan * 0.5f + 0.5f) * 1.57079633f;
            d.gain_l = g * cosf(theta);
            d.gain_r = g * sinf(theta);
        }
        if (!d.primed || tune != d.tune) {
            d.tune = tune;
            d.rate_mul = powf(2.0f, tune * (1.0f / 12.0f));
        }
        if (!d.primed || decay != d.decay) {
            d.decay = decay;
            if (decay >= 0.995f) {
                d.decay_coef = 1.0f; /* natural: let the sample ring out */
            } else {
                /* 10 ms (fully clipped) .. ~3 s, geometric so the knob feels
                 * even across its travel. */
                const float t = 0.01f * powf(300.0f, decay);
                d.decay_coef = expf(-1.0f / (t * kSampleRate));
            }
        }
        d.primed = true;
    }
}

/* Take over a sounding voice without cutting it dead.
 *
 * alloc_voice() steals the quietest voice when all eight are busy, and
 * start_voice() then overwrites pos, data and env — so whatever that voice
 * was emitting went to zero in one sample. Stealing the quietest limits the
 * damage; it does not remove it, and with a crash, a ride and a pair of hats
 * all ringing, "quietest" can still be well above audibility. The choke path
 * ten lines below already refuses to do this ("a choke does not cut dead —
 * that clicks") and rings its victim out over kChokeMs; a steal had no
 * equivalent.
 *
 * The tail is the voice's last output value decaying to zero with a kDeclickMs
 * time constant — so it is inaudible within about 5 ms and the pass below
 * drops it a few blocks later. That is the standard declick and it is all
 * this needs to be: what makes the step audible is the discontinuity, not the
 * missing sample content. Kept on the Voice being taken over rather than in a
 * pool of its own, because there is exactly one tail per steal and the slot
 * is right there.
 *
 * The same treatment covers the kit swap in drums_pre_fx(), which silences
 * every voice at once because their sample pointers are about to be freed:
 * this captures a *value*, not a pointer, so the tail outlives the kit. */
void SYNTH_RENDER_IRAM steal_declick(Voice& v) {
    if (!v.active || v.data == nullptr || v.frames == 0) return;
    /* Still waiting out its delay: it has emitted nothing, so there is no
     * discontinuity to cover. */
    if (v.delay > 0) return;
    const uint32_t i0 = (uint32_t)v.pos;
    if (i0 >= v.frames - 1) return; /* ran off the end; already silent */
    const float frac = v.pos - (float)i0;
    const float a = sample_at(v.data, v.format, i0);
    const float b = sample_at(v.data, v.format, i0 + 1);
    const float s = (a + (b - a) * frac) * v.env;
    /* Accumulated, not assigned: a voice stolen twice inside one block would
     * otherwise drop the first tail and reintroduce the step it was covering. */
    v.fade_l += s * v.gain_l;
    v.fade_r += s * v.gain_r;
}

int alloc_voice() {
    int best = -1;
    float quietest = 1e30f;
    for (int v = 0; v < kVoices; ++v) {
        if (!s_voice[v].active) return v;
        /* Steal whatever is contributing least right now. */
        const float loud =
            s_voice[v].env * (s_voice[v].gain_l + s_voice[v].gain_r);
        if (loud < quietest) {
            quietest = loud;
            best = v;
        }
    }
    return best;
}

void start_voice(const drum_kit_t* kit, int slot, int vel, uint32_t delay) {
    if (kit == nullptr || slot < 0 || slot >= kit->slot_count) return;
    const drum_sample_t& s = kit->slots[slot];
    if (s.data == nullptr || s.frames == 0) return;
    const SlotDerived& d = s_slot[slot];

    if (s.choke_group != 0 && pv(s_p_choke) >= 0.5f) {
        const float coef = expf(-1.0f / (kChokeMs * 1e-3f * kSampleRate));
        for (int v = 0; v < kVoices; ++v) {
            if (s_voice[v].active && s_voice[v].choke == s.choke_group) {
                s_voice[v].env_coef = coef; /* ramp out, never a hard cut */
            }
        }
    }

    const int idx = alloc_voice();
    if (idx < 0) return;
    Voice& v = s_voice[idx];
    /* Before anything below overwrites it: if this slot was sounding, it was
     * stolen, and the step that leaves has to be covered. Costs nothing in
     * the common case — alloc_voice() returns an inactive voice whenever one
     * is free, and steal_declick() returns immediately on those. */
    steal_declick(v);
    const float amp = (float)vel * (1.0f / 127.0f);
    v.data = s.data;
    v.frames = s.frames;
    v.loop_start = s.loop_start;
    v.loop_end = s.loop_end;
    v.format = s.format;
    v.pos = 0.0f;
    /* Stored rate vs the engine's rate is the whole resampling story for a
     * one-shot; `tune` rides on top of it. */
    v.step = ((float)s.rate / kSampleRate) * d.rate_mul;
    v.gain_l = d.gain_l * amp;
    v.gain_r = d.gain_r * amp;
    v.env = 1.0f;
    v.env_coef = d.decay_coef;
    v.delay = delay;
    v.slot = (uint8_t)slot;
    v.choke = s.choke_group;
    v.active = true;

    /* Publish the hit for this block's sidechain key. Several hits on one
     * slot in one block keep the loudest: a ducker should follow the strongest
     * onset, and 1.33 ms apart they are one event to the ear anyway. */
    if (slot < DRUM_SLOTS && (uint8_t)vel > s_hit_vel[slot]) {
        s_hit_vel[slot] = (uint8_t)vel;
        s_hit_delay[slot] = (uint16_t)delay;
    }
}

void SYNTH_RENDER_IRAM render_voices(const drum_kit_t* kit, size_t frames) {
    memset(s_dl, 0, frames * sizeof(float));
    memset(s_dr, 0, frames * sizeof(float));
    (void)kit;
    int live = 0;

    /* Declick tails from stolen voices, ahead of the voices themselves — a
     * slot can be both fading out from what it was and playing what took it
     * over, and the two simply sum. One compare per voice per block when
     * nothing is fading, which is the ordinary case; the decay itself never
     * enters the sample loop below. */
    for (int i = 0; i < kVoices; ++i) {
        Voice& v = s_voice[i];
        if (v.fade_l == 0.0f && v.fade_r == 0.0f) continue;
        float fl = v.fade_l, fr = v.fade_r;
        for (size_t n = 0; n < frames; ++n) {
            s_dl[n] += fl;
            s_dr[n] += fr;
            fl *= s_declick_coef;
            fr *= s_declick_coef;
        }
        /* Under the 16-bit floor: stop rather than decay forever, so the
         * compare above goes back to being the whole cost. */
        if (fabsf(fl) < kSilence && fabsf(fr) < kSilence) {
            fl = 0.0f;
            fr = 0.0f;
        }
        v.fade_l = fl;
        v.fade_r = fr;
    }

    for (int i = 0; i < kVoices; ++i) {
        Voice& v = s_voice[i];
        if (!v.active) continue;

        size_t start = 0;
        if (v.delay > 0) {
            if (v.delay >= frames) { /* still waiting: nothing this block */
                v.delay -= (uint32_t)frames;
                ++live;
                continue;
            }
            start = v.delay;
            v.delay = 0;
        }

        const uint8_t* data = v.data;
        const uint8_t fmt = v.format;
        const uint32_t last = v.frames - 1;
        const uint32_t loop_end = v.loop_end;
        float pos = v.pos;
        const float step = v.step;
        float env = v.env;
        const float coef = v.env_coef;
        const float gl = v.gain_l;
        const float gr = v.gain_r;
        bool done = false;

        for (size_t n = start; n < frames; ++n) {
            uint32_t i0 = (uint32_t)pos;
            if (SYNTH_UNLIKELY(i0 >= last)) {
                if (loop_end != 0) {
                    /* Wrap into the loop and keep the fractional phase, so a
                     * looped sample does not gain a click at every pass. */
                    const float span = (float)(loop_end - v.loop_start);
                    if (span <= 1.0f) {
                        done = true;
                        break;
                    }
                    pos = (float)v.loop_start + fmodf(pos - (float)v.loop_start,
                                                      span);
                    i0 = (uint32_t)pos;
                    if (i0 >= last) {
                        done = true;
                        break;
                    }
                } else {
                    done = true;
                    break;
                }
            }
            const float frac = pos - (float)i0;
            const float a = sample_at(data, fmt, i0);
            const float b = sample_at(data, fmt, i0 + 1);
            const float s = (a + (b - a) * frac) * env;
            s_dl[n] += s * gl;
            s_dr[n] += s * gr;
            pos += step;
            env *= coef;
            if (SYNTH_UNLIKELY(env < kSilence)) {
                done = true;
                break;
            }
        }

        v.pos = pos;
        v.env = env;
        if (done) {
            v.active = false;
            v.data = nullptr;
        } else {
            ++live;
        }
    }
    s_active_voices.store(live, std::memory_order_relaxed);
}

/* ---- parameter listener: the two trigger-style params ---- */
TaskHandle_t s_ctl_task = nullptr;
std::atomic<int> s_kit_request{-1};

void param_listener(uint16_t id, float value, ParamOrigin origin, void* ctx) {
    (void)origin;
    (void)ctx;
    if (id == DRUM_PID_TRIG) {
        const int slot = (int)(value + 0.5f);
        if (slot >= 0 && slot < DRUM_SLOTS) drums_trigger(slot, 100, 0);
    } else if (id == DRUM_PID_KIT) {
        const int idx = (int)(value + 0.5f);
        if (idx != s_kit_current) {
            s_kit_request.store(idx, std::memory_order_release);
            if (s_ctl_task != nullptr) xTaskNotifyGive(s_ctl_task);
        }
    }
}

/* Waits for `n` render boundaries so the audio task cannot still be holding
 * a pointer we are about to invalidate. Mirrors the S6 engine-detach dance. */
bool wait_render_boundaries(int n, int timeout_ms) {
    const uint32_t start = s_render_seq.load(std::memory_order_acquire);
    for (int waited = 0; waited < timeout_ms; ++waited) {
        if ((int)(s_render_seq.load(std::memory_order_acquire) - start) >= n) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    /* Audio task not running (no sink yet, or stopped): nothing can be
     * holding a pointer, so proceeding is safe. */
    return s_render_seq.load(std::memory_order_acquire) == start;
}

void ctl_task(void* arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const int want = s_kit_request.exchange(-1, std::memory_order_acq_rel);
        if (want < 0) continue;
        const esp_err_t err = drums_kit_select(want);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "kit %d failed (%s) — keeping '%s'", want,
                     esp_err_to_name(err), drums_kit_name());
            ParamStore::instance().set(DRUM_PID_KIT, (float)s_kit_current);
        }
    }
}

} // namespace

/* ======================= public API ==================================== */

void drums_click(bool accent) {
    s_click_pending.store(accent ? 2 : 1, std::memory_order_release);
}

uint8_t SYNTH_RENDER_IRAM drums_block_hit(int slot, uint16_t* delay_frames) {
    if (slot < 0 || slot >= DRUM_SLOTS) return 0;
    if (delay_frames != nullptr) *delay_frames = s_hit_delay[slot];
    return s_hit_vel[slot];
}

void drums_trigger(int slot, int velocity, int micro_frames) {
    if (slot < 0 || slot >= DRUM_SLOTS || velocity <= 0) return;
    if (micro_frames < 0) micro_frames = 0;
    if (micro_frames > 65535) micro_frames = 65535;

    taskENTER_CRITICAL(&s_ring_lock);
    const uint32_t head = s_ring_head.load(std::memory_order_relaxed);
    const uint32_t tail = s_ring_tail.load(std::memory_order_acquire);
    if (head - tail < (uint32_t)kTrigRing) {
        Trig& t = s_ring[head % kTrigRing];
        t.slot = (uint8_t)slot;
        t.vel = (uint8_t)(velocity > 127 ? 127 : velocity);
        t.delay = (uint16_t)micro_frames;
        s_ring_head.store(head + 1, std::memory_order_release);
    }
    /* Ring full: drop the hit. A drum machine that stalls its sequencer to
     * queue a 33rd simultaneous hit is worse than one that misses it. */
    taskEXIT_CRITICAL(&s_ring_lock);
}

bool drums_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    const int ch = (int)(pv(s_p_midich) + 0.5f);
    if (ch <= 0 || (int)channel + 1 != ch) return false;
    const drum_kit_t* kit = s_kit.load(std::memory_order_acquire);
    if (kit == nullptr) return false;
    for (int i = 0; i < kit->slot_count && i < DRUM_SLOTS; ++i) {
        if (kit->slots[i].data != nullptr && kit->slots[i].note == note) {
            if (velocity > 0) drums_trigger(i, velocity, 0);
            return true; /* claimed even at velocity 0: a drum has no note-off */
        }
    }
    return false;
}

void SYNTH_RENDER_IRAM drums_pre_fx(float* l, float* r, size_t frames) {
    if (frames > SYNTH_BLOCK_SIZE) frames = SYNTH_BLOCK_SIZE;
    s_scratch_frames = frames;

    /* Before anything can set one: the tap describes this block only, and
     * every early exit below still has to leave it empty. */
    memset(s_hit_vel, 0, sizeof(s_hit_vel));

    const drum_kit_t* kit = s_kit.load(std::memory_order_acquire);

    /* Silence on either signal: an explicit kill (the swap protocol arms one
     * before it publishes, so the gap while a kit loads is quiet), or the kit
     * pointer having moved since the last block.
     *
     * The second test is what makes drums_kit_select()'s two render boundaries
     * sufficient. The flag alone was consumed by whichever block saw it first,
     * necessarily *before* the publish — and every block between that one and
     * the publish could still drain a trigger and latch the old kit's sample
     * pointers into a voice. Those voices outlive both boundaries (a crash
     * cymbal rings for seconds), so the free at the end of the swap was a
     * use-after-free right here in the render loop. Comparing the pointer
     * instead means the first block that loads the new kit clears every voice
     * the old one spawned, and no block that loaded the old pointer can still
     * be running by the second boundary. */
    if (SYNTH_UNLIKELY(s_kill_voices.load(std::memory_order_acquire) ||
                       kit != s_last_kit)) {
        for (int i = 0; i < kVoices; ++i) {
            /* Same declick as a steal, and needed more here: this drops every
             * sounding voice at once, so the step is the whole drum bus. The
             * tail is a captured *value*, so it stays valid after the old
             * kit's sample data is freed. */
            steal_declick(s_voice[i]);
            s_voice[i].active = false;
            s_voice[i].data = nullptr;
        }
        s_kill_voices.store(false, std::memory_order_release);
        s_last_kit = kit;
    }

    /* Derivations first: start_voice() latches a voice's gains and playback
     * rate from them, so they have to be current before the ring is drained. */
    refresh_slots(kit);

    /* Drain the trigger ring before rendering so a hit queued during the
     * previous block starts at this block's first sample. */
    uint32_t tail = s_ring_tail.load(std::memory_order_relaxed);
    const uint32_t head = s_ring_head.load(std::memory_order_acquire);
    while (tail != head) {
        const Trig& t = s_ring[tail % kTrigRing];
        start_voice(kit, t.slot, t.vel, t.delay);
        ++tail;
    }
    s_ring_tail.store(tail, std::memory_order_release);

    render_voices(kit, frames);

    const float level = smooth_lin(s_sm_level, pv(s_p_level));
    s_send_now = smooth_lin(s_sm_send, pv(s_p_send));
    const float g = level * s_send_now;
    if (g > 0.0f) {
        for (size_t n = 0; n < frames; ++n) {
            l[n] += s_dl[n] * g;
            r[n] += s_dr[n] * g;
        }
    }
}

void SYNTH_RENDER_IRAM drums_post_fx(float* l, float* r, size_t frames) {
    if (frames > s_scratch_frames) frames = s_scratch_frames;
    const float g = s_sm_level.cur * (1.0f - s_send_now);
    if (g > 0.0f) {
        for (size_t n = 0; n < frames; ++n) {
            l[n] += s_dl[n] * g;
            r[n] += s_dr[n] * g;
        }
    }

    s_render_seq.fetch_add(1, std::memory_order_release);
}

/* The metronome is a monitoring aid, not part of the performance, so it is
 * mixed after the looper's record tap and never reaches a take. It used to
 * live at the end of drums_post_fx — which runs *before* looper_process —
 * and a count-in tick that overlapped the start of a recording was printed
 * into the loop.
 *
 * Being past the FX bus also keeps the click dry, so a patch with a long
 * reverb cannot smear the count. */
void SYNTH_RENDER_IRAM drums_render_click(float* l, float* r, size_t frames) {
    const int arm = s_click_pending.exchange(0, std::memory_order_acq_rel);
    if (SYNTH_UNLIKELY(arm != 0)) {
        s_click_phase = 0.0f;
        s_click_env = 1.0f;
        s_click_step = (arm == 2 ? kClickAccentHz : kClickHz) / kSampleRate;
        s_click_coef = expf(-1.0f / (kClickDecayMs * 1e-3f * kSampleRate));
    }
    if (SYNTH_UNLIKELY(s_click_env > kSilence)) {
        const float lvl = pv(s_p_click);
        for (size_t n = 0; n < frames; ++n) {
            const float v = sinf(s_click_phase * 6.28318531f) * s_click_env * lvl;
            l[n] += v;
            r[n] += v;
            s_click_phase += s_click_step;
            if (s_click_phase >= 1.0f) s_click_phase -= 1.0f;
            s_click_env *= s_click_coef;
            if (s_click_env <= kSilence) {
                s_click_env = 0.0f;
                break;
            }
        }
    }
}

int drums_slot_for_note(uint8_t note) {
    const drum_kit_t* kit = s_kit.load(std::memory_order_acquire);
    if (kit == nullptr) return -1;
    const int n = kit->slot_count < DRUM_SLOTS ? kit->slot_count : DRUM_SLOTS;
    for (int i = 0; i < n; ++i) {
        if (kit->slots[i].data != nullptr && kit->slots[i].note == note) {
            return i;
        }
    }
    return -1;
}

int drums_slot_note(int slot) {
    const drum_kit_t* kit = s_kit.load(std::memory_order_acquire);
    if (kit == nullptr || slot < 0 || slot >= kit->slot_count) return -1;
    if (kit->slots[slot].data == nullptr) return -1;
    return (int)kit->slots[slot].note;
}

int drums_slot_count(void) {
    const drum_kit_t* kit = s_kit.load(std::memory_order_acquire);
    return kit != nullptr ? kit->slot_count : 0;
}

const char* drums_slot_name(int slot) {
    const drum_kit_t* kit = s_kit.load(std::memory_order_acquire);
    if (kit == nullptr || slot < 0 || slot >= kit->slot_count) return "";
    return kit->slots[slot].name;
}

const char* drums_kit_name(void) {
    const drum_kit_t* kit = s_kit.load(std::memory_order_acquire);
    return kit != nullptr ? kit->name : "none";
}

int drums_active_voices(void) {
    return s_active_voices.load(std::memory_order_relaxed);
}

int drums_kit_count(void) { return s_kit_count; }

const char* drums_kit_name_at(int index) {
    if (index < 0 || index >= s_kit_count) return "";
    return s_kit_names[index];
}

esp_err_t drums_kit_select(int index) {
    if (index < 0 || index >= s_kit_count) return ESP_ERR_INVALID_ARG;
    if (index == s_kit_current) return ESP_OK;

    drum_kit_t fresh = {};
    esp_err_t err;
    if (index == 0) {
        err = drum_kit_load_rom(&fresh);
    } else {
        err = drum_kit_load_sd(s_kit_names[index], &fresh);
    }
    if (err != ESP_OK) return err;

    /* Silence first, then publish, then reclaim — see the file header. */
    s_kill_voices.store(true, std::memory_order_release);
    /* A timeout here is survivable: publishing is one atomic store, and a
     * voice mid-render still holds its own copy of the old kit's pointers,
     * which stay valid until the free below. */
    wait_render_boundaries(2, 500);

    drum_kit_t* old = s_kit.load(std::memory_order_acquire);
    s_kits[s_kit_back] = fresh;
    s_kit.store(&s_kits[s_kit_back], std::memory_order_release);
    s_kit_back ^= 1;
    s_kit_current = index;

    /* This one is not survivable, and its answer used to be discarded. False
     * means the audio task never reached a render boundary, so it may still
     * be reading sample data out of the old kit — freeing it here is a
     * use-after-free in the render loop. Leaking the block instead costs at
     * most a few hundred KB of PSRAM (the ROM kit owns nothing at all) and
     * only happens when the audio task has already stalled for half a
     * second, which is its own, louder problem.
     *
     * True is now a real guarantee rather than a hope: the publish above is
     * what arms drums_pre_fx's kit-changed silencing, so by the second
     * boundary no voice can still hold a pointer into `old`. */
    const bool settled = wait_render_boundaries(2, 500);
    if (old != nullptr) {
        if (settled) {
            drum_kit_free(old);
        } else {
            ESP_LOGE(TAG,
                     "kit swap: render handshake timed out — leaking %u KB "
                     "rather than freeing a kit that may still be playing",
                     (unsigned)(old->owned_bytes / 1024));
        }
    }

    for (int i = 0; i < DRUM_SLOTS; ++i) s_slot[i].primed = false;
    ESP_LOGI(TAG, "kit %d selected: '%s' (%d slots)", index, fresh.name,
             fresh.slot_count);
    return ESP_OK;
}

esp_err_t drums_init(void) {
    build_ulaw_table();
    s_declick_coef = expf(-1.0f / (kDeclickMs * 1e-3f * kSampleRate));

    /* The factory kit must be parsed before the parameters are built: slot
     * pan defaults come from it. A failure here is not fatal — the bus just
     * has nothing to play. */
    const bool have_rom = drum_kit_load_rom(&s_kits[0]) == ESP_OK;
    if (have_rom) {
        s_kit.store(&s_kits[0], std::memory_order_release);
        s_kit_back = 1;
    } else {
        ESP_LOGW(TAG, "no factory kit — the drum bus starts silent");
    }
    const drum_kit_t* kit = s_kit.load(std::memory_order_acquire);

    strlcpy(s_kit_names[0], have_rom ? s_kits[0].name : "none",
            DRUM_KIT_NAME_MAX);
    s_kit_count = 1;
    if (drum_kit_sd_supported()) {
        s_kit_count += drum_kit_scan_sd(&s_kit_names[1], kMaxKits - 1);
    }

    /* ---- descriptors ---- */
    int n = 0;
    s_descs[n++] = {DRUM_PID_LEVEL, "drums.level", ParamType::Float,
                    ParamCurve::Linear, 0.0f, 1.0f, 0.8f, nullptr, 0};
    s_descs[n++] = {DRUM_PID_SEND, "drums.send", ParamType::Float,
                    ParamCurve::Linear, 0.0f, 1.0f, 0.2f, nullptr, 0};
    s_descs[n++] = {DRUM_PID_CHOKE, "drums.choke", ParamType::Bool,
                    ParamCurve::Linear, 0.0f, 1.0f, 1.0f, nullptr, 0};
    s_descs[n++] = {DRUM_PID_KIT,   "drums.kit",   ParamType::Int,
                    ParamCurve::Linear, 0.0f, (float)(s_kit_count - 1), 0.0f,
                    nullptr, 0};
    s_descs[n++] = {DRUM_PID_TRIG,  "drums.trig",  ParamType::Int,
                    ParamCurve::Linear, 0.0f, (float)(DRUM_SLOTS - 1), 0.0f,
                    nullptr, 0};
    s_descs[n++] = {DRUM_PID_MIDICH, "drums.midich", ParamType::Int,
                    ParamCurve::Linear, 0.0f, 16.0f, 10.0f, nullptr, 0};
    s_descs[n++] = {DRUM_PID_CLICK, "drums.click", ParamType::Float,
                    ParamCurve::Linear, 0.0f, 1.0f, 0.35f, nullptr, 0};

    static const char* const kSuffix[4] = {"level", "pan", "tune", "decay"};
    for (int s = 0; s < DRUM_SLOTS; ++s) {
        const float def_pan =
            (kit != nullptr && s < kit->slot_count) ? kit->slots[s].pan : 0.0f;
        for (int k = 0; k < 4; ++k) {
            char* nm = s_slot_names[s * 4 + k];
            /* The two-digit modulo is for the compiler, not the logic: `s` is
             * a plain int, so -Wformat-truncation assumes its full range and
             * rejects "drum%d.decay" against a 16-byte buffer otherwise. */
            snprintf(nm, sizeof(s_slot_names[0]), "drum%u.%s",
                     (unsigned)(s + 1) % 100u, kSuffix[k]);
            ParamDesc d{};
            d.id = (uint16_t)(DRUM_PID_SLOT_BASE + s * DRUM_PID_SLOT_STRIDE + k);
            d.name = nm;
            d.type = ParamType::Float;
            d.curve = ParamCurve::Linear;
            switch (k) {
                case 0: d.min = 0.0f;   d.max = 2.0f;  d.def = 1.0f;    break;
                case 1: d.min = -1.0f;  d.max = 1.0f;  d.def = def_pan; break;
                case 2: d.min = -24.0f; d.max = 24.0f; d.def = 0.0f;    break;
                default: d.min = 0.0f;  d.max = 1.0f;  d.def = 1.0f;    break;
            }
            s_descs[n++] = d;
        }
    }

    ParamStore& ps = ParamStore::instance();
    const size_t added = ps.add(s_descs, (size_t)n);
    if (added != (size_t)n) {
        ESP_LOGE(TAG, "registered %u/%d params", (unsigned)added, n);
        return ESP_FAIL;
    }
    s_p_level = ps.valuePtr(DRUM_PID_LEVEL);
    s_p_send = ps.valuePtr(DRUM_PID_SEND);
    s_p_choke = ps.valuePtr(DRUM_PID_CHOKE);
    s_p_midich = ps.valuePtr(DRUM_PID_MIDICH);
    s_p_click = ps.valuePtr(DRUM_PID_CLICK);
    for (int s = 0; s < DRUM_SLOTS; ++s) {
        for (int k = 0; k < 4; ++k) {
            s_p_slot[s][k] = ps.valuePtr(
                (uint16_t)(DRUM_PID_SLOT_BASE + s * DRUM_PID_SLOT_STRIDE + k));
        }
    }
    refresh_slots(kit);
    ps.addListener(param_listener, nullptr);

    /* 8 KB: loading a WAV-folder kit walks a directory and stages file
     * reads on this stack. */
    if (xTaskCreatePinnedToCore(ctl_task, "drum_ctl", 8192, nullptr, 4,
                                &s_ctl_task, 0) != pdPASS) {
        ESP_LOGW(TAG, "control task not started — kit switching disabled");
        s_ctl_task = nullptr;
    }

    ESP_LOGI(TAG,
             "up: kit '%s' (%d slots), %d voices, %d params, %d kit(s) "
             "available%s",
             drums_kit_name(), drums_slot_count(), kVoices, n, s_kit_count,
             drum_kit_sd_supported() ? ", SD kits enabled" : "");
    return ESP_OK;
}
