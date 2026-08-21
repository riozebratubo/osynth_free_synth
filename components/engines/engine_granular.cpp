/*
 * osynth — granular engine (Session 38).
 *
 * Per voice: a cloud of short windowed grains -> filter -> amp env, composed
 * from the shared blocks in synth_dsp.h. What is inside a grain is `grn.src`:
 *
 *   synth  an oscillator burst at its own frequency. This is FOF / pulsar
 *          synthesis, and the reason the engine earns a slot next to the
 *          other four: the grain *rate* carries the pitch while the grain
 *          *content* frequency (grn.form) is a formant peak that moves
 *          independently of it. Vowels, buzzy resonant trains and metallic
 *          clouds are what a fixed oscillator chain cannot reach.
 *   in     a window onto the capture ring, which begin_block() fills from
 *          the audio input. Grains are transposed by the key against
 *          buf.root, scattered over the ring by buf.pos / buf.spray, and
 *          buf.rev plays a share of them backwards. buf.freeze stops the
 *          write head, which turns the ring into a fixed sample.
 *
 * Both sources are registered on every build. Where there is no audio input
 * — or where the ring could not be allocated — `in` renders silence and says
 * so once in the log. That is the same contract the modular graph's LineIn
 * node has (audio_io.h), and it is what keeps a saved patch meaning the same
 * thing on every firmware that can read it: a parameter that exists on one
 * build and not the next is a preset that loads differently depending on who
 * reads it, and presets store values, not availability.
 *
 * Grain scheduling is sample-accurate, not block-accurate — the one place
 * this engine cannot copy the S11 granular delay, which spawns on block
 * boundaries because 1.33 ms of onset jitter is inaudible in an effect. Here
 * the onset grid *is* the pitch in `sync` mode, and quantising it to 750 Hz
 * would detune every note. The spawn accumulator carries its fraction across
 * samples so the average rate is exact, and each grain starts sub-sample
 * accurately (its oscillator phase and its read position are pre-advanced by
 * the fraction).
 *
 * Grain parameters are read at spawn time, so a knob move only shapes the
 * *next* grain and a flying grain can never have its bounds moved out from
 * under it. Only what is consumed per sample — pulse width, the filter, the
 * capture gain — is smoothed (S21).
 *
 * CPU is bounded by a grain budget rather than by refusing spawns. The pool
 * a voice may use is kGrainBudget divided by however many voices rendered in
 * the previous block, and the grain *length* is then clamped so the expected
 * overlap fits that pool. Shortening grains at high pitch or high polyphony
 * is what FOF does anyway (a shorter grain is a wider formant); skipping
 * spawns instead would drop grains out of the train and take the pitch with
 * them, which is why the S11 effect's "pool full: skip, never steal" rule is
 * deliberately not the rule here.
 *
 * Gain staging: grains are normalized by their expected overlap so the level
 * holds still while density and size move. The exponent is the subtlety —
 * see grain_norm() — because a `sync` cloud sums coherently and a jittered
 * or scattered one does not, and the two differ by the whole of sqrt(N).
 * The result lands near 0.43 RMS for a sine, in line with the wavetable
 * engine's RMS-normalized frames, so full polyphony cannot clip under the
 * voice manager's 1/SYNTH_VOICES headroom.
 */
#include "engine_granular.h"

#include <atomic>
#include <cmath>

#include "esp_log.h"

#include "synth_config.h"
#include "synth_dsp.h"
#include "synth_line.h"
#include "synth_mod.h"
#include "synth_params.h"
#include "synth_smooth.h"

#if SYNTH_ENABLE_AUDIO_IN
#include "audio_io.h"
#endif

static const char* TAG = "eng_gran";

namespace dsp = osynth::dsp;
using osynth::ParamCurve;
using osynth::ParamDesc;
using osynth::ParamStore;
using osynth::ParamType;

