#!/usr/bin/env python3
"""S43 fix: re-stage the vocoder against a frozen Q instead of invented constants.

The BpN change was right (it removes a Q^2 term that made four tone controls
into level controls) but the staging around it was wrong twice over:

  * fx.voc.gate maps to an ABSOLUTE threshold, `fx.voc.gate * 0.2`, calibrated
    against Bp-domain envelopes. BpN = Bp / Q_eff exactly, so the envelopes
    shrank 4.67x at the default settings while the gate did not -- most bands
    then fell under it and were skipped outright.
  * kVocBankGain=6 and kVocSibGain=3 were guesses, and fx.voc.level's default
    and range were re-scaled around them.

Since BpN = Bp / Q_eff exactly, substituting a CONSTANT for the varying Q_eff
reproduces the pre-S43 arithmetic term for term while keeping the win:

    old:  wet = Q * SUM(BpN_car * (Q*b.env - gate)) ; wet += sib ; wet *= level
    new:  identical, with kVocQRef frozen in place of Q

so fx.voc.gate and fx.voc.level keep their old meanings, defaults and ranges,
existing patches keep working, and the level no longer moves with q / bands /
span. Applied edit, kept per the project's convention.

fx.cpp has mixed line endings; every match is line-ending-agnostic.
"""
import io
import math
import re
import sys

PATH = 'components/fx/fx.cpp'


def qeff(bands, low, high, q01):
    ratio = (high / low) ** (1.0 / (bands - 1))
    q_nat = math.sqrt(ratio) / (ratio - 1.0)
    return q_nat * (0.4 + 1.6 * q01)


def replace_once(text, old, new):
    pat = re.compile(r'\r?\n'.join(re.escape(x) for x in old.split('\n')))
    matches = list(pat.finditer(text))
    if len(matches) != 1:
        raise SystemExit('expected 1 match, found %d for: %r'
                         % (len(matches), old[:70]))
    m = matches[0]
    nl = '\r\n' if '\r\n' in m.group(0) else '\n'
    return text[:m.start()] + nl.join(new.split('\n')) + text[m.end():]


CONSTS = '''/* The bank's frozen Q (S43). SvfMode::BpN is exactly SvfMode::Bp divided by
 * the band's Q, so every magnitude in this unit moved by that factor when the
 * banks changed -- including, fatally, its position relative to `fx.voc.gate`,
 * which is an absolute threshold and did not move with it. At the registered
 * defaults the envelopes shrank 4.67x while the gate stayed at 0.012, so most
 * bands sat below it and were skipped outright: quiet, and unintelligible
 * with it.
 *
 * The repair is not a new set of staging constants -- that was the first
 * attempt and it was two guesses wearing a comment. Because BpN = Bp / Q_eff
 * is an identity, putting a *constant* where Q_eff used to be reproduces the
 * old arithmetic term for term:
 *
 *     wet = Q * SUM(BpN_car * (Q*b.env - gate)),  then sibilance, then level
 *
 * with Q pinned here instead of derived per rebuild. fx.voc.gate and
 * fx.voc.level therefore keep the meanings, defaults and ranges they had
 * before S43 -- a stored patch is unaffected -- and the output no longer
 * tracks fx.voc.q, fx.voc.bands or the low/high span, which was the entire
 * point of the change.
 *
 * The value is Q_eff at the registered defaults: 16 bands over 150-7000 Hz at
 * q = 0.5. A 10-band build (no PSRAM) ran at 2.79 before and now runs here
 * too, so it is ~4.5 dB louder than it used to be. That is the dependence
 * being removed rather than a regression: the same patch now sounds the same
 * on both parts, which it never did. */
constexpr float kVocQRef = 4.671f;

/* Normalization (fx.voc.norm). kVocNormRef is the broadband follower value
 * the modulator is pinned to, so a speaker sitting at it gets nrm = 1 and
 * behaves exactly as the unit does with normalization off -- that is the
 * calibration point, and it is why turning the switch on is not also a level
 * change. kVocNormFloor caps the boost at ref/floor (20x, +26 dB) so a quiet
 * room between phrases is not lifted into the patch.
 *
 * The gate is subtracted before this, in the modulator's own domain, because
 * it describes the room rather than the voice. The trade-off is real and
 * worth knowing: a very quiet source has band envelopes near the gate, so it
 * is gated before it can be normalized up. fx.voc.gate is the control for
 * that, and lowering it is the answer to "the vocoder ignores me". */
constexpr float kVocNormRef = 0.10f;
constexpr float kVocNormFloor = 0.005f;'''


