#!/usr/bin/env python3
"""Report what fills the ESP32-P4's `sram_low`, the region that actually binds.

THE LAYOUT, because none of it is guessable from the errors:

    memory.ld:
      sram_low  (RWX) : org = 0x4FF00000, len = 0x4FF2BBD0 - 0x4FF00000  175 KB
      sram_high (RW)  : org = 0x4FF40000, len = 0x80000 - 0x20000        384 KB
      REGION_ALIAS("iram_text_seg", sram_low)
      REGION_ALIAS("dram_seg",      sram_low)
      REGION_ALIAS("dram_high_seg", sram_high)

    sections.ld:
      .iram0.text  > iram_text_seg   IRAM code
      .dram0.data  > dram_seg        .sdata + .data   (INITIALISED data only)
      .dram1.bss   > dram_high_seg   .bss + .sbss + COMMON
      .dram1.heap_start > dram_high_seg

So `sram_low` = IRAM code + *initialised* data, and nothing else. .bss is not
in it: `.dram1.bss` targets sram_high and is declared ~330 lines before
`.dram0.bss`, so it claims every .bss input section first. Moving a .bss
object to PSRAM therefore frees sram_high, which is not the constraint, and
does exactly nothing for a sram_low overflow. (Asked and answered the hard
way.)

WHAT IRAM ACTUALLY IS. Summing sections named .iram* undercounts badly. The
.iram0.text output section carries ~350 per-object rules from IDF's linker
fragments that pull ordinary `.text.<fn>` into IRAM:

    *libspi_flash.a:spi_flash_os_func_app.*(.literal.spi_flash_os_yield
                                            .text.spi_flash_os_yield)

Those are invisible to a naive .iram* sum and are a large share of the total.
This script parses the rules out of the generated sections.ld and applies
them, so `fragment` below is real IRAM that no source file asks for.

WHAT THE OVERFLOW LOOKS LIKE. With --enable-non-contiguous-regions the linker
does not say "region overflowed". It discards what it could not place and
names the casualties:

    ld: error: --enable-non-contiguous-regions discards section `.sdata.stdout'
        from `...libc.a(picolibc_init.c.o)'
    ld: error: Total discarded sections size is 960 bytes

A hundred lines of picolibc, lwip and FreeRTOS, and not a word about the code
that filled the region. Two traps in reading it: the total is an upper bound
on the shortfall rather than the shortfall (whole sections go once placement
fails), and the figure is *stable* against changes that do not touch this
region -- three unrelated fixes in a row all reported exactly 960, which is
the signal that the thing being changed is not the thing that is full.

Needs only the archives, which ninja has built by the time the link fails --
a failed link leaves a truncated map with no section table, which is exactly
when the numbers are wanted.

Numbers are pre-gc-sections: an over-estimate for archives the image barely
uses (libbt, libcmock), close for this project's own components. Ranking, not
a budget.

Usage:  python tools/iram_budget.py [build_dir] [--all]
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join(ROOT, "build")

SRAM_LOW = 0x4FF2BBD0 - 0x4FF00000  # 175 KB, from memory.ld

# .iram* is what IRAM_ATTR / SYNTH_RENDER_IRAM produce. .data/.sdata are the
# initialised-data half of the same region. .bss deliberately absent: it lives
# in sram_high -- see the header.
IRAM_RE = re.compile(r"^\.iram\d*(\.|$)")
DATA_RE = re.compile(r"^\.(data|sdata)(\.|$)")

OURS = {
    "fx", "fx_gpl", "synth_core", "engines", "graph", "audio_io", "looper",
    "drums", "seqarp", "presets", "ble_ctrl", "midi", "chord", "codec",
    "local_ui", "persist", "usb_dev", "usb_host_midi", "main",
}

SECTION_LINE = re.compile(r"^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s")
ARCHIVE_MEMBER = re.compile(r"^(\S+\.o(?:bj)?):\s*file format")
# *libfoo.a:bar.*(.literal.x .text.x)  -> ("foo", "bar", ["...", ...])
FRAGMENT_RULE = re.compile(r"^\s*\*lib([\w.+-]+)\.a:([\w.+-]+)\.\*\(([^)]*)\)")


def find_objdump():
    for name in ("riscv32-esp-elf-objdump", "xtensa-esp32s3-elf-objdump",
                 "xtensa-esp32-elf-objdump"):
        found = shutil.which(name)
        if found:
            return found
    for base in (r"C:\Espressif\tools", os.path.expanduser("~/.espressif/tools")):
        if not os.path.isdir(base):
            continue
        for dirpath, _dirnames, filenames in os.walk(base):
            for fn in filenames:
                if fn.startswith("riscv32-esp-elf-objdump"):
                    return os.path.join(dirpath, fn)
    return None


def iram_fragment_rules(build):
    """{(libname, objstem): set(section names)} from .iram0.text in sections.ld."""
    path = os.path.join(build, "esp-idf", "esp_system", "ld", "sections.ld")
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    start = None
    for i, line in enumerate(lines):
        if re.match(r"^\s*\.iram0\.text\s*:", line):
            start = i
            break
    if start is None:
        return None
    rules = {}
    for line in lines[start:]:
        if re.search(r"\}\s*>\s*iram_text_seg", line):
            break
        m = FRAGMENT_RULE.match(line)
        if m:
            key = (m.group(1), m.group(2))
            rules.setdefault(key, set()).update(m.group(3).split())
    return rules


def measure(objdump, archive, rules):
    """(iram_attr, iram_fragment, data) bytes for one archive."""
    try:
        out = subprocess.run([objdump, "-h", archive], capture_output=True,
                             text=True, timeout=180).stdout
    except (OSError, subprocess.SubprocessError):
        return 0, 0, 0
    name = os.path.basename(archive)
    lib = name[3:-2] if name.startswith("lib") and name.endswith(".a") else name
    iram = frag = data = 0
    member = None
    for line in out.splitlines():
        m = ARCHIVE_MEMBER.match(line.strip())
        if m:
            member = os.path.splitext(m.group(1))[0]
            # bar.c.obj -> bar
            if member.endswith(".c") or member.endswith(".cpp"):
                member = os.path.splitext(member)[0]
            continue
        m = SECTION_LINE.match(line)
        if not m:
            continue
        sec, size = m.group(1), int(m.group(2), 16)
        if IRAM_RE.match(sec):
            iram += size
        elif DATA_RE.match(sec):
            data += size
        elif rules and member is not None:
            wanted = rules.get((lib, member))
            if wanted and sec in wanted:
                frag += size
    return iram, frag, data


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    show_all = "--all" in argv
    build = args[0] if args else DEFAULT_BUILD

    objdump = find_objdump()
    if objdump is None:
        print("no objdump found -- run IDF's export script first")
        return 1
    if not os.path.isdir(build):
        print("no build directory at %s" % build)
        return 1

    rules = iram_fragment_rules(build)
    if rules is None:
        print("note: no generated sections.ld -- fragment-placed .text not"
              " counted, so IRAM below is an undercount.\n")
    else:
        print("parsed %d per-object IRAM fragment rules from sections.ld\n"
              % len(rules))

    archives = []
    for dirpath, _dirnames, filenames in os.walk(build):
        for fn in filenames:
            if fn.endswith(".a"):
                archives.append(os.path.join(dirpath, fn))
    if not archives:
        print("no .a archives under %s -- build first" % build)
        return 1

    rows = []
    for path in sorted(archives):
        iram, frag, data = measure(objdump, path, rules)
        if iram or frag or data:
            name = os.path.basename(path)
            comp = name[3:-2] if name.startswith("lib") else name
            rows.append((name, comp in OURS, iram, frag, data))
    rows.sort(key=lambda r: -(r[2] + r[3] + r[4]))

    print("sram_low: %d B (%.1f KB) = IRAM code + INITIALISED data."
          " .bss is in sram_high.\n" % (SRAM_LOW, SRAM_LOW / 1024.0))
    print("  %-42s %9s %9s %9s" % ("archive", "iram", "fragment", "data"))
    print("  " + "-" * 74)
    for name, ours, iram, frag, data in rows:
        if not show_all and not ours and (iram + frag + data) < 2000:
            continue
        print("  %-42s %9d %9d %9d%s"
              % (name, iram, frag, data, "  *" if ours else ""))

    t_i = sum(r[2] for r in rows)
    t_f = sum(r[3] for r in rows)
    t_d = sum(r[4] for r in rows)
    o_i = sum(r[2] for r in rows if r[1])
    o_f = sum(r[3] for r in rows if r[1])
    o_d = sum(r[4] for r in rows if r[1])
    print("\n  * = this project's own components")
    print("  all:   iram=%d + fragment=%d + data=%d = %d  (%.0f%% of sram_low)"
          % (t_i, t_f, t_d, t_i + t_f + t_d,
             100.0 * (t_i + t_f + t_d) / SRAM_LOW))
    print("  ours:  iram=%d + fragment=%d + data=%d = %d  (%.0f%% of sram_low)"
          % (o_i, o_f, o_d, o_i + o_f + o_d,
             100.0 * (o_i + o_f + o_d) / SRAM_LOW))
    print("\n  ours' iram is what CONFIG_OSYNTH_RENDER_IN_IRAM controls.")
    print("  (pre-gc-sections; ranking, not a budget.)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
