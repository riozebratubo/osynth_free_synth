#!/usr/bin/env python3
"""S36: rebuild fx.cpp's reverb unit around the four-algorithm dispatch.

Kept because it documents exactly what the mechanical part of the S36 reverb
change was: the ReverbFx struct grew the selector plus the shared pre/post
stages, and reverb_process() was split into freeverb_render() (the old body,
wet-only) plus a dispatch that treats freeverb as one algorithm among four.

Idempotent: re-running after a successful run is a no-op, because the anchors
it looks for are gone. Run from the repo root:  python tools/s36_patch_reverb_dispatch.py
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FX = ROOT / "components" / "fx" / "fx.cpp"

OLD_STRUCT = """struct ReverbFx {
    Comb cl[8], cr[8];
    Line al[4], ar[4];
    osynth::dsp::Smooth s_fb, s_damp;
    UnitState u;
    bool ok = false;
};

ReverbFx s_rev;
Line* s_rev_lines[24]; /* every line, for the bypass scrub */
"""

NEW_STRUCT = """struct ReverbFx {
    Comb cl[8], cr[8];
    Line al[4], ar[4];
    osynth::dsp::Smooth s_fb, s_damp;
    UnitState u;
    bool ok = false;

    /* ---- S36: the algorithm selector and the stages shared across it ---- */

    /* 0xFF so the first block always takes the switch path and populates
     * s_rev_lines; there is no valid algorithm this can be mistaken for. */
    uint8_t algo = 0xFF;
    uint8_t nlines = 0;
    /* A second scrub cursor, used only while changing algorithm. It is
     * separate from `u` because the two scrubs happen for different reasons
     * and can be in flight at once: `u` clears a unit the player switched
     * off, this clears a topology nobody is listening to yet. Sharing one
     * cursor would have the later reason silently cancel the earlier one. */
    UnitState sw;
    bool switching = false;

    Line pre_l, pre_r;                 /* shared pre-delay, in front of all */
    float tone_l = 0.0f, tone_r = 0.0f;
    osynth::dsp::Smooth s_pre, s_tone, s_width;
};

ReverbFx s_rev;

/* Whether each optional algorithm got its buffers at boot. Checked by
 * rev_impl() so a selection that cannot render falls back rather than
 * going silent — the same sink-fallback rule the rest of the bus follows. */
bool s_rev_wet_ok = false;
#if SYNTH_ENABLE_FX_GPL
bool s_rev_mverb_ok = false;
bool s_rev_dusk_ok = false;
#endif

/* Every line of whichever algorithm is selected, for the bypass scrub.
 * Sized for the largest of the four (DuskVerb, at 38) with room to spare —
 * the array is 192 bytes and being one entry short of an algorithm's line
 * count would leave a tail unscrubbed, which is a stale-audio bug that only
 * shows up the second time you enable the effect. */
