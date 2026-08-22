#!/usr/bin/env python3
"""S39b wording: say exactly what `src` = input leaves alone.

"the synth comes out sample for sample unchanged" is loose in the one way this
codebase minds: the bus sum obviously changes, since a correction is added to
it. What is true, and is the claim worth making, is that nothing the unit does
is a function of the synth and no gain of any kind is applied to it — the only
thing that reaches the bus is a term derived from the input alone.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

SUBS = [(
    "components/fx/include/fx.h",
    """ * At `input` it processes only what audio_io mixed in, and the synth beside it
 * comes out sample for sample unchanged. That is exact rather than
 * approximate, and it is worth knowing why: both units are *corrections*.""",
    """ * At `input` it processes only what audio_io mixed in. Nothing it does is a
 * function of the synth beside it and no gain of any kind is applied to that
 * — the only thing reaching the bus is a term derived from the input alone,
 * so muting the input leaves the output bit-identical to the unit being off.
 * That is exact rather than approximate, and it is worth knowing why: both
 * units are *corrections*."""
), (
    "components/fx/fx.cpp",
    """ * position and adds only the difference back. It needs `in.route` = fx, and""",
    """ * position and adds only the difference back — so nothing reaching the bus is
 * a function of anything but the input. It needs `in.route` = fx, and"""
), (
    "private_docs/PARAM_MAP.md",
    """* `input` — only what `audio_io` mixed in, with the synth beside it coming out
  sample for sample unchanged. That is exact rather than approximate, because""",
    """* `input` — only what `audio_io` mixed in. Nothing the unit does is a function
  of the synth beside it and no gain is applied to that, so muting the input
  leaves the output identical to the unit being off. Exact, not approximate,
  because"""
)]


def main() -> int:
    rc = 0
    for rel, old, new in SUBS:
        p = ROOT / rel
        s = p.read_text(encoding="utf-8")
        # The last line of the replacement, not the first: a first line
        # shared with the text being replaced makes the guard fire before
        # the edit has happened.
        if new.strip().splitlines()[-1] in s:
            print(f"  ok   {rel}: already applied")
            continue
        if s.count(old) != 1:
            print(f"  FAIL {rel}: {s.count(old)} matches")
            rc = 1
            continue
        p.write_text(s.replace(old, new), encoding="utf-8")
        print(f"  +    {rel}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
