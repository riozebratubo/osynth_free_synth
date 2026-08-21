#!/usr/bin/env python3
"""S37h - the microphone data pin is GPIO48, not GPIO11.

The board turned out not to be the one every note in this tree assumed. It is a
**Guition JC-ESP32P4-M3-DEV** (schematics/JC-ESP32P4-M3-DEV Specifications-EN.pdf,
and the Altium library's SOURCELIBRARYNAME=ESP32P4_KSDIY_V3.SCHLIB), not a
Waveshare P4-NANO-class tablet board. Its audio block is:

    BCLK   GPIO12      (as configured)
    MCLK   GPIO13      (as configured)
    LRCLK  GPIO10      (as configured)
    DOUT   GPIO9       P4 -> ES8311 DSDIN, the speaker path
    DIN    GPIO48      ES8311 ASDOUT -> P4, **the microphone**
    AMP    GPIO11      amplifier enable
    I2C    GPIO7/8     (as configured)

So four of the six were right and the one that mattered was not: GPIO11 is the
speaker amplifier's enable line, not the codec's data output. That single fact
explains the entire investigation. GPIO11 sat at a hard zero through a pull-up
because it is a PA enable with a pull-down holding the amplifier off; it never
switched because nothing was ever going to drive it; and the ADC was very
probably converting the whole time, onto GPIO48, which nothing was reading.

It also explains why the register work kept coming up clean: it was clean. The
codec was configured correctly on every attempt from S37c onwards.

Changes:
 - sdkconfig.defaults.esp32p4: CONFIG_OSYNTH_MIC_DIN_GPIO 11 -> 48.
 - The pad probe's spare slots now watch GPIO9 (the codec's DSDIN) and GPIO11
   (the amp enable), so the boot log carries the evidence for this rather than
   the assumption.
 - PINMAP.md, tools/_p4_board.txt, codec.h and codec_es8311.cpp corrected.

Kept per the project's artifacts rule.
"""
import io
import sys


def edit(path, subs):
    with io.open(path, encoding='utf-8', newline='') as f:
        src = f.read()
    for old, new in subs:
        if new in src:
            print('  (already applied) %r' % old[:50])
            continue
        n = src.count(old)
        if n != 1:
            sys.exit('%s: anchor count %d: %r' % (path, n, old[:70]))
        src = src.replace(old, new)
    with io.open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(src)
    print('patched %s' % path)


D = u'—'

# ------------------------------------------------------- sdkconfig.defaults
edit('sdkconfig.defaults.esp32p4', [(
    u'''CONFIG_OSYNTH_MIC_DIN_GPIO=11
''',
    u'''# **GPIO48, and this is the pin the whole S37 investigation turned on.**
#
# It was 11 for five rounds, from an `i2s_audio:` block believed to be this
# board's. It is not: this is a **Guition JC-ESP32P4-M3-DEV** (see
# schematics/), whose audio block puts the codec's ASDOUT on GPIO48 and uses
# **GPIO11 as the speaker amplifier's enable**. A PA enable is held low by a
# pull-down so the amplifier stays off until firmware asks for it, which is
# exactly what the pin measured: a hard zero through a 45k pull-up, at every
# stage of boot, that never once switched.
#
# Everything else in that block was right {D} BCLK 12, WS 10, MCLK 13, I2C 7/8
# {D} which is why the clocks came up, the codec answered, and its register file
# read back byte-for-byte identical to Espressif's own driver. The ADC was very
# likely converting the entire time, onto a pin nothing was reading.
CONFIG_OSYNTH_MIC_DIN_GPIO=48
'''.replace('{D}', D)), (
    u'''# **The board's own microphone, on GPIO11.** Confirmed present, not yet''',
    u'''# **The board's own microphone, on GPIO48** (GPIO11 until S37h; see the pin
# setting below for what GPIO11 actually is). Confirmed present, not yet'''), (
    u'''# **One thing to settle first.** It is still unresolved whether GPIO11 carries''',
    u'''# **Settled in S37h:** it is the ADC half of the ES8311, and its data pin is
# GPIO48. The paragraph below is kept for the reasoning, which was right even
# though the pin in it was not. It was unresolved whether GPIO11 carries''')])

