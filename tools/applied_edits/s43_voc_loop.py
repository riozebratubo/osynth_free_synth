#!/usr/bin/env python3
"""S43: fx.voc.loop -- play a captured phrase once, or round and round.

Once (the default, and what the capture did when it was first built) suits a
spoken phrase: each note says it and stops. Looping suits a rhythmic one --
a syllable, a beatboxed pattern -- where the point is that it keeps going
under a held chord.

The id is borrowed from the LFO2 block's tail, and 0x03CF specifically because
it sits immediately below FX_PID_VOC_ON at 0x03D0: the vocoder's run simply
extends downward by one and stays contiguous. The LFO blocks cannot grow into
it -- there are exactly two LFOs of six parameters each, fixed by the two
kParams runs and the two-element s_lfo array -- so this is dead space rather
than someone else's headroom. Same reasoning the mic NR used for bitcrush's
tail and the limiter for the stereo stage's.

Applied edit, kept per the project's convention. Both files have mixed line
endings, so matches are line-ending-agnostic.
"""
import io
import re
import sys

FX = 'components/fx/fx.cpp'
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
    # ---- fx.h ---------------------------------------------------------------
    h = io.open(HDR, encoding='utf-8', newline='').read()

    h = replace_once(
        h,
        '#define FX_PID_VOC_ON      0x03D0',
        '''/* Capture playback (S43): 0 plays the recorded phrase once per note, 1 loops
 * it until the note ends. Borrowed from the LFO2 block's tail, and 0x03CF
 * rather than 0x03CE because it sits immediately below FX_PID_VOC_ON: the
 * vocoder's run extends downward by one and stays contiguous. The LFO blocks
 * cannot grow into it -- there are exactly two LFOs of six parameters each --
 * so this is dead space, not someone else's headroom. */
#define FX_PID_VOC_LOOP    0x03CF

#define FX_PID_VOC_ON      0x03D0''')

    h = replace_once(
        h,
        '''#define FX_PID_VOC_CLARITY 0x03DF
/* This block is now full. */''',
        '''#define FX_PID_VOC_CLARITY 0x03DF
/* This block is full; FX_PID_VOC_LOOP above is the overflow, and 0x03CE is
 * the last id adjacent to it. */''')
    io.open(HDR, 'w', encoding='utf-8', newline='').write(h)

    # ---- fx.cpp -------------------------------------------------------------
    t = io.open(FX, encoding='utf-8', newline='').read()

    t = replace_once(
        t,
        '''    VOC_RELEASE, VOC_SHIFT, VOC_SIB, VOC_GATE, VOC_NORM, VOC_CLARITY,
    VOC_LEVEL, VOC_CARRIER, VOC_FREEZE,''',
        '''    VOC_RELEASE, VOC_SHIFT, VOC_SIB, VOC_GATE, VOC_NORM, VOC_CLARITY,
    VOC_LEVEL, VOC_CARRIER, VOC_FREEZE, VOC_LOOP,''')

    t = replace_once(
        t,
        '''    {FX_PID_VOC_FREEZE, "fx.voc.freeze", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},''',
        '''    {FX_PID_VOC_FREEZE, "fx.voc.freeze", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    /* Off = the phrase plays once per note and then the vocoder falls silent
     * over a still-sounding carrier; on = it repeats until the note ends. Off
     * by default because a spoken phrase said twice is a stutter, and speech
     * is what the capture is for; a rhythmic syllable is the case that wants
     * the other setting. */
    {FX_PID_VOC_LOOP, "fx.voc.loop", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},''')

    t = replace_once(
        t,
        '''        if (voice_manager_block_note() != 0) v.play = 0;
        for (size_t i = 0; i < frames; ++i) {
            s_voc_mod[i] = (v.play < v.rec_len)
                               ? (float)v.rec.buf[v.play++] * (1.0f / 32768.0f)
                               : 0.0f;
        }''',
        '''        if (voice_manager_block_note() != 0) v.play = 0;
        /* Past the end: silence, or back to the start. The rec_len test is
         * what keeps an empty recording from wrapping onto itself and reading
         * a buffer nothing was ever written into.
         *
         * The splice is not crossfaded, and does not need to be: the
         * modulator is a control signal rather than something anyone hears,
         * so a discontinuity here costs a fast step in the band followers --
         * which have attack and release smoothing of their own -- instead of
         * the click it would cost in an audio loop. */
        const bool loop = pv(VOC_LOOP) >= 0.5f && v.rec_len > 0;
        for (size_t i = 0; i < frames; ++i) {
            if (v.play >= v.rec_len) {
                if (!loop) {
                    s_voc_mod[i] = 0.0f;
                    continue;
                }
                v.play = 0;
            }
            s_voc_mod[i] = (float)v.rec.buf[v.play++] * (1.0f / 32768.0f);
        }''')

    t = replace_once(
        t,
        ''' * each note-on restarts it from the beginning. Play a chord and the synth
 * says the phrase; play another and it says it again. Past the end of the
 * recording there is nothing to analyse, so a note held longer than the
 * phrase simply stops speaking — the vocoder goes quiet over a carrier that
 * is still sounding, which is what silence into a vocoder has always meant.''',
        ''' * each note-on restarts it from the beginning. Play a chord and the synth
 * says the phrase; play another and it says it again. Past the end of the
 * recording there is nothing to analyse, so a note held longer than the
 * phrase simply stops speaking — the vocoder goes quiet over a carrier that
 * is still sounding, which is what silence into a vocoder has always meant.
 * `fx.voc.loop` is the other reading of the same gesture: the phrase repeats
 * for as long as the note is held, which is what a rhythmic capture wants and
 * what a spoken one does not.''')

    io.open(FX, 'w', encoding='utf-8', newline='').write(t)
    print('applied')
    return 0


if __name__ == '__main__':
    sys.exit(main())
