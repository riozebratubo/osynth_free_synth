#!/usr/bin/env python3
"""S39b — `src` selector on both noise-reduction units: bus, or the input alone.

The problem the selector solves: audio_io_line_in_fx() has already summed the
input into the bus by the time fx_process() runs, so a unit on that bus cannot
tell the microphone from the synth and cleans both. Pointing it at the
microphone is therefore not a matter of reading somewhere else — the raw copy
is already in the mix and has to come back out.

What makes it exact is that both units are *corrections*:

    adaptive   the band sum is (g-1) x each band, i.e. already a difference
    fixed      filters + expander, so the difference is g*filt(x) - x

So the unit reproduces the block audio_io mixed in (audio_io_in_fx_block(),
added here), runs its DSP on that, and adds the difference to the bus. The bus
then carries a cleaned input beside a synth that is untouched sample for
sample — no second mix point, no reordering of the render chain.

    python tools/s39/input_source_selector.py [--check]
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

EDITS = []


def edit(path, marker, anchor, text, where="before"):
    EDITS.append((path, marker, anchor, text, where))


def replace(path, marker, old, new):
    EDITS.append((path, marker, old, new, "replace"))


# ------------------------------------------------------------ audio_io ----

AIOH = "components/audio_io/include/audio_io.h"

edit(AIOH, "audio_io_in_fx_block",
     "/* Picks and starts the output sink, then starts the audio task.", """\
/* The input exactly as audio_io_line_in_fx() mixed it into the bus this block,
 * written into `l` and `r` — overwritten, not accumulated. Returns false, and
 * leaves both untouched, when this build has no input, the RX half never came
 * up, or the fx position is silent. Both buffers must hold at least `frames`
 * floats, `frames` may not exceed SYNTH_BLOCK_SIZE, and the block is valid
 * only for the current one. Audio task only.
 *
 * This exists for one caller shape, and the shape is worth stating because it
 * is not the one the three stages above have: a *bus* unit that wants to
 * process the input alone and leave the synth beside it untouched — S39's two
 * noise-reduction units, whose whole point is cleaning a microphone without
 * putting a denoiser across the instrument. By the time the FX bus runs the
 * input has already been summed in, so such a unit cannot pull it back out.
 * It can, though, reproduce the exact block that was added, run its DSP on
 * that, and add the *difference* to the bus — which lands the same result and
 * needs no second mix point.
 *
 * The fx position and not a choice of the three, because it is the only one
 * already summed when the FX bus runs. `mon` and `dry` join afterwards, so
 * there is nothing on the bus to correct there: a caller gets false and should
 * do nothing rather than correct a signal that has not arrived.
 *
 * Follows `in.route`, `in.gain`, `in.source` and `in.micgain` down to the last
 * multiply — the opposite of audio_io_in_mono() above, which deliberately
 * ignores the first two. The difference is what each is for: that one names a
 * source to listen to, this one reproduces a mix that already happened, and a
 * correction that does not match what it is correcting is worse than none. */
bool audio_io_in_fx_block(float* l, float* r, size_t frames);

""")


AIOC = "components/audio_io/audio_io.cpp"

edit(AIOC, "bool SYNTH_RENDER_IRAM audio_io_in_fx_block",
     "const int16_t* SYNTH_RENDER_IRAM audio_io_line_in_block(void) {", """\
/* mix_in() into a cleared pair of buffers rather than a second copy of its
 * arithmetic: the value of this function is that it agrees with what was
 * actually mixed, and two expressions that have to agree are one expression
 * with a copy of it somewhere else. */
bool SYNTH_RENDER_IRAM audio_io_in_fx_block(float* l, float* r, size_t frames) {
    if (!s_in_ok || l == nullptr || r == nullptr) return false;
    if (frames > SYNTH_BLOCK_SIZE) frames = SYNTH_BLOCK_SIZE;
    /* Nothing was added at this position, so there is nothing to correct.
     * Reported rather than answered with a block of zeros: a caller that
     * corrected a silent block would still be running its filters on it, and
     * would have no way to tell "the input is quiet" from "the input is not
     * here". */
    if (s_in_g[kInFx] <= kInSilent) return false;
    memset(l, 0, frames * sizeof(float));
    memset(r, 0, frames * sizeof(float));
    mix_in(kInFx, l, r, frames);
    return true;
}

""")

replace(AIOC, "bool audio_io_in_fx_block(float*, float*, size_t)",
        "bool audio_io_in_mono(float*, size_t) { return false; }",
        "bool audio_io_in_mono(float*, size_t) { return false; }\n"
        "bool audio_io_in_fx_block(float*, float*, size_t) { return false; }")


# ---------------------------------------------------------------- fx.h ----

FXH = "components/fx/include/fx.h"

replace(FXH, "FX_PID_ANR_SRC",
        "#define FX_PID_ANR_LEARN   0x03E9",
        "#define FX_PID_ANR_LEARN   0x03E9\n"
        "#define FX_PID_ANR_SRC     0x03EA")

replace(FXH, "FX_PID_NR_SRC",
        "#define FX_PID_NR_RELEASE 0x03F8",
        "#define FX_PID_NR_RELEASE 0x03F8\n"
        "#define FX_PID_NR_SRC     0x03F9")

edit(FXH, "`fx.anr.src` / `fx.nr.src`",
     "/* Adaptive: a learned noise profile, subtracted per band.", """\
