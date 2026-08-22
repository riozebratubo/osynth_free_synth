#!/usr/bin/env python3
"""S39b: PARAM_MAP.md + README wording for the `src` selector.

private_docs/ is gitignored, so this script is the only version-controlled
record of what the parameter map now says about these two units.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

PMAP = [(
    """They run **before the vocoder**, `fx.anr` first and `fx.nr` second, and that""",

    """Each has a **`src`** selector, and it is the control that decides whether
these are microphone cleanup or an effect on the instrument:

* `bus` (default) — the finished mix, which is right for a noisy sampled loop
  and wrong for a synth that was never noisy in the first place.
* `input` — only what `audio_io` mixed in, with the synth beside it coming out
  sample for sample unchanged. That is exact rather than approximate, because
  both units are *corrections*: the adaptive one's band sum is already (g-1)
  times each band, and the fixed one's is `g*filt(x) - x`. Neither ever needed
  the bus to work, only something to be a difference from, so each runs on the
  block `audio_io_in_fx_block()` hands back and adds the result to the bus.

`input` requires **`in.route` = `fx`**: that is the only position summed into
the bus by the time the FX bus runs. From `mon` or `dry` there is nothing here
yet to correct and the unit stays inert rather than inventing a correction for
a signal that arrives later.

They run **before the vocoder**, `fx.anr` first and `fx.nr` second, and that"""
), (
    """| 3 | `fx.anr.on` = 1, then hold **Hold to learn** for a second in a quiet room |
| 4 | `fx.nr.on` = 1, `fx.nr.thresh` just under the quietest thing worth keeping |""",

    """| 3 | `fx.anr.src` = `input`, `fx.anr.on` = 1, then hold **Hold to learn** for a second in a quiet room |
| 4 | `fx.nr.src` = `input`, `fx.nr.on` = 1, `fx.nr.thresh` just under the quietest thing worth keeping |"""
), (
    """profile;
* if two whole windows pass with nothing offered, the estimate may climb""",

    """profile — and this one is not dropped at `src` = `input` either: a sung note,
  a bowed string and a guitar drone are all steady for longer than a window,
  and a denoiser that learns one removes it;
* if two whole windows pass with nothing offered, the estimate may climb"""
), (
    """| `0x03E0` | `fx.anr.on`      | bool  | 0..1           | 0       |                                     |""",

    """| `0x03E0` | `fx.anr.on`      | bool  | 0..1           | 0       |                                     |
| `0x03EA` | `fx.anr.src`     | enum  | 0..1           | bus     | bus / input — see above; `input` needs `in.route` = fx |"""
), (
    """| `0x03F0` | `fx.nr.on`      | bool  | 0..1            | 0       |                                    |""",

    """| `0x03F0` | `fx.nr.on`      | bool  | 0..1            | 0       |                                    |
| `0x03F9` | `fx.nr.src`     | enum  | 0..1            | bus     | bus / input — see above; `input` needs `in.route` = fx |"""
), (
    """| `0x03F3` | `fx.nr.thresh`  | float | -80..0 dB       | -45     | peak, measured after the filters |""",

    """| `0x03F3` | `fx.nr.thresh`  | float | -80..0 dB       | -45     | peak, measured after the filters and on whatever `src` names — at `input` that is the input *as it arrives on the bus*, i.e. after `in.gain` |"""
), (
    """These two blocks **fill the 0x03xx namespace**. A tenth FX unit needs a page
of its own.""",

    """These two blocks **fill the 0x03xx namespace** (0x03EB and 0x03FA-0x03FF are
what is left of it). A tenth FX unit needs a page of its own."""
)]

RDME = [(
    """  with a hold and a floor, so the gaps between words go quiet instead of going
  dead.
""",
    """  with a hold and a floor, so the gaps between words go quiet instead of going
  dead. Either can be pointed at the **input alone** instead of the bus, which
  is what keeps a denoiser off an instrument that was never noisy: the unit
  corrects only what came in through the jack, and the synth beside it comes
  out unchanged.
""")]


def apply(rel, subs, done_marker):
    p = ROOT / rel
    s = p.read_text(encoding="utf-8")
    if done_marker in s:
        print(f"  ok   {rel}: already applied")
        return 0
    for old, new in subs:
        if s.count(old) != 1:
            print(f"  FAIL {rel}: {s.count(old)} matches for {old[:44]!r}")
            return 1
        s = s.replace(old, new)
    p.write_text(s, encoding="utf-8")
    print(f"  +    {rel}")
    return 0


def main() -> int:
    rc = apply("private_docs/PARAM_MAP.md", PMAP, "`fx.anr.src`")
    rc |= apply("README.md", RDME, "pointed at the **input alone**")
    return rc


if __name__ == "__main__":
    sys.exit(main())
