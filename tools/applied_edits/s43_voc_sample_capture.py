#!/usr/bin/env python3
"""S43: fx.voc.freeze records a real sample and replays it under every note.

Replaces the spectral-snapshot capture, which never worked and had two
distinct failure modes on the way:

  * freezing the live band envelopes captured the silence *after* the word,
    because the envelopes follow fx.voc.release (25-40 ms) and nobody lets go
    of a button while still making the sound;
  * a per-band peak-hold captured the union of every vowel said, which is a
    flat spectrum, and a flat bank is a flat filter that passes the carrier
    through unchanged.

Both are the same underlying problem: one frozen spectral frame is a single
vowel, and a single vowel is not what anyone means by "capture a sample". So
this stores the modulator itself and replays it, which is what the gesture has
always looked like it did.

  fx.voc.freeze = 0  live: the mic is the modulator, and it is recorded into
                     the buffer from the start of the press.
  fx.voc.freeze = 1  sample: the recording is the modulator, replayed from
                     the beginning on every note-on. Past its end it is
                     silence, so a held note says the phrase once and stops.

The note-on tap mirrors drums_block_hit() exactly, including where it may be
read: voice_manager_render() drains its event queue before fx_process() runs
in the same render callback, so the FX bus sees this block's note-ons.

Applied edit, kept per the project's convention. Both files have mixed line
endings, so matches are line-ending-agnostic.
"""
import io
import re
import sys