/* `fx.anr.src` / `fx.nr.src` (S39b) decide what each unit is looking at, and
 * are the difference between "clean up my microphone" and "put a denoiser
 * across my instrument". At `bus` — entry 0, so a patch saved before the
 * control existed still means what it did — the unit processes the finished
 * mix, which is the right thing for a noisy sampled loop and the wrong thing
 * for a synth that was never noisy.
 *
 * At `input` it processes only what audio_io mixed in, and the synth beside it
 * comes out sample for sample unchanged. That is exact rather than
 * approximate, and it is worth knowing why: both units are *corrections*. The
 * adaptive one's band sum is already (g-1) times each band, and the fixed
 * one's is g*filt(x) - x. Neither ever needed the bus in order to work — only
 * something to be a difference *from* — so pointing them at the block
 * audio_io_in_fx_block() hands back and adding the result to the bus replaces
 * the input's contribution and nothing else.
 *
 * `input` needs `in.route` = fx. That is the only position summed into the bus
 * by the time this stage runs; from `mon` or `dry` there is nothing here yet
 * to correct, and the unit stays inert rather than inventing a correction for
 * a signal that arrives later. */

""")


# -------------------------------------------------------------- fx.cpp ----

FXC = "components/fx/fx.cpp"

edit(FXC, "kNrSrcs[]", "/* Mains hum (S39). Append-only,", """\
/* What a noise-reduction unit is looking at (S39b). Shared by both units —
 * one list, one meaning — and append-only like every other enum here, with
 * entry 0 the behaviour they had before the control existed.
 *
 * `input` is the answer to "clean my microphone without putting a denoiser
 * across my synth": the unit runs on the block audio_io mixed in at the fx
 * position and adds only the difference back. It needs `in.route` = fx, and
 * is inert otherwise — the reasoning is above FX_PID_ANR_SRC in fx.h. */
const char* const kNrSrcs[] = {"bus", "input"};

""")

edit(FXC, "kNrSrcCount", "constexpr int kNrHumCount = count_of(kNrHum);",
     "\nconstexpr int kNrSrcCount = count_of(kNrSrcs);", where="after")

replace(FXC, "    ANR_ON, ANR_SRC,",
        """    ANR_ON, ANR_AMOUNT, ANR_FLOOR, ANR_BANDS, ANR_LOW, ANR_HIGH, ANR_ADAPT,
    ANR_ATTACK, ANR_RELEASE, ANR_LEARN,
    NR_ON, NR_HPF, NR_HUM, NR_THRESH, NR_RATIO, NR_FLOOR, NR_ATTACK, NR_HOLD,
    NR_RELEASE,""",
        """    ANR_ON, ANR_SRC, ANR_AMOUNT, ANR_FLOOR, ANR_BANDS, ANR_LOW, ANR_HIGH,
    ANR_ADAPT, ANR_ATTACK, ANR_RELEASE, ANR_LEARN,
    NR_ON, NR_SRC, NR_HPF, NR_HUM, NR_THRESH, NR_RATIO, NR_FLOOR, NR_ATTACK,
    NR_HOLD, NR_RELEASE,""")

# The two rows go straight after each unit's `on`, so the app draws the
# selector beside the bypass rather than at the end of a row of knobs.
replace(FXC, '"fx.anr.src"',
        """    {FX_PID_ANR_ON, "fx.anr.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},""",
        """    {FX_PID_ANR_ON, "fx.anr.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_ANR_SRC, "fx.anr.src", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kNrSrcCount - 1), 0.0f /* bus */, kNrSrcs, kNrSrcCount},""")

replace(FXC, '"fx.nr.src"',
        """    {FX_PID_NR_ON, "fx.nr.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},""",
        """    {FX_PID_NR_ON, "fx.nr.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_NR_SRC, "fx.nr.src", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kNrSrcCount - 1), 0.0f /* bus */, kNrSrcs, kNrSrcCount},""")


S39 = ROOT / "tools" / "s39"


def blk(name):
    return (S39 / name).read_text(encoding="utf-8")


edit(FXC, "s_nr_src_l[SYNTH_BLOCK_SIZE]", "AnrFx s_anr;", """\
/* One block of the input, stereo, exactly as audio_io mixed it into the bus
 * (S39b). Shared by both units because neither holds it across a call: each
 * fills it, uses it and is done, and the two run one after the other on the
 * audio task. Live only inside anr_process() and nr_process(). */
float s_nr_src_l[SYNTH_BLOCK_SIZE];
float s_nr_src_r[SYNTH_BLOCK_SIZE];

""")

