#!/usr/bin/env python3
"""Check that every user-settable OSYNTH Kconfig symbol is mentioned in one of
the sdkconfig.defaults files (set, or documented as a commented-out default).

Symbols without a prompt (e.g. OSYNTH_SD_BUS, which is derived) are skipped:
they are not user-settable and have no business in a defaults file.

Usage: python tools/check_sdkconfig_coverage.py
Exit status 1 if anything is uncovered.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KCONFIG = ROOT / "components" / "synth_core" / "Kconfig.projbuild"
DEFAULTS = [
    ROOT / "sdkconfig.defaults",
    ROOT / "sdkconfig.defaults.esp32",
    ROOT / "sdkconfig.defaults.esp32s3",
    ROOT / "sdkconfig.defaults.esp32p4",
]


def settable_symbols(text):
    """Symbols declared with `config X` that carry a prompt (bool/int/choice
    member with a quoted label on the type line)."""
    out, current, has_prompt = [], None, False
    for line in text.splitlines():
        m = re.match(r"\s*config\s+(\w+)\s*$", line)
        if m:
            if current and has_prompt:
                out.append(current)
            current, has_prompt = m.group(1), False
            continue
        if current and re.match(r'\s*(bool|int|string|hex)\s+"', line):
            has_prompt = True
    if current and has_prompt:
        out.append(current)
    return [s for s in out if s.startswith("OSYNTH_")]


def main():
    syms = settable_symbols(KCONFIG.read_text(encoding="utf-8"))
    blob = "\n".join(p.read_text(encoding="utf-8") for p in DEFAULTS)
    missing = [s for s in syms if f"CONFIG_{s}" not in blob]

    print(f"{len(syms)} settable OSYNTH symbols, {len(syms) - len(missing)} covered")
    for s in missing:
        print(f"  MISSING: CONFIG_{s}")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
