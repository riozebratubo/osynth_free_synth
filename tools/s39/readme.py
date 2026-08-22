#!/usr/bin/env python3
"""S39: README entries for the two noise-reduction units.

Separate from the main patch script only because it landed after it. Same
shape, same idempotence check. The FX-bus bullet is replaced whole rather than
line by line: the chain listing sits mid-paragraph, so inserting into it
reflows every line after it and a line-level anchor stops matching.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

FX_OLD = """- **FX bus:** drive → chorus → flanger → phaser → delay → granular delay →
  reverb → bitcrush → filter → EQ → compressor → stereo/output. Every unit has
  its own enable switch and is skipped outright while it is off — or while its
  mix is 0 — so the ones you aren't using are free. The switch is a real
  bypass: it keeps every setting, so you can A/B an effect without losing the
  sound you were comparing against.
"""

FX_NEW = """- **FX bus:** adaptive noise reduction → noise reduction → vocoder → drive →
  chorus → flanger → phaser → delay → granular delay → reverb → bitcrush →
  filter → EQ → compressor → stereo/output. Every unit has its own enable
  switch and is skipped outright while it is off — or while its mix is 0 — so
  the ones you aren't using are free. The switch is a real bypass: it keeps
  every setting, so you can A/B an effect without losing the sound you were
  comparing against.
"""

NR_ANCHOR = ("- **Tempo sync** — the delay locks to a note division "
             "(1/8, 1/8., 1/8T…) and\n")

NR_NEW = """- **Two noise reducers**, at the head of the bus, which is what makes the
  thing usable as a **USB microphone**: an *adaptive* one that learns the
  steady part of a room — fans, hiss, a spinning disk — over a few seconds and
  subtracts it per band, with a Hold-to-learn button for when you would rather
  sample the room now; and a *fixed* one with the chain a USB microphone has
  inside it — rumble high-pass, 50/60 Hz hum notches, and a downward expander
  with a hold and a floor, so the gaps between words go quiet instead of going
  dead.
"""

USB_OLD = """- **USB Audio (UAC2) + USB MIDI** as one composite device — record the synth
  straight into a DAW *(ESP32-S3)*
"""

USB_NEW = """- **USB Audio (UAC2) + USB MIDI** as one composite device — record the synth
  straight into a DAW *(ESP32-S3 and ESP32-P4)*. With a microphone on the
  input and the noise reduction above, the same interface is a usable USB
  microphone: route the input through the FX bus and the computer sees one
  cleaned-up capture device.
"""

SUBS = [(FX_OLD, FX_NEW), (NR_ANCHOR, NR_NEW + NR_ANCHOR), (USB_OLD, USB_NEW)]


def main() -> int:
    p = ROOT / "README.md"
    s = p.read_text(encoding="utf-8")
    if "Two noise reducers" in s:
        print("  ok   README.md: already applied")
        return 0
    for old, new in SUBS:
        if s.count(old) != 1:
            print(f"  FAIL README.md: {s.count(old)} matches for {old[:44]!r}")
            return 1
        s = s.replace(old, new)
    p.write_text(s, encoding="utf-8")
    print("  +    README.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