constexpr size_t kRevLineMax = 48;
Line* s_rev_lines[kRevLineMax];
"""

NEW_BODY = '''/* Freeverb's own render, wet only, so the dispatch below has one shape for
 * all four algorithms. Byte-for-byte the arithmetic it has always run — the
 * only change is that the dry/wet mix moved out to the caller, which does it
 * identically for every algorithm. */
void SYNTH_RENDER_IRAM freeverb_render(ReverbFx& v, const float* il,
                                       const float* ir, float* wl, float* wr,
                                       size_t frames, float fb, float damp) {
    for (size_t i = 0; i < frames; ++i) {
        const float in = (il[i] + ir[i]) * kRevInGain;
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
        wl[i] = sl;
        wr[i] = sr;
    }
}

/* The implementation behind an algorithm index, or nullptr for freeverb,
 * which lives in this file and has no RevAlgorithm to hand back. Also
 * nullptr for an algorithm whose init() failed, which is how a board short
 * of PSRAM ends up refusing a selection instead of rendering silence. */
osynth::fx::RevAlgorithm* rev_impl(int algo) {
    switch (algo) {
        case kAlgoWet:
            return s_rev_wet_ok ? osynth::fx::wetreverb_instance() : nullptr;
#if SYNTH_ENABLE_FX_GPL
        case kAlgoMVerb:
            return s_rev_mverb_ok ? osynth::fx::mverb_instance() : nullptr;
        case kAlgoDusk:
            return s_rev_dusk_ok ? osynth::fx::duskverb_instance() : nullptr;
#endif
        default:
            return nullptr;
    }
}

/* Rebuilds s_rev_lines for `algo` and returns how many entries it holds. */
size_t rev_collect_lines(int algo) {
    if (algo == kAlgoFreeverb) {
        size_t n = 0;
        for (int i = 0; i < 8; ++i) {
            s_rev_lines[n++] = &s_rev.cl[i].line;
            s_rev_lines[n++] = &s_rev.cr[i].line;
        }
        for (int i = 0; i < 4; ++i) {
            s_rev_lines[n++] = &s_rev.al[i];
            s_rev_lines[n++] = &s_rev.ar[i];
        }
        return n;
    }
    osynth::fx::RevAlgorithm* a = rev_impl(algo);
    return a != nullptr ? a->lines(s_rev_lines, kRevLineMax) : 0;
}

/* Wet scratch. One block, stereo — the algorithms write here and the shared
 * post stages and the dry/wet mix read it back, so no algorithm has to know
 * what the dry signal was. */
float s_rev_wl[SYNTH_BLOCK_SIZE];
float s_rev_wr[SYNTH_BLOCK_SIZE];

void SYNTH_RENDER_IRAM reverb_process(float* __restrict__ bl,
                                      float* __restrict__ br, size_t frames) {
    ReverbFx& v = s_rev;
    if (!v.ok) return;
    if (frames > SYNTH_BLOCK_SIZE) return; /* scratch is one block wide */

    /* An out-of-range or unavailable selection falls back to freeverb rather
     * than to silence: on a build without CONFIG_OSYNTH_FX_GPL the store
     * clamps 2 and 3 down to 1 already, so reaching here means the algorithm
     * exists but could not allocate, and a reverb the player can hear beats
     * a reverb that is correct about being absent. */
    int algo = (int)pv(REV_ALGO);
    if (algo < 0 || algo >= kRevAlgoCount) algo = kAlgoFreeverb;
    osynth::fx::RevAlgorithm* impl = rev_impl(algo);
    if (algo != kAlgoFreeverb && impl == nullptr) algo = kAlgoFreeverb;

    const float m =
        unit_gate(v.u, gated(pv(REV_ON), pvm(REV_MIX)), s_rev_lines, v.nlines);
    if (m < 0.0f) {
        for (int i = 0; i < 8; ++i) v.cl[i].store = v.cr[i].store = 0.0f;
        v.tone_l = v.tone_r = 0.0f;
        return;
    }

    /* Changing topology mid-tail. The new algorithm's lines hold whatever the
     * last selection left in them, and there is no arrangement of a few
     * hundred KB of memset that fits in a 1.33 ms block, so the unit goes
     * quiet and scrubs a chunk per block — about 40 ms at the largest
     * algorithm. A brief gap on an algorithm change is what the plugins these
     * came from do too; a burst of somebody else's reverb tail is not. */
    if (algo != v.algo) {
        v.algo = (uint8_t)algo;
        v.nlines = (uint8_t)rev_collect_lines(algo);
        if (impl != nullptr) {
            impl->reset();
        } else {
            for (int i = 0; i < 8; ++i) v.cl[i].store = v.cr[i].store = 0.0f;
        }
        v.tone_l = v.tone_r = 0.0f;
        v.sw.sl = 0;
        v.sw.sp = 0;
        v.switching = true;
    }
    if (v.switching) {
        if (scrub_step(v.sw, s_rev_lines, v.nlines)) v.switching = false;
        return; /* dry through; the wet is not ready to be heard */
    }

    /* ---- shared front: pre-delay ---- */
    const float pre_ms = osynth::dsp::smooth_lin(v.s_pre, pvm(REV_PRE));
    const float pre_d = pre_ms * 0.001f * (float)kSr;
    const bool do_pre = pre_d >= 1.0f;
    for (size_t i = 0; i < frames; ++i) {
        const float dl = bl[i], dr = br[i];
        if (do_pre) {
            s_rev_wl[i] = line_read_frac(v.pre_l, pre_d);
            s_rev_wr[i] = line_read_frac(v.pre_r, pre_d);
        } else {
            s_rev_wl[i] = dl;
            s_rev_wr[i] = dr;
        }
        /* Kept primed even while bypassed, so dialling pre-delay up from zero
         * fades in real signal instead of a hole the length of the delay. */
        line_push(v.pre_l, dl);
        line_push(v.pre_r, dr);
    }

    /* ---- the algorithm ---- */
    float makeup = 1.0f;
    bool comp = false;
    if (algo == kAlgoFreeverb) {
        /* Both feed the comb loop per sample: a raw jump steps the running
         * tail (a size change is audible as a click on a long decay).
         * Smoothed S21. */
        const float fb =
            0.70f + 0.28f * osynth::dsp::smooth_lin(v.s_fb, pvm(REV_SIZE));
        const float damp =
            0.95f * osynth::dsp::smooth_lin(v.s_damp, pvm(REV_DAMP));

        /* kRevWet is folded into the wet gain here rather than multiplied per
         * sample below, which is where the make-up joins it. */
        makeup = kRevWet;
        comp = pv(REV_COMP) >= 0.5f;
        if (comp) {
            const float g = sqrtf(fmaxf(1.0f - fb * fb, 1e-4f)) /
                            (kRevRefGain * kRevCombSum);
            makeup = kRevWet * fminf(fmaxf(g, kRevCompMin), kRevCompMax);
        }
        freeverb_render(v, s_rev_wl, s_rev_wr, s_rev_wl, s_rev_wr, frames, fb,
                        damp);
    } else {
        /* The other three are level-matched by construction and take the
         * knobs raw: each smooths internally what it needs to, and their
         * `size` moves delay lengths rather than a feedback coefficient, so
         * the S21 argument for smoothing here does not apply to them.
         *
         * fx.rev.comp stays freeverb-only. It undoes one specific staging
         * decision in *that* algorithm — see the derivation above — and has
         * nothing to undo in the others. */
        const osynth::fx::RevParams rp = {pvm(REV_SIZE), pvm(REV_DAMP),
                                          pvm(REV_DIFF), pvm(REV_EARLY)};
        impl->render(s_rev_wl, s_rev_wr, s_rev_wl, s_rev_wr, frames, rp);
    }

    /* ---- shared back: tone, then width ---- */
    const float tone_hz = osynth::dsp::smooth_exp(v.s_tone, pvm(REV_TONE));
    if (tone_hz < kRevToneOpen * 0.999f) {
        /* One-pole, wet only. A tone control on the dry path would be an EQ,
         * and the bus already has one of those. */
        const float c = 1.0f - expf(-2.0f * (float)M_PI * tone_hz / (float)kSr);
        for (size_t i = 0; i < frames; ++i) {
            v.tone_l += c * (s_rev_wl[i] - v.tone_l);
            v.tone_r += c * (s_rev_wr[i] - v.tone_r);
            s_rev_wl[i] = v.tone_l;
            s_rev_wr[i] = v.tone_r;
        }
    }

    const float width = osynth::dsp::smooth_lin(v.s_width, pvm(REV_WIDTH));
    if (fabsf(width - 1.0f) > 1e-4f) {
        /* Skipped rather than run at unity: a mid/side round trip at width 1
         * is only *almost* the identity in float, and "almost" is the
         * difference between a pre-S36 patch rendering identically and
         * rendering nearly identically. */
        for (size_t i = 0; i < frames; ++i) {
            const float mid = (s_rev_wl[i] + s_rev_wr[i]) * 0.5f;
            const float side = (s_rev_wl[i] - s_rev_wr[i]) * 0.5f * width;
            s_rev_wl[i] = mid + side;
            s_rev_wr[i] = mid - side;
        }
    }

    const MixGains mg = mix_gains(m, comp, makeup);
    for (size_t i = 0; i < frames; ++i) {
        bl[i] = mg.dry * bl[i] + mg.wet * s_rev_wl[i];
        br[i] = mg.dry * br[i] + mg.wet * s_rev_wr[i];
    }
}

'''

BODY_START = "void SYNTH_RENDER_IRAM reverb_process(float* __restrict__ bl,"
BODY_END = "/* ---- bitcrush: bit-depth quantize + sample-rate divide (S17) ----"


def main() -> int:
    src = FX.read_text(encoding="utf-8")
    if OLD_STRUCT not in src:
        print("nothing to do: ReverbFx already carries the S36 selector")
        return 0
    src = src.replace(OLD_STRUCT, NEW_STRUCT)
    start = src.index(BODY_START)
    end = src.index(BODY_END)
    src = src[:start] + NEW_BODY + src[end:]
    FX.write_text(src, encoding="utf-8")
    print("fx.cpp: reverb unit rebuilt around the four-algorithm dispatch")
    return 0


if __name__ == "__main__":
    sys.exit(main())