FX = 'components/fx/fx.cpp'
VH = 'components/synth_core/include/synth_voice.h'
VC = 'components/synth_core/synth_voice.cpp'


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
    # ---- 1. the note-start tap ---------------------------------------------
    h = io.open(VH, encoding='utf-8', newline='').read()
    h = replace_once(
        h,
        'void voice_manager_note_on(uint8_t note, uint8_t velocity);',
        '''void voice_manager_note_on(uint8_t note, uint8_t velocity);

/* Note-start tap (S43): the velocity (1..127) of a note that *started a
 * voice* during the block currently being rendered, or 0 for "none did".
 * Several note-ons in one block report the loudest, so a chord reads as one
 * event, which is what a retrigger wants.
 *
 * The sibling of drums_block_hit(), with the same contract and the same
 * caveat about when it may be read: voice_manager_render() drains its event
 * queue at block start, and main.cpp calls it before fx_process(), so the FX
 * bus sees this block's note-ons. Reading it from anywhere else, or before
 * voice_manager_render() has run in this callback, gets the previous block.
 *
 * The vocoder's sample replay is the caller -- it restarts the recorded
 * phrase on each note. */
uint8_t voice_manager_block_note(void);''')
    io.open(VH, 'w', encoding='utf-8', newline='').write(h)

    c = io.open(VC, encoding='utf-8', newline='').read()
    c = replace_once(
        c,
        'std::atomic<uint32_t> s_evt_dropped{0};',
        '''std::atomic<uint32_t> s_evt_dropped{0};

/* Note-start tap (S43) -- see voice_manager_block_note(). Written and cleared
 * inside drain_events(), read later in the same render callback by the FX
 * bus, both on the audio task, so a plain byte is the whole synchronisation
 * story. */
uint8_t s_block_note = 0;''')

    c = replace_once(
        c,
        '''void drain_events(const synth_engine_t* eng) {
    uint32_t tail = s_evt_tail.load(std::memory_order_relaxed);''',
        '''void drain_events(const synth_engine_t* eng) {
    /* Cleared here rather than after the FX bus has read it: this is the one
     * place that knows a new block's events are about to be applied, and the
     * value has to survive from here to fx_process() in the same callback. */
    s_block_note = 0;
    uint32_t tail = s_evt_tail.load(std::memory_order_relaxed);''')

    c = replace_once(
        c,
        '''            case EvType::NoteOn:
                ev_note_on(eng, e.note, e.velocity);
                break;''',
        '''            case EvType::NoteOn:
                ev_note_on(eng, e.note, e.velocity);
                /* Loudest of the block: a chord is one retrigger, not one per
                 * note. */
                if (e.velocity > s_block_note) s_block_note = e.velocity;
                break;''')

    c = replace_once(
        c,
        'extern "C" void voice_manager_note_on(uint8_t note, uint8_t velocity) {',
        '''extern "C" uint8_t SYNTH_RENDER_IRAM voice_manager_block_note(void) {
    return s_block_note;
}

extern "C" void voice_manager_note_on(uint8_t note, uint8_t velocity) {''')
    io.open(VC, 'w', encoding='utf-8', newline='').write(c)

    # ---- 2. the vocoder's sample buffer ------------------------------------
    t = io.open(FX, encoding='utf-8', newline='').read()

    t = replace_once(
        t,
        '#include "audio_io.h" /* the vocoder\'s modulator: the selected audio input (S38) */',
        '#include "audio_io.h" /* the vocoder\'s modulator: the selected audio input (S38) */\n'
        '#include "synth_voice.h" /* voice_manager_block_note(): the vocoder\'s retrigger */')

    t = replace_once(
        t,
        ''' * `fx.voc.freeze` holds the band envelopes where they are. The input stops
 * being read (so it costs *less* while frozen), the carrier keeps flowing, and
 * the synth sustains whatever vowel was last said — the app's Hold-to-sample
 * button is this parameter inverted: recording while pressed, frozen on
 * release. Sibilance is live HF by definition and does not survive a freeze.''',
        ''' * `fx.voc.freeze` records a phrase and replays it under every note (S43).
 * Held down it is live and recording; released it becomes the modulator, and
 * each note-on restarts it from the beginning. Play a chord and the synth
 * says the phrase; play another and it says it again. Past the end of the
 * recording there is nothing to analyse, so a note held longer than the
 * phrase simply stops speaking — the vocoder goes quiet over a carrier that
 * is still sounding, which is what silence into a vocoder has always meant.
 *
 * It replaced a spectral freeze, and the reason is worth keeping. That
 * version held the band envelopes where they stood, which is one frame of
 * spectrum: a single vowel. It failed twice over. Frozen live, the envelopes
 * follow fx.voc.release (25-40 ms) and nobody releases a button while still
 * making the sound, so it captured the silence after the word — measured, 12
 * bands above the gate at the instant speech stopped and none 100 ms later.
 * Given a peak-hold to reach back through that gap, each band then took its
 * maximum from a *different* moment, which is the union of every vowel said
 * and therefore flat — and a flat set of band gains is a flat filter, so the
 * carrier passed through untouched. Both failures are one fact seen twice: a
 * vowel is a shape across the bank at a single instant, and holding one is
 * not what "capture a sample" means to anyone holding the button.
 *
 * Sibilance survives here where it could not survive a freeze: it is live HF
 * from whatever the modulator currently is, and in sample mode that is the
 * recording, so the consonants come back with it.''')

    t = replace_once(
        t,
        'struct VocBand {',
        '''/* The capture buffer (S43). Mono int16 at the render rate, PSRAM-preferred
 * through line_alloc() like every other buffer on this bus.
 *
 * Sized in seconds rather than samples because what it holds is a spoken
 * phrase, and gated on the same PSRAM proxy the band count uses: 2.5 s is
 * 240 KB, which is nothing next to the looper on a P4 or S3 and impossible on
 * a classic ESP32, where 0.75 s still holds a word or two.
 *
 * Recording is linear from the press, not circular: a circular buffer would
 * always hold the last N seconds, which sounds like the same thing but makes
 * the release edge decide where the phrase *starts*, and that is exactly the
 * timing nobody can hit — the mistake the spectral freeze made twice. Here
 * the press starts the recording and the release only ends it, so both edges
 * are ones a person can place. */
#if CONFIG_SPIRAM
constexpr float kVocSampS = 2.5f;
#else
constexpr float kVocSampS = 0.75f;
#endif
constexpr uint32_t kVocSampLen = (uint32_t)(kVocSampS * (float)kSr);

struct VocBand {''')

    t = replace_once(
        t,
        '''    float nrm = 1.0f;     /* fx.voc.norm scale, block rate, held on freeze */
    float hold_bb = 0.0f; /* env_bb at the snapshot instant; also its trigger */
    bool was_frozen = false; /* freeze edge detector; see the capture note */''',
        '''    float nrm = 1.0f;     /* fx.voc.norm scale, block rate */
    /* Capture (S43). `rec` is linear: `rec_w` is the write cursor while
     * recording and `rec_len` freezes it on release. `play` runs past
     * `rec_len` and stays there, which is what makes the tail silent. */
    osynth::dsp::Line rec;
    uint32_t rec_w = 0;
    uint32_t rec_len = 0;
    uint32_t play = 0;
    bool rec_ok = false;
    bool was_frozen = false; /* edge detector, both directions */''')

    t = replace_once(
        t,
        '    float hold = 0.0f;    /* this band at the snapshot instant (see below) */\n',
        '')

    i = t.index('/* How far back fx.voc.freeze reaches when it captures (ms)')
    j = t.index('constexpr float kVocHoldMs = 800.0f;')
    t = t[:i] + t[j + len('constexpr float kVocHoldMs = 800.0f;'):].lstrip('\r\n')

    t = replace_once(
        t,
        '    s_flg.ok = line_alloc(s_flg.l, kFlgLen) && line_alloc(s_flg.r, kFlgLen);',
        '''    s_voc.rec_ok = line_alloc(s_voc.rec, kVocSampLen);
    if (!s_voc.rec_ok) {
        ESP_LOGW(TAG, "vocoder: capture buffer alloc failed — fx.voc.freeze "
                      "stays live instead of replaying a sample");
    }

    s_flg.ok = line_alloc(s_flg.l, kFlgLen) && line_alloc(s_flg.r, kFlgLen);''')

    t = replace_once(
        t,
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
        return;''',
        '''        for (int i = 0; i < kVocBandsMax; ++i) v.b[i].env = 0.0f;
        v.env_bb = 0.0f;
        v.env_car = 0.0f;
        v.nrm = 1.0f;
        /* The recording survives a bypass, and so must was_frozen: the edges
         * belong to the parameter, not to the switch. Clearing either would
         * throw a phrase away, or re-arm a recording over it, because someone
         * toggled the unit off and on. */
        return;''')

    t = replace_once(
        t,
        '''    /* Frozen: the envelopes hold, so there is nothing to analyse and the
     * modulator is not even read. */
    const bool frozen = pv(VOC_FREEZE) >= 0.5f;
    /* The frames <= guard is the scratch buffer's contract, not defensiveness
     * about a value that varies: the render callback always passes exactly
     * SYNTH_BLOCK_SIZE. It is here so that if that ever stops being true the
     * unit goes quiet instead of reading past s_voc_mod. */
    const bool live = !frozen && frames <= SYNTH_BLOCK_SIZE &&
                      audio_io_in_mono(s_voc_mod, frames);''',
        '''    /* Where the modulator comes from this block: the microphone, or the
     * recorded phrase. Either way the analysis below is identical, which is
     * the whole point of storing audio rather than a spectral frame.
     *
     * The frames <= guard is the scratch buffer's contract, not defensiveness
     * about a value that varies: the render callback always passes exactly
     * SYNTH_BLOCK_SIZE. It is here so that if that ever stops being true the
     * unit goes quiet instead of reading past s_voc_mod. */
    const bool sampling = pv(VOC_FREEZE) >= 0.5f && v.rec_ok;
    const bool fits = frames <= SYNTH_BLOCK_SIZE;
    bool live = false;

    if (sampling != v.was_frozen) {
        if (sampling) {
            /* Release: the phrase ends wherever the recording got to, and the
             * playhead is left at that end so nothing sounds until a note
             * asks for it. */
            v.rec_len = v.rec_w;
            v.play = v.rec_len;
        } else {
            /* Press: a new phrase starts here. */
            v.rec_w = 0;
            v.rec_len = 0;
        }
        v.was_frozen = sampling;
    }

    if (sampling && fits) {
        /* A note that started this block restarts the phrase. Read once per
         * block: the tap reports the loudest note-on of the block, so a chord
         * retriggers once rather than once per key. */
        if (voice_manager_block_note() != 0) v.play = 0;
        for (size_t i = 0; i < frames; ++i) {
            s_voc_mod[i] = (v.play < v.rec_len)
                               ? (float)v.rec.buf[v.play++] * (1.0f / 32768.0f)
                               : 0.0f;
        }
        /* Live even past the end of the phrase: the analysis has to keep
         * running on the silence so the band envelopes decay through their
         * release instead of stopping wherever the last sample left them. */
        live = true;
    } else if (!sampling && fits && audio_io_in_mono(s_voc_mod, frames)) {
        live = true;
        /* Record while the button is down, linearly, until the buffer is
         * full. Full is not an error: it is the longest phrase this build can
         * hold, and the release still ends it wherever it got to. */
        if (v.rec_ok) {
            for (size_t i = 0; i < frames && v.rec_w < v.rec.len; ++i) {
                int32_t s = (int32_t)(s_voc_mod[i] * 32767.0f);
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                v.rec.buf[v.rec_w++] = (int16_t)s;
            }
        }
    }''')

    t = replace_once(t, 'if (!live && !frozen && !v.warned) {',
                     'if (!live && !v.warned) {')

    io.open(FX, 'w', encoding='utf-8', newline='').write(t)
    print('applied')
    return 0


if __name__ == '__main__':
    sys.exit(main())
