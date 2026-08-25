#!/usr/bin/env python3
"""S43: repair fx.voc.norm, and add fx.voc.clarity for the consonant path.

fx.voc.norm was reported as "much clipping, worse than just using the
compressor", and it was: three separate faults, all measured with
tools/voc_sim.py rather than reasoned about.

  a) kVocNormRef was 0.10, but env_bb for an ordinary mic signal sits at
     0.02-0.04. The reference was above anything a voice produces, so nrm was
     a boost essentially always and never a cut -- the switch added ~+10 dB
     and clipped, instead of normalising. 0.04 is where peak-with-norm-on
     matches peak-with-norm-off, which is what the switch should do to a
     reference-level source: nothing.

  b) kVocNormFloor allowed a 20x boost (ref/floor). Measured, nrm reached the
     full 20.0 and roughly doubled the output peak. 0.02 caps it at 2x.

  c) nrm rose as fast as env_bb decayed -- an AGC with no release at all. When
     a word ends env_bb collapses while high-Q band envelopes are still
     ringing, so the ring-out got multiplied by the rising gain: a swell after
     every word, which is the clipping that was heard. Fixed with the limiter
     idiom already used elsewhere on this bus: instant down, slow up.

fx.voc.clarity (0x03DF, the vocoder block's last free id) is the consonant
lever, switchable so the two can be compared. It changes the sibilance
presence test, which had two problems of its own: it saturated at env_bb =
0.125 (`bb * 8.0f`), which is far above where a voice sits, so it attenuated
consonants to a measured mean of 0.07; and it used RAW env_bb, so alone among
everything in this unit it was not loudness-normalised -- a quiet speaker got
the bank normalised up while their consonants stayed buried.

Applied edit, kept per the project's convention. fx.cpp has mixed line
endings, so matches are line-ending-agnostic.
"""
import io
import re
import sys

