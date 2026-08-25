#!/usr/bin/env python3
"""S43 fix: capture a same-instant spectral snapshot, not a per-band maximum.

The previous edit (s43_voc_capture_hold.py) replaced "captures the silence
after the word" with "captures a flat spectrum", which is worse: it was
reported as the notes coming out as the unprocessed synth patch.

Why a per-band peak-hold cannot work. Each band tracked its own maximum
independently, so the bands' peaks came from *different moments* -- the union
of every vowel said while the button was down. Measured over a two-vowel
phrase, the held set across the speech region was:

    0.51 0.70 0.79 0.95 0.61 0.74 1.00 0.92 0.67 0.37 ...

against a real instant's

    0.50 0.70 0.83 1.00 0.62 0.46 0.32 0.21 0.15 0.11 ...

i.e. roughly 2:1 where a vowel is 5:1. A flat set of band gains is a flat
filter, and a flat filter passes the carrier through unchanged. The longer the
button is held and the more that is said, the flatter it gets.

A vowel IS its formant structure, and that structure only exists across the
bank at one moment. So the snapshot has to be atomic: when the broadband
follower reaches a new peak, copy every band's envelope at once. What is
captured is then the bank exactly as it stood at the loudest instant of the
phrase, which is a vowel someone actually said.

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

    text = replace_once(
        text,
        '''    float hold = 0.0f;    /* peak-hold of `env`, what a freeze captures */''',
        '''    float hold = 0.0f;    /* this band at the snapshot instant (see below) */''')

    text = replace_once(
        text,
        '''    float hold_bb = 0.0f; /* peak-hold of env_bb, so a capture's nrm matches */''',
        '''    float hold_bb = 0.0f; /* env_bb at the snapshot instant; also its trigger */''')

    text = replace_once(
        text,
        '''/* How far back fx.voc.freeze reaches when it captures (ms).''',
        '''/* How far back fx.voc.freeze reaches when it captures (ms), and how long a
 * captured snapshot stands before a quieter moment may replace it.''')

    text = replace_once(
        text,
        ''' * So a peak-hold runs beside the followers while live, and the freeze edge
 * snaps the envelopes to it. 800 ms is long enough to span the gap between
 * finishing a word and letting go, short enough that a second capture does
 * not inherit the first. Block rate, not per sample: what it tracks moves on
 * a millisecond scale and a block is 1.33 ms. */''',
        ''' * So a snapshot runs beside the followers while live, and the freeze edge
 * snaps the envelopes to it. 800 ms is long enough to span the gap between
 * finishing a word and letting go, short enough that a second capture does
 * not inherit the first. Block rate, not per sample: what it tracks moves on
 * a millisecond scale and a block is 1.33 ms.
 *
 * It is a SNAPSHOT of the whole bank at one instant, not a peak-hold per
 * band, and that distinction is the difference between this working and not.
 * A per-band maximum takes each band's peak from a different moment, so what
 * it holds is the union of every vowel said while the button was down --
 * measured over a two-vowel phrase it came out roughly 2:1 across the speech
 * region where a real vowel is 5:1. A flat set of band gains is a flat
 * filter, and a flat filter passes the carrier straight through: the reported
 * symptom was the notes sounding like the untouched synth patch, and it got
 * worse the longer the button was held. A vowel *is* its formant structure,
 * and that structure only exists across the bank at one moment. */''')

    text = replace_once(
        text,
        '''    /* Peak-hold, block rate: what fx.voc.freeze will capture if it is pressed
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
}''',
        '''    /* The snapshot, block rate: what fx.voc.freeze will capture if it is
     * pressed next block.
     *
     * The broadband follower is both the trigger and the reference. When it
     * reaches a new peak the whole bank is copied *at once* -- that atomicity
     * is the point, and taking each band's own maximum instead is what made
     * an earlier version of this capture a flat spectrum. Otherwise the
     * reference decays, so after kVocHoldMs a quieter moment can claim the
     * snapshot and a capture reflects the recent phrase rather than the
     * loudest thing said since boot.
     *
     * Silence cannot claim it: env_bb falls to the room floor there, far
     * under a decaying reference, so the vowel stands through the 200-500 ms
     * it takes to let go of the button. That is the whole gesture. */
    if (live) {
        if (v.env_bb > v.hold_bb) {
            v.hold_bb = v.env_bb;
            for (int k = 0; k < n; ++k) v.b[k].hold = v.b[k].env;
        } else {
            v.hold_bb *= expf(-((float)frames / kSr) / (kVocHoldMs * 0.001f));
        }
    }
}''')

    io.open(PATH, 'w', encoding='utf-8', newline='').write(text)
    print('applied')
    return 0


if __name__ == '__main__':
    sys.exit(main())
