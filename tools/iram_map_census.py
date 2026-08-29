#!/usr/bin/env python3
"""Attribute every byte of the ESP32-P4 `sram_low` region from a *successful*
link's map file.

Companion to tools/iram_budget.py. That tool works on a FAILED link (it sums
archives with objdump, pre-gc-sections, so it over-counts archives the image
barely uses). This one needs build/osynth.map -- a link that succeeded -- and
is exact: it reads the addresses the linker actually assigned.

    iram_budget.py      link is broken, you need a ranking
    iram_map_census.py  link works, you need to know where the bytes went

    sram_low  0x4FF00000 + 0x2BBD0 (179,152 B) = IRAM code + .sdata/.data
    sram_high 0x4FF40000 + 0x60000 (393,216 B) = the spill target + heap

.dram0.bss and .dram1.bss capture the SAME input patterns and .dram0.bss is
declared first, so .bss is *eligible* for sram_low; in practice sram_low is so
full that --enable-non-contiguous-regions spills all of it to .dram1.bss and
.dram0.bss measures 0. Read that off the REGION SUMMARY rather than assuming
either way -- the ordering is an IDF detail and has flipped before.

Usage:
    python tools/iram_map_census.py [map] [--objects] [--symbols] [--ours]
    --objects    per-object-file breakdown (IRAM)
    --symbols    biggest individual symbols in IRAM
    --ours       restrict the listings to this project's own components
    --top=N      how many rows per listing (default 40)
"""
import os
import re
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_MAP = os.path.join(ROOT, "build", "osynth.map")

SRAM_LOW = 0x4FF2BBD0 - 0x4FF00000  # 179,152 -- from build/.../ld/memory.ld

OURS = {
    "fx", "fx_gpl", "synth_core", "engines", "graph", "audio_io", "looper",
    "drums", "seqarp", "presets", "ble_ctrl", "midi", "chord", "codec",
    "local_ui", "persist", "usb_dev", "usb_host_midi", "main",
}

TRACKED = (".iram0.text", ".iram0.data", ".iram0.bss",
           ".dram0.data", ".dram0.bss", ".dram1.data", ".dram1.bss")
LOW_SECS = (".iram0.text", ".iram0.data", ".iram0.bss",
            ".dram0.data", ".dram0.bss")

