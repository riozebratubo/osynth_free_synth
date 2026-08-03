/*
 * osynth — master FX bus (Sessions 10 + 11; bitcrush S17; filter S33):
 * chorus -> delay -> granular delay -> reverb -> bitcrush -> filter.
 *
 * Runs on the audio task, chained after the voice sum (main.cpp calls
 * fx_process() from the render callback; audio_io applies master volume and
 * the int16 conversion afterwards, so it scales dry and wet together).
 *
 * Memory: every line stores int16 — the whole output chain is 16-bit, so
 * the wet paths lose nothing audible, and the footprint (plus PSRAM cache
 * pressure on the S3) halves vs float. line_alloc() prefers PSRAM and falls
 * back to internal RAM; an effect whose lines cannot be allocated is
 * disabled with a warning (sink-fallback philosophy). The delay ceiling is
 * per target — 1.5 s on the S3 (PSRAM), 0.4 s on the classic ESP32
 * (internal-RAM budget) — and the registered fx.dly.time max reflects it.
 *
 * Every effect sits behind a dry/wet crossfade `mix`. Mixes are one-pole
 * smoothed per block (no zipper on CC sweeps) and the delay time glides
 * tape-style (repitches, never clicks). A fully-dry effect is skipped
 * entirely; once dry, its lines are zeroed incrementally (a few KB per
 * block) so stale audio can never replay on re-enable — that skip is what
 * keeps the always-on bus nearly free when unused.
 *
 * Reverb is a Freeverb (Schroeder/Moorer): 8 parallel lowpass-feedback
 * combs + 4 series allpasses per channel, 44.1 kHz tunings scaled to the
 * build's sample rate, right channel offset for width.
 *
 * The granular delay (S11) captures the dry mono sum (plus feedback of its
 * own wet output) into one circular line and scatters parabolic-windowed
 * grains over it: each grain starts at a random extra delay (spray), plays
 * at a fixed per-grain rate (pitch, immutable once launched — so a live
 * pitch change never breaks a flying grain's bounds) and lands at a random
 * equal-power pan. The grain pool is a fixed per-target ceiling — the S10
 * budget figures were never recorded, so the worst case is bounded by
 * construction; when the pool is full a spawn is skipped, never stolen.
 */
#include "fx.h"

#include <atomic>
#include <cmath>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "sdkconfig.h"

#include "synth_config.h"
#include "synth_dsp.h"
#include "synth_params.h"
#include "synth_smooth.h"

static const char* TAG = "fx";

using osynth::ParamCurve;
using osynth::ParamDesc;
using osynth::ParamStore;
using osynth::ParamType;

