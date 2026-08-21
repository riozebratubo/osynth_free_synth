#!/usr/bin/env python3
"""Check every factory preset's mod-matrix destination against what its engine
actually reads through the matrix.

Why this exists (S38): a matrix slot aimed at a parameter the bound engine
does not route through synth_mod_apply() is *silently inert* — by design, so
that a slot surviving an engine switch does nothing rather than something
wrong (synth_mod.h). That is the right runtime behaviour and a terrible
failure mode for a factory preset, which then ships promising a modulation it
never performs. Nothing catches it: the id is real, the value is in range,
the preset loads, and the only symptom is that a control does not move.

It is easy to write: the destination list per engine is exactly the set of
`synth_mod_apply(<PID>, ...)` calls in that engine's .cpp, which no reviewer
holds in their head while writing a preset. So read it out of the source and
cross-check the MOD() rows in presets_factory.cpp against it.

Global destinations (0x01xx engine-common, and anything an engine does not
own) are not checked — this only knows about the per-engine 0x02xx sets.

Run from anywhere:  python tools/check_mod_dests.py
Exit status is non-zero if any preset aims a slot somewhere inert.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Engine prefix -> the .cpp whose synth_mod_apply() calls define its dest set.
ENGINES = {
    "SUB_PID_": "components/engines/engine_subtractive.cpp",
    "ADD_PID_": "components/engines/engine_additive.cpp",
    "FM_PID_": "components/engines/engine_fm.cpp",
    "WT_PID_": "components/engines/engine_wavetable.cpp",
    "GRAN_PID_": "components/engines/engine_granular.cpp",
}

FACTORY = "components/presets/presets_factory.cpp"


def applied(path):
    """The PIDs an engine passes to synth_mod_apply()."""
    src = (ROOT / path).read_text(encoding="utf-8")
    return set(re.findall(r"synth_mod_apply\(\s*([A-Z0-9_]+)\s*,", src))


def main():
    dests = {}
    for prefix, path in ENGINES.items():
        dests[prefix] = applied(path)
        if not dests[prefix]:
            print(f"warn: no synth_mod_apply() calls found in {path}")

    src = (ROOT / FACTORY).read_text(encoding="utf-8")

    # Which preset table each MOD() row belongs to, so a failure names it.
    tables = [(m.start(), m.group(1))
              for m in re.finditer(r"static const preset_pair_t (k\w+)\[\]", src)]

    def table_at(pos):
        name = "?"
        for start, nm in tables:
            if start <= pos:
                name = nm
            else:
                break
        return name

    bad = []
    checked = 0
    for m in re.finditer(r"MOD\(\s*\d+\s*,\s*[A-Z0-9_]+\s*,\s*([A-Z0-9_]+)\s*,", src):
        dest = m.group(1)
        prefix = next((p for p in ENGINES if dest.startswith(p)), None)
        if prefix is None:
            continue  # a global/engine-common destination; not ours to judge
        checked += 1
        if dest not in dests[prefix]:
            bad.append((table_at(m.start()), dest, prefix))

    for table, dest, prefix in bad:
        engine = ENGINES[prefix].rsplit("engine_", 1)[1][:-4]
        print(f"  INERT  {table}: {dest} is not a {engine} matrix destination")

    print(f"checked {checked} engine-owned matrix destinations")
    if bad:
        print(f"{len(bad)} preset slot(s) aim at a parameter the engine never "
              f"reads through synth_mod_apply() — they would load and do nothing")
        return 1
    print("every destination is one its engine actually reads")
    return 0


if __name__ == "__main__":
    sys.exit(main())