# " .text.foo   0x4ff00330   0x2c  path/libfoo.a(bar.c.obj)"   -- one line
FULL = re.compile(r"^ (\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
# " .text.a_name_too_long_to_fit"  -- address/size/origin on the NEXT line
NAME_ONLY = re.compile(r"^ (\.\S+)\s*$")
CONT = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
# "                0x4ff00330                some_symbol"
SYMBOL = re.compile(r"^\s+0x([0-9a-f]+)\s{10,}(\S+)$")
OUTSEC_HDR = re.compile(r"^(\.\S+)(?:\s+0x([0-9a-f]+)\s+0x([0-9a-f]+))?")


def origin_key(origin):
    """path/libfoo.a(bar.c.obj) -> ("foo", "bar.c.obj")."""
    origin = origin.strip()
    m = re.match(r"^(.*?)\(([^)]+)\)$", origin)
    if m:
        lib, member = m.group(1), m.group(2)
    else:
        lib, member = origin, os.path.basename(origin)
    lib = os.path.basename(lib.replace("\\", "/").rstrip("/"))
    if lib.startswith("lib") and lib.endswith(".a"):
        lib = lib[3:-2]
    elif lib.endswith(".a"):
        lib = lib[:-2]
    else:
        lib = "<app>"
    return lib, os.path.basename(member)


def parse(path):
    sections = {s: defaultdict(int) for s in TRACKED}
    fills = defaultdict(int)
    symbols = defaultdict(list)
    chunks = defaultdict(list)   # every placed input section, in map order
    totals = {}

    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()

    start = 0
    for i, line in enumerate(lines):
        if line.startswith("Linker script and memory map"):
            start = i
            break

    cur = None      # current output section, if it is one we track
    pending = None  # input section name awaiting its address/size line
    last = None     # last placed chunk, so a symbol line can attach to it

    for line in lines[start:]:
        if not line.strip():
            continue
        if line[0] == ".":                      # output section header
            m = OUTSEC_HDR.match(line)
            name = m.group(1)
            cur = name if name in TRACKED else None
            if cur and m.group(3):
                totals[name] = int(m.group(3), 16)
            pending = last = None
            continue
        if cur is None:
            continue
        if line.lstrip().startswith("*fill*"):
            m = re.match(r"^\s*\*fill\*\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)", line)
            if m:
                fills[cur] += int(m.group(2), 16)
            continue

        if pending is not None:
            m = CONT.match(line)
            if m:
                size = int(m.group(2), 16)
                key = origin_key(m.group(3))
                sections[cur][key] += size
                last = (size, pending, key)
                chunks[cur].append([size, pending, key[0], key[1], None])
                pending = None
                continue
            pending = None

        m = FULL.match(line)
        if m and not m.group(1).startswith("*"):
            size = int(m.group(3), 16)
            key = origin_key(m.group(4))
            sections[cur][key] += size
            last = (size, m.group(1), key)
            chunks[cur].append([size, m.group(1), key[0], key[1], None])
            continue

        m = NAME_ONLY.match(line)
        if m:
            pending = m.group(1)
            continue

        m = SYMBOL.match(line)
        if m and last is not None:
            size, _sec, key = last
            symbols[cur].append((size, m.group(2), key[0], key[1]))
            if chunks[cur] and chunks[cur][-1][4] is None:
                chunks[cur][-1][4] = m.group(2)
            last = None
            continue

    # A missing total means the section header carried no size (empty section).
    for s in TRACKED:
        totals.setdefault(s, sum(sections[s].values()))
    return sections, symbols, fills, totals, chunks


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    path = args[0] if args else DEFAULT_MAP
    show_objects = "--objects" in argv
    show_symbols = "--symbols" in argv
    ours_only = "--ours" in argv
    top = 40
    for a in argv:
        if a.startswith("--top="):
            top = int(a.split("=", 1)[1])

    if not os.path.exists(path):
        print("no map at " + path + " -- this tool needs a link that SUCCEEDED.")
        print("For a failed link use: python tools/iram_budget.py")
        return 1

    sections, symbols, fills, totals, chunks = parse(path)
    low_used = sum(totals.get(s, 0) for s in LOW_SECS)
    free = SRAM_LOW - low_used

    print("=" * 76)
    print("REGION SUMMARY")
    print("=" * 76)
    for s in TRACKED:
        region = "sram_low " if s in LOW_SECS else "sram_high"
        print("  %-13s %s %10s  (%s padding)"
              % (s, region, format(totals.get(s, 0), ","), format(fills[s], ",")))
    print()
    print("  sram_low  %s / %s used  (%.1f%%)     FREE: %s"
          % (format(low_used, ","), format(SRAM_LOW, ","),
             100.0 * low_used / SRAM_LOW, format(free, ",")))
    if free < 8192:
        print("  ^ under 8 KB. Any new IRAM function can break the link.")
    print()

    per_lib = defaultdict(lambda: [0, 0])
    for key, n in sections[".iram0.text"].items():
        per_lib[key[0]][0] += n
    for s in (".dram0.data", ".iram0.data", ".iram0.bss", ".dram0.bss"):
        for key, n in sections[s].items():
            per_lib[key[0]][1] += n

    print("=" * 76)
    print("sram_low BY ARCHIVE   iram = .iram0.text,  data = .dram0.data/.sdata")
    print("=" * 76)
    print("  %-30s%10s%10s%11s" % ("archive", "iram", "data", "total"))
    print("  " + "-" * 68)
    rows = sorted(per_lib.items(), key=lambda kv: -(kv[1][0] + kv[1][1]))
    shown = [r for r in rows if not ours_only or r[0] in OURS][:top]
    for lib, (ir, da) in shown:
        mark = " *" if lib in OURS else ""
        print("  %-30s%10s%10s%11s%s"
              % (lib, format(ir, ","), format(da, ","), format(ir + da, ","), mark))
    ours_iram = sum(v[0] for k, v in per_lib.items() if k in OURS)
    ours_all = sum(v[0] + v[1] for k, v in per_lib.items() if k in OURS)
    print("  " + "-" * 68)
    print("  %-30s%10s%10s%11s"
          % ("OURS (* rows above)", format(ours_iram, ","), "",
             format(ours_all, ",")))
    print("  %-30s%10s%10s%11s"
          % ("ALL", format(sum(v[0] for v in per_lib.values()), ","),
             format(sum(v[1] for v in per_lib.values()), ","),
             format(sum(sum(v) for v in per_lib.values()), ",")))
    print()

    if show_objects:
        print("=" * 76)
        print("IRAM BY OBJECT FILE")
        print("=" * 76)
        rows = sorted(sections[".iram0.text"].items(), key=lambda kv: -kv[1])
        rows = [r for r in rows if not ours_only or r[0][0] in OURS]
        for (lib, member), n in rows[:top]:
            print("  %9s  %s:%s" % (format(n, ","), lib, member))
        print()

    if show_symbols:
        print("=" * 76)
        print("BIGGEST IRAM SYMBOLS")
        print("=" * 76)
        rows = sorted(chunks[".iram0.text"], key=lambda r: -r[0])
        rows = [r for r in rows if not ours_only or r[2] in OURS]
        for size, sec, lib, member, sym in rows[:top]:
            # Prefer the symbol the map printed; fall back to the input-section
            # name, which -ffunction-sections makes ".text.<mangled name>".
            name = sym or re.sub(r"^\.(text|iram\d*|literal)\.", "", sec)
            print("  %8s  %-58s %s:%s"
                  % (format(size, ","), name[:58], lib, member))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
