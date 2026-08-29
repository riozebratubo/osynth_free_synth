#!/usr/bin/env python3
"""Count how many times an instruction pattern was inlined, per archive.

Written in S46b, for the decision iram_budget.py cannot answer. That tool
ranks archives by size; this one answers "how much is *this* line of C
costing me", which is the question that actually comes up whenever something
in a header inlines into the render path and sram_low is the binding region.

The case it was written for: adding `if (v != v) return 0;` to f2i16()
(synth_core/synth_line.h) to fence NaN at every int16 delay-line write. One
instruction of source, and it overflowed sram_low by 794 bytes at link,
because f2i16() inlines into every line_push() on the FX bus. Running this
found 29 expansions across libfx/libengines/liblooper, which is what made the
call obvious: keep the fence at the sink (one site, in audio_io.cpp) and drop
it from the shared primitive.

The default pattern is that NaN test. `feq.s rd, fs, fs` with both source
registers identical is `x != x` and essentially nothing else -- an ordinary
float comparison has two different operands -- so counting it is a direct
count of inlined NaN fences.

Usage:
    python tools/count_inline_expansions.py [build_dir] [--pattern REGEX]

Needs IDF's toolchain on PATH (run the export script first). Reads the .a
files a failed link leaves behind, so it works when nothing else does -- which
is the state you are in when you need it.
"""

import os
import re
import subprocess
import sys

DEFAULT_BUILD = "build"

# feq.s rd, fN, fN -- both sources the same register, i.e. an `x != x` NaN test.
NAN_TEST = r"feq\.s\s+\w+,(f[a-z0-9]+),\1(\s|$)"

# The project's own components, in the order iram_budget.py ranks them.
OURS = ("fx", "fx_gpl", "engines", "graph", "looper", "drums", "audio_io",
        "synth_core", "seqarp", "chord", "presets", "midi", "codec",
        "usb_dev", "usb_host_midi", "ble_ctrl", "persist", "local_ui", "main")


def find_objdump():
    for name in ("riscv32-esp-elf-objdump", "xtensa-esp32s3-elf-objdump",
                 "xtensa-esp32-elf-objdump"):
        try:
            subprocess.check_output([name, "--version"],
                                    stderr=subprocess.STDOUT)
            return name
        except (OSError, subprocess.CalledProcessError):
            continue
    return None


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    build = args[0] if args else DEFAULT_BUILD

    pattern = NAN_TEST
    for a in argv[1:]:
        if a.startswith("--pattern="):
            pattern = a.split("=", 1)[1]
    rx = re.compile(pattern)

    objdump = find_objdump()
    if objdump is None:
        print("no objdump found -- run IDF's export script first")
        return 1
    if not os.path.isdir(build):
        print("no build directory at %s" % build)
        return 1

    print("pattern: %s\n" % pattern)

    rows = []
    seen = 0
    for dirpath, _dirs, files in os.walk(build):
        for fn in files:
            if not fn.endswith(".a"):
                continue
            seen += 1
            path = os.path.join(dirpath, fn)
            try:
                dis = subprocess.check_output([objdump, "-d", path],
                                              stderr=subprocess.DEVNULL)
            except (OSError, subprocess.CalledProcessError):
                continue
            dis = dis.decode("utf-8", "replace")
            n = sum(1 for line in dis.splitlines() if rx.search(line))
            if n:
                comp = os.path.basename(os.path.dirname(path))
                rows.append((n, fn, comp in OURS))

    if seen == 0:
        print("no .a files under %s -- the archives are only there after a\n"
              "compile has run. A *failed link* leaves them, which is the\n"
              "case this tool is for; a fullclean or an in-flight build does\n"
              "not." % build)
        return 1
    if not rows:
        print("%d archives scanned, no matches -- nothing inlined this, or\n"
              "the pattern is wrong for this target's ISA." % seen)
        return 0

    rows.sort(reverse=True)
    total = sum(r[0] for r in rows)
    ours = sum(r[0] for r in rows if r[2])
    print("  %-40s %s" % ("archive", "matches"))
    print("  " + "-" * 52)
    for n, fn, is_ours in rows:
        print("  %-40s %7d %s" % (fn, n, "*" if is_ours else ""))
    print("\n  * = this project's own components")
    print("  total=%d  ours=%d" % (total, ours))
    print("\n  Rough sram_low cost: an inlined compare-and-branch runs about")
    print("  16-24 bytes once scheduling and register pressure are counted,")
    print("  so `ours` x ~20 is the number to weigh against the link's")
    print("  'Total discarded sections size' when it overflows.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