OLD_CONSTS = '''/* Bank staging. With BpN the bank sums back to roughly unity, so a band
 * envelope arrives at its natural size — a fraction of the modulator — and
 * the wet path needs a fixed lift to sit against the carrier it replaces.
 * Constant on purpose: it is the whole point of the BpN change that this does
 * not have to know the band count, the span or the Q — which also makes it the
 * one number to move if the unit comes out uniformly hot or uniformly quiet,
 * rather than re-tuning fx.voc.level's range around a staging error. */
constexpr float kVocBankGain = 6.0f;

/* Normalization (fx.voc.norm). kVocNormRef is the broadband follower value
 * the modulator is pinned to — a comfortably recorded voice, mean-rectified,
 * lands near it — and kVocNormFloor caps the boost at ref/floor (20x, +26 dB)
 * so a quiet room between phrases is not lifted into the patch. The gate runs
 * before this, so the floor is a second line of defence rather than the only
 * one. */
constexpr float kVocNormRef = 0.10f;
constexpr float kVocNormFloor = 0.005f;

/* The sibilance tap is the raw modulator, not a bank output, so it is staged
 * against the bank rather than with it: this is what puts a consonant at a
 * comparable loudness to the vowel beside it at fx.voc.sib = 1. */
constexpr float kVocSibGain = 3.0f;'''


def main():
    q16 = qeff(16, 150.0, 7000.0, 0.5)
    if abs(q16 - 4.671) > 0.002:
        raise SystemExit('kVocQRef drifted from the registered defaults: %.4f' % q16)

    text = io.open(PATH, encoding='utf-8', newline='').read()

    text = replace_once(text, OLD_CONSTS, CONSTS)

    # en folded the invented bank gain in; the frozen Q replaces it.
    text = replace_once(
        text,
        '''    /* The bank's staging folds into the envelope scale rather than riding on
     * the summed wet: `en` is positive either way, so the gate test below is
     * unchanged, and it saves a multiply per sample. */
    const float nrm = v.nrm;
    const float en = nrm * kVocBankGain;''',
        '''    const float nrm = v.nrm;''')

    text = replace_once(
        text,
        '            const float e = (b.env - gate) * en;',
        '            const float e = (b.env * kVocQRef - gate) * nrm;')

    # The bank's own output scale, the second of the two frozen-Q factors.
    text = replace_once(
        text,
        '''            wet += osynth::dsp::svf_next(b.car, v.cc[k],
                                         osynth::dsp::SvfMode::BpN, c) * e;
        }''',
        '''            wet += osynth::dsp::svf_next(b.car, v.cc[k],
                                         osynth::dsp::SvfMode::BpN, c) * e;
        }
        /* The synthesis bank's half of the frozen Q. Applied to the sum
         * rather than per band: identical result, one multiply. */
        wet *= kVocQRef;''')

    # Sibilance: pre-S43 had no extra gain, only `level`. Keep nrm so a
    # consonant does not grow louder as you lean in while the vowels do not.
    text = replace_once(
        text,
        '''                /* Normalized with the bank, for the same reason and by the
                 * same number: a consonant that grew louder as you leant in
                 * while the vowels did not would be worse than either. */
                wet += hp * sib * open * nrm * kVocSibGain;''',
        '''                /* Normalized with the bank, for the same reason and by the
                 * same number: a consonant that grew louder as you leant in
                 * while the vowels did not would be worse than either. No
                 * staging gain of its own -- it rides `level` with the bank,
                 * exactly as it did before S43, and the bank's frozen Q has
                 * already been applied above so the balance is preserved. */
                wet += hp * sib * open * nrm;''')

    io.open(PATH, 'w', encoding='utf-8', newline='').write(text)
    print('applied (kVocQRef = %.3f)' % q16)
    return 0


if __name__ == '__main__':
    sys.exit(main())