namespace {

constexpr float kSr = (float)SYNTH_SAMPLE_RATE;
constexpr float kInvSr = 1.0f / (float)SYNTH_SAMPLE_RATE;
constexpr float kMaxStep = 0.49f; /* keep phase increments below Nyquist */

/* PSRAM, not a particular chip, sets these — the same rule the FX bus uses
 * for its lines. The ring is what needs the memory; the pool sizes are a CPU
 * budget, and the classic ESP32 has to leave room for the FX bus after the
 * voices. */
#if CONFIG_SPIRAM
constexpr float kRingSec = 2.0f;      /* mono int16: 192 KB, PSRAM */
constexpr int kGrainsPerVoice = 12;   /* ceiling for one voice */
constexpr int kGrainBudget = 32;      /* shared across sounding voices */
#else
constexpr float kRingSec = 0.35f;     /* 33 KB, classic internal-RAM budget */
constexpr int kGrainsPerVoice = 6;
constexpr int kGrainBudget = 12;
#endif
constexpr int kGrainsMin = 2; /* a chord never thins below this per voice */

constexpr uint32_t kRingLen = (uint32_t)(kRingSec * kSr) + 8;
constexpr float kSizeMaxMs = 200.0f;
constexpr uint32_t kGrainMinSamples = 24; /* below this the window is a click */
constexpr float kRateMax = 8.0f;          /* src = in transposition ceiling */
constexpr float kRingReadMax = (float)(kRingLen - 4);

/* Source selector values (grn.src). Append only — presets store the index. */
enum : int { SRC_SYNTH = 0, SRC_IN = 1 };
/* Rate mode (grn.mode). */
enum : int { MODE_SYNC = 0, MODE_FREE = 1 };

inline float clamp01(float x) { return fminf(fmaxf(x, 0.0f), 1.0f); }

/* ---- one grain ----
 *
 * `d0` / `e` / `wbase` are the exact state; `d` and `wp` are the running
 * copies the sample loop advances, re-derived from the exact state at the
 * top of every block. That split is deliberate: accumulating a float delay
 * of ~1e5 across a 9600-sample grain drifts by tens of samples (24-bit
 * mantissa), which is both an audible detune and a lost bound. Re-anchoring
 * per block leaves at most `frames` accumulated steps, where the drift is
 * nothing. Same reasoning as the S11 effect's "recomputed from the integer
 * sample count, not accumulated" note — a different remedy for it, because
 * this ring is written once per block rather than once per sample.
 */
struct Grain {
    float pos = 0.0f;  /* src=synth: oscillator phase in [0,1) */
    float step = 0.0f; /* synth: phase increment. in: read rate (signed) */
    float d0 = 0.0f;   /* src=in: delay at birth, vs that block's write head */
    float d = 0.0f;    /* src=in: running delay for this block */
    float wp = 0.0f;   /* running window phase in [0,1) */
    float winc = 0.0f; /* 1 / nt */
    float peak = 0.5f; /* window warp split point, captured at spawn */
    float ka = 0.0f;   /* 0.5 / peak */
    float kb = 0.0f;   /* 0.5 / (1 - peak) */
    float gl = 0.0f, gr = 0.0f; /* pan gains, normalization baked in */
    uint32_t e = 0;     /* elapsed samples */
    uint32_t nt = 0;    /* total samples */
    uint32_t wbase = 0; /* ring samples written since birth */
};

struct GranVoice {
    Grain g[kGrainsPerVoice];
    int n = 0;         /* g[0..n-1] are live */
    float acc = 0.0f;  /* spawn accumulator, in grains */
    dsp::Noise rng;
    /* One filter per channel. The other engines pan a mono voice with
     * f->gain_l/r and need one; here every grain lands at its own place in
     * the field, so a single filter would have to run on the mono sum and
     * the stereo image would have to be rebuilt after it — which cannot be
     * done from a sum. Two states, one coefficient set. */
    dsp::Filt filt_l, filt_r;
    dsp::Adsr env1; /* amplitude, per sample */
    dsp::Adsr env2; /* mod (formant + filter), block rate */
    dsp::Lfo lfo1, lfo2;
    /* Which grn.src the live grains were born under. A Grain reuses the same
     * fields for both sources — `step` is a phase increment in [0, kMaxStep]
     * for `synth` and a signed read *rate* for `in` — so a grain read under
     * the other source's interpretation is not merely wrong, it is out of
     * bounds: a reversed `in` grain has step < 0, which the synth path walks
     * straight into a negative oscillator phase and sine01() turns into a
     * wild LUT index. Grains are therefore dropped when the source changes;
     * -1 means "none yet". */
    int src_cur = -1;
    uint8_t note = 60;
    float vel = 0.0f;
};

/* ---- parameter set (order matches PIdx) ---- */

enum PIdx {
    SRC, WAVE, MODE, DENS, SIZE, FORM, SHAPE, JIT, SCAT, SPREAD, PW,
    BUF_POS, BUF_SPRAY, BUF_REV, BUF_FREEZE, BUF_ROOT, BUF_GAIN,
    ENV_FORM,
    FLT_ON, FLT_TYPE, FLT_MODE, FLT_CUTOFF, FLT_RESO, FLT_ENV, FLT_KBD,
    FLT_DRIVE, FLT_SPREAD, FLT_VOWEL,
    ENV1_A, ENV1_D, ENV1_S, ENV1_R,
    ENV2_A, ENV2_D, ENV2_S, ENV2_R,
    LFO1_RATE, LFO1_WAVE, LFO1_PITCH,
    LFO2_RATE, LFO2_WAVE, LFO2_FORM,
    P_COUNT
};

/* Append-only, all of them: presets store the index, not the name. */
const char* const kSrcNames[] = {"synth", "in"};
const char* const kModeNames[] = {"sync", "free"};
/* The first four match dsp::OscWave; `noise` is this engine's own fifth and
 * has to stay last for that reason. */
const char* const kGrainWaves[] = {"sine", "triangle", "saw", "pulse",
                                   "noise"};
const char* const kFltModes[] = {"lp",   "bp", "hp", "notch",
                                 "peak", "ap", "bp norm"};
const char* const kFltTypes[] = {"svf 12", "svf 24", "ladder", "dual", "vowel"};
const char* const kLfoWaves[] = {"sine", "triangle", "saw", "square", "s&h"};

const ParamDesc kParams[P_COUNT] = {
    {GRAN_PID_SRC, "grn.src", ParamType::Enum, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f /* synth */, kSrcNames, 2},
    {GRAN_PID_WAVE, "grn.wave", ParamType::Enum, ParamCurve::Linear,
     0.0f, 4.0f, 0.0f /* sine */, kGrainWaves, 5},
    /* sync: one grain per cycle of the note, so the train *is* the pitch and
     * grn.form is a formant. free: grains at grn.dens regardless of the key,
     * so the cloud is asynchronous and the pitch comes from the content. */
    {GRAN_PID_MODE, "grn.mode", ParamType::Enum, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f /* sync */, kModeNames, 2},
    {GRAN_PID_DENS, "grn.dens", ParamType::Float, ParamCurve::Exp,
     1.0f, 400.0f, 60.0f, nullptr, 0}, /* grains/s, free mode */
    {GRAN_PID_SIZE, "grn.size", ParamType::Float, ParamCurve::Exp,
     1.0f, kSizeMaxMs, 30.0f, nullptr, 0}, /* ms, before the overlap clamp */
    /* Grain frequency as a ratio of the note. Exp because a formant is heard
     * in octaves, and because 1.0 has to sit in the middle of the travel
     * rather than at the bottom of a linear sweep. */
    {GRAN_PID_FORM, "grn.form", ParamType::Float, ParamCurve::Exp,
     0.25f, 16.0f, 2.0f, nullptr, 0},
    /* Where the window's peak sits, 0.5 = symmetric. Below it the grain
     * strikes and decays (percussive); above it, it swells (bowed). */
    {GRAN_PID_SHAPE, "grn.shape", ParamType::Float, ParamCurve::Linear,
     0.05f, 0.95f, 0.5f, nullptr, 0},
    {GRAN_PID_JIT, "grn.jit", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* onset jitter, fraction of the period */
    {GRAN_PID_SCAT, "grn.scat", ParamType::Float, ParamCurve::Linear,
     0.0f, 24.0f, 0.0f, nullptr, 0}, /* semitones, random per grain */
    {GRAN_PID_SPREAD, "grn.spread", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.35f, nullptr, 0}, /* 0 = every grain centred */
    {GRAN_PID_PW, "grn.pw", ParamType::Float, ParamCurve::Linear,
     0.05f, 0.95f, 0.5f, nullptr, 0}, /* pulse width, grn.wave = pulse */
    {GRAN_PID_BUF_POS, "buf.pos", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.5f, nullptr, 0}, /* 0 = newest audio, 1 = oldest */
    {GRAN_PID_BUF_SPRAY, "buf.spray", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.1f, nullptr, 0}, /* random position offset, per grain */
    {GRAN_PID_BUF_REV, "buf.rev", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* share of grains that play backwards */
    {GRAN_PID_BUF_FREEZE, "buf.freeze", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {GRAN_PID_BUF_ROOT, "buf.root", ParamType::Int, ParamCurve::Linear,
     0.0f, 127.0f, 60.0f, nullptr, 0}, /* the key that plays the ring at 1x */
    /* Linear, not Exp: smooth_exp() divides by its running value, so an
     * exponential curve needs min > 0 — and 0 here has to mean silent. The
     * engine's own trim rather than in.gain, so a granular preset sounds the
     * same whatever the monitor path happens to be set to. */
    /* Ceiling well above unity, and a default above it too: the ES8311 mic
     * path arrives far below full scale even after `in.micgain`, and a
     * capture ring fed at -40 dBFS granulates into something technically
     * correct and inaudible. 2.0 is a starting point a voice at normal
     * distance can actually be heard at; a hot line source wants it back
     * near 1. */
    {GRAN_PID_BUF_GAIN, "buf.gain", ParamType::Float, ParamCurve::Linear,
     0.0f, 16.0f, 2.0f, nullptr, 0},
    {GRAN_PID_ENV_FORM, "env.form", ParamType::Float, ParamCurve::Linear,
     -4.0f, 4.0f, 0.0f, nullptr, 0}, /* octaves; env2 -> formant */
    {GRAN_PID_FLT_ON, "flt.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0},
    {GRAN_PID_FLT_TYPE, "flt.type", ParamType::Enum, ParamCurve::Linear,
     0.0f, 4.0f, 0.0f /* svf 12 */, kFltTypes, 5},
    {GRAN_PID_FLT_MODE, "flt.mode", ParamType::Enum, ParamCurve::Linear,
     0.0f, 6.0f, 0.0f /* lp */, kFltModes, 7},
    {GRAN_PID_FLT_CUTOFF, "flt.cutoff", ParamType::Float, ParamCurve::Exp,
     20.0f, 18000.0f, 9000.0f, nullptr, 0}, /* mostly open by default */
    {GRAN_PID_FLT_RESO, "flt.reso", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.1f, nullptr, 0},
    {GRAN_PID_FLT_ENV, "flt.env", ParamType::Float, ParamCurve::Linear,
     -4.0f, 4.0f, 0.0f, nullptr, 0}, /* octaves; env2 doubles as filter env */
    {GRAN_PID_FLT_KBD, "flt.kbd", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.5f, nullptr, 0},
    {GRAN_PID_FLT_DRIVE, "flt.drive", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {GRAN_PID_FLT_SPREAD, "flt.spread", ParamType::Float, ParamCurve::Linear,
     0.0f, 6.0f, 2.0f, nullptr, 0}, /* dual: passband width in octaves */
    {GRAN_PID_FLT_VOWEL, "flt.vowel", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* vowel: morph a-e-i-o-u */
    {GRAN_PID_ENV1_ATTACK, "env1.attack", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.01f, nullptr, 0},
    {GRAN_PID_ENV1_DECAY, "env1.decay", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.4f, nullptr, 0},
    {GRAN_PID_ENV1_SUSTAIN, "env1.sustain", ParamType::Float,
     ParamCurve::Linear, 0.0f, 1.0f, 0.7f, nullptr, 0},
    {GRAN_PID_ENV1_RELEASE, "env1.release", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.35f, nullptr, 0},
    {GRAN_PID_ENV2_ATTACK, "env2.attack", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.002f, nullptr, 0},
    {GRAN_PID_ENV2_DECAY, "env2.decay", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.35f, nullptr, 0},
    {GRAN_PID_ENV2_SUSTAIN, "env2.sustain", ParamType::Float,
     ParamCurve::Linear, 0.0f, 1.0f, 0.15f, nullptr, 0},
    {GRAN_PID_ENV2_RELEASE, "env2.release", ParamType::Float, ParamCurve::Exp,
     0.001f, 10.0f, 0.3f, nullptr, 0},
    {GRAN_PID_LFO1_RATE, "lfo1.rate", ParamType::Float, ParamCurve::Exp,
     0.02f, 20.0f, 5.0f, nullptr, 0},
    {GRAN_PID_LFO1_WAVE, "lfo1.wave", ParamType::Enum, ParamCurve::Linear,
     0.0f, 4.0f, 0.0f /* sine */, kLfoWaves, 5},
    {GRAN_PID_LFO1_PITCH, "lfo1.pitch", ParamType::Float, ParamCurve::Linear,
     0.0f, 2.0f, 0.0f, nullptr, 0}, /* semitones */
    {GRAN_PID_LFO2_RATE, "lfo2.rate", ParamType::Float, ParamCurve::Exp,
     0.02f, 20.0f, 0.8f, nullptr, 0},
    {GRAN_PID_LFO2_WAVE, "lfo2.wave", ParamType::Enum, ParamCurve::Linear,
     0.0f, 4.0f, 1.0f /* triangle */, kLfoWaves, 5},
    {GRAN_PID_LFO2_FORM, "lfo2.form", ParamType::Float, ParamCurve::Linear,
     0.0f, 2.0f, 0.0f, nullptr, 0}, /* octaves of formant wobble */
};

const std::atomic<float>* s_p[P_COUNT];

inline float pv(PIdx i) { return s_p[i]->load(std::memory_order_relaxed); }

/* ---- the capture ring (src = in) ---- */

dsp::Line s_ring;
uint32_t s_adv = 0; /* ring samples written this block (0 while frozen) */
bool s_warned_no_in = false;
#if SYNTH_ENABLE_AUDIO_IN
/* One block of captured input, mono float. Audio task only, and only ever
 * live between the audio_io_in_mono() call and the loop that drains it. */
float s_inbuf[SYNTH_BLOCK_SIZE];
#endif

/* Capture telemetry. "No sound from grn.src = in" has several causes that
 * look identical from the outside — `in.source` naming a device with nothing
 * plugged into it (and `line` is its default, so this is the common one), a
 * mic whose RX half never came up, buf.gain too low for a MEMS mic — and none
 * of them is visible from the engine. So say what arrived: one line a second
 * while the source is `in`, at INFO while there is signal and at WARN while
 * there is not, naming the selector that decides which device this is. */
float s_in_peak = 0.0f;
uint32_t s_in_blocks = 0;
bool s_in_was_quiet = false;
const std::atomic<float>* s_in_source = nullptr; /* null where unregistered */

constexpr uint32_t kInReportBlocks = SYNTH_SAMPLE_RATE / SYNTH_BLOCK_SIZE;
constexpr float kInQuiet = 1.0f / 32768.0f; /* one LSB: provably nothing */

void report_input_level(int src) {
    if (src != SRC_IN || s_in_blocks < kInReportBlocks) return;
    const float pk = s_in_peak;
    s_in_peak = 0.0f;
    s_in_blocks = 0;

    const bool quiet = (pk <= kInQuiet);
    /* Signal is reported once on the way back up, so a working input does not
     * print forever; silence keeps printing, because that is the case someone
     * is actually watching the log for. */
    if (!quiet && !s_in_was_quiet) return;
    s_in_was_quiet = quiet;

    static const char* const kSel[] = {"line", "mic", "both"};
    const char* sel = "line";
    if (s_in_source != nullptr) {
        const int i = (int)s_in_source->load(std::memory_order_relaxed);
        if (i >= 0 && i < 3) sel = kSel[i];
    }
    if (quiet) {
        ESP_LOGW(TAG,
                 "grn.src = in: capture silent (in.source = %s, buf.gain "
                 "%.1f). Check in.source names the device you are using, and "
                 "raise buf.gain for a microphone.",
                 sel, (double)pv(BUF_GAIN));
    } else {
        ESP_LOGI(TAG, "grn.src = in: capture peak %.3f (in.source = %s)",
                 (double)pk, sel);
    }
}

/* ---- block-shared cache, rebuilt in begin_block() ---- */

struct BlockCache {
    int src, mode;
    bool noise;
    dsp::OscWave wave;
    float dens, size_ms, form, shape, jit, scat, spread, pw;
    float buf_pos, buf_spray, buf_rev;
    float root_hz;
    float env_form;
    dsp::SvfMode fmode;
    dsp::FltType ftype;
    float cutoff, reso, fenv_oct, fkbd, fdrive, fspread, fvowel;
    dsp::AdsrCoef amp; /* per-sample rates */
    dsp::AdsrCoef mod; /* per-block rates */
    float lfo1_inc, lfo2_inc;
    dsp::LfoWave lw1, lw2;
    float l1_pitch, l2_form;
    int pool;     /* grains one voice may hold, after the budget share */
    bool ring_ok; /* src = in can actually read something */
};

BlockCache s_bc;

/* Voices that rendered in the previous block — the divisor for the grain
 * budget. Counted rather than asked for because the voice manager does not
 * publish it, and one block of lag is far finer than the grain lengths this
 * is scaling. */
int s_voices_cur = 0;
int s_voices_last = 1;

/* Only what the sample loop consumes needs smoothing — the grain parameters
 * are read at spawn, where a step is simply the next grain being different
 * from the last one (S11's rule, and the reason none of them are here). */
struct Smoothers {
    dsp::Smooth pw, gain, env_form;
    dsp::Smooth cutoff, reso, fenv, fkbd, fdrive, fspread, fvowel;
    dsp::Smooth l1_pitch, l2_form;
};

Smoothers s_sm;

/* ---- grain helpers ---- */

inline float rand01(GranVoice& v) {
    return 0.5f * (dsp::noise_next(v.rng) + 1.0f);
}

/* Expected-overlap normalization.
 *
 * `n` grains are audible at once, and how their levels combine depends on
 * whether they are the same waveform arriving on a regular grid or not:
 *
 *  - a `sync` cloud with no jitter and no scatter is a FOF train. Every
 *    grain resets its oscillator at its own onset, so all of them trace the
 *    same trajectory and they add in *amplitude*: n, not sqrt(n). That
 *    coherence is the formant.
 *  - jitter, pitch scatter or free-running density break the phase relation,
 *    and uncorrelated grains add in power: sqrt(n).
 *
 * The two differ by the whole of sqrt(n) — at an overlap of 12 that is
 * 11 dB — so picking either one alone leaves half the engine's range either
 * thin or overloaded. The exponent interpolates on how decorrelated the
 * cloud actually is. Six semitones of scatter counts as fully decorrelated:
 * past a fourth apart, grains no longer reinforce.
 */
float SYNTH_RENDER_IRAM grain_norm(float overlap, float decorr) {
    const float n = fmaxf(overlap, 1.0f);
    if (n <= 1.0f) return 1.0f;
    return 1.0f / powf(n, 1.0f - 0.5f * clamp01(decorr));
}

/* Equal-power pan through the sine LUT. The sqrt(2) is not a taste decision:
 * at spread 0 every grain sits dead centre at cos = sin = 0.7071, and
 * without it this engine would be 3 dB quieter than the other four at the
 * same nominal level. */
constexpr float kPanMakeup = 1.41421356f;

/* Everything a spawn needs that the mod matrix may have moved. Resolved once
 * per voice-block in render() and handed to every grain born in that block —
 * a struct rather than eleven arguments, and it keeps the "read the
 * parameters at spawn, never mid-flight" rule visible in one place. */
struct SpawnParams {
    float f0;     /* voice pitch after glide, bend, unison and vibrato */
    uint32_t nt;  /* grain length in samples, after the pool clamp */
    float norm;   /* overlap normalization */
    float form, shape, scat, spread;
    float buf_pos, buf_spray;
};

/* One grain. `frac` is how far into the past the ideal onset was, in
 * samples — the schedule keeps sub-sample accuracy and the grain is
 * pre-advanced by it. Returns nothing: a spawn that cannot be placed
 * legally is simply not made. */
void SYNTH_RENDER_IRAM grain_spawn(GranVoice& v, const BlockCache& b,
                                   const SpawnParams& sp, float frac,
                                   size_t frames) {
    if (v.n >= b.pool) return;
    Grain& g = v.g[v.n];

    const float scat_mul =
        (sp.scat > 0.0f)
            ? exp2f(sp.scat * (2.0f * rand01(v) - 1.0f) * (1.0f / 12.0f))
            : 1.0f;

    g.nt = sp.nt;
    g.e = 0;
    g.wbase = 0;
    g.wp = 0.0f;
    g.peak = sp.shape;
    g.ka = 0.5f / sp.shape;
    g.kb = 0.5f / (1.0f - sp.shape);

    if (b.src == SRC_IN) {
        /* Read rate: the key against buf.root, scattered, and negated for
         * the share of grains buf.rev asks to run backwards. */
        float rate = (sp.f0 / b.root_hz) * scat_mul;
        if (rate > kRateMax) rate = kRateMax;
        if (b.buf_rev > 0.0f && rand01(v) < b.buf_rev) rate = -rate;

        /* How far the read position can travel from where it started, in
         * ring samples per grain sample. The write head advances one per
         * output sample and the read head advances `rate`, so an unfrozen
         * grain's distance behind the head changes by (1 - rate) — but
         * buf.freeze is a live control and stops the head mid-grain, where
         * that becomes (-rate). Both regimes have to fit, because a grain
         * can be flying when the switch is thrown.
         *
         *   shrink  the most the delay can ever fall  = max(rate, 0)
         *   grow    the most it can ever rise         = max(1 - rate, 0)
         *
         * `frames` appears in the bounds as well: the write head moves in
         * block-sized jumps while the read head moves per sample, so the
         * delay overshoots its ideal path by up to one block in either
         * direction. */
        const float shrink = fmaxf(rate, 0.0f);
        const float grow = fmaxf(1.0f - rate, 0.0f);
        const float span = shrink + grow; /* >= 1 by construction */
        const float room = (float)kRingLen - 8.0f - 2.0f * (float)frames;

        uint32_t maxnt = (uint32_t)(room / span);
        if (maxnt < kGrainMinSamples) return; /* ring too small for any grain */
        if (g.nt > maxnt) g.nt = maxnt;

        const float lo = 3.0f + (float)frames + (float)g.nt * shrink;
        const float hi = (float)kRingLen - 4.0f - (float)frames -
                         (float)g.nt * grow;
        if (hi <= lo) return;

        const float p01 =
            clamp01(sp.buf_pos + sp.buf_spray * (2.0f * rand01(v) - 1.0f));
        float d0 = lo + p01 * (hi - lo) - rate * frac;
        if (d0 < lo) d0 = lo;
        if (d0 > hi) d0 = hi;
        g.d0 = d0;
        g.d = d0;
        g.step = rate;
    } else {
        const float fg = sp.f0 * sp.form * scat_mul;
        /* Lower clamp as well as upper: everything feeding fg is positive by
         * construction today, and the sample loop's cheap `if (p >= 1) p -= 1`
         * wrap silently relies on that — a negative increment would walk the
         * phase out of [0,1) and off the end of the sine LUT. */
        g.step = fminf(fmaxf(fg * kInvSr, 0.0f), kMaxStep);
        float p = g.step * frac;
        if (p >= 1.0f) p -= (float)(int)p;
        g.pos = p;
    }

    g.winc = 1.0f / (float)g.nt;

    /* Equal-power pan: theta uniform in a band of width `spread` around
     * centre. sine01(x) is sin(2*pi*x), so cos comes from a quarter turn. */
    const float th = 0.125f * (1.0f + sp.spread * (2.0f * rand01(v) - 1.0f));
    float thc = th + 0.25f;
    if (thc >= 1.0f) thc -= 1.0f;
    g.gl = sp.norm * kPanMakeup * dsp::sine01(thc);
    g.gr = sp.norm * kPanMakeup * dsp::sine01(th);

    ++v.n;
}

/* ---- vtable entries ---- */

esp_err_t gran_init(void) {
    dsp::tables_init(); /* the LFOs, the window and the pan share the sine LUT */
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
    s_sm = Smoothers{}; /* unprimed: the first block snaps to the patch */
    s_voices_cur = 0;
    s_voices_last = 1;
    s_adv = 0;
    s_warned_no_in = false;
    s_in_peak = 0.0f;
    s_in_blocks = 0;
    s_in_was_quiet = false;
    /* Registered only where there are two devices to choose between, so a
     * null here is a single-input build and the report just says "line". */
    s_in_source = ps.valuePtr(osynth::PID_LINE_IN_SOURCE);

#if SYNTH_ENABLE_AUDIO_IN
    /* Allocated on bind and released on unbind, so the ring costs nothing
     * while one of the four other engines is playing. The switch protocol
     * runs init()/deinit() on a control task with the audio task already
     * detached, which is what makes that safe. A ring that cannot be
     * allocated is not fatal: grn.src = in renders silence, exactly as on a
     * build with no input at all (sink-fallback philosophy). */
    if (s_ring.buf == nullptr && !dsp::line_alloc(s_ring, kRingLen)) {
        ESP_LOGW(TAG,
                 "capture ring (%u KB) not allocated — grn.src = in will be "
                 "silent",
                 (unsigned)(kRingLen * sizeof(int16_t) / 1024));
    }
#endif

    ESP_LOGI(TAG,
             "granular engine up: %u params, caps 0x%02x (%d grains/voice, "
             "budget %d, ring %u ms %s, voice %u B)",
             (unsigned)P_COUNT, (unsigned)g_engine_granular.caps,
             kGrainsPerVoice, kGrainBudget,
             (unsigned)(kRingLen * 1000u / SYNTH_SAMPLE_RATE),
             s_ring.buf != nullptr ? "ready" : "absent",
             (unsigned)sizeof(GranVoice));
    return ESP_OK;
}

void gran_deinit(void) {
    ParamStore::instance().removeRange(osynth::PID_ENGINE_BASE,
                                       osynth::PID_FX_BASE);
    if (s_ring.buf != nullptr) {
        heap_caps_free(s_ring.buf);
        s_ring.buf = nullptr;
        s_ring.len = 0;
        s_ring.w = 0;
    }
}

void SYNTH_RENDER_IRAM gran_begin_block(size_t frames) {
    BlockCache& b = s_bc;
    b.src = (int)pv(SRC);
    const int w = (int)pv(WAVE);
    b.noise = (w >= 4);
    b.wave = (dsp::OscWave)(b.noise ? 0 : w);
    b.mode = (int)pv(MODE);
    b.dens = pv(DENS);
    b.size_ms = pv(SIZE);
    b.form = pv(FORM);
    b.shape = pv(SHAPE);
    b.jit = pv(JIT);
    b.scat = pv(SCAT);
    b.spread = pv(SPREAD);
    b.pw = dsp::smooth_lin(s_sm.pw, pv(PW));
    b.buf_pos = pv(BUF_POS);
    b.buf_spray = pv(BUF_SPRAY);
    b.buf_rev = pv(BUF_REV);
    b.root_hz = dsp::midi_to_freq(pv(BUF_ROOT));
    b.env_form = dsp::smooth_lin(s_sm.env_form, pv(ENV_FORM));
    b.fmode = (dsp::SvfMode)(int)pv(FLT_MODE);
    /* Bypass folds into the type here, so render() stays one call. */
    b.ftype = (pv(FLT_ON) < 0.5f) ? dsp::FltType::Bypass
                                  : (dsp::FltType)(int)pv(FLT_TYPE);
    b.cutoff = dsp::smooth_exp(s_sm.cutoff, pv(FLT_CUTOFF));
    b.reso = dsp::smooth_lin(s_sm.reso, pv(FLT_RESO));
    b.fenv_oct = dsp::smooth_lin(s_sm.fenv, pv(FLT_ENV));
    b.fkbd = dsp::smooth_lin(s_sm.fkbd, pv(FLT_KBD));
    b.fdrive = dsp::smooth_lin(s_sm.fdrive, pv(FLT_DRIVE));
    b.fspread = dsp::smooth_lin(s_sm.fspread, pv(FLT_SPREAD));
    b.fvowel = dsp::smooth_lin(s_sm.fvowel, pv(FLT_VOWEL));
    b.amp = dsp::adsr_coef_block(pv(ENV1_A), pv(ENV1_D), pv(ENV1_S),
                                 pv(ENV1_R), kSr, (uint32_t)frames);
    b.mod = dsp::adsr_coef(pv(ENV2_A), pv(ENV2_D), pv(ENV2_S), pv(ENV2_R),
                           kSr / (float)frames);
    b.lfo1_inc = pv(LFO1_RATE) * (float)frames * kInvSr;
    b.lw1 = (dsp::LfoWave)(int)pv(LFO1_WAVE);
    b.l1_pitch = dsp::smooth_lin(s_sm.l1_pitch, pv(LFO1_PITCH));
    b.lfo2_inc = pv(LFO2_RATE) * (float)frames * kInvSr;
    b.lw2 = (dsp::LfoWave)(int)pv(LFO2_WAVE);
    b.l2_form = dsp::smooth_lin(s_sm.l2_form, pv(LFO2_FORM));

    /* Roll the voice count over first, then divide the budget by it: the
     * share a voice gets this block is based on what actually sounded last
     * block, not on a count two blocks stale. */
    s_voices_last = s_voices_cur > 0 ? s_voices_cur : 1;
    s_voices_cur = 0;
    int share = kGrainBudget / s_voices_last;
    if (share > kGrainsPerVoice) share = kGrainsPerVoice;
    if (share < kGrainsMin) share = kGrainsMin;
    b.pool = share;

    /* Fill the capture ring. Always the whole block, even when the input is
     * absent — a grain's read position is a distance *behind the write head*,
     * so a head that quietly stopped moving would repitch every flying
     * grain. buf.freeze stops it on purpose, and s_adv is how the grains are
     * told about it. */
    s_adv = 0;
    b.ring_ok = false;
#if SYNTH_ENABLE_AUDIO_IN
    if (s_ring.buf != nullptr) {
        b.ring_ok = true;
        if (pv(BUF_FREEZE) < 0.5f) {
            /* audio_io_in_mono() and not audio_io_line_in_block(): the latter
             * is always the *line* device on a build that has one, so on a
             * board with both a jack and a microphone the mic could never
             * reach this ring however `in.source` was set. This asks for "the
             * audio input" and gets whatever the player selected, already
             * folded to mono and trimmed per device. */
            const float g = dsp::smooth_lin(s_sm.gain, pv(BUF_GAIN));
            float pk = 0.0f;
            if (audio_io_in_mono(s_inbuf, frames)) {
                for (size_t i = 0; i < frames; ++i) {
                    const float x = s_inbuf[i] * g;
                    pk = fmaxf(pk, fabsf(x));
                    dsp::line_push(s_ring, x);
                }
            } else {
                for (size_t i = 0; i < frames; ++i) dsp::line_push(s_ring, 0.0f);
            }
            s_adv = (uint32_t)frames;
            if (pk > s_in_peak) s_in_peak = pk;
            ++s_in_blocks;
        }
    }
#endif
    if (b.src == SRC_IN && !b.ring_ok && !s_warned_no_in) {
        s_warned_no_in = true;
        ESP_LOGW(TAG,
                 "grn.src = in, but this build has no capture ring — the "
                 "engine stays silent until grn.src is set back to synth");
    }
    report_input_level(b.src);
}

void gran_voice_reset(void* vs) {
    GranVoice& v = *(GranVoice*)vs;
    v = GranVoice{};
}

void gran_note_on(void* vs, uint8_t note, float vel01, bool was_sounding) {
    GranVoice& v = *(GranVoice*)vs;
    const float vel = fmaxf(vel01, 1.0f / 127.0f);
    if (!was_sounding) {
        v.n = 0;
        /* A whole grain already owed, so the first one lands on the first
         * sample of the note rather than a period later — at 2 Hz in free
         * mode that wait would otherwise be half a second of silence under
         * the envelope's attack. */
        v.acc = 1.0f;
        dsp::noise_seed(v.rng, 0x9e3779b9u ^ ((uint32_t)note * 2654435761u));
        dsp::lfo_retrig(v.lfo1);
        dsp::lfo_retrig(v.lfo2);
    } else if (v.env1.level > 0.0f && v.vel > 0.0f) {
        /* retrigger/steal: rescale so vel * env is continuous (no step) */
        v.env1.level = fminf(1.0f, v.env1.level * v.vel / vel);
    }
    v.note = note;
    v.vel = vel;
    dsp::adsr_gate_on(v.env1);
    dsp::adsr_gate_on(v.env2);
}

void gran_note_off(void* vs) {
    GranVoice& v = *(GranVoice*)vs;
    dsp::adsr_gate_off(v.env1);
    dsp::adsr_gate_off(v.env2);
}

void SYNTH_RENDER_IRAM gran_render(void* vs, const synth_voice_frame_t* f,
                                   float* __restrict__ out_l,
                                   float* __restrict__ out_r, size_t frames) {
    GranVoice& v = *(GranVoice*)vs;
    const BlockCache& b = s_bc;

    /* block-rate modulators */
    const float l1 = dsp::lfo_next(v.lfo1, b.lw1, b.lfo1_inc);
    const float l2 = dsp::lfo_next(v.lfo2, b.lw2, b.lfo2_inc);
    const float menv = dsp::adsr_next(v.env2, b.mod);

    /* mod matrix: per-voice sources, then the modulatable destinations */
    const synth_mod_voice_src_t ms = {menv, l1, l2, v.vel, (float)v.note};
    const float l1_pitch = synth_mod_apply(GRAN_PID_LFO1_PITCH, b.l1_pitch, &ms);
    const float l2_form = synth_mod_apply(GRAN_PID_LFO2_FORM, b.l2_form, &ms);
    const float env_form = synth_mod_apply(GRAN_PID_ENV_FORM, b.env_form, &ms);
    const float form_base = synth_mod_apply(GRAN_PID_FORM, b.form, &ms);
    const float size_ms = synth_mod_apply(GRAN_PID_SIZE, b.size_ms, &ms);
    const float dens = synth_mod_apply(GRAN_PID_DENS, b.dens, &ms);
    const float scat = synth_mod_apply(GRAN_PID_SCAT, b.scat, &ms);
    const float spread = synth_mod_apply(GRAN_PID_SPREAD, b.spread, &ms);
    /* jit and shape are destinations for the same reason form and scat are:
     * they are how this engine is played, not how it is set up. Velocity onto
     * the pair is the engine's signature gesture — soft gives a clean
     * coherent train, hard scatters it into a swarm and turns each grain from
     * a swell into a strike — and it is worth the two block-rate reads. */
    const float jit = clamp01(synth_mod_apply(GRAN_PID_JIT, b.jit, &ms));
    const float shape = synth_mod_apply(GRAN_PID_SHAPE, b.shape, &ms);
    const float buf_pos = synth_mod_apply(GRAN_PID_BUF_POS, b.buf_pos, &ms);
    const float buf_spray =
        synth_mod_apply(GRAN_PID_BUF_SPRAY, b.buf_spray, &ms);
    const float cutoff = synth_mod_apply(GRAN_PID_FLT_CUTOFF, b.cutoff, &ms);
    const float reso = synth_mod_apply(GRAN_PID_FLT_RESO, b.reso, &ms);
    const float fenv_oct = synth_mod_apply(GRAN_PID_FLT_ENV, b.fenv_oct, &ms);
    const float fdrive = synth_mod_apply(GRAN_PID_FLT_DRIVE, b.fdrive, &ms);
    const float fvowel = synth_mod_apply(GRAN_PID_FLT_VOWEL, b.fvowel, &ms);

    /* amp envelope: one state-machine pass, then a branch-free linear ramp */
    const dsp::AdsrRamp ar = dsp::adsr_block(v.env1, b.amp, (uint32_t)frames);
    if (SYNTH_UNLIKELY(dsp::adsr_ramp_silent(ar))) {
        /* The voice is done. Grains are gated by this envelope, so there is
         * nothing left to hear — and dropping them now hands their share of
         * the budget back to the voices that are still sounding. */
        v.n = 0;
        return;
    }

    /* Counted here rather than on entry: a voice that returned above holds no
     * grains, and letting it into the divisor would shrink the share of the
     * voices that do. */
    ++s_voices_cur;

    const float pitch_mul =
        (l1_pitch != 0.0f) ? exp2f(l1 * l1_pitch * (1.0f / 12.0f)) : 1.0f;
    const float f0 = f->freq_hz * pitch_mul;

    /* Formant: the base ratio shifted in octaves by env2 and lfo2. */
    const float form = form_base * exp2f(env_form * menv + l2_form * l2);

    /* Onsets. In sync mode the train is the pitch, so its rate is the note's
     * frequency and nothing may quantise it. */
    const float spawn_hz = (b.mode == MODE_SYNC) ? f0 : dens;
    const float sinc = fminf(spawn_hz * kInvSr, 1.0f);

    /* Grain length, then the clamp that keeps the cloud inside its pool:
     * expected overlap is nt * spawn_hz / sr, and the grains past `pool`
     * are the ones whose absence would break the train. Shortening is the
     * graceful answer — a narrower window is a wider formant, which is the
     * trade FOF makes at high pitch anyway.
     *
     * `pool - 1` and not `pool`: a grain is spawned before the loop below
     * retires the ones that just ended, so with a period T the peak
     * concurrent count is floor(nt/T) + 1. Sizing against the mean would let
     * a train at exactly `pool` grains of overlap hit the spawn cap on the
     * boundary sample and drop a grain — which in sync mode is heard as a
     * click in the pitch, the one failure this clamp exists to avoid. The
     * share is never below kGrainsMin, so this is at least one grain of
     * overlap; at the floor, grains stop overlapping and simply abut. */
    uint32_t nt = (uint32_t)(size_ms * 0.001f * kSr);
    const float nt_max = (float)(b.pool - 1) * kSr / fmaxf(spawn_hz, 0.001f);
    if ((float)nt > nt_max) nt = (uint32_t)nt_max;
    if (nt < kGrainMinSamples) nt = kGrainMinSamples;

    const float overlap = (float)nt * spawn_hz * kInvSr;
    const float decorr =
        (b.mode == MODE_FREE) ? 1.0f : (jit + scat * (1.0f / 6.0f));
    const float norm = grain_norm(overlap, decorr);

    /* Re-anchor the grains that were already flying: the ring's write head
     * moved by s_adv while they aged by `frames`. Grains spawned below start
     * from their own exact state and need none of this. */
    const bool src_in = (b.src == SRC_IN);
    if (SYNTH_UNLIKELY(v.src_cur != b.src)) {
        /* grn.src just moved under a held note — a preset load is the usual
         * way. See GranVoice::src_cur: these grains cannot be reinterpreted,
         * only dropped. The cloud rebuilds within one grain period. */
        v.n = 0;
        v.src_cur = b.src;
    }
    for (int k = 0; k < v.n; ++k) {
        Grain& g = v.g[k];
        g.wp = (float)g.e * g.winc;
        if (src_in) {
            g.wbase += s_adv;
            g.d = g.d0 + (float)g.wbase - g.step * (float)g.e;
        }
    }

    const float oct =
        fenv_oct * menv + b.fkbd * ((float)v.note - 60.0f) * (1.0f / 12.0f);
    const dsp::FiltCoef fc =
        dsp::filt_coef(b.ftype, b.fmode, cutoff * exp2f(oct), reso, fdrive,
                       b.fspread, fvowel, kSr);

    const float gl = f->gain_l * v.vel;
    const float gr = f->gain_r * v.vel;
    const bool can_spawn = !src_in || b.ring_ok;

    const SpawnParams sp = {f0,     nt,     norm,     form,    shape,
                            scat,   spread, buf_pos,  buf_spray};

    float a = ar.base;
    for (size_t i = 0; i < frames; ++i) {
        a += ar.step;

        /* Sample-accurate onsets. The accumulator keeps its fraction so the
         * average rate is exact even though a grain can only start on a
         * sample boundary. Jitter is applied to the *schedule* — the amount
         * subtracted here — rather than to the grain, so a jittered cloud
         * still has the right long-run density and cannot drift. The
         * subtraction is always in [0.5, 1.5], which is what guarantees this
         * loop terminates. */
        v.acc += sinc;
        while (v.acc >= 1.0f) {
            const float frac = (v.acc - 1.0f) / sinc;
            v.acc -= 1.0f + jit * (rand01(v) - 0.5f);
            if (can_spawn) grain_spawn(v, b, sp, frac, frames);
        }

        float wl = 0.0f, wr = 0.0f;
        for (int k = 0; k < v.n;) {
            Grain& g = v.g[k];

            /* Window: a Hann on a phase warped so its peak sits at
             * grn.shape. The warp is continuous and both halves reach the
             * peak with zero slope, so an asymmetric grain is as click-free
             * as a symmetric one — which a two-piece parabola is not. */
            const float p = g.wp;
            float q = (p < g.peak) ? p * g.ka : 0.5f + (p - g.peak) * g.kb;
            q -= 0.25f;
            if (q < 0.0f) q += 1.0f;
            const float w = 0.5f + 0.5f * dsp::sine01(q);

            float s;
            if (src_in) {
                /* Backstop, not the correctness argument: grain_spawn() proves
                 * d stays inside [3, kRingLen-4] for the grain's whole life.
                 * That proof leans on wbase tracking the write head across
                 * blocks, and the cost of it being wrong once is an
                 * out-of-bounds read on a 192 KB buffer — a reboot, not a
                 * glitch. Two ops against that is a good trade. */
                const float d = fminf(fmaxf(g.d, 3.0f), kRingReadMax);
                s = dsp::line_read_frac(s_ring, d);
                g.d -= g.step;
            } else if (b.noise) {
                s = dsp::noise_next(v.rng);
            } else {
                s = dsp::osc_at(b.wave, g.pos, g.step, b.pw);
                float np = g.pos + g.step;
                if (np >= 1.0f) np -= 1.0f;
                g.pos = np;
            }

            const float ws = s * w;
            wl += ws * g.gl;
            wr += ws * g.gr;

            g.wp = p + g.winc;
            if (++g.e >= g.nt) {
                v.g[k] = v.g[--v.n]; /* swap the last live grain into the hole */
                continue;            /* same k, now a different grain */
            }
            ++k;
        }

        out_l[i] += dsp::filt_next(v.filt_l, fc, wl) * a * gl;
        out_r[i] += dsp::filt_next(v.filt_r, fc, wr) * a * gr;
    }
}

bool gran_busy(const void* vs) {
    return dsp::adsr_active(((const GranVoice*)vs)->env1);
}

float gran_level(const void* vs) {
    return ((const GranVoice*)vs)->env1.level;
}

} // namespace

extern "C" const synth_engine_t g_engine_granular = {
    "granular",
    SYNTH_CAP_FILTER | SYNTH_CAP_ENV2 | SYNTH_CAP_LFO1 | SYNTH_CAP_LFO2 |
        SYNTH_CAP_MODMATRIX,
    sizeof(GranVoice),
    gran_init,
    gran_deinit,
    gran_begin_block,
    gran_voice_reset,
    gran_note_on,
    gran_note_off,
    gran_render,
    gran_busy,
    gran_level,
    nullptr, /* render_block (S28): fixed engines render per voice */
};
