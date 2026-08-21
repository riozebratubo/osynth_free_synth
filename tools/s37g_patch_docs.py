#!/usr/bin/env python3
"""S37g docs - the ES8311's real power-on register file, and what it closes.

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
    u"""**Still open: where the analogue element actually is.** `microphone_type: analog`
says it is on the ES8311's preamp, not which pins — an on-board element and the
mic pin of a TRRS headset jack look identical from the firmware.
""",
    u"""**The ES8311's real power-on register file** (S37g, after a 10-second unplug —
worth keeping, since nothing else on hand documents it):

    reg  00 01 02 03 04 05 06 07
         1f 00 00 10 10 00 03 00
    reg  08 09 0a 0b 0c 0d 0e 0f
         ff 00 00 00 20 fc 6a 00
    reg  10 11 12 13 14 15 16 17
         13 7c 02 40 10 00 04 00
    reg  18 19 1a 1b 1c
         00 00 00 0c 4c
    reg  44 45
         00 00                      chip id 8311, ver 01

**That closes the register table for good.** REG07's bits 7:6 — the tri-state
field, the last firmware-side suspect — are **zero at reset**, and so are REG06's
bits 7:5, REG03/REG04 bit 7 and REG02's low three. The absolute writes were never
clearing anything the vendor preserves. S37e's masked writes are correct and make
no difference; keep them, but the driver is exonerated on every reading now.
(REG09/0A also read 0x00 at reset, not the 0x0C assumed earlier, so the vendor's
`iface |= 0x10` would in fact have produced a valid 32-bit width too.)

**GPIO9 is stuck low as well**, at both `port up` and `codec up`. So the data
lines are not simply reversed — that hypothesis is out too.

**And `din GPIO11` is still dead after the codec is configured.** Clocks at the
pads, codec identified, register file exactly the vendor's, ADC unmuted at 0 dB
with 24 dB of PGA in front — and `raw 000000/000000` every window. A ΣΔ
converter that is clocked does not emit bit-exact zeros; it emits a noise floor.
So the ADC is not converting, which means either its clock never arrives or the
chip on the other end of these pads is not the one answering at 0x18.

S37g sharpens the last ambiguous instrument: the pad probe now reads each pad
under a pull-up *and* a pull-down and classifies the result — `toggles`,
`FLOATS` (follows both pulls, nothing driving), or `held LOW/HIGH` (something
drives it, statically). On ASDOUT those point opposite ways: a codec converting
silence holds its data pin low legitimately, while a codec whose output stage is
off, or a pad that reaches no codec, floats.

**Still open: where the analogue element actually is.** `microphone_type: analog`
says it is on the ES8311's preamp, not which pins — an on-board element and the
mic pin of a TRRS headset jack look identical from the firmware.
""")])

print('done')
