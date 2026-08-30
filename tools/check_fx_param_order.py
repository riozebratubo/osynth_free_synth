#!/usr/bin/env python3
"""Check components/fx/fx.cpp's PIdx enum against the kParams[] table.

fx_init() binds the two by *index*:

    for (size_t i = 0; i < P_COUNT; ++i) s_p[i] = ps.valuePtr(kParams[i].id);

so pv(REV_COMP) reads FX_PID_REV_COMP only while the enum's Nth name and the
table's Nth entry are the same parameter. Nothing in the build enforces that --
the static_assert next to them compares counts, and a count matches just as
well when two entries are swapped. This script compares them name for name.

Usage: python tools/check_fx_param_order.py [path/to/fx.cpp]
Exit 0 when they agree, 1 when they do not.
"""
import re
import sys
from pathlib import Path

src = Path(sys.argv[1] if len(sys.argv) > 1 else "components/fx/fx.cpp").read_text(
    encoding="utf-8", errors="replace"
)

m = re.search(r"enum PIdx \{(.*?)\bP_COUNT\b", src, re.S)
if not m:
    sys.exit("could not find `enum PIdx {`")
body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
enum_names = [t.strip() for t in body.split(",") if t.strip()]

m = re.search(r"const ParamDesc kParams\[P_COUNT\] = \{(.*?)\n\};", src, re.S)
if not m:
    sys.exit("could not find `const ParamDesc kParams[P_COUNT]`")
table = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
# One entry per `{FX_PID_X, "fx.y.z", ...`
entries = re.findall(r"\{\s*(FX_PID_[A-Z0-9_]+)\s*,\s*\"([^\"]+)\"", table)

print(f"enum PIdx: {len(enum_names)} names")
print(f"kParams[]: {len(entries)} entries")

bad = 0
for i in range(max(len(enum_names), len(entries))):
    name = enum_names[i] if i < len(enum_names) else "<missing>"
    pid, text = entries[i] if i < len(entries) else ("<missing>", "")
    if pid != f"FX_PID_{name}":
        print(f"  [{i:3d}] enum {name:<14} -> table {pid} ({text})")
        bad += 1

if len(enum_names) != len(entries):
    print("MISMATCH: the two lists are different lengths")
    sys.exit(1)
if bad:
    print(f"MISMATCH: {bad} of {len(enum_names)} slots disagree")
    sys.exit(1)
print("OK: enum and table agree slot for slot")