replace(FXC, "if ((int)pv(ANR_SRC) == 1) {",
        """    /* The bank, and the correction it earned last block. bl[i] is read into a
     * local first: every band has to analyse the *input*, not what the bands
     * ahead of it have already been subtracted from. */
    for (size_t i = 0; i < frames; ++i) {
        const float xl = bl[i], xr = br[i];
        float cl = 0.0f, cr = 0.0f;
        for (int k = 0; k < n; ++k) {
            AnrBand& b = a.b[k];
            const float yl = osynth::dsp::svf_next(b.l, b.c, b.mode, xl);
            const float yr = osynth::dsp::svf_next(b.r, b.c, b.mode, xr);
            b.acc += fabsf(yl) + fabsf(yr);
            cl += b.d * yl;
            cr += b.d * yr;
            b.d += b.dstep;
        }
        bl[i] = xl + m * cl;
        br[i] = xr + m * cr;
    }
""",
        blk("anr_loop_new.txt"))

replace(FXC, "const bool from_input = ((int)pv(NR_SRC) == 1);",
        "    const float hpf = osynth::dsp::smooth_exp(c.s_hpf, pvm(NR_HPF));",
        blk("nr_fetch_new.txt") +
        "    const float hpf = osynth::dsp::smooth_exp(c.s_hpf, pvm(NR_HPF));")

replace(FXC, "Detected on what the unit is *looking at*",
        """    /* Pass 1: the filters, in place, and the detector on what leaves them. */
    float peak = 0.0f;
    for (size_t i = 0; i < frames; ++i) {
        float l = bl[i], r = br[i];
        if (use_hp) {
            l = osynth::dsp::svf_next(c.hp_l, c.hp_c, osynth::dsp::SvfMode::Hp,
                                      l);
            r = osynth::dsp::svf_next(c.hp_r, c.hp_c, osynth::dsp::SvfMode::Hp,
                                      r);
        }
        if (hum > 0) {
            for (int h = 0; h < kNrHumHarmonics; ++h) {
                l = osynth::dsp::svf_next(c.hum_l[h], c.hum_c[h],
                                          osynth::dsp::SvfMode::Notch, l);
                r = osynth::dsp::svf_next(c.hum_r[h], c.hum_c[h],
                                          osynth::dsp::SvfMode::Notch, r);
            }
        }
        bl[i] += m * (l - bl[i]);
        br[i] += m * (r - br[i]);
        const float al = fabsf(bl[i]), ar = fabsf(br[i]);
        const float t = (al > ar) ? al : ar;
        c.env += ((t > c.env) ? ka : kr) * (t - c.env);
        if (c.env > peak) peak = c.env;
    }
""",
        blk("nr_pass1_new.txt"))

replace(FXC, "would duck the synth along with it",
        """    /* Pass 2, and `m` folded into the gain exactly as the compressor folds
     * its mix: at m = 0 both this and the crossfade above are the identity. */
    const float g0 = 1.0f + m * (c.gain - 1.0f);
    const float g1 = 1.0f + m * (target - 1.0f);
    const float gstep = (g1 - g0) / (float)frames;
    float g = g0;
    for (size_t i = 0; i < frames; ++i) {
        g += gstep;
        bl[i] *= g;
        br[i] *= g;
    }
    c.gain = target;
""",
        blk("nr_pass2_new.txt"))

replace(FXC, "They were written for `src` = bus",
        """ * Two things guard it, and both exist because this runs on a *synth* bus,
 * where a held pad looks exactly like a fan:""",
        """ * Two things guard it. They were written for `src` = bus, where a held pad
 * looks exactly like a fan, and they are not dropped when the unit is pointed
 * at the input instead: a sung note, a bowed string and a guitar drone are all
 * steady for longer than a window, and a denoiser that learns one removes it.
 * The source changes what is at stake, not whether the test is right:""")


# ------------------------------------------------------- synth_params.h ----

replace("components/synth_core/include/synth_params.h", "375 since S39b",
        """     * fourteen, and 373 since S39 added the nineteen belonging to the two
     * noise-reduction units. Raised from 384 to 448 in""",
        """     * fourteen, and 373 since S39 added the nineteen belonging to the two
     * noise-reduction units — 375 since S39b gave each of those a source
     * selector. Raised from 384 to 448 in""")


# ------------------------------------------------------------------ run ----

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    rc = 0
    pending = 0
    for rel, marker, anchor, text, where in EDITS:
        path = ROOT / rel
        src = path.read_text(encoding="utf-8")
        if marker in src:
            print(f"  ok   {rel}: {marker!r} already present")
            continue
        pending += 1
        if src.count(anchor) != 1:
            print(f"  FAIL {rel}: anchor for {marker!r} found "
                  f"{src.count(anchor)} times")
            rc = 1
            continue
        if where == "replace":
            out = src.replace(anchor, text)
        else:
            out = (src.replace(anchor, text + anchor) if where == "before"
                   else src.replace(anchor, anchor + text))
        if args.check:
            print(f"  todo {rel}: would insert {marker!r}")
            continue
        path.write_text(out, encoding="utf-8")
        print(f"  +    {rel}: {marker!r}")

    if rc == 0 and pending == 0:
        print("nothing to do — S39b is already applied")
    return rc


if __name__ == "__main__":
    sys.exit(main())
