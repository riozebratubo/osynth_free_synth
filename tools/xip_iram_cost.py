#!/usr/bin/env python3
"""Measure what CONFIG_SPIRAM_XIP_FROM_PSRAM adds to `sram_low` on the P4.

Written in S47, when enabling XiP-from-PSRAM broke the link that had been
working with 5,572 B of sram_low to spare. The surprise it explains: XiP is
supposed to make IRAM residency *unnecessary*, and the first thing it does is
consume more of it.

`SPIRAM_XIP_FROM_PSRAM` selects `SPIRAM_FLASH_LOAD_TO_PSRAM`, and three linker
fragments key off that symbol -- esp_psram, esp_hw_support and esp_mm each add
`(noflash)` mappings, i.e. ordinary `.text` pulled into `.iram0.text`. That is
code which has to run while the MMU is being repointed from flash to PSRAM,
so it cannot itself live in either. It is a real requirement, not an oversight,
and it is paid up front whether or not you then drop OSYNTH_RENDER_IN_IRAM.

This sums those specific mappings from the object files, so it works on a
failed link -- which is the state enabling XiP leaves you in.

Usage:  python tools/xip_iram_cost.py [build_dir]
Needs IDF's toolchain on PATH (run the export script first).
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join(ROOT, "build")

# The `(noflash)` entries guarded by `if SPIRAM_FLASH_LOAD_TO_PSRAM = y:` in
# each component's linker.lf, as of IDF 6.0.2. Whole-object entries only --
# the per-function ones (esp_psram_init, s_xip_psram_placement, esp_mmu_map's
# four, pmu_param) are counted by symbol below.
WHOLE_OBJECTS = [
    ("esp-idf/esp_psram/libesp_psram.a", "mmu_psram_flash_v2.c.obj"),
    ("esp-idf/esp_hw_support/libesp_hw_support.a", "pmu_init.c.obj"),
    ("esp-idf/esp_hw_support/libesp_hw_support.a", "pmu_param.c.obj"),
    ("esp-idf/esp_mm/libesp_mm.a", "ext_mem_layout.c.obj"),
]

# (archive, object, [symbols]) for the per-function `(noflash)` entries.
BY_SYMBOL = [
    ("esp-idf/esp_psram/libesp_psram.a", "esp_psram.c.obj",
     ["esp_psram_chip_init", "esp_psram_init", "s_psram_chip_init",
      "s_xip_psram_placement"]),
    ("esp-idf/esp_mm/libesp_mm.a", "esp_mmu_map.c.obj",
     ["s_get_bus_mask", "s_reserve_irom_region", "s_reserve_drom_region",
      "esp_mmu_map_init"]),
]

SECTION = re.compile(r"^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s")
MEMBER = re.compile(r"^(\S+\.o(?:bj)?):\s*file format")
TEXT_RE = re.compile(r"^\.(text|literal|rodata|srodata)(\.|$)")


def find_tool(stem):
    for name in ("riscv32-esp-elf-" + stem,):
        found = shutil.which(name)
        if found:
            return found
    for base in (r"C:\Espressif\tools", os.path.expanduser("~/.espressif/tools")):
        if not os.path.isdir(base):
            continue
        for dirpath, _d, filenames in os.walk(base):
            for fn in filenames:
                if fn.startswith("riscv32-esp-elf-" + stem):
                    return os.path.join(dirpath, fn)
    return None


def object_text_bytes(objdump, archive, member):
    """Sum of .text/.literal/.rodata in one archive member."""
    try:
        out = subprocess.run([objdump, "-h", archive], capture_output=True,
                             text=True, timeout=180).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    cur, total, seen = None, 0, False
    for line in out.splitlines():
        m = MEMBER.match(line.strip())
        if m:
            cur = m.group(1)
            continue
        if cur != member:
            continue
        m = SECTION.match(line)
        if m and TEXT_RE.match(m.group(1)):
            total += int(m.group(2), 16)
            seen = True
    return total if seen else None


def symbol_bytes(nm, archive, member, wanted):
    """{symbol: size} for named symbols in one archive member."""
    try:
        out = subprocess.run([nm, "-S", "--defined-only", archive],
                             capture_output=True, text=True, timeout=180).stdout
    except (OSError, subprocess.SubprocessError):
        return {}
    cur, found = None, {}
    for line in out.splitlines():
        line = line.rstrip()
        if line.endswith(":") and (".obj" in line or ".o" in line):
            cur = line[:-1].strip()
            continue
        if cur != member:
            continue
        parts = line.split()
        if len(parts) >= 4 and parts[3] in wanted:
            found[parts[3]] = int(parts[1], 16)
    return found


def main(argv):
    build = argv[1] if len(argv) > 1 else DEFAULT_BUILD
    objdump, nm = find_tool("objdump"), find_tool("nm")
    if not objdump or not nm:
        print("no toolchain found -- run IDF's export script first")
        return 1

    print("IRAM added by CONFIG_SPIRAM_XIP_FROM_PSRAM")
    print("  (the `if SPIRAM_FLASH_LOAD_TO_PSRAM = y:` linker.lf entries)")
    print()
    total = 0

    print("  whole objects mapped (noflash):")
    for rel, member in WHOLE_OBJECTS:
        path = os.path.join(build, rel)
        n = object_text_bytes(objdump, path, member) if os.path.exists(path) else None
        if n is None:
            print("    %-34s %s" % (member, "not built / not found"))
            continue
        total += n
        print("    %-34s %9s" % (member, format(n, ",")))

    print()
    print("  individual functions mapped (noflash):")
    for rel, member, syms in BY_SYMBOL:
        path = os.path.join(build, rel)
        got = symbol_bytes(nm, path, member, set(syms)) if os.path.exists(path) else {}
        for s in syms:
            n = got.get(s)
            if n is None:
                print("    %-34s %s" % (s, "not found (inlined or gc'd)"))
                continue
            total += n
            print("    %-34s %9s" % (s, format(n, ",")))

    print()
    print("  TOTAL added to sram_low            %9s" % format(total, ","))
    print()
    print("  For scale: the last good link left 5,572 B of sram_low free,")
    print("  and OSYNTH_RENDER_IN_IRAM controls about 100,000 B of it.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