namespace {

constexpr float kSr = (float)SYNTH_SAMPLE_RATE;
constexpr float kTwoPi = 6.28318530718f;

/* PSRAM, not a particular chip, is what sets these ceilings. */
#if CONFIG_SPIRAM
constexpr float kDelayMaxS = 1.5f; /* stereo int16 @ 48 kHz: 288 KB, PSRAM */
#else
constexpr float kDelayMaxS = 0.4f; /* 77 KB, classic internal-RAM budget */
#endif

constexpr float kChoBaseMs = 12.0f;
constexpr float kChoDepthMaxMs = 12.0f;

#if CONFIG_SPIRAM
constexpr float kGrnSizeMaxS = 0.5f;   /* grain length ceiling */
constexpr float kGrnSprayMaxS = 0.25f; /* random extra-delay ceiling */
constexpr int kGrainMax = 16;          /* fixed pool: bounds the worst case */
#else
constexpr float kGrnSizeMaxS = 0.25f;  /* classic: small internal buffer */
constexpr float kGrnSprayMaxS = 0.12f;
constexpr int kGrainMax = 8;
#endif

/* ---- parameter set (order matches PIdx) ---- */

enum PIdx {
    CHO_MIX, CHO_RATE, CHO_DEPTH,
    DLY_MIX, DLY_TIME, DLY_FB, DLY_TONE, DLY_PP,
    GRN_MIX, GRN_SIZE, GRN_DENS, GRN_PITCH, GRN_FB, GRN_SPRAY,
    REV_MIX, REV_SIZE, REV_DAMP,
    CRUSH_MIX, CRUSH_BITS, CRUSH_DOWN,
    FLT_ON, FLT_TYPE, FLT_MODE, FLT_CUTOFF, FLT_RESO, FLT_DRIVE, FLT_SPREAD,
    FLT_VOWEL,
    P_COUNT
};

/* Same lists the engines register, same order — a filter should not mean
 * something different because it is on the master bus. Append-only. */
const char* const kFltModes[] = {"lp",   "bp", "hp", "notch",
                                 "peak", "ap", "bp norm"};
const char* const kFltTypes[] = {"svf 12", "svf 24", "ladder", "dual", "vowel"};

const ParamDesc kParams[P_COUNT] = {
    {FX_PID_CHO_MIX, "fx.cho.mix", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* 0.5 ~ classic chorus, 1 = pure vibrato */
    {FX_PID_CHO_RATE, "fx.cho.rate", ParamType::Float, ParamCurve::Exp,
     0.05f, 8.0f, 0.8f, nullptr, 0},
    {FX_PID_CHO_DEPTH, "fx.cho.depth", ParamType::Float, ParamCurve::Linear,
     0.0f, kChoDepthMaxMs, 3.5f, nullptr, 0}, /* ms */
    {FX_PID_DLY_MIX, "fx.dly.mix", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_DLY_TIME, "fx.dly.time", ParamType::Float, ParamCurve::Exp,
     0.02f, kDelayMaxS, 0.35f, nullptr, 0}, /* s; max = the line actually allocated */
    {FX_PID_DLY_FB, "fx.dly.fb", ParamType::Float, ParamCurve::Linear,
     0.0f, 0.95f, 0.35f, nullptr, 0},
    {FX_PID_DLY_TONE, "fx.dly.tone", ParamType::Float, ParamCurve::Exp,
     500.0f, 16000.0f, 4500.0f, nullptr, 0}, /* feedback LP: repeats darken */
    {FX_PID_DLY_PP, "fx.dly.pingpong", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_GRN_MIX, "fx.grn.mix", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_GRN_SIZE, "fx.grn.size", ParamType::Float, ParamCurve::Exp,
     0.02f, kGrnSizeMaxS, 0.09f, nullptr, 0}, /* s */
    {FX_PID_GRN_DENS, "fx.grn.dens", ParamType::Float, ParamCurve::Exp,
     1.0f, 64.0f, 12.0f, nullptr, 0}, /* grains/s; pool caps the overlap */
    {FX_PID_GRN_PITCH, "fx.grn.pitch", ParamType::Float, ParamCurve::Linear,
     -12.0f, 12.0f, 0.0f, nullptr, 0}, /* st, per grain at spawn */
    {FX_PID_GRN_FB, "fx.grn.fb", ParamType::Float, ParamCurve::Linear,
     0.0f, 0.9f, 0.25f, nullptr, 0}, /* wet mono back into the capture line */
    {FX_PID_GRN_SPRAY, "fx.grn.spray", ParamType::Float, ParamCurve::Linear,
     0.0f, kGrnSprayMaxS, 0.03f, nullptr, 0}, /* s of random extra delay */
    {FX_PID_REV_MIX, "fx.rev.mix", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.15f, nullptr, 0}, /* subtle room out of the box */
    {FX_PID_REV_SIZE, "fx.rev.size", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.55f, nullptr, 0},
    {FX_PID_REV_DAMP, "fx.rev.damp", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.3f, nullptr, 0},
    {FX_PID_CRUSH_MIX, "fx.crush.mix", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* 1 = fully crushed */
    {FX_PID_CRUSH_BITS, "fx.crush.bits", ParamType::Float, ParamCurve::Linear,
     2.0f, 16.0f, 8.0f, nullptr, 0}, /* fractional bits allowed */
    {FX_PID_CRUSH_DOWN, "fx.crush.down", ParamType::Int, ParamCurve::Linear,
     1.0f, 32.0f, 1.0f, nullptr, 0}, /* sample-rate divider (hold) */
    /* The switch doubles as this unit's dry/wet: unit_gate() ramps it, so
     * toggling crossfades over a few blocks instead of stepping, and fully
     * off skips the stage. */
    {FX_PID_FLT_ON, "fx.flt.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_FLT_TYPE, "fx.flt.type", ParamType::Enum, ParamCurve::Linear,
     0.0f, 4.0f, 0.0f /* svf 12 */, kFltTypes, 5},
    {FX_PID_FLT_MODE, "fx.flt.mode", ParamType::Enum, ParamCurve::Linear,
     0.0f, 6.0f, 0.0f /* lp */, kFltModes, 7},
    {FX_PID_FLT_CUTOFF, "fx.flt.cutoff", ParamType::Float, ParamCurve::Exp,
     20.0f, 18000.0f, 18000.0f, nullptr, 0}, /* wide open: switching it on
                                                should not mute the mix */
    {FX_PID_FLT_RESO, "fx.flt.reso", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.1f, nullptr, 0},
    {FX_PID_FLT_DRIVE, "fx.flt.drive", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_FLT_SPREAD, "fx.flt.spread", ParamType::Float, ParamCurve::Linear,
     0.0f, 6.0f, 2.0f, nullptr, 0}, /* dual: passband width in octaves */
    {FX_PID_FLT_VOWEL, "fx.flt.vowel", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* vowel: morph a-e-i-o-u */
};

const std::atomic<float>* s_p[P_COUNT];

inline float pv(PIdx i) { return s_p[i]->load(std::memory_order_relaxed); }

/* ---- int16 circular delay line ---- */

struct Line {
    int16_t* buf = nullptr;
    uint32_t len = 0;
    uint32_t w = 0; /* next write index */
};

size_t s_bytes_spiram = 0;
size_t s_bytes_internal = 0;

bool line_alloc(Line& l, uint32_t len) {
    int16_t* p = nullptr;
#if CONFIG_SPIRAM
    p = (int16_t*)heap_caps_calloc(len, sizeof(int16_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (p == nullptr) {
        p = (int16_t*)heap_caps_calloc(len, sizeof(int16_t),
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p == nullptr) return false;
    }
    (esp_ptr_external_ram(p) ? s_bytes_spiram : s_bytes_internal) +=
        (size_t)len * sizeof(int16_t);
    l.buf = p;
    l.len = len;
    l.w = 0;
    return true;
}

inline void line_push(Line& l, float v) {
    int32_t s = (int32_t)(v * 32767.0f);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    l.buf[l.w] = (int16_t)s;
    if (++l.w == l.len) l.w = 0;
}

/* Oldest sample — a delay of exactly len; read before line_push overwrites. */
inline float line_tap(const Line& l) {
    return (float)l.buf[l.w] * (1.0f / 32768.0f);
}

/* x[n - d] with fractional d in [1, len-3], linear interpolation; call
 * before pushing sample n. */
inline float line_read_frac(const Line& l, float d) {
    const uint32_t di = (uint32_t)d;
    const float frac = d - (float)di;
    uint32_t i0 = l.w + l.len - di;
    if (i0 >= l.len) i0 -= l.len;
    const uint32_t i1 = (i0 == 0) ? l.len - 1 : i0 - 1;
    const float a = (float)l.buf[i0];
    return (a + frac * ((float)l.buf[i1] - a)) * (1.0f / 32768.0f);
}

/* ---- per-effect dry/wet gate with incremental bypass scrub ---- */

constexpr float kMixSlew = 0.08f;      /* per block: ~90 ms for a full swing */
constexpr float kSilent = 1e-3f;
constexpr uint32_t kScrubChunk = 4096; /* samples zeroed per block while dry */

struct UnitState {
    float mix = 0.0f; /* smoothed */
    bool active = false;
    bool scrubbing = false;
    uint8_t sl = 0;   /* scrub cursor: line index / offset */
    uint32_t sp = 0;
};

/* Zeroes up to kScrubChunk samples across the unit's lines; true when done. */
bool SYNTH_RENDER_IRAM scrub_step(UnitState& u, Line* const* lines, size_t n) {
    uint32_t budget = kScrubChunk;
    while (budget > 0 && u.sl < n) {
        Line* l = lines[u.sl];
        if (l->buf == nullptr || u.sp >= l->len) {
            u.sl++;
            u.sp = 0;
            continue;
        }
        const uint32_t left = l->len - u.sp;
        const uint32_t c = (budget < left) ? budget : left;
        memset(l->buf + u.sp, 0, c * sizeof(int16_t));
        u.sp += c;
        budget -= c;
    }
    return u.sl >= n;
}

/* Smooths the unit's mix toward `target`. Returns the block mix when the
 * effect should process, or < 0 when it is dry — the caller returns
 * immediately (resetting its filter states); the tail lines are zeroed
 * here, one chunk per block, so re-enabling never replays stale audio. */
float SYNTH_RENDER_IRAM unit_gate(UnitState& u, float target,
                                  Line* const* lines, size_t n) {
    u.mix += kMixSlew * (target - u.mix);
    if (u.mix > kSilent || target > kSilent) {
        u.active = true;
        u.scrubbing = false;
        return u.mix;
    }
    if (u.active) {
        u.active = false;
        u.mix = 0.0f;
        u.scrubbing = true;
        u.sl = 0;
        u.sp = 0;
    }
    if (u.scrubbing && scrub_step(u, lines, n)) u.scrubbing = false;
    return -1.0f;
}

/* ---- chorus: one modulated tap per channel, right LFO in quadrature ---- */

constexpr uint32_t kChoLen =
    (uint32_t)((kChoBaseMs + kChoDepthMaxMs + 2.0f) * 0.001f * kSr) + 4;

struct ChorusFx {
    Line l, r;
    float phase = 0.0f;
    float dl = -1.0f, dr = -1.0f; /* block-boundary tap delays; < 0 = snap */
    osynth::dsp::Smooth s_depth;
    UnitState u;
    bool ok = false;
};

ChorusFx s_cho;

void SYNTH_RENDER_IRAM chorus_process(float* __restrict__ bl,
                                      float* __restrict__ br, size_t frames) {
    ChorusFx& c = s_cho;
    if (!c.ok) return;
    Line* const lines[] = {&c.l, &c.r};
    const float m = unit_gate(c.u, pv(CHO_MIX), lines, 2);
    if (m < 0.0f) {
        c.dl = c.dr = -1.0f;
        return;
    }

    c.phase += pv(CHO_RATE) * (float)frames / kSr;
    c.phase -= (float)(int)c.phase;
    float pr = c.phase + 0.25f;
    pr -= (float)(int)pr;

    /* Target tap delays for this block boundary; the per-sample ramp between
     * boundaries keeps the delay continuous (the LFO is grossly oversampled
     * by the 1.33 ms block rate). */
    const float base = kChoBaseMs * 0.001f * kSr;
    const float depth =
        osynth::dsp::smooth_lin(c.s_depth, pv(CHO_DEPTH)) * 0.001f * kSr;
    const float tl = base + depth * (0.5f + 0.5f * sinf(kTwoPi * c.phase));
    const float tr = base + depth * (0.5f + 0.5f * sinf(kTwoPi * pr));
    if (c.dl < 0.0f) {
        c.dl = tl;
        c.dr = tr;
    }
    const float stepl = (tl - c.dl) / (float)frames;
    const float stepr = (tr - c.dr) / (float)frames;

    float dl = c.dl, dr = c.dr;
    for (size_t i = 0; i < frames; ++i) {
        dl += stepl;
        dr += stepr;
        const float wl = line_read_frac(c.l, dl);
        const float wr = line_read_frac(c.r, dr);
        line_push(c.l, bl[i]);
        line_push(c.r, br[i]);
        bl[i] += m * (wl - bl[i]);
        br[i] += m * (wr - br[i]);
    }
    c.dl = tl;
    c.dr = tr;
}

/* ---- delay: stereo (or cross-fed ping-pong), damped feedback ---- */

constexpr float kTimeSlew = 0.025f; /* per block: tape-style glide, ~110 ms */

struct DelayFx {
    Line l, r;
    float lpl = 0.0f, lpr = 0.0f; /* feedback tone filters */
    float t = -1.0f;              /* smoothed time in samples; < 0 = snap */
    osynth::dsp::Smooth s_fb, s_tone;
    UnitState u;
    bool ok = false;
};

DelayFx s_dly;

void SYNTH_RENDER_IRAM delay_process(float* __restrict__ bl,
                                     float* __restrict__ br, size_t frames) {
    DelayFx& d = s_dly;
    if (!d.ok) return;
    Line* const lines[] = {&d.l, &d.r};
    const float m = unit_gate(d.u, pv(DLY_MIX), lines, 2);
    if (m < 0.0f) {
        d.lpl = d.lpr = 0.0f;
        d.t = -1.0f;
        return;
    }

    float tt = pv(DLY_TIME) * kSr;
    const float tmax = (float)(d.l.len - 4);
    tt = fminf(fmaxf(tt, 2.0f), tmax);
    if (d.t < 0.0f) d.t = tt;
    const float t1 = d.t + kTimeSlew * (tt - d.t);
    const float tstep = (t1 - d.t) / (float)frames;

    /* Feedback and tone run per sample inside the loop below: a raw jump
     * would step the whole tail, so both are smoothed (S21). The delay time
     * already has its own tape-style glide above. */
    const float fb = osynth::dsp::smooth_lin(d.s_fb, pv(DLY_FB));
    const float a =
        1.0f - expf(-kTwoPi * osynth::dsp::smooth_exp(d.s_tone, pv(DLY_TONE)) /
                    kSr);
    const bool pp = pv(DLY_PP) >= 0.5f;

    float t = d.t;
    for (size_t i = 0; i < frames; ++i) {
        t += tstep;
        const float wl = line_read_frac(d.l, t);
        const float wr = line_read_frac(d.r, t);
        d.lpl += a * (wl - d.lpl);
        d.lpr += a * (wr - d.lpr);
        if (pp) { /* cross-feedback: echoes alternate right, left, right… */
            const float mono = 0.5f * (bl[i] + br[i]);
            line_push(d.l, fb * d.lpr);
            line_push(d.r, mono + fb * d.lpl);
        } else {
            line_push(d.l, bl[i] + fb * d.lpl);
            line_push(d.r, br[i] + fb * d.lpr);
        }
        bl[i] += m * (wl - bl[i]);
        br[i] += m * (wr - br[i]);
    }
    d.t = t1;
}

/* ---- granular delay: windowed grains scattered over a mono capture line ---- */

/* Worst-case reach behind the write head: a +12 st grain needs size*(2-1)
 * of lead room (its delay shrinks by rate-1 per output sample), plus the
 * spray ceiling; a -12 st grain's delay grows by at most size/2, which the
 * same bound covers. */
constexpr uint32_t kGrnLen =
    (uint32_t)((kGrnSizeMaxS + kGrnSprayMaxS) * kSr) + 16;

struct Grain {
    float d0 = 0.0f;    /* start delay behind the write head, samples */
    float dstep = 0.0f; /* delay drift per output sample: 1 - rate */
    float pinc = 0.0f;  /* window-phase increment: 1 / nt */
    float gl = 0.0f, gr = 0.0f; /* pan gains, density normalization baked in */
    float gm = 0.0f;            /* mono gain for the feedback path */
    uint32_t e = 0, nt = 0;     /* elapsed / total samples; e >= nt = free */
};

struct GranularFx {
    Line line; /* mono capture: dry input sum + fb * wet */
    Grain g[kGrainMax];
    float acc = 0.0f; /* spawn accumulator, in grains */
    uint32_t rng = 0x9e3779b9u;
    osynth::dsp::Smooth s_fb;
    UnitState u;
    bool ok = false;
};

GranularFx s_grn;

inline float grn_rand01(GranularFx& g) {
    uint32_t x = g.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g.rng = x;
    return (float)(x >> 8) * (1.0f / 16777216.0f);
}

void granular_spawn(GranularFx& g, float size_s, float pitch_st, float spray_s,
                    float dens) {
    Grain* slot = nullptr;
    for (int i = 0; i < kGrainMax; ++i) {
        if (g.g[i].e >= g.g[i].nt) {
            slot = &g.g[i];
            break;
        }
    }
    if (slot == nullptr) return; /* pool full: skip, never steal */

    const float rate = exp2f(pitch_st * (1.0f / 12.0f));
    uint32_t nt = (uint32_t)(size_s * kSr);
    if (nt < 64) nt = 64;

    /* Start delay: read margin + spray jitter, plus lead room so a
     * pitched-up grain never overruns the write head; clamped so a
     * pitched-down grain never falls off the far end of the line. The
     * per-sample delay is d0 + e*dstep (recomputed from the integer sample
     * count, not accumulated — float accumulation over a 24k-sample grain
     * could drift by whole samples and break these bounds). */
    float d0 = 3.0f + spray_s * kSr * grn_rand01(g);
    if (rate > 1.0f) d0 += (float)nt * (rate - 1.0f);
    const float dmax =
        (float)(g.line.len - 4) - (float)nt * fmaxf(1.0f - rate, 0.0f);
    if (d0 > dmax) d0 = dmax;

    /* Keep the wet level even as density/size change: expected overlap is
     * dens*size grains; uncorrelated grains sum ~ sqrt. */
    const float norm = 1.0f / sqrtf(fmaxf(1.0f, dens * size_s));
    const float pan = grn_rand01(g) * (0.25f * kTwoPi); /* equal power */

    slot->d0 = d0;
    slot->dstep = 1.0f - rate;
    slot->pinc = 1.0f / (float)nt;
    slot->gl = norm * cosf(pan);
    slot->gr = norm * sinf(pan);
    slot->gm = norm;
    slot->e = 0;
    slot->nt = nt;
}

void SYNTH_RENDER_IRAM granular_process(float* __restrict__ bl,
                                        float* __restrict__ br,
                                        size_t frames) {
    GranularFx& g = s_grn;
    if (!g.ok) return;
    Line* const lines[] = {&g.line};
    const float m = unit_gate(g.u, pv(GRN_MIX), lines, 1);
    if (m < 0.0f) {
        for (int i = 0; i < kGrainMax; ++i) g.g[i].nt = 0;
        g.acc = 0.0f;
        return;
    }

    const float size = pv(GRN_SIZE);
    const float dens = pv(GRN_DENS);
    const float pitch = pv(GRN_PITCH);
    const float spray = pv(GRN_SPRAY);
    /* size/dens/pitch/spray are read at spawn time, so a jump only shapes
     * the *next* grain — no smoothing needed. The feedback gain is applied
     * per sample into the capture line, so it is smoothed (S21). */
    const float fb = osynth::dsp::smooth_lin(g.s_fb, pv(GRN_FB));

    /* Spawns land on block boundaries (1.33 ms grid — spray jitters the
     * audible onsets anyway). */
    g.acc += dens * (float)frames / kSr;
    while (g.acc >= 1.0f) {
        g.acc -= 1.0f;
        granular_spawn(g, size, pitch, spray, dens);
    }

    for (size_t i = 0; i < frames; ++i) {
        float wl = 0.0f, wr = 0.0f, wm = 0.0f;
        for (int k = 0; k < kGrainMax; ++k) {
            Grain& gn = g.g[k];
            if (gn.e >= gn.nt) continue;
            const float e = (float)gn.e;
            const float p = e * gn.pinc;
            const float w = 4.0f * p * (1.0f - p); /* parabolic window */
            const float s = line_read_frac(g.line, gn.d0 + e * gn.dstep) * w;
            wl += s * gn.gl;
            wr += s * gn.gr;
            wm += s * gn.gm;
            gn.e++;
        }
        line_push(g.line, 0.5f * (bl[i] + br[i]) + fb * wm);
        bl[i] += m * (wl - bl[i]);
        br[i] += m * (wr - br[i]);
    }
}

/* ---- reverb: Freeverb (8 LP-feedback combs + 4 allpasses per channel) ---- */

constexpr uint32_t kCombTune[8] = {1116, 1188, 1277, 1356,
                                   1422, 1491, 1557, 1617};
constexpr uint32_t kApTune[4] = {556, 441, 341, 225};
constexpr uint32_t kSpread = 23; /* right-channel offset, pre-scale */

/* 44.1 kHz tuning -> this build's sample rate. */
constexpr uint32_t rv(uint32_t n44) {
    return (uint32_t)(((uint64_t)n44 * SYNTH_SAMPLE_RATE + 22050u) / 44100u);
}

/* Gain staging for the int16 lines: the input is padded into the combs and
 * the comb sum padded again into the allpasses, made up at the wet output.
 * The product kRevInGain * kRevPreAp * kRevWet = 0.045 equals Freeverb's
 * fixedgain (0.015) * scalewet (3), so the perceived level is the classic
 * one while every stored signal keeps real headroom above the 16-bit floor. */
constexpr float kRevInGain = 0.06f;
constexpr float kRevPreAp = 0.25f;
constexpr float kRevWet = 3.0f;

struct Comb {
    Line line;
    float store = 0.0f; /* damping LP state */
};

struct ReverbFx {
    Comb cl[8], cr[8];
    Line al[4], ar[4];
    osynth::dsp::Smooth s_fb, s_damp;
    UnitState u;
    bool ok = false;
};

ReverbFx s_rev;
Line* s_rev_lines[24]; /* every line, for the bypass scrub */

inline float comb_next(Comb& c, float in, float fb, float damp) {
    const float out = line_tap(c.line);
    c.store = out + damp * (c.store - out);
    line_push(c.line, in + fb * c.store);
    return out;
}

inline float allpass_next(Line& l, float in) {
    const float b = line_tap(l);
    line_push(l, in + 0.5f * b);
    return b - in;
}

void SYNTH_RENDER_IRAM reverb_process(float* __restrict__ bl,
                                      float* __restrict__ br, size_t frames) {
    ReverbFx& v = s_rev;
    if (!v.ok) return;
    const float m = unit_gate(v.u, pv(REV_MIX), s_rev_lines, 24);
    if (m < 0.0f) {
        for (int i = 0; i < 8; ++i) v.cl[i].store = v.cr[i].store = 0.0f;
        return;
    }

    /* Both feed the comb loop per sample: a raw jump steps the running tail
     * (a size change is audible as a click on a long decay). Smoothed S21. */
    const float fb =
        0.70f + 0.28f * osynth::dsp::smooth_lin(v.s_fb, pv(REV_SIZE));
    const float damp =
        0.95f * osynth::dsp::smooth_lin(v.s_damp, pv(REV_DAMP));

    for (size_t i = 0; i < frames; ++i) {
        const float in = (bl[i] + br[i]) * kRevInGain;
        float sl = 0.0f, sr = 0.0f;
        for (int c = 0; c < 8; ++c) {
            sl += comb_next(v.cl[c], in, fb, damp);
            sr += comb_next(v.cr[c], in, fb, damp);
        }
        sl *= kRevPreAp;
        sr *= kRevPreAp;
        for (int a = 0; a < 4; ++a) {
            sl = allpass_next(v.al[a], sl);
            sr = allpass_next(v.ar[a], sr);
        }
        bl[i] += m * (kRevWet * sl - bl[i]);
        br[i] += m * (kRevWet * sr - br[i]);
    }
}

/* ---- bitcrush: bit-depth quantize + sample-rate divide (S17) ----
 *
 * Master lo-fi, last in the chain so the crunch prints on everything.
 * Quantize step is 2^(bits-1) (fractional bits allowed — the step just
 * lands between powers of two); the rate divider holds each sampled value
 * for `down` output samples (naive zero-order hold: the aliasing IS the
 * effect). No delay lines, so the bypass gate has nothing to scrub. */

struct CrushFx {
    float hl = 0.0f, hr = 0.0f; /* held (quantized) samples */
    uint32_t cnt = 0;           /* samples until the next capture */
    osynth::dsp::Smooth s_bits;
    UnitState u;
};

CrushFx s_crush;

void SYNTH_RENDER_IRAM crush_process(float* __restrict__ bl,
                                     float* __restrict__ br, size_t frames) {
    CrushFx& c = s_crush;
    const float m = unit_gate(c.u, pv(CRUSH_MIX), nullptr, 0);
    if (m < 0.0f) {
        c.hl = c.hr = 0.0f;
        c.cnt = 0;
        return;
    }

    /* bits is already a log-domain control, so it smooths linearly; the
     * rate divider is an integer hold count and cannot be ramped. */
    const float q = exp2f(osynth::dsp::smooth_lin(c.s_bits, pv(CRUSH_BITS)) -
                          1.0f);
    const float iq = 1.0f / q;
    int32_t down = (int32_t)pv(CRUSH_DOWN);
    if (down < 1) down = 1;

    for (size_t i = 0; i < frames; ++i) {
        if (c.cnt == 0) {
            c.hl = floorf(bl[i] * q + 0.5f) * iq;
            c.hr = floorf(br[i] * q + 0.5f) * iq;
            c.cnt = (uint32_t)down;
        }
        c.cnt--;
        bl[i] += m * (c.hl - bl[i]);
        br[i] += m * (c.hr - br[i]);
    }
}

/* ---- master filter (S33): the voice filter family, run on the bus ----
 *
 * Everything the engines can do to one voice, applied to the finished mix —
 * which means it also filters the drums and the looper, neither of which
 * goes anywhere near a voice filter. That is the reason it exists: a
 * build-up that closes down the *whole* track was not previously expressible
 * anywhere in this synth.
 *
 * Cheap by construction: two channels once per block, against eight voices
 * once per block upstream. One coefficient build serves both channels —
 * they share every parameter and differ only in their filter state, so
 * there is no per-voice cutoff spread to pay for here.
 *
 * Placed last in the chain, after the reverb and the crusher, so the sweep
 * takes the tails and the quantization noise with it. A filter before the
 * reverb leaves a bright tail hanging over a closed filter, which sounds
 * like a mistake rather than an effect. */

struct FilterFx {
    osynth::dsp::Filt l, r;
    osynth::dsp::Smooth s_cut, s_reso, s_drive, s_spread, s_vowel;
    UnitState u;
};

FilterFx s_flt;

void SYNTH_RENDER_IRAM filter_process(float* __restrict__ bl,
                                      float* __restrict__ br, size_t frames) {
    FilterFx& c = s_flt;
    const float m = unit_gate(c.u, pv(FLT_ON), nullptr, 0);
    if (m < 0.0f) {
        /* Off: drop the filter states so switching back on cannot replay a
         * resonant ring from whatever was playing minutes ago. */
        c.l = osynth::dsp::Filt{};
        c.r = osynth::dsp::Filt{};
        return;
    }

    const osynth::dsp::FiltCoef fc = osynth::dsp::filt_coef(
        (osynth::dsp::FltType)(int)pv(FLT_TYPE),
        (osynth::dsp::SvfMode)(int)pv(FLT_MODE),
        osynth::dsp::smooth_exp(c.s_cut, pv(FLT_CUTOFF)),
        osynth::dsp::smooth_lin(c.s_reso, pv(FLT_RESO)),
        osynth::dsp::smooth_lin(c.s_drive, pv(FLT_DRIVE)),
        osynth::dsp::smooth_lin(c.s_spread, pv(FLT_SPREAD)),
        osynth::dsp::smooth_lin(c.s_vowel, pv(FLT_VOWEL)), kSr);

    for (size_t i = 0; i < frames; ++i) {
        bl[i] += m * (osynth::dsp::filt_next(c.l, fc, bl[i]) - bl[i]);
        br[i] += m * (osynth::dsp::filt_next(c.r, fc, br[i]) - br[i]);
    }
}

bool s_up = false;

} // namespace

esp_err_t fx_init(void) {
    ParamStore& ps = ParamStore::instance();
    const size_t added = ps.add(kParams, P_COUNT);
    if (added != P_COUNT) {
        ESP_LOGE(TAG, "registered %u/%u params", (unsigned)added,
                 (unsigned)P_COUNT);
        return ESP_FAIL;
    }
    for (size_t i = 0; i < P_COUNT; ++i) {
        s_p[i] = ps.valuePtr(kParams[i].id);
    }

    s_cho.ok = line_alloc(s_cho.l, kChoLen) && line_alloc(s_cho.r, kChoLen);
    if (!s_cho.ok) ESP_LOGW(TAG, "chorus disabled: line alloc failed");

    const uint32_t dlen = (uint32_t)(kDelayMaxS * kSr) + 8;
    s_dly.ok = line_alloc(s_dly.l, dlen) && line_alloc(s_dly.r, dlen);
    if (!s_dly.ok) ESP_LOGW(TAG, "delay disabled: line alloc failed");

    s_grn.ok = line_alloc(s_grn.line, kGrnLen);
    if (!s_grn.ok) ESP_LOGW(TAG, "granular disabled: line alloc failed");

    s_rev.ok = true;
    size_t nl = 0;
    for (int i = 0; i < 8; ++i) {
        s_rev.ok = s_rev.ok && line_alloc(s_rev.cl[i].line, rv(kCombTune[i]));
        s_rev.ok =
            s_rev.ok && line_alloc(s_rev.cr[i].line, rv(kCombTune[i] + kSpread));
        s_rev_lines[nl++] = &s_rev.cl[i].line;
        s_rev_lines[nl++] = &s_rev.cr[i].line;
    }
    for (int i = 0; i < 4; ++i) {
        s_rev.ok = s_rev.ok && line_alloc(s_rev.al[i], rv(kApTune[i]));
        s_rev.ok = s_rev.ok && line_alloc(s_rev.ar[i], rv(kApTune[i] + kSpread));
        s_rev_lines[nl++] = &s_rev.al[i];
        s_rev_lines[nl++] = &s_rev.ar[i];
    }
    if (!s_rev.ok) ESP_LOGW(TAG, "reverb disabled: line alloc failed");

    s_up = true;
    ESP_LOGI(TAG,
             "fx bus up: chorus -> delay -> granular -> reverb -> crush, "
             "%u params, "
             "delay max %.2f s, %d grains / %.2f s window, buffers %u KB "
             "PSRAM + %u KB internal",
             (unsigned)P_COUNT, (double)kDelayMaxS, kGrainMax,
             (double)kGrnLen / (double)kSr, (unsigned)(s_bytes_spiram / 1024),
             (unsigned)(s_bytes_internal / 1024));
    return ESP_OK;
}

void SYNTH_RENDER_IRAM fx_process(float* l, float* r, size_t frames) {
    if (!s_up || l == nullptr || r == nullptr) return;
    chorus_process(l, r, frames);
    delay_process(l, r, frames);
    granular_process(l, r, frames);
    reverb_process(l, r, frames);
    crush_process(l, r, frames);
    filter_process(l, r, frames);
}
