#!/usr/bin/env python3
"""S37d cleanup - undo the double-application of s37d_patch_mic_diag.py.

The first run of that script stopped part-way (a non-unique anchor in
audio_io.cpp), and the re-run's "already applied" guard only caught the edits
whose `old` text was *not* a substring of their `new` text. The six that were
insertions around a kept anchor therefore landed twice. This collapses each
duplicate and, while it is here, moves mag_bits() above narrow()'s doc comment
instead of between that comment and the function it documents.

Kept alongside the patch it repairs, per the project's artifacts rule.
"""
import io
import sys

PATH = 'components/audio_io/source_mic.cpp'

with io.open(PATH, encoding='utf-8', newline='') as f:
    src = f.read()

MAG = u'''/* |w| as a bit mask, without the sign extension that would make every negative
 * sample read 0xFFFFFFFF and throw the magnitude away. */
inline uint32_t SYNTH_RENDER_IRAM mag_bits(mic_word_t w) {
    const int32_t v = (int32_t)w;
    return (uint32_t)(v ^ (v >> 31));
}

'''

PIN_MASK = u'''/* 1 << pin, and 0 for I2S_GPIO_UNUSED (-1). A function rather than the obvious
 * expression because shifting by a negative count is undefined even in the arm
 * of a conditional the compiler can fold away. */
inline uint64_t pin_mask(gpio_num_t pin) {
    return ((int)pin >= 0) ? (1ULL << (int)pin) : 0ULL;
}

'''

DUMP = u'''#if OSYNTH_MIC_DUMP_GPIO
    /* Read each row for one thing: the clock pins claimed as *outputs* with an
     * I2S signal on them, and DIN claimed as an *input*. A pad that is neither
     * is one the matrix never took, which is the difference between a port that
     * is misconfigured and a port that is not connected to anything. */
    (void)gpio_dump_io_configuration(
        stdout, pin_mask(OSYNTH_MIC_DIN) | pin_mask(OSYNTH_MIC_BCLK) |
                    pin_mask(OSYNTH_MIC_WS) | pin_mask(OSYNTH_MIC_MCLK));
#endif
'''

TAKE = u'''void audio_source_mic_raw_take(uint32_t* or_l, uint32_t* or_r) {
    *or_l = s_raw_or[0].exchange(0, std::memory_order_relaxed);
    *or_r = s_raw_or[1].exchange(0, std::memory_order_relaxed);
}

'''

FETCH = u'''    s_raw_or[0].fetch_or(or_l, std::memory_order_relaxed);
    s_raw_or[1].fetch_or(or_r, std::memory_order_relaxed);

'''

PAD_DOC = u'''/* ---- pad dump --------------------------------------------------------------
 *
 * Print what the GPIO matrix actually did with this port's four pads, once, as
 * soon as the port is up.
 *
 * It answers a question every other instrument on this port assumes the answer
 * to: that the driver claimed the pins it was given, as outputs where it
 * should and with the input enabled on DIN. i2s_channel_init_std_mode()
 * returning ESP_OK does not say that — a pad another peripheral already holds
 * is a warning in the driver's log and a working channel that reaches nothing,
 * which is a fault this board has produced before (PINMAP.md, GPIO45/46/47).
 *
 * Cheap enough to leave on while the on-board microphone is unresolved: four
 * pads, four lines, once per boot. Turn it off once this port is trusted. */
#define OSYNTH_MIC_DUMP_GPIO 1

'''

RAW_OR_DOC = u'''/* Every bit that has appeared on either slot since the last read (S37d).
 *
 * This exists because nothing else on this port can distinguish a data pin
 * nothing drives from one carrying a signal too quiet to survive the pipeline
 * in front of the meter. Two stages crush it: narrow() keeps the top 16 bits of
 * a 24-bit word, and the heartbeat prints the result as %.2f — so everything
 * below about -46 dBFS reads as exactly `mic 0.00/0.00`, which is also what a
 * disconnected pin reads. Five rounds of ES8311 register work were spent inside
 * that ambiguity.
 *
 * An OR of the *magnitudes* resolves it and carries the level with it: the
 * highest set bit is the loudest sample the window saw, and 0x00000000 across a
 * whole window means the pin never left zero — which no register change can
 * fix, and which is the point at which the fault is in copper rather than code.
 *
 * Relaxed atomics, folded once per block rather than once per sample: this runs
 * in the render path, and the reader is a heartbeat that cares about the value
 * and not about which block it landed in. */
std::atomic<uint32_t> s_raw_or[2];

'''

for name, block in (('pad dump', PAD_DOC), ('s_raw_or', RAW_OR_DOC),
                    ('mag_bits', MAG), ('pin_mask', PIN_MASK),
                    ('gpio dump', DUMP), ('raw_take', TAKE),
                    ('fetch_or', FETCH)):
    if block + block in src:
        src = src.replace(block + block, block)
        print('collapsed duplicate: %s' % name)
    if src.count(block) != 1:
        sys.exit('%s: %s appears %d times' % (PATH, name, src.count(block)))

# mag_bits sat between narrow()'s doc comment and narrow() itself. Move it above
# the comment, so the comment is adjacent to the function it describes again.
TAIL = u" * hardware has failed. */\n"
if TAIL + MAG in src:
    src = src.replace(TAIL + MAG, TAIL)
    HEAD = u"/* One slot to one int16.\n"
    if src.count(HEAD) != 1:
        sys.exit('%s: narrow() doc anchor not unique' % PATH)
    src = src.replace(HEAD, MAG + HEAD)
    print('moved mag_bits above narrow()\'s doc comment')

with io.open(PATH, 'w', encoding='utf-8', newline='') as f:
    f.write(src)
print('done')
