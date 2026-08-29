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

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "drum_kit.h"
#include "drums_priv.h"
#include "sampler.h"
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
/* Factory kit plus the recordable ones (S44). Was 9 as a fixed "factory + 8
 * from the SD card"; it is now the same number for a different reason, and one
 * that follows a Kconfig option rather than a guess about what a card holds. */
constexpr int kKitCount = 1 + SYNTH_SAMPLE_KITS;
/* Release ramp for a gate or loop pad whose slot leaves `decay` at natural —
 * long enough not to click, short enough to feel like a key release. */
constexpr float kGateReleaseMs = 40.0f;

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
/* Every member is zero-initialised, and that is deliberate rather than lazy.
 *
 * `primed` starts false, so refresh_slots() rewrites all eight fields before
 * the first block can read one — the defaults were never observable. What they
 * *were* was expensive: a non-zero member initialiser moves the whole 576-byte
 * array out of `.bss` and into `.data`, and on the ESP32-P4 those live in
 * different regions (sram_high, 384 KB, versus sram_low, 175 KB and shared
 * with every byte of IRAM code). See the same note on UndoOp in sampler.cpp,
 * and tools/iram_budget.py for how to watch it happen. */
struct SlotDerived {
    float level = 0.0f;   /* raw param */
    float pan = 0.0f;     /* raw param */
    float tune = 0.0f;    /* raw param, semitones */
    float decay = 0.0f;   /* raw param */
    float gain_l = 0.0f;  /* derived from level*kit gain and pan */
    float gain_r = 0.0f;
    float rate_mul = 0.0f;  /* derived from tune */
    float decay_coef = 0.0f; /* derived from decay; 1.0 = natural length */
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
    /* Signed since S44: a reversed pad walks the same buffer backwards rather
     * than being stored twice, so `rev` stays a toggle on the kit instead of
     * being baked into a second copy of the audio. */
    float step = 0.0f;
    /* Playback bounds for this voice, precomputed so the inner loop tests two
     * floats instead of branching on the direction. Forward runs [-inf, last),
     * reverse [0, +inf) — in each case the bound that cannot be hit is set out
     * of reach rather than tested for. */
    float lo = 0.0f;
    float hi = 0.0f;
    float gain_l = 0.0f;
    float gain_r = 0.0f;
    float env = 0.0f;
    float env_coef = 0.0f;
    /* The coefficient to switch to when a held pad is released. Latched at
     * trigger time from the slot's decay knob, so a gate pad's release follows
     * the same control that shapes a one-shot's tail. */
    float release_coef = 0.0f;
    bool held = false; /* gate/loop: sustaining until drums_release() */
    uint32_t delay = 0; /* frames still to wait before the first sample */
    /* Zero-initialised throughout, for the region reason SlotDerived above
     * spells out: start_voice() writes every field a sounding voice reads and
     * `active` starts false, so the old non-zero defaults bought nothing and
     * cost 576 bytes of sram_low.
     *
     * Declick tail, held across the voice being taken away from whatever was
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

/* ---- trigger ring (producers: any control task) ----
 *
 * `rel` carries a note-off for the gate and loop pads S44 added. It rides the
 * same ring as a hit rather than reaching into the voice array directly,
 * because the voices belong to the audio task: a control task writing `held`
 * and `env_coef` under a running render loop is a data race, and the ring is
 * the mechanism this file already has for exactly that problem. */
struct Trig {
    uint8_t slot;
    uint8_t vel;
    uint8_t rel; /* 1 = release the slot's held voices, ignore vel */
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

/* ---- kits (S44: all of them resident) ----
 *
 * The table is pointers, not kits, for one reason: a drum_kit_t is ~3 KB of
 * slot table and nine of them in .bss would be 26 KB of *internal* RAM spent
 * mostly on empty pads. The factory kit has to exist before any allocator is
 * safe to call (drums_init() runs early and must not fail the boot), so it is
 * the one static; the recordable kits are allocated in PSRAM, which is also
 * where their samples live. An entry stays null when that allocation failed,
 * and every path here treats null as "an empty, unselectable kit" rather than
 * as an error — the sink-fallback rule. */
drum_kit_t s_factory_kit;
drum_kit_t* s_kit_tab[kKitCount] = {};
std::atomic<drum_kit_t*> s_kit{nullptr}; /* what the audio task plays */
std::atomic<uint32_t> s_render_seq{0};
std::atomic<bool> s_kill_voices{false};
/* Bumped on a kit switch and on every single-pad republish. drums.h explains
 * the contract; inside this file it is what the sampler engine and the local
 * UI compare against, and it is deliberately separate from `s_last_kit` below,
 * which cannot see a pad change because the kit pointer does not move. */
std::atomic<uint32_t> s_kit_gen{1};
/* Set by drums_slot_replace() to silence just the voices playing one slot.
 * -1 when idle; consumed by the audio task at block start. */
std::atomic<int> s_kill_slot{-1};
/* The kit the audio task last rendered with — audio task only, never read by
 * a control task. A voice latches raw sample pointers at trigger time and
 * holds them for its whole decay, so "silence the voices" has to key off the
 * pointer the block actually loaded and not off a flag some earlier block
 * happened to consume. See the comparison in drums_pre_fx(). */
const drum_kit_t* s_last_kit = nullptr;
int s_kit_count = kKitCount;
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
    /* Tested as a float before the conversion, because a voice can legally be
     * sitting outside the buffer when this is called. render_voices() advances
     * `pos` past its bound and only notices at the top of the *next* sample,
     * so a reversed voice that ran off the front is still active with a
     * negative pos until the following block — and a float-to-unsigned
     * conversion of a negative value is undefined, which is a wild index
     * rather than a wrong one. There is nothing to declick out there in any
     * case: the voice is about to be retired. */
    if (!(v.pos >= 0.0f && v.pos < (float)(v.frames - 1))) return;
    const uint32_t i0 = (uint32_t)v.pos;
    const float frac = v.pos - (float)i0;
    const float a = sample_at(v.data, v.format, i0);
    const float b = sample_at(v.data, v.format, i0 + 1);
    const float s = (a + (b - a) * frac) * v.env;
    /* Accumulated, not assigned: a voice stolen twice inside one block would
     * otherwise drop the first tail and reintroduce the step it was covering. */
    v.fade_l += s * v.gain_l;
    v.fade_r += s * v.gain_r;
}

/* Hand a slot's held voices over to their release ramp. Audio task, from the
 * trigger-ring drain. A one-shot voice is untouched: it has no gate to let go
 * of, which is why a control surface can send this on every touch-up without
 * first asking what kind of pad it is. */
void SYNTH_RENDER_IRAM release_slot(int slot) {
    for (int v = 0; v < kVoices; ++v) {
        Voice& x = s_voice[v];
        if (x.active && x.held && x.slot == (uint8_t)slot) {
            x.held = false;
            x.env_coef = x.release_coef;
            /* A looping pad must stop looping when it is let go, or the
             * release ramp would run forever against a signal that keeps
             * coming back round. */
            x.loop_end = 0;
            /* ...and the bound goes back to the buffer's, or the voice would
             * stop dead at the loop point instead of ringing out through
             * whatever follows it. start_voice() aims the bound at the loop
             * edge precisely because that is where a *looping* voice wraps;
             * once it is not looping, the edge it may reach is the sample's
             * again. The unreachable one of the pair is left where it is. */
            const float last = (float)(x.frames > 0 ? x.frames - 1 : 0);
            if (x.step >= 0.0f) {
                x.hi = last;
            } else {
                x.lo = 0.0f;
            }
        }
    }
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
    v.format = s.format;

    /* Loop points. A pad set to `loop` with no explicit points loops the whole
     * sample: the alternative — refusing to loop — would make the mode look
     * broken on every recorded pad, since nothing that comes off the recorder
     * carries loop points. */
    v.loop_start = s.loop_start;
    v.loop_end = s.loop_end;
    if (s.play_mode == DRUM_PLAY_LOOP && s.loop_end == 0) {
        v.loop_start = 0;
        v.loop_end = s.frames;
    } else if (s.play_mode == DRUM_PLAY_ONESHOT) {
        /* A one-shot ignores loop points it may have inherited from a kit
         * image, so switching a pad back to one-shot really does stop it —
         * and so this bus and the sampler engine, which does the same, cannot
         * disagree about what a pad does. */
        v.loop_end = 0;
    }
    /* The last frame a loop may name is frames-1, not frames, and the
     * difference is the whole of whether `loop` loops at all.
     *
     * The playback bound below is `last` = frames-1, because the interpolator
     * reads pos and pos+1. A loop that ends at `frames` therefore has a span
     * one sample longer than the distance the voice can actually travel, so
     * `fmodf(pos - loop_start, span)` at the moment it runs off is the
     * identity — pos comes back unchanged, trips the out-of-range guard in
     * render_voices(), and the voice ends. That is every whole-sample loop,
     * i.e. every `loop` pad with no explicit points (the branch above) and
     * every kit image whose loop_end == frames, which drum_kit.cpp accepts.
     * Measured as "loop mode behaves exactly like gate" at any rate <= 1.
     *
     * Clamping here rather than in the wrap keeps the span, the playback
     * bound and the safety guard all describing the same last frame. */
    if (v.loop_end > 0 && s.frames > 0 && v.loop_end > s.frames - 1) {
        v.loop_end = s.frames - 1;
    }
    /* A loop with under two frames in it is not a loop — same test the wrap
     * in render_voices() makes on `span`, made once here so the bounds below
     * never describe a degenerate region. */
    if (v.loop_end != 0 && v.loop_start + 1 >= v.loop_end) v.loop_end = 0;

    /* Stored rate vs the engine's rate is the whole resampling story for a
     * one-shot; `tune` rides on top of it. Reverse flips the sign and starts
     * from the far end — same buffer, no second copy. */
    const float rate = ((float)s.rate / kSampleRate) * d.rate_mul;
    const float last = (float)(s.frames > 0 ? s.frames - 1 : 0);
    float ofs = s.start_ofs;
    if (ofs < 0.0f) ofs = 0.0f;
    if (ofs > 0.999f) ofs = 0.999f;
    if (s.reverse != 0) {
        v.step = -rate;
        /* start_ofs measures in from the *playback* start, which for a
         * reversed pad is the end of the buffer — otherwise the control would
         * trim the tail the player can already hear rather than the head they
         * are aiming at.
         *
         * Held just below `last`, because the interpolator reads pos and
         * pos+1: landing exactly on the final frame would make that second
         * read one past the end of the sample. Forward playback gets the same
         * protection from `hi`, which it can and does trip; reverse never
         * approaches that edge again after the first sample, so it has to be
         * handled here instead. */
        v.pos = fminf(last - ofs * last, last - 0.001f);
        /* The bound a looping voice trips is the loop's own edge, not the
         * buffer's: wrapping is what `loop_start`/`loop_end` mean, and taking
         * the bound from the buffer instead is what made a loop pad play its
         * sample once and stop (see the clamp above). A one-shot keeps the
         * out-of-reach value it always had. */
        v.lo = (v.loop_end != 0) ? (float)v.loop_start : 0.0f;
        v.hi = (float)s.frames + 1.0f; /* out of reach: only `lo` can trip */
    } else {
        v.step = rate;
        v.pos = ofs * last;
        v.lo = -1.0f; /* out of reach: only `hi` can trip */
        v.hi = (v.loop_end != 0) ? fminf(last, (float)v.loop_end) : last;
    }

    v.gain_l = d.gain_l * amp;
    v.gain_r = d.gain_r * amp;
    v.env = 1.0f;
    /* Gate and loop pads sustain: no decay at all until the release arrives,
     * at which point env_coef becomes release_coef. A one-shot keeps the S22
     * behaviour of decaying from the moment it starts. */
    const bool sustains =
        (s.play_mode == DRUM_PLAY_GATE || s.play_mode == DRUM_PLAY_LOOP);
    v.env_coef = sustains ? 1.0f : d.decay_coef;
    v.release_coef = (d.decay_coef < 1.0f)
                         ? d.decay_coef
                         : expf(-1.0f / (kGateReleaseMs * 1e-3f * kSampleRate));
    v.held = sustains;
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
        const float last = (float)(v.frames - 1);
        const uint32_t loop_end = v.loop_end;
        const uint32_t loop_start = v.loop_start;
        /* One of these two is deliberately unreachable — see Voice::lo. Two
         * float compares replace the direction branch the reverse mode would
         * otherwise need in the innermost loop. */
        const float lo = v.lo;
        const float hi = v.hi;
        float pos = v.pos;
        const float step = v.step;
        float env = v.env;
        const float coef = v.env_coef;
        const float gl = v.gain_l;
        const float gr = v.gain_r;
        bool done = false;

        for (size_t n = start; n < frames; ++n) {
            if (SYNTH_UNLIKELY(pos < lo || pos >= hi)) {
                if (loop_end != 0) {
                    /* Wrap into the loop and keep the fractional phase, so a
                     * looped sample does not gain a click at every pass —
                     * from whichever end the voice ran off. */
                    const float span = (float)(loop_end - loop_start);
                    if (span <= 1.0f) {
                        done = true;
                        break;
                    }
                    pos = (step >= 0.0f)
                              ? (float)loop_start +
                                    fmodf(pos - (float)loop_start, span)
                              : (float)loop_end -
                                    fmodf((float)loop_end - pos, span);
                    /* A loop whose points sit outside the sample (a bad kit
                     * image, or a pad re-recorded shorter than its old loop)
                     * would otherwise index past the end. */
                    if (pos < 0.0f || pos >= last) {
                        done = true;
                        break;
                    }
                } else {
                    done = true;
                    break;
                }
            }
            const uint32_t i0 = (uint32_t)pos;
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

namespace {

/* The one producer path into the trigger ring, shared by hits and releases. */
void ring_push(int slot, int velocity, int micro_frames, bool release) {
    if (micro_frames < 0) micro_frames = 0;
    if (micro_frames > 65535) micro_frames = 65535;

    taskENTER_CRITICAL(&s_ring_lock);
    const uint32_t head = s_ring_head.load(std::memory_order_relaxed);
    const uint32_t tail = s_ring_tail.load(std::memory_order_acquire);
    if (head - tail < (uint32_t)kTrigRing) {
        Trig& t = s_ring[head % kTrigRing];
        t.slot = (uint8_t)slot;
        t.vel = (uint8_t)(velocity > 127 ? 127 : velocity);
        t.rel = release ? 1u : 0u;
        t.delay = (uint16_t)micro_frames;
        s_ring_head.store(head + 1, std::memory_order_release);
    }
    /* Ring full: drop the hit. A drum machine that stalls its sequencer to
     * queue a 33rd simultaneous hit is worse than one that misses it.
     *
     * A dropped *release* is worse than a dropped hit — it leaves a gate pad
     * sounding forever — but the alternatives are worse still (blocking a
     * control task, or a second ring that can also fill), and the case needs
     * 32 events inside 1.33 ms to arise at all. The backstop is that a held
     * voice is still an ordinary voice: the next hit that needs it steals it,
     * with the usual declick. */
    taskEXIT_CRITICAL(&s_ring_lock);
}

} // namespace

void drums_trigger(int slot, int velocity, int micro_frames) {
    if (slot < 0 || slot >= DRUM_SLOTS || velocity <= 0) return;
    ring_push(slot, velocity, micro_frames, false);
}

void drums_release(int slot) {
    if (slot < 0 || slot >= DRUM_SLOTS) return;
    ring_push(slot, 0, 0, true);
}

bool drums_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    const int ch = (int)(pv(s_p_midich) + 0.5f);
    if (ch <= 0 || (int)channel + 1 != ch) return false;
    const drum_kit_t* kit = s_kit.load(std::memory_order_acquire);
    if (kit == nullptr) return false;
    for (int i = 0; i < kit->slot_count && i < DRUM_SLOTS; ++i) {
        if (kit->slots[i].data != nullptr && kit->slots[i].note == note) {
            if (velocity > 0) {
                drums_trigger(i, velocity, 0);
            } else {
                /* Since S44 a note-off is not always nothing: a gate or loop
                 * pad sustains until it is let go, so the keyboard has to be
                 * able to let go of it. drums_release() is a no-op on the
                 * one-shot pads that made "a drum has no note-off" true, so
                 * this stays correct for every kit that predates the mode. */
                drums_release(i);
            }
            return true;
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

    /* One pad being republished (S44: recorded over, erased, undone, copied
     * into). Narrower than the whole-kit silencing above and for the same
     * reason it exists: the block behind *this* slot is about to be released,
     * and only the voices holding it have to go. Everything else — a crash
     * still ringing, a loop still running — keeps playing, which is what makes
     * sampling into a kit usable while the kit is being played. */
    const int kill_slot = s_kill_slot.exchange(-1, std::memory_order_acq_rel);
    if (SYNTH_UNLIKELY(kill_slot >= 0)) {
        for (int i = 0; i < kVoices; ++i) {
            if (!s_voice[i].active || s_voice[i].slot != (uint8_t)kill_slot) {
                continue;
            }
            steal_declick(s_voice[i]);
            s_voice[i].active = false;
            s_voice[i].data = nullptr;
        }
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
        if (t.rel != 0) {
            release_slot(t.slot);
        } else {
            start_voice(kit, t.slot, t.vel, t.delay);
        }
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

/* IRAM since S44: the sampler engine calls this from note_on, which is the
 * audio task. A flash-resident call there is exactly the cache-miss jitter
 * SYNTH_RENDER_IRAM exists to keep off the render path. */
int SYNTH_RENDER_IRAM drums_slot_for_note(uint8_t note) {
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

const int16_t* drums_ulaw_table(void) { return s_ulaw; }

int drums_kit_count(void) { return s_kit_count; }

int drums_kit_index(void) { return s_kit_current; }

bool drums_kit_is_user(int index) {
    return index >= 1 && index < kKitCount && s_kit_tab[index] != nullptr;
}

uint32_t drums_kit_generation(void) {
    return s_kit_gen.load(std::memory_order_acquire);
}

const char* drums_kit_name_at(int index) {
    if (index < 0 || index >= s_kit_count) return "";
    const drum_kit_t* k = s_kit_tab[index];
    return k != nullptr ? k->name : "";
}

drum_kit_t* drums_kit_at(int index) {
    if (index < 0 || index >= kKitCount) return nullptr;
    return s_kit_tab[index];
}

void drums_kit_mark_dirty(int index) {
    drum_kit_t* k = drums_kit_at(index);
    if (k != nullptr && index != 0) k->dirty = true;
}

esp_err_t drums_kit_rename(int index, const char* name) {
    if (!drums_kit_is_user(index) || name == nullptr) return ESP_ERR_INVALID_ARG;
    strlcpy(s_kit_tab[index]->name, name, DRUM_KIT_NAME_MAX);
    drums_kit_mark_dirty(index);
    return ESP_OK;
}

bool SYNTH_RENDER_IRAM drums_pad_get(int slot, drums_pad_t* out) {
    if (out == nullptr || slot < 0 || slot >= DRUM_SLOTS) return false;
    const drum_kit_t* kit = s_kit.load(std::memory_order_acquire);
    if (kit == nullptr || slot >= kit->slot_count) return false;
    const drum_sample_t& s = kit->slots[slot];
    if (s.data == nullptr || s.frames == 0) return false;
    out->data = s.data;
    out->frames = s.frames;
    out->rate = s.rate;
    out->loop_start = s.loop_start;
    out->loop_end = s.loop_end;
    out->gain = s.gain;
    out->pan = s.pan;
    out->start_ofs = s.start_ofs;
    out->format = s.format;
    out->choke_group = s.choke_group;
    out->note = s.note;
    out->play_mode = s.play_mode;
    out->reverse = s.reverse;
    return true;
}

void drums_apply_kit_mix(int index) {
    const drum_kit_t* k = drums_kit_at(index);
    if (k == nullptr) return;
    ParamStore& ps = ParamStore::instance();
    for (int s = 0; s < DRUM_SLOTS; ++s) {
        const drum_slot_mix_t& m = k->mix[s];
        const uint16_t base =
            (uint16_t)(DRUM_PID_SLOT_BASE + s * DRUM_PID_SLOT_STRIDE);
        ps.set((uint16_t)(base + 0), m.level, ParamOrigin::Internal);
        ps.set((uint16_t)(base + 1), m.pan, ParamOrigin::Internal);
        ps.set((uint16_t)(base + 2), m.tune, ParamOrigin::Internal);
        ps.set((uint16_t)(base + 3), m.decay, ParamOrigin::Internal);
    }
}

void drums_capture_kit_mix(void) {
    drum_kit_t* k = drums_kit_at(s_kit_current);
    if (k == nullptr) return;
    for (int s = 0; s < DRUM_SLOTS; ++s) {
        k->mix[s].level = pv(s_p_slot[s][0]);
        k->mix[s].pan = pv(s_p_slot[s][1]);
        k->mix[s].tune = pv(s_p_slot[s][2]);
        k->mix[s].decay = pv(s_p_slot[s][3]);
    }
}

/* Selecting a resident kit (S44).
 *
 * What used to be a load, a publish and a reclaim is now a publish. Nothing is
 * read from storage and nothing is freed, because every kit is already here —
 * see the note in drums.h on why that reversal was forced by recording. What
 * survives from the old protocol is the silencing, and only because the reason
 * for it survives too: a voice holds its sample pointer for its whole decay,
 * and the pads under it are about to be different ones. */
esp_err_t drums_kit_select(int index) {
    if (index < 0 || index >= s_kit_count) return ESP_ERR_INVALID_ARG;
    drum_kit_t* fresh = s_kit_tab[index];
    if (fresh == nullptr) return ESP_ERR_NOT_FOUND;
    if (index == s_kit_current) return ESP_OK;

    /* Keep what the player just dialled in on the kit they are leaving. */
    drums_capture_kit_mix();

    s_kill_voices.store(true, std::memory_order_release);
    s_kit_gen.fetch_add(1, std::memory_order_acq_rel);
    s_kit.store(fresh, std::memory_order_release);
    s_kit_current = index;
    /* Two boundaries so no voice anywhere — including the sampler engine's,
     * which watches the generation counter — is still holding a pad from the
     * kit we just left. Nothing is being freed here, so a timeout is merely a
     * stalled audio task and not a correctness problem; it is still worth
     * waiting for, because the mix push below is audible and should not land
     * on the old kit's voices. */
    wait_render_boundaries(2, 500);

    drums_apply_kit_mix(index);
    for (int i = 0; i < DRUM_SLOTS; ++i) s_slot[i].primed = false;
    ESP_LOGI(TAG, "kit %d selected: '%s' (%d slots)", index, fresh->name,
             fresh->slot_count);
    return ESP_OK;
}

esp_err_t drums_slot_replace(int kit, int slot, const drum_sample_t* fresh,
                             drum_sample_t* out_old) {
    if (slot < 0 || slot >= DRUM_SLOTS || fresh == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    drum_kit_t* k = drums_kit_at(kit);
    if (k == nullptr || kit == 0) return ESP_ERR_NOT_SUPPORTED;

    /* Order matters, and it is the same order the kit swap uses. The
     * generation bump goes first so that any engine voice starting between
     * here and the write already knows to drop what it holds; the slot kill
     * follows so the drum bus's own voices go quiet; and only then do we wait,
     * because waiting before arming either of them would prove nothing. */
    s_kit_gen.fetch_add(1, std::memory_order_acq_rel);
    if (k == s_kit.load(std::memory_order_acquire)) {
        s_kill_slot.store(slot, std::memory_order_release);
        if (!wait_render_boundaries(2, 500)) {
            /* The audio task is not reaching boundaries. Publishing anyway
             * would hand the caller a block it is about to free while a voice
             * may still be reading it, so refuse and change nothing — the
             * caller keeps its new block and can try again. */
            s_kill_slot.store(-1, std::memory_order_release);
            ESP_LOGE(TAG, "pad %d: render handshake timed out, not published",
                     slot + 1);
            return ESP_ERR_TIMEOUT;
        }
    }

    if (out_old != nullptr) *out_old = k->slots[slot];
    k->slots[slot] = *fresh;
    if (k->slot_count < DRUM_SLOTS) k->slot_count = DRUM_SLOTS;
    k->dirty = true;
    s_slot[slot].primed = false;
    return ESP_OK;
}

esp_err_t drums_pad_rename(int kit, int slot, const char* name) {
    if (kit < 0) kit = s_kit_current;
    if (slot < 0 || slot >= DRUM_SLOTS || name == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    drum_kit_t* k = drums_kit_at(kit);
    if (k == nullptr || kit == 0) return ESP_ERR_NOT_SUPPORTED;
    strlcpy(k->slots[slot].name, name, DRUM_SLOT_NAME_MAX);
    k->dirty = true;
    return ESP_OK;
}

const char* drums_storage_name(void) { return drum_kit_storage_name(); }

esp_err_t drums_pad_set_field(int kit, int slot, drums_pad_field_t field,
                              float value) {
    if (kit < 0) kit = s_kit_current;
    if (slot < 0 || slot >= DRUM_SLOTS) return ESP_ERR_INVALID_ARG;
    drum_kit_t* k = drums_kit_at(kit);
    /* The factory kit is flash-mapped and cannot be written back, so it must
     * not appear to accept edits either — a control that silently forgets is
     * worse than one that says no. */
    if (k == nullptr || kit == 0) return ESP_ERR_NOT_SUPPORTED;
    drum_sample_t& s = k->slots[slot];

    switch (field) {
        case DRUM_PAD_FIELD_MODE: {
            int m = (int)(value + 0.5f);
            if (m < DRUM_PLAY_ONESHOT || m > DRUM_PLAY_LOOP) {
                return ESP_ERR_INVALID_ARG;
            }
            s.play_mode = (uint8_t)m;
            break;
        }
        case DRUM_PAD_FIELD_REVERSE:
            s.reverse = value >= 0.5f ? 1u : 0u;
            break;
        case DRUM_PAD_FIELD_START:
            s.start_ofs = value < 0.0f ? 0.0f : (value > 0.999f ? 0.999f : value);
            break;
        case DRUM_PAD_FIELD_CHOKE: {
            const int g = (int)(value + 0.5f);
            if (g < 0 || g > 7) return ESP_ERR_INVALID_ARG;
            s.choke_group = (uint8_t)g;
            break;
        }
        case DRUM_PAD_FIELD_NOTE: {
            const int n = (int)(value + 0.5f);
            if (n < 0 || n > 127) return ESP_ERR_INVALID_ARG;
            s.note = (uint8_t)n;
            break;
        }
        case DRUM_PAD_FIELD_GAIN:
            s.gain = value < 0.0f ? 0.0f : (value > 4.0f ? 4.0f : value);
            s_slot[slot].primed = false; /* refresh_slots folds kit gain in */
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }
    k->dirty = true;
    /* No render handshake: every field above is read at trigger time by
     * start_voice() from the kit, never latched into a sounding voice, so the
     * worst a concurrent block can do is start one hit under the old value.
     * The pointer the voices actually hold is untouched, which is the thing
     * the handshake exists to protect. */
    return ESP_OK;
}

esp_err_t drums_init(void) {
    build_ulaw_table();
    s_declick_coef = expf(-1.0f / (kDeclickMs * 1e-3f * kSampleRate));

    /* The factory kit must be parsed before the parameters are built: slot
     * pan defaults come from it. A failure here is not fatal — the bus just
     * has nothing to play. */
    const bool have_rom = drum_kit_load_rom(&s_factory_kit) == ESP_OK;
    if (have_rom) {
        strlcpy(s_factory_kit.name, s_factory_kit.name[0] ? s_factory_kit.name
                                                          : "factory",
                DRUM_KIT_NAME_MAX);
    } else {
        s_factory_kit = drum_kit_t{};
        strlcpy(s_factory_kit.name, "none", DRUM_KIT_NAME_MAX);
        ESP_LOGW(TAG, "no factory kit — the drum bus starts silent");
    }
    s_kit_tab[0] = &s_factory_kit;
    s_kit.store(&s_factory_kit, std::memory_order_release);
    const drum_kit_t* kit = s_kit.load(std::memory_order_acquire);

    /* The recordable kits (S44). Each is ~3 KB of slot table in PSRAM; the
     * samples they carry come out of the sampler pool and are accounted
     * separately. A kit whose table will not allocate stays null and is
     * reported as unavailable rather than taking the boot down with it. */
    int user_kits = 0;
    for (int i = 1; i < kKitCount; ++i) {
        drum_kit_t* k =
            (drum_kit_t*)heap_caps_calloc(1, sizeof(drum_kit_t),
                                          MALLOC_CAP_SPIRAM);
        if (k == nullptr) {
            ESP_LOGW(TAG, "kit %d: no PSRAM for its slot table", i);
            continue;
        }
        k->per_slot_owned = true;
        /* Every pad addressable from the first boot, empty or not: a sampler's
         * pads are destinations before they are sounds, so a kit that reported
         * "0 slots" until something was recorded into it would give the app
         * nothing to aim at. */
        k->slot_count = DRUM_SLOTS;
        snprintf(k->name, DRUM_KIT_NAME_MAX, "Kit %d", i);
        for (int s = 0; s < DRUM_SLOTS; ++s) {
            k->slots[s].note = (uint8_t)(36 + s);
        }
        s_kit_tab[i] = k;
        ++user_kits;
    }
    s_kit_count = kKitCount;

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

    /* Seed every kit's stored mix from the parameter defaults, so a kit that
     * has never been touched selects to something sane rather than to whatever
     * zero-initialised memory happens to mean (level 0 — a silent kit that
     * looks like a bug). The factory kit additionally keeps its per-slot pan,
     * which its image carries. */
    for (int i = 0; i < kKitCount; ++i) {
        drum_kit_t* k = s_kit_tab[i];
        if (k == nullptr) continue;
        for (int s = 0; s < DRUM_SLOTS; ++s) {
            k->mix[s].level = 1.0f;
            k->mix[s].pan = (s < k->slot_count) ? k->slots[s].pan : 0.0f;
            k->mix[s].tune = 0.0f;
            k->mix[s].decay = 1.0f;
        }
    }

    /* 8 KB: loading a WAV-folder kit walks a directory and stages file
     * reads on this stack. */
    if (xTaskCreatePinnedToCore(ctl_task, "drum_ctl", 8192, nullptr, 4,
                                &s_ctl_task, 0) != pdPASS) {
        ESP_LOGW(TAG, "control task not started — kit switching disabled");
        s_ctl_task = nullptr;
    }

    /* The recorder registers its own parameters and allocates the pre-roll
     * ring. After the kit table, because arming a pad has to be able to name
     * one; before the log line, so its own line comes first and the summary
     * below can report the pool. */
    const esp_err_t serr = sampler_init();
    if (serr != ESP_OK) ESP_LOGW(TAG, "sampler init: %s", esp_err_to_name(serr));

    drums_apply_kit_mix(0);

    ESP_LOGI(TAG,
             "up: kit '%s' (%d slots), %d voices, %d params, %d kit(s) "
             "(%d recordable — contents load later, see drums_kits_load)",
             drums_kit_name(), drums_slot_count(), kVoices, n, s_kit_count,
             user_kits);
    return ESP_OK;
}

/* Deliberately not part of drums_init(), and the reason is a mount order.
 *
 * The LittleFS fallback backend lives on the `storage` partition, which
 * presets owns and mounts — and presets_init() runs *after* drums_init(). A
 * kit loader inside drums_init() would therefore find no /lfs on a board with
 * no SD card, and would have to either mount the partition behind presets'
 * back (racing its format-on-corrupt path) or give up on the fallback
 * entirely. Splitting the load out costs one line in main.cpp and removes the
 * choice.
 *
 * It still has to run before looper_init(), which sizes its loop cap from the
 * *free* PSRAM pool: kits loaded after that point would over-commit memory the
 * looper had already counted as its own. */
void drums_kits_load(void) {
    drum_kit_storage_init();
    int loaded = 0;
    for (int i = 1; i < kKitCount; ++i) {
        if (s_kit_tab[i] == nullptr) continue;
        if (drum_kit_load_user(i, s_kit_tab[i]) == ESP_OK) ++loaded;
    }
    /* The bound kit's stored mix may have arrived with it. */
    drums_apply_kit_mix(s_kit_current);
    ESP_LOGI(TAG, "kits: %d restored from %s, sample pool %u/%u KB", loaded,
             drum_kit_storage_name(),
             (unsigned)(sampler_pool_used() / 1024),
             (unsigned)(sampler_pool_total() / 1024));
}