PATH = 'components/fx/fx.cpp'
HDR = 'components/fx/include/fx.h'


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
    # ---- fx.h: the new id ---------------------------------------------------
    h = io.open(HDR, encoding='utf-8', newline='').read()
    h = replace_once(
        h,
        '''#define FX_PID_VOC_NORM    0x03DE
/* 0x03DF is the last id in this block. */''',
        '''#define FX_PID_VOC_NORM    0x03DE
/* Consonant clarity (S43). The sibilance path's presence test saturates well
 * above any level a voice reaches, and is the one quantity in the unit that
 * is not loudness-normalised; this switches both, and lifts the tap. It is a
 * switch rather than a knob because it is a taste fork -- more intelligible
 * against more "unmistakably a vocoder" -- and those are worth A/B-ing. */
#define FX_PID_VOC_CLARITY 0x03DF
/* This block is now full. */''')
    io.open(HDR, 'w', encoding='utf-8', newline='').write(h)

    text = io.open(PATH, encoding='utf-8', newline='').read()

    # ---- normalisation constants -------------------------------------------
    text = replace_once(
        text,
        '''constexpr float kVocNormRef = 0.10f;
constexpr float kVocNormFloor = 0.005f;''',
        '''constexpr float kVocNormRef = 0.04f;
constexpr float kVocNormFloor = 0.02f;

/* How fast the normaliser is allowed to turn UP, in ms. Down is instantaneous
 * (the look-ahead below sees the block before it is used), which is what stops
 * a syllable opening with a burst; up has to be slow, and originally was not
 * slow at all -- nrm simply tracked 1/env_bb, so it rose as fast as env_bb
 * decayed. That is an AGC with no release: when a word ends env_bb collapses
 * while the high-Q band envelopes are still ringing, and the ring-out gets
 * multiplied by the rising gain. A swell after every word, which is what
 * "fx.voc.norm clips" turned out to be. 300 ms is slower than any band
 * envelope's release (fx.voc.release tops out at 500 ms, but the useful range
 * is tens of ms), so the gain can no longer outrun the decay it is dividing. */
constexpr float kVocNormRiseMs = 300.0f;''')

    # ---- clarity constants --------------------------------------------------
    text = replace_once(
        text,
        '''/* ---- the microphone chain is not IRAM-resident (S43) ----''',
        '''/* fx.voc.clarity (S43): the consonant path, and what is wrong with it off.
 *
 * The sibilance tap is gated by a presence test, `open`, which asks whether
 * the modulator is saying anything and whether there is a carrier to say it
 * on. Two things about the first half are wrong, and both only became visible
 * once anyone tried to be understood through the unit:
 *
 *   - it saturates at env_bb = 0.125 (`bb * 8.0f`), and an ordinary mic signal
 *     puts env_bb at 0.02-0.04. So it never came close to opening: measured
 *     over a speech-like modulator its mean value is 0.07, which is -23 dB on
 *     the consonants;
 *   - it reads RAW env_bb, so it is the one quantity in this unit that
 *     fx.voc.norm does not reach. A quiet speaker got the bank normalised up
 *     to reference while their consonants stayed exactly as buried.
 *
 * With clarity on the test is taken in the normalised domain and scaled to
 * saturate at the reference loudness rather than three times above it, and
 * the tap is lifted by kVocClarityGain. Off is the pre-S43 behaviour exactly,
 * so the two can be A/B'd, which is the point: this is a taste fork. More
 * consonant is more intelligible and also more raw microphone -- at some
 * point you stop hearing a vocoder and start hearing a voice with a synth
 * behind it, and where that point sits is not something a default can know.
 *
 * kVocClarityGain is sized so consonants land 6-10 dB under the vowels during
 * a burst, measured; it is not a taste value and moving it wants voc_sim. */
constexpr float kVocClarityGain = 0.35f;

/* ---- the microphone chain is not IRAM-resident (S43) ----''')

    # ---- kParams row --------------------------------------------------------
    text = replace_once(
        text,
        '''    {FX_PID_VOC_NORM, "fx.voc.norm", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0},''',
        '''    {FX_PID_VOC_NORM, "fx.voc.norm", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0},
    /* Off by default: it is the classic-vocoder/legible-speech fork, and the
     * classic sound is what the unit is for out of the box. The preset card
     * turns it on for the sets that are about words. */
    {FX_PID_VOC_CLARITY, "fx.voc.clarity", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},''')

    text = replace_once(
        text,
        '''    VOC_RELEASE, VOC_SHIFT, VOC_SIB, VOC_GATE, VOC_NORM, VOC_LEVEL,
    VOC_CARRIER, VOC_FREEZE,''',
        '''    VOC_RELEASE, VOC_SHIFT, VOC_SIB, VOC_GATE, VOC_NORM, VOC_CLARITY,
    VOC_LEVEL, VOC_CARRIER, VOC_FREEZE,''')

    # ---- the normaliser itself ---------------------------------------------
    text = replace_once(
        text,
        '''    const bool norm = pv(VOC_NORM) >= 0.5f;
    if (live && norm && frames > 0) {
        float sum = 0.0f;
        for (size_t i = 0; i < frames; ++i) sum += fabsf(s_voc_mod[i]);
        const float blk = sum / (float)frames;
        const float ref = (blk > v.env_bb) ? blk : v.env_bb;
        v.nrm = kVocNormRef / fmaxf(ref, kVocNormFloor);
    } else if (!frozen) {''',
        '''    const bool norm = pv(VOC_NORM) >= 0.5f;
    if (live && norm && frames > 0) {
        float sum = 0.0f;
        for (size_t i = 0; i < frames; ++i) sum += fabsf(s_voc_mod[i]);
        const float blk = sum / (float)frames;
        const float ref = (blk > v.env_bb) ? blk : v.env_bb;
        const float target = kVocNormRef / fmaxf(ref, kVocNormFloor);
        /* Instant down, slow up -- the limiter idiom, and here for the same
         * reason: a gain that is allowed to rise as fast as its divisor falls
         * will amplify every decay it is supposed to be levelling. */
        if (target < v.nrm) {
            v.nrm = target;
        } else {
            const float k = 1.0f - expf(-((float)frames / kSr) /
                                        (kVocNormRiseMs * 0.001f));
            v.nrm += k * (target - v.nrm);
        }
    } else if (!frozen) {''')

    # ---- the presence test --------------------------------------------------
    text = replace_once(
        text,
        '''                const float open = fminf(bb * 8.0f, 1.0f) *
                                   fminf(v.env_car * 20.0f, 1.0f);''',
        '''                /* Normalised and scaled to saturate at the reference
                 * loudness when clarity is on; the raw, far-too-high original
                 * when it is off. See the kVocClarityGain note. */
                const float open =
                    (clarity ? fminf(bb * nrm * (1.0f / kVocNormRef), 1.0f)
                             : fminf(bb * 8.0f, 1.0f)) *
                    fminf(v.env_car * 20.0f, 1.0f);''')

    text = replace_once(
        text,
        '''                wet += hp * sib * open * nrm;''',
        '''                wet += hp * sib * open * nrm *
                       (clarity ? kVocClarityGain : 1.0f);''')

    text = replace_once(
        text,
        '''    const int carrier = (int)pv(VOC_CARRIER);''',
        '''    const bool clarity = pv(VOC_CLARITY) >= 0.5f;
    const int carrier = (int)pv(VOC_CARRIER);''')

    io.open(PATH, 'w', encoding='utf-8', newline='').write(text)
    print('applied')
    return 0


if __name__ == '__main__':
    sys.exit(main())
