#!/usr/bin/env python3
"""S36: turn on the effects the factory presets rely on.

The per-effect enable switches default to OFF, and a factory preset is a
sparse list of overrides on the default patch — so without this, all 166
factory presets that use an effect would load with that effect bypassed and
the whole bank would go dry. Every preset that sets a unit's `mix` above zero
gets that unit's `on` set to 1, inserted immediately before the mix pair so
the pair reads as one statement.

Units the factory bank never touches (drive, phaser, flanger, EQ, compressor,
stereo) need nothing: their mix is already 0 in the default patch, so the
switch's state is unobservable.

Idempotent — a table that already names the switch is skipped. Run from the
repo root:  python tools/s36_patch_factory_presets.py
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "components" / "presets" / "presets_factory.cpp"

# Only the units the factory bank actually uses.
UNITS = ["CHO", "DLY", "GRN", "REV", "CRUSH"]

MIX_RE = re.compile(
    r"P\(FX_PID_(" + "|".join(UNITS) + r")_MIX,\s*([0-9.]+)f?\)"
)


def main() -> int:
    src = SRC.read_text(encoding="utf-8")
    # Specifically an FX switch: the engines have their own *_FLT_ON, which
    # an "_ON," test would match and silently skip the whole file.
    if "FX_PID_REV_ON" in src:
        print("nothing to do: factory presets already name their switches")
        return 0

    out = []
    added = 0
    touched = 0
    for line in src.split("\n"):
        hits = [m for m in MIX_RE.finditer(line) if float(m.group(2)) > 0.0]
        if not hits:
            out.append(line)
            continue
        indent = re.match(r"\s*", line).group(0)
        # One `on` per unit named on this line, in the order they appear, on
        # their own line above it — keeping the mix lines byte-identical makes
        # the diff readable and the values easy to re-audit.
        ons = " ".join(f"P(FX_PID_{m.group(1)}_ON, 1)," for m in hits)
        out.append(f"{indent}{ons}")
        out.append(line)
        added += len(hits)
        touched += 1

    SRC.write_text("\n".join(out), encoding="utf-8")
    print(f"presets_factory.cpp: {added} switches added across {touched} lines")
    return 0


if __name__ == "__main__":
    sys.exit(main())
