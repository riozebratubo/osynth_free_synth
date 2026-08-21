#!/usr/bin/env python3
"""S37e docs - record what the S37d boot log actually proved.

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


edit('tools/_p4_board.txt', [(
    u"""**Still unmeasured, and now the top of the list: do GPIO10/12/13 drive?** Nothing
has ever checked, and DMA filling at the right rate proves only that the
peripheral's internal clock runs — a master fills its ring whether or not a
single pad is connected. This board has form: 45/46/47 read 0.6 V and cost a
week. A multimeter on each while the synth runs settles it in two minutes; a
toggling pin reads about half of 3.3 V, a dead one reads 0 V, 3.3 V or 0.6 V.

**Also unresolved: where the analogue element actually is.** `microphone_type:
analog` says it is on the ES8311's preamp, not which pins — an on-board element
and the mic pin of a TRRS headset jack look identical from the firmware. Plugging
a headset in is a two-minute test of the second reading.
""",
    u"""**What that round then established, from the boot log.** Recorded so none of it
is re-derived:

- `clk apll`, with IDF reporting the PLL already occupied at 24 575 996 Hz and
  reusing it. MCLK on GPIO13 is a clean 12.288 MHz off the same PLL the DAC
  runs on, not a dithered fractional divide of the XTAL.
- The GPIO matrix has all four pads on the right I2S1 signals: GPIO13 → sig 32
  (`I2S1_MCLK`), GPIO12 → 35 (`I2S1_I_BCK`), GPIO10 → 36 (`I2S1_I_WS`), all
  three output-enabled under peripheral control; GPIO11 input-enabled on 34
  (`I2S1_I_SD`). Nothing is misrouted at the SoC.
- `chip id 8311 ver 01`. The part is an ES8311 and the register dump is a real
  read of it, which every "the register file matches the vendor" claim rests on
  and which nothing had checked.
- `raw 000000/000000`, every window, with +18 dB of shift and someone shouting
  at the board. Not "quiet": the data pin is at **exactly** zero. `starve 1/1`
  and not climbing, so the DMA is filling normally the whole time.

So the register table is finished as a suspect, and so is the clock source. The
codec is real, addressed, holding the vendor's own end state, and emitting
nothing.

**S37e attacks the two candidates that leaves.**

- `probe_clock_pins()` (source_mic.cpp): after the port comes up, enable each
  pad's *input* alongside the peripheral output — `gpio_input_enable()` sets
  only the IE bit — and poll it a few thousand times. A pad carrying MCLK, BCLK
  or WS reads both levels; a dead one reads one. This is the 45/46/47 test done
  in firmware, and it is the thing that separates "the codec is never clocked"
  from everything else. DIN is probed too, as a second opinion on `raw` reached
  without the I2S receiver in the path.
- Masked register writes (codec_es8311.cpp): the file wrote whole registers
  where the vendor read-modify-writes, and two of those masks are not about
  tolerating an unknown state — they are bits the vendor deliberately does not
  own. `es8311_config_sample()` keeps `REG07 & 0xC0`, and REG07 is the register
  Espressif's own header calls **"tri-state, lrck divider"**; same for
  `REG06 & 0xE0`. A tri-stated ASDOUT and a disconnected ASDOUT read identically.
  Every masked write now carries its mask, and a `reset regs …` dump taken
  before the table runs records the power-on defaults — diff it against the
  `init regs …` dump and any bit that moved without appearing in kInit is a
  finding.

**Still open: where the analogue element actually is.** `microphone_type: analog`
says it is on the ES8311's preamp, not which pins — an on-board element and the
mic pin of a TRRS headset jack look identical from the firmware.

**And the schematic exists.** `private_docs/datasheets/2_EXPAND_IO&BAT.png` is
sheet 2 of this board's own drawing (it is where the JP1 header list came from,
and it confirms GPIO9–13 are not brought out). The audio sheet from the same
document would settle in seconds what has cost several rounds: whether GPIO9–13
reach the ES8311 at all, and what is on its MIC pins.
""")])

print('done')
