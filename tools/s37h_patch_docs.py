#!/usr/bin/env python3
"""S37h docs - the board's real identity, and the resolution.

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

edit('tools/_p4_board.txt', [(
    u'''### What this board actually is (S37c)

The defaults above were written for an assumed "module on a carrier". The board
they are now used on is a **Waveshare P4-NANO-class 10.1" tablet development
board**, and the difference matters: it has far more copper already claimed than
the header listing above suggests, which is what made GPIO4 look free when it is
the board's SPI clock.

From the board's own ESPHome configuration:

| Pin(s) | Claimed by |
| --- | --- |
| GPIO0 / 1 / 4 | SPI2 {D} MISO / MOSI / **CLK** |
| GPIO7 / 8 | I2C bus A {D} ES8311 (0x18), GT911 touch, backlight controller |
| GPIO9 | I2S DOUT {D} ES8311 DAC (speaker) |
| GPIO10 | I2S LRCLK {D} ES8311 |
| GPIO11 | I2S DIN {D} ES8311 ADC (the on-board analogue mic) |
| GPIO12 | I2S BCLK {D} ES8311 |
| GPIO13 | I2S MCLK {D} ES8311 |
| GPIO31 / 50 / 51 / 52 | IP101 Ethernet {D} MDC / CLK (`CLK_EXT_IN`) / power / MDIO |
| GPIO53 | **PA-CTRL**, the speaker amplifier enable (off at boot) |
'''.replace('{D}', D),
    u'''### What this board actually is (S37c, corrected in S37h)

The defaults above were written for an assumed "module on a carrier". It is a
**Guition JC-ESP32P4-M3-DEV** {D} 92 x 62 mm, RJ45, three USB-C, micro-SD, MIPI
DSI/CSI FPCs, an ESP32-C6 on the module for radio. Not, as S37c through S37g
assumed, a Waveshare P4-NANO-class tablet board. Sources now in `schematics/`:
`JC-ESP32P4-M3-DEV Specifications-EN.pdf`, and the Altium library whose
`SOURCELIBRARYNAME` is `ESP32P4_KSDIY_V3.SCHLIB`. Those two files carry the
module's pin list and the board outline but **no dev-board net list**, which is
why the audio block had to be established from firmware written for this board
rather than from a drawing.

**That misidentification cost the whole S37 microphone investigation**, because
the `i2s_audio:` block the mic pins came from belonged to a different board. Four
of the six pins happened to be right, which is the worst possible outcome: the
clocks came up, the codec answered, and the one pin that was wrong was the one
carrying the audio.

| Pin(s) | Claimed by |
| --- | --- |
| GPIO0 / 1 / 4 | SPI2 {D} MISO / MOSI / **CLK** |
| GPIO7 / 8 | I2C {D} ES8311 (0x18) |
| GPIO9 | I2S DOUT {D} ES8311 **DSDIN**, the speaker path |
| GPIO10 | I2S LRCLK {D} ES8311 |
| GPIO11 | **speaker amplifier enable** {D} *not* a data pin |
| GPIO12 | I2S BCLK {D} ES8311 |
| GPIO13 | I2S MCLK {D} ES8311 |
| GPIO48 | I2S DIN {D} ES8311 **ASDOUT**, the on-board microphone |
| GPIO31 / 50 / 51 / 52 | IP101 Ethernet {D} MDC / CLK (`CLK_EXT_IN`) / power / MDIO |
'''.replace('{D}', D)), (
    u'''**Still open: where the analogue element actually is.** `microphone_type: analog`
says it is on the ES8311's preamp, not which pins {D} an on-board element and the
mic pin of a TRRS headset jack look identical from the firmware.
'''.replace('{D}', D),
    u'''### S37h: it was the pin

`CONFIG_OSYNTH_MIC_DIN_GPIO` was **11**. On this board GPIO11 is the speaker
amplifier's **enable line**, and the codec's ASDOUT is on **GPIO48**.

Every measurement in this file falls out of that one fact:

- GPIO11 read a hard zero through a 45k pull-up at every stage of boot. A PA
  enable is held down by a pull-down so the amplifier stays off until firmware
  asks for it. It was never going to be anything else.
- It never switched, before or after `codec_mic_init()`, because nothing drives
  it {D} not the codec, not us.
- `raw 000000/000000` was exactly right, and so was the deduction from it that a
  clocked sigma-delta ADC does not emit bit-exact zeros. The ADC was almost
  certainly converting the entire time, onto a pin nothing was reading.
- The register file matched Espressif's driver byte for byte on every attempt
  from S37c onwards **because it was correct**. Five rounds of register work
  found nothing wrong because there was nothing wrong.

What the detour was worth keeping: APLL on a master mic port (a clean 256 x fs
MCLK, and a capture sample-locked to the output), the masked register writes,
the raw-slot telemetry, the pad probe, the reset-state dump, and the discovery
that **this codec does not reset when the P4 does** {D} which quietly
contaminated every warm-boot experiment before S37f.

The lesson is the one PINMAP.md already had in bold for GPIO45/46/47, one level
up: *before trusting any pin on an unfamiliar board, establish what board it is.*
The pin list was measured against a config file, and the config file was never
checked against the hardware in hand.

**Still worth having, and now nearly free: the speaker.** GPIO9 is the ES8311's
DSDIN and GPIO11 enables the amplifier. The mic port already masters BCLK, WS
and MCLK for this codec and its DAC is already powered as the ADC's reference,
so a TX channel on that port plus one GPIO high would give the board audio out
of its own speaker.
'''.replace('{D}', D))])

print('done')
