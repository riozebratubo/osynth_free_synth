#!/usr/bin/env python3
"""Probe: does fx.cpp's PIdx enum line up, item for item, with kParams?

check_param_tables.py only counts rows. A table whose *order* drifted from the
enum has the same count and is silently wrong: pv(EQ_LOW) then reads whatever
parameter happens to sit at that index. This compares the enumerator name with
the FX_PID_* id of the row at the same index, and prints every mismatch.

Run:  python tools/probe_fx_param_order.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = (ROOT / "components/fx/fx.cpp").read_text(encoding="utf-8", errors="replace")


def strip_comments(s: str) -> str:
    s = re.sub(r"/\*.*?\*/", " ", s, flags=re.S)
    return re.sub(r"//[^\n]*", " ", s)


m = re.search(r"enum PIdx \{(.*?)\};", SRC, re.S)
names = [t.split("=")[0].strip()
         for t in strip_comments(m.group(1)).split(",") if t.split("=")[0].strip()]
names = [n for n in names if n != "P_COUNT"]

m = re.search(r"kParams\[P_COUNT\] = \{(.*?)\n\};", SRC, re.S)
rows = re.findall(r"\{\s*FX_PID_([A-Z0-9_]+)\s*,\s*\"([^\"]+)\"", m.group(1))

print(f"enum={len(names)} rows={len(rows)}")
bad = 0
for i, name in enumerate(names):
    if i >= len(rows):
        print(f"  [{i:3d}] {name:14s} -> (no row)")
        bad += 1
        continue
    pid, sname = rows[i]
    if pid != name:
        print(f"  [{i:3d}] enum {name:14s} != row FX_PID_{pid:14s} ({sname})")
        bad += 1
print("MISMATCHES:", bad)
sys.exit(1 if bad else 0)
