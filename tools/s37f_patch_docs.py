#!/usr/bin/env python3
"""S37f docs - the pads toggle, and the codec never resets on a reflash.

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
    u"""**S37e's answer, and a trap underneath it.**

- **The pads switch.** `bclk GPIO12 50%`, `ws GPIO10 50%`, `mclk GPIO13 49%`
  of a 4096-sample sweep. This is *not* the GPIO45/46/47 failure: the clock pins
  are alive at the die. Whether the traces complete is still unmeasured.
- **`din GPIO11` read STUCK at 0 — and that reading was worthless.** The probe
  sits at the end of `audio_source_mic_start()`, which runs inside
  `audio_io_start()`; `codec_mic_init()` runs afterwards. So DIN was sampled
  while the ES8311 was still in its power-on state and had been told nothing.
  S37f re-times it: `audio_io_mic_probe_pads()` takes the same reading again
  after the codec is up, and the two lines are labelled `port up` / `codec up`.
- **⚠ The codec does not reset when the P4 does.** The `reset regs` dump came
  back byte-for-byte identical to `init regs` — REG09/0A at 0x10, REG16 at 0x24,
  REG17 at 0xBF, REG44 at 0x58, which are *our written values*, not an ES8311's
  power-on defaults. It keeps its rail across a reflash, so every warm boot
  starts from whatever the previous boot left behind.

  Two consequences, and neither has a code fix. The masked writes S37e added
  cannot be seen to preserve anything until the board is **fully unpowered**
  — on a warm boot REG07 already reads back the 0x00 an earlier build wrote
  absolutely, so the mask preserves the wrong value. And every "tried it, still
  silent" result in this history was taken on a chip carrying state from the
  attempt before it. **Power-cycle the board before trusting any codec result.**

**S37f also adds the last firmware-only test of the clock path.**
`SYNTH_MIC_CODEC_MASTER_TEST` (synth_config.h, default 0) moves both ends at
once: the ES8311 becomes the I2S master (REG00 bit 6) and the P4 stops driving
BCLK and WS entirely, leaving it driving only MCLK. If the codec is receiving
that clock it will divide it into BCLK and LRCK and drive GPIO12/GPIO10 by
itself, which the pad probe will see with nothing on this chip driving them.
Stuck pads mean MCLK is not reaching the part and the fault is in the trace.
Expect `in starve` to climb throughout — with no BCLK of its own the port never
completes a frame, which is the arrangement and not a symptom.

The probe also sweeps **GPIO9**, the pin the board's config calls the ES8311's
DSDIN. It should sit still; if it turns out to be the pad that moves, the data
lines are the other way round from that transcription and GPIO11 is the codec's
*input*, which would explain a pin held hard low that never changes whatever the
ADC is told to do.

**Still open: where the analogue element actually is.** `microphone_type: analog`
says it is on the ES8311's preamp, not which pins — an on-board element and the
mic pin of a TRRS headset jack look identical from the firmware.
""")])

print('done')
