#!/usr/bin/env python3
"""S43: fix the two things that stopped the vocoder speaking.

Both were measured with tools/voc_sim.py, an offline port of vocoder_process(),
rather than estimated -- estimating is what produced the previous two rounds.

1. fx.voc.gate defaulted to 0.06, which is a 0.012 absolute floor under band
   envelopes that run 0.02-0.09 for ordinary speech. Measured against a
   speech-like modulator, that leaves 38-45% of the modulator's spectral
   envelope standing; the other half to two thirds is the message. 0.02 keeps
   77-81% and still cuts the between-words leakage about threefold in a quiet
   room. This was the pre-S43 default too, so it is a long-standing value that
   only became visible once anyone tried to be understood through the unit.

2. The noise carrier was summed at a hard-coded 0.5. Noise is not comparable
   to a pitched carrier at equal amplitude through a filter bank: a saw has
   energy only at its harmonics, so most bands see very little, while white
   noise feeds every band fully. Measured, 0.5 noise produced 1.19x the bank
   output of a 0.4 saw -- so `bus+noise` was mostly noise, and the three
   presets using it sat at 0.95-0.97 peak and into soft_clip. 0.25 puts it at
   about 0.6x the bus, which is a supplement rather than a takeover.

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

    # --- 1. the gate default -------------------------------------------------
    text = replace_once(
        text,
        '''    {FX_PID_VOC_GATE, "fx.voc.gate", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.06f, nullptr, 0},   /* modulator noise floor */''',
        '''    /* 0.02, not the 0.06 this defaulted to through S42. The control is an
     * absolute floor subtracted from every band envelope, and speech runs
     * band envelopes of roughly 0.02-0.09 in this unit's domain, so 0.06
     * (a 0.012 floor) was standing in the middle of the voice rather than
     * under it: measured against a speech-like modulator it left 38-45% of
     * the spectral envelope, and the two thirds it removed is precisely the
     * information that makes words legible. 0.02 keeps 77-81% and still cuts
     * the between-words leakage about threefold in a quiet room.
     *
     * Turn it back up when the room is the problem rather than the words --
     * that is what it is for -- but reach for fx.mnr first, which was built
     * to remove noise from *under* the voice instead of gating around it. */
    {FX_PID_VOC_GATE, "fx.voc.gate", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.02f, nullptr, 0},   /* modulator noise floor */''')

    # --- 2. the noise carrier's level ---------------------------------------
    text = replace_once(
        text,
        '''/* Vocoder carrier (S38). Append-only. `noise` alone is what makes a whisper''',
        '''/* How much noise the `noise` and `bus+noise` carriers add (S43). Not 0.5,
 * which is what this was, and the difference is not a taste call: noise and a
 * pitched carrier are not comparable at equal amplitude through a filter
 * bank. A saw carries energy only at its harmonics, so most bands see very
 * little of it; white noise feeds every band fully. Measured through this
 * bank, noise at 0.5 produced 1.19x the output of a saw at 0.4 -- so
 * `bus+noise` was mostly noise, and every preset using it ran into the output
 * clipper. At 0.25 it sits around 0.6x the bus: a supplement for the
 * unpitched consonants, which is what it is for, rather than a takeover. */
constexpr float kVocNoiseGain = 0.25f;

/* Vocoder carrier (S38). Append-only. `noise` alone is what makes a whisper''')

    text = replace_once(
        text,
        '        if (use_noise) c += 0.5f * osynth::dsp::noise_next(v.rng);',
        '        if (use_noise) c += kVocNoiseGain * osynth::dsp::noise_next(v.rng);')

    io.open(PATH, 'w', encoding='utf-8', newline='').write(text)
    print('applied')
    return 0


if __name__ == '__main__':
    sys.exit(main())
