#!/usr/bin/env python3
"""Check each fixed engine's kParams table against its PIdx enum.

Why this exists: the tables are declared `const ParamDesc kParams[P_COUNT]`,
and C++ happily accepts *fewer* initializers than the array size — the tail is
zero-filled and the engine then registers phantom parameters with id 0. A
miscount is silent at compile time and confusing at runtime, so count them
here instead. Added in S33, when four engines gained filter parameters at once.

Run from anywhere:  python tools/check_param_tables.py
Exit status is non-zero if any table disagrees with its enum.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

TARGETS = [
    ("subtractive", "components/engines/engine_subtractive.cpp", "SUB_PID_"),
    ("wavetable", "components/engines/engine_wavetable.cpp", "WT_PID_"),
    ("fm", "components/engines/engine_fm.cpp", "FM_PID_"),
    ("additive", "components/engines/engine_additive.cpp", "ADD_PID_"),
    ("granular", "components/engines/engine_granular.cpp", "GRAN_PID_"),
    ("fx", "components/fx/fx.cpp", "FX_PID_"),
]


def enum_names(src: str) -> list[str]:
    """The PIdx enumerators, in order, expanded past `NAME = expr` forms."""
    m = re.search(r"enum PIdx \{(.*?)\};", src, re.S)
    if not m:
        return []
    # Strip comments first: a trailing /* ... */ sits *after* the comma, so
    # splitting before removing it swallows the enumerator that follows.
    body = re.sub(r"/\*.*?\*/", " ", m.group(1), flags=re.S)
    body = re.sub(r"//[^\n]*", " ", body)
    names = []
    for tok in body.split(","):
        name = tok.split("=")[0].strip()
        if name:
            names.append(name)
    return names


def table_entries(src: str, prefix: str) -> int:
    """Count `{PREFIX_..., "name", ...}` rows in the kParams initializer."""
    m = re.search(r"kParams\[P_COUNT\] = \{(.*?)\n\};", src, re.S)
    if not m:
        return -1
    body = m.group(1)
    # One row per id reference; drawbar macros expand to one row each.
    rows = len(re.findall(r"\{\s*(?:\(uint16_t\)\()?" + prefix, body))
    rows += len(re.findall(r"^\s*ADD_DRAWBAR\(", body, re.M))
    return rows


def check_graph() -> int:
    """The graph has the same hazard twice over.

    `kKinds[(int)Kind::Count]` is short-initializable like the engine tables,
    and separately each kind declares `pidx::X_N` parameters while its
    ParamSpec array is sized by its own initializer list — if they disagree,
    register_slot() walks off the end of the array.
    """
    bad = 0
    model = (ROOT / "components/graph/graph_model.cpp").read_text(encoding="utf-8")
    header = (ROOT / "components/graph/include/graph_model.h").read_text(
        encoding="utf-8"
    )

    m = re.search(r"enum class Kind : uint8_t \{(.*?)\};", header, re.S)
    body = re.sub(r"/\*.*?\*/", " ", m.group(1), flags=re.S)
    body = re.sub(r"//[^\n]*", " ", body)
    kinds = [t.split("=")[0].strip() for t in body.split(",") if t.strip()]
    n_kinds = len(kinds) - 1  # Count itself

    m = re.search(r"kKinds\[\(int\)Kind::Count\] = \{(.*?)\n\};", model, re.S)
    rows = len(re.findall(r'^\s*\{"', m.group(1), re.M))
    status = "ok" if rows == n_kinds else "MISMATCH"
    if rows != n_kinds:
        bad += 1
    print(f"{'graph kinds':12s} {status:9s} enum={n_kinds:3d} table={rows:3d}")

    # Per-kind: pidx::<X>_N against the length of its ParamSpec array. The
    # kind table names both together, as `pidx::FLT_N, kPFilter`.
    for n_sym, spec_name in re.findall(r"pidx::(\w+_N), (kP\w+),", model):
        m = re.search(rf"{spec_name}\[\] = \{{(.*?)\n\}};", model, re.S)
        if not m:
            continue
        rows = len(re.findall(r'^\s*\{"', m.group(1), re.M))
        # X_N is the last enumerator of its own enum in the header.
        me = re.search(rf"(\w+P) \{{([^}}]*?){n_sym}[,\s]*\}};", header, re.S)
        if not me:
            continue
        eb = re.sub(r"/\*.*?\*/", " ", me.group(2), flags=re.S)
        count = len([t for t in eb.split(",") if t.strip()])
        status = "ok" if rows == count else "MISMATCH"
        if rows != count:
            bad += 1
        print(f"  {spec_name:10s} {status:9s} {n_sym}={count:3d} specs={rows:3d}")
    return bad


def check_factory_banks() -> int:
    """Every factory bank must be exactly PRESETS_FACTORY_SLOTS rows.

    Same hazard as the tables above, with a worse ending: a short initializer
    zero-fills the tail, and fetch_snapshot() copies `name` unconditionally,
    so selecting one of those slots dereferences a null. Also checks that
    every F(name, table) names a table that exists and uses it once.
    """
    bad = 0
    src = (ROOT / "components/presets/presets_factory.cpp").read_text(
        encoding="utf-8"
    )
    hdr = (ROOT / "components/presets/include/presets.h").read_text(
        encoding="utf-8"
    )
    slots = int(re.search(r"#define PRESETS_FACTORY_SLOTS\s+(\d+)", hdr).group(1))

    body = re.search(
        r"g_factory_presets\[SYNTH_ENGINE_COUNT\]\[PRESETS_FACTORY_SLOTS\] = \{(.*)\n\};",
        src,
        re.S,
    ).group(1)
    # Comments first: the modular bank's note contains a literal "{id, value}",
    # and a brace scanner that cannot see comments reads it as an empty bank.
    body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
    body = re.sub(r"//[^\n]*", " ", body)
    # Split into banks at brace depth 1.
    banks, depth, cur = [], 0, ""
    for ch in body:
        if ch == "{":
            depth += 1
            if depth == 1:
                cur = ""
                continue
        elif ch == "}":
            depth -= 1
            if depth == 0:
                banks.append(cur)
                continue
        if depth >= 1:
            cur += ch

    defined = set(re.findall(r"^static const preset_pair_t (\w+)\[\]", src, re.M))
    used: list[str] = []
    for i, bank in enumerate(banks):
        rows = len(re.findall(r'F\(\s*"', bank)) + len(
            re.findall(r'\{\s*"init"', bank)
        )
        used += re.findall(r"F\(\s*\"[^\"]*\",\s*(\w+)\s*\)", bank)
        status = "ok" if rows == slots else "MISMATCH"
        if rows != slots:
            bad += 1
        print(f"  bank {i:<7d} {status:9s} slots={slots:3d} rows={rows:3d}")

    missing = [t for t in used if t not in defined]
    dupes = sorted({t for t in used if used.count(t) > 1})
    if missing:
        print(f"  undefined tables referenced: {missing}")
        bad += 1
    if dupes:
        print(f"  tables used more than once: {dupes}")
        bad += 1
    if not missing and not dupes:
        unused = sorted(
            t
            for t in defined
            if t not in used and re.match(r"k(Sub|Add|Fm|Wt)", t)
        )
        if unused:
            print(f"  defined but never banked: {unused}")
            bad += 1
    return bad


def main() -> int:
    bad = 0
    for name, rel, prefix in TARGETS:
        src = (ROOT / rel).read_text(encoding="utf-8")
        names = enum_names(src)
        if not names or names[-1] != "P_COUNT":
            print(f"{name:12s} SKIP  (no PIdx enum ending in P_COUNT)")
            continue
        expected = len(names) - 1  # P_COUNT itself is not a parameter
        # Engines using `NAME = BASE + n` (additive's drawbars) understate the
        # enum count; fall back to the declared block size where that happens.
        m = re.search(r"TILT = P1_LEVEL \+ kPartials", src)
        if m:
            expected += 16 - 1  # the 16 drawbars collapse to one enumerator
        got = table_entries(src, prefix)
        status = "ok" if got == expected else "MISMATCH"
        if got != expected:
            bad += 1
        print(f"{name:12s} {status:9s} enum={expected:3d} table={got:3d}")
    bad += check_graph()
    print("factory presets")
    bad += check_factory_banks()
    if bad:
        print(f"\n{bad} table(s) disagree with their enum.")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
