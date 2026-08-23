#!/usr/bin/env python3
"""Sum the IRAM-resident (.iram1.*) sections of every object in build/.

Reads only what the compiler already produced — it builds nothing. IRAM_ATTR
emits `.iram1.*`, so this is the direct cost of everything deliberately placed
in internal RAM, per component. It does NOT see the sections IDF's linker
*fragments* relocate (whole-archive `noflash` mappings such as spi_flash's, or
the per-Kconfig ISR placements); those are named separately by the callers of
this script.

Usage: python tools/s41_iram_census.py [build_dir]
"""
import re
import subprocess
import sys
from pathlib import Path

OBJDUMP = (r"C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204"
           r"\riscv32-esp-elf\bin\riscv32-esp-elf-objdump.exe")


def sections(obj):
    out = subprocess.run([OBJDUMP, "-h", str(obj)], capture_output=True,
                         text=True).stdout
    for m in re.finditer(r"^\s*\d+\s+(\S+)\s+([0-9a-f]{8})", out, re.M):
        yield m.group(1), int(m.group(2), 16)


def main(argv):
    build = Path(argv[1]) if len(argv) > 1 else Path("build")
    per_comp, per_obj = {}, []
    for obj in build.rglob("*.obj"):
        if "bootloader" in obj.parts:
            continue
        tot = sum(sz for name, sz in sections(obj) if name.startswith(".iram1"))
        if tot == 0:
            continue
        # build/esp-idf/<component>/CMakeFiles/... -> <component>
        parts = obj.parts
        comp = parts[parts.index("esp-idf") + 1] if "esp-idf" in parts else "?"
        per_comp[comp] = per_comp.get(comp, 0) + tot
        per_obj.append((tot, comp, obj.name))

    print(f"{'component':28} {'.iram1 bytes':>12}")
    print("-" * 42)
    for comp, tot in sorted(per_comp.items(), key=lambda kv: -kv[1]):
        print(f"{comp:28} {tot:12d}")
    print("-" * 42)
    print(f"{'TOTAL':28} {sum(per_comp.values()):12d}")
    print("\ntop objects:")
    for tot, comp, name in sorted(per_obj, reverse=True)[:18]:
        print(f"  {tot:7d}  {comp}/{name}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