# ---------------------------------------------------------------- source_mic
edit('components/audio_io/source_mic.cpp', [(
    u'''#if SYNTH_ENABLE_CODEC_ES8311
#define OSYNTH_MIC_PROBE_EXTRA_GPIO 9
#else
#define OSYNTH_MIC_PROBE_EXTRA_GPIO -1
#endif
''',
    u'''#if SYNTH_ENABLE_CODEC_ES8311
#define OSYNTH_MIC_PROBE_EXTRA_GPIO  9  /* ES8311 DSDIN: an input, should float */
#define OSYNTH_MIC_PROBE_EXTRA2_GPIO 11 /* the speaker amp's enable, held low */
#else
#define OSYNTH_MIC_PROBE_EXTRA_GPIO  -1
#define OSYNTH_MIC_PROBE_EXTRA2_GPIO -1
#endif
'''), (
    u''' * On the ESP32-P4 carrier that is GPIO9, which the board's own configuration
 * calls the ES8311's *DSDIN* {D} an input to the codec, so it should sit still.
 * It is swept because "should" is doing a lot of work there: the pin roles come
 * from an ESPHome block, not a schematic, and if GPIO9 turns out to be the pad
 * that moves then the data lines are simply the other way round and GPIO11 is
 * the codec's input, which would explain a data pin that is held low and never
 * changes no matter what the ADC is told to do. */'''.replace('{D}', D),
    u''' * On the Guition JC-ESP32P4-M3-DEV those are GPIO9, the ES8311's DSDIN {D} an
 * input to the codec, so it should read as floating {D} and GPIO11, the speaker
 * amplifier's enable, which should read as held LOW because a PA enable has a
 * pull-down keeping the amplifier off.
 *
 * They are swept because those two readings are the evidence for S37h. GPIO11
 * was this port's DIN for five rounds, on the strength of an `i2s_audio:` block
 * attributed to the wrong board, and it produced a hard zero that no codec
 * register could ever have changed. Keeping both pins in the log means the next
 * person sees *why* the data pin is GPIO48 rather than being told. */'''.replace('{D}', D)), (
    u'''        {(gpio_num_t)OSYNTH_MIC_PROBE_EXTRA_GPIO, "extra"},''',
    u'''        {(gpio_num_t)OSYNTH_MIC_PROBE_EXTRA_GPIO, "extra"},
        {(gpio_num_t)OSYNTH_MIC_PROBE_EXTRA2_GPIO, "extra2"},''')])

# ------------------------------------------------------------------- codec.h
edit('components/codec/include/codec.h', [(
    u''' * component drives one chip. osynth now clocks that mic's I2S pins (DIN 11,
 * BCLK 12, WS 10, MCLK 13 {D} see sdkconfig.defaults.esp32p4), which is
 * necessary and not sufficient; the codec stays unconfigured and silent, and
 * the symptom is the same full-blocks-of-zeros every other fault on that port
 * produces. The external-mic path (OSYNTH_MIC_SHARE_CLOCKS, a digital MEMS
 * part) needs none of this and is proven working.'''.replace('{D}', D),
    u''' * component drives one chip. codec_es8311.cpp closes that gap, and osynth
 * clocks the mic's I2S pins for it (DIN 48, BCLK 12, WS 10, MCLK 13 {D} see
 * sdkconfig.defaults.esp32p4). The external-mic path
 * (OSYNTH_MIC_SHARE_CLOCKS, a digital MEMS part) needs none of this and is
 * proven working.'''.replace('{D}', D))])

# -------------------------------------------------------------- codec_es8311
edit('components/codec/codec_es8311.cpp', [(
    u''' * Driving the four I2S pins at it (DIN 11, BCLK 12, WS 10, MCLK 13) is
 * necessary and not sufficient.''',
    u''' * Driving the four I2S pins at it (DIN 48, BCLK 12, WS 10, MCLK 13) is
 * necessary and not sufficient. DIN read 11 until S37h, from an `i2s_audio:`
 * block attributed to the wrong board; GPIO11 on this one is the speaker
 * amplifier's enable, which is why it measured as a hard zero that nothing in
 * this file could ever have moved.'''), (
    u''' * on GPIO11), so the only arrangement anyone has demonstrated on this part has''',
    u''' * on GPIO48), so the only arrangement anyone has demonstrated on this part has'''), (
    u''' * board's speaker PA is a separate GPIO (53, "PA-CTRL") that nothing here''',
    u''' * board's speaker PA is a separate GPIO (11 on this board) that nothing here''')])

# ---------------------------------------------------------------- PINMAP.md
edit('PINMAP.md', [(
    u'''| GPIO11 | Mic DIN | on-board mic (ES8311 ADC?) | `OSYNTH_MIC_DIN_GPIO` |''',
    u'''| GPIO48 | Mic DIN | on-board mic {D} ES8311 **ASDOUT** | `OSYNTH_MIC_DIN_GPIO` |
| GPIO9 | (unused) | ES8311 **DSDIN** {D} the board's speaker path | {D} |
| GPIO11 | (unused) | the speaker amplifier's **enable** {D} not a data pin | {D} |'''.replace('{D}', D)), (
    u'''That also settles what the board's **on-board microphone on GPIO11** is not:''',
    u'''That also settles what the board's **on-board microphone** is not:'''), (
    u'''It is also unresolved whether GPIO11 carries a bare MEMS mic or the ADC half of''',
    u'''**Settled in S37h**: it is the ADC half of the ES8311, its data pin is GPIO48,
and GPIO11 is the speaker amplifier's enable. The rest of this paragraph is the
reasoning that got there, which was sound even where the pin was not. It was
unresolved whether that pin carried a bare MEMS mic or the ADC half of'''), (
    u'''`OSYNTH_MIC_SHARE_CLOCKS=n`, **DIN 11, BCLK 12, WS 10, MCLK 13** {D} the numbers'''.replace('{D}', D),
    u'''`OSYNTH_MIC_SHARE_CLOCKS=n`, **DIN 48, BCLK 12, WS 10, MCLK 13** {D} the numbers'''.replace('{D}', D))])

print('done')
