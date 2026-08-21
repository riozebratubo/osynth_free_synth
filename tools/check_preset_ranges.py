#!/usr/bin/env python3
"""Check every factory preset value against its parameter's registered range.

Why this exists: a preset is a bare {id, value} pair and ParamStore clamps on
set, so an out-of-range value is not an error — it is a preset that quietly
sounds like something other than what was written. With 48 slots per engine
hand-written, that is not something to catch by ear. Added in S33, when the
factory banks went from 16 slots to 48.

Ranges are matched by *symbol*, not by numeric id. Every engine registers its
parameters in the same 0x02xx range (they are mutually exclusive at runtime),
so SUB_PID_FLT_CUTOFF and ADD_PID_P11_LEVEL are both 0x020A — an id-keyed
lookup silently checks values against the wrong engine's parameter.

Run from anywhere:  python tools/check_preset_ranges.py
Exit status is non-zero if any value falls outside its range.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Sources whose ParamDesc tables carry the ranges.
RANGE_SOURCES = [
    "components/engines/engine_subtractive.cpp",
    "components/engines/engine_wavetable.cpp",
    "components/engines/engine_fm.cpp",
    "components/engines/engine_additive.cpp",
    "components/engines/engine_granular.cpp",
    "components/fx/fx.cpp",
    "components/synth_core/synth_voice.cpp",
    "components/seqarp/seqarp.cpp",
    "components/presets/presets.cpp",
]

NUM = r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?f?"


def consts(src: str) -> dict[str, tuple[float, float]]:
    """`constexpr float kX = <num>;` -> (tightest, widest).

    Several FX ceilings are defined twice behind `#if CONFIG_SPIRAM` — 1.5 s
    of delay on the S3, 0.4 s on the classic ESP32. A preset written for the
    PSRAM target is not wrong on the classic one, it just clamps, so the two
    bounds are kept apart and reported differently.
    """
    out: dict[str, list[float]] = {}
    for m in re.finditer(
        r"constexpr\s+(?:float|int)\s+(\w+)\s*=\s*(" + NUM + r")\s*;", src
    ):
        out.setdefault(m.group(1), []).append(float(m.group(2).rstrip("f")))
    return {k: (min(v), max(v)) for k, v in out.items()}


def num_or_const(
    tok: str, table: dict[str, tuple[float, float]]
) -> tuple[float, float] | None:
    tok = tok.strip()
    if re.fullmatch(NUM, tok):
        v = float(tok.rstrip("f"))
        return (v, v)
    return table.get(tok)


def load_ranges() -> dict[str, tuple[float, float, float, float, str]]:
    """symbol -> (lo_tight, hi_tight, lo_wide, hi_wide, registered name)."""
    out: dict[str, tuple[float, float, float, float, str]] = {}
    for rel in RANGE_SOURCES:
        p = ROOT / rel
        if not p.exists():
            continue
        src = p.read_text(encoding="utf-8")
        ktab = consts(src)
        # Deliberately tight. An earlier draft allowed [^,]* between the id
        # and the name, which matches across newlines — one stray brace
        # earlier in the file then produced a match spanning the entry that
        # followed it, and that entry was silently skipped as "unresolved".
        pat = (
            r"\{\s*(\w+)\s*,\s*\"([^\"]+)\"\s*,\s*"
            r"ParamType::\w+\s*,\s*ParamCurve::\w+\s*,\s*"
            r"([\w.+\-]+)\s*,\s*([\w.+\-]+)\s*,"
        )
        for m in re.finditer(pat, src):
            sym, name, lo_s, hi_s = m.groups()
            lo, hi = num_or_const(lo_s, ktab), num_or_const(hi_s, ktab)
            if lo is None or hi is None:
                continue
            # tight = every target accepts it; wide = at least one does
            out[sym] = (lo[1], hi[0], lo[0], hi[1], name)
        if "ADD_DRAWBAR" in src:
            for n in range(1, 17):
                out[f"ADD_P({n})"] = (0.0, 1.0, 0.0, 1.0, f"add.p{n}.level")
    return out


def main() -> int:
    ranges = load_ranges()
    src = (ROOT / "components/presets/presets_factory.cpp").read_text(
        encoding="utf-8"
    )

    bad = 0
    warn = 0
    checked = 0
    unresolved: dict[str, int] = {}
    current = "?"
    for line in src.splitlines():
        m = re.match(r"static const preset_pair_t (\w+)\[\]", line)
        if m:
            current = m.group(1)
        for pm in re.finditer(
            r"P\(\s*(ADD_P\(\d+\)|\w+)\s*,\s*(" + NUM + r")\s*\)", line
        ):
            sym, val = pm.groups()
            if sym.startswith("SYNTH_PID_MOD_"):
                continue  # emitted by MOD(); the dest is an id, not a value
            key = sym
            if key not in ranges and key.startswith("SYNTH_"):
                key = key[len("SYNTH_"):]  # synth_voice.cpp drops the prefix
            if key not in ranges:
                unresolved[sym] = unresolved.get(sym, 0) + 1
                continue
            lo_t, hi_t, lo_w, hi_w, name = ranges[key]
            v = float(val.rstrip("f"))
            checked += 1
            if v < lo_w or v > hi_w:
                bad += 1
                print(
                    f"  ERROR {current}: {name} = {v:g} outside "
                    f"[{lo_w:g}, {hi_w:g}] on every target"
                )
            elif v < lo_t or v > hi_t:
                warn += 1
                print(
                    f"  warn  {current}: {name} = {v:g} exceeds the smaller "
                    f"target's [{lo_t:g}, {hi_t:g}]; clamps there, fine on PSRAM"
                )

    print(f"checked {checked} preset values against registered ranges")
    if unresolved:
        # Maxima written as cast expressions ((float)kMaxUnison and friends);
        # too few to be worth teaching this script to evaluate C.
        print(f"  unresolved (not checked): {sorted(unresolved)}")
    if warn:
        print(f"{warn} value(s) clamp on the smaller target (not an error)")
    print(f"{bad} value(s) out of range." if bad else "no out-of-range values")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
