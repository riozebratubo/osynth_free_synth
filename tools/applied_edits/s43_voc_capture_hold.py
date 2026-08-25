#!/usr/bin/env python3
"""S43: make fx.voc.freeze capture the vowel you said, not the silence after it.

The bug, measured with tools/voc_sim.py: freezing stops the analysis at the
instant fx.voc.freeze goes high, and the band envelopes follow fx.voc.release
(25 ms by default). One word, then:

    finger leaves at   bands above the gate
    +0 ms                    12
    +50 ms                    2
    +100 ms                   0
    +200..500 ms              0

A person releases a hold button 200-500 ms after finishing a word, so the
capture was always empty. With every band gated shut the unit goes silent;
with the residue near-flat the bank sums back to roughly the carrier and the
synth is heard essentially unfiltered. Both were reported.

The repair is a peak-hold running alongside the followers while live, and a
snap to it on the freeze edge -- so the capture is the loudest thing said in
the last kVocHoldMs, which is what the gesture has always claimed to do. The
hold runs at block rate: the envelopes it tracks move on a millisecond scale
and a block is 1.33 ms, so per-sample would buy nothing for 16x the work.

Applied edit, kept per the project's convention. fx.cpp has mixed line
endings, so matches are line-ending-agnostic.
"""
import io
import re
import sys

PATH = 'components/fx/fx.cpp'


def replace_once(text, old, new):
    pat = re.compile(r'\r?\n'.join(re.escape(x) for x in old.split('\n')))
    matches = list(pat.finditer(text))
    if len(matches) != 1:
        raise SystemExit('expected 1 match, found %d for: %r'
                         % (len(matches), old[:70]))
    m = matches[0]
    nl = '\r\n' if '\r\n' in m.group(0) else '\n'
    return text[:m.start()] + nl.join(new.split('\n')) + text[m.end():]


def main():
    text = io.open(PATH, encoding='utf-8', newline='').read()

    # --- state ---------------------------------------------------------------
    text = replace_once(
        text,
        '''    float env = 0.0f;     /* follower output, held while frozen */''',
        '''    float env = 0.0f;     /* follower output, held while frozen */
    float hold = 0.0f;    /* peak-hold of `env`, what a freeze captures */''')

    text = replace_once(
        text,
        '''    float nrm = 1.0f;     /* fx.voc.norm scale, block rate, held on freeze */''',
        '''    float nrm = 1.0f;     /* fx.voc.norm scale, block rate, held on freeze */
    float hold_bb = 0.0f; /* peak-hold of env_bb, so a capture's nrm matches */
    bool was_frozen = false; /* freeze edge detector; see the capture note */''')

    # --- the constant --------------------------------------------------------
    text = replace_once(
        text,
        '''constexpr float kVocNormRiseMs = 300.0f;''',
        '''constexpr float kVocNormRiseMs = 300.0f;

/* How far back fx.voc.freeze reaches when it captures (ms).
 *
 * The gesture is "hold the button, speak, release", and it used to freeze the
 * band envelopes exactly as they stood the instant the parameter went high.
 * Those envelopes follow fx.voc.release -- 25 ms by default -- and nobody
 * lets go of a button while still making the sound: measured, one word then
 * silence leaves 12 bands above the gate at +0 ms, 2 at +50 ms and none at
 * +100 ms. A human releases at 200-500 ms, so the capture was reliably of the
 * silence *after* the word. Empty when the gate then shut every band, and an
 * unfiltered synth when the residue was flat enough that the bank summed back
 * to the carrier.
 *
 * So a peak-hold runs beside the followers while live, and the freeze edge
 * snaps the envelopes to it. 800 ms is long enough to span the gap between
 * finishing a word and letting go, short enough that a second capture does
 * not inherit the first. Block rate, not per sample: what it tracks moves on
 * a millisecond scale and a block is 1.33 ms. */
constexpr float kVocHoldMs = 800.0f;''')

    # --- reset on bypass -----------------------------------------------------
    text = replace_once(
        text,
        '''        for (int i = 0; i < kVocBandsMax; ++i) v.b[i].env = 0.0f;
        v.env_bb = 0.0f;
        v.env_car = 0.0f;
        v.nrm = 1.0f;
        return;''',
        '''        for (int i = 0; i < kVocBandsMax; ++i) {
            v.b[i].env = 0.0f;
            v.b[i].hold = 0.0f;
        }
        v.env_bb = 0.0f;
        v.env_car = 0.0f;
        v.hold_bb = 0.0f;
        v.nrm = 1.0f;
        /* Not was_frozen: the edge belongs to the parameter, not to the
         * bypass, and clearing it here would fire a capture of these zeroes
         * the first block after the unit comes back up. */
        return;''')

    # --- the edge: snap before this block synthesises ------------------------
    text = replace_once(
        text,
        '''    const float nrm = v.nrm;''',
        '''    /* Freeze edge. Done before the loop so this very block already plays the
     * captured vowel, and from the peak-hold so it is the vowel that was
     * actually said rather than whatever survived the release time constant.
     * nrm is recomputed from the held broadband peak for the same reason: it
     * would otherwise carry the normalisation of the silence the finger came
     * up in, which is its maximum, and the capture would play back that much
     * too loud. */
    if (frozen && !v.was_frozen) {
        for (int k = 0; k < n; ++k) v.b[k].env = v.b[k].hold;
        v.env_bb = v.hold_bb;
        if (norm) {
            v.nrm = kVocNormRef / fmaxf(v.hold_bb, kVocNormFloor);
        }
    }
    v.was_frozen = frozen;

    const float nrm = v.nrm;''')

    # --- the peak-hold itself, after the sample loop -------------------------
    text = replace_once(
        text,
        '''        /* Backstop, not a sound — see the staging note in the header. It is''',
        '''        /* Backstop, not a sound — see the staging note in the header. It is''')

    text = replace_once(
        text,
        '''        bl[i] += m * (wet - bl[i]);
        br[i] += m * (wet - br[i]);
    }
}''',
        '''        bl[i] += m * (wet - bl[i]);
        br[i] += m * (wet - br[i]);
    }

    /* Peak-hold, block rate: what fx.voc.freeze will capture if it is pressed
     * next block. Decays so a capture reflects the last kVocHoldMs rather
     * than the loudest thing since boot. */
    if (live) {
        const float hd =
            expf(-((float)frames / kSr) / (kVocHoldMs * 0.001f));
        for (int k = 0; k < n; ++k) {
            VocBand& b = v.b[k];
            b.hold = (b.env > b.hold) ? b.env : b.hold * hd;
        }
        v.hold_bb = (v.env_bb > v.hold_bb) ? v.env_bb : v.hold_bb * hd;
    }
}''')

    io.open(PATH, 'w', encoding='utf-8', newline='').write(text)
    print('applied')
    return 0


if __name__ == '__main__':
    sys.exit(main())
