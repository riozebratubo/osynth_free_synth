#!/usr/bin/env python3
"""Report which osynth symbols the host port's static library still leaves open.

Why this exists
---------------
`osynth_core` is a static library, so it archives successfully with symbols
still unresolved -- that is normal and is how the port is staged: components
land one at a time, and until `drums` is ported every `drums_*` reference is
expected to be open. A successful build therefore says nothing about whether
the library is complete.

This says it. It subtracts the defined set from the referenced set and splits
what is left into osynth symbols (the answer to "what is still missing") and
toolchain/CRT/STL symbols (which the linker supplies, and which are noise
here).

The subtraction matters: dumpbin reports symbols per object file, so a
cross-object reference inside the same library shows as UNDEF. Reading the
UNDEF column alone reports hundreds of symbols that are perfectly resolved.

Usage:
    python tools/check_host_symbols.py                    # default lib path
    python tools/check_host_symbols.py <path-to-.lib>
    python tools/check_host_symbols.py --dump <dumpbin-output.txt>

`dumpbin` must be on PATH, i.e. run from a Visual Studio developer prompt (or
via tools/build_host.bat's vcvars). With --dump it parses a saved dumpbin
listing instead and needs no toolchain:

    dumpbin /SYMBOLS build_host\\Release\\osynth_core.lib > syms.txt
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

DEFAULT_LIBS = [
    ROOT / "build_host" / "Release" / "osynth_core.lib",
    ROOT / "build_host" / "Debug" / "osynth_core.lib",
    ROOT / "build_host" / "libosynth_core.a",
]

# Anything matching this is osynth's own; everything else in the leftover set is
# the C runtime, the STL, or compiler support, all supplied at link time.
OSYNTH_RE = re.compile(
    r"drums|sampler|looper|loop_|presets|persist|codec|local_ui|usb_|ble_ctrl"
    r"|audio_|synth_|graph|seq|chord|midi|fx_|engine|voice_|param|esp_"
    r"|[xv]Task|xQueue|xSemaphore|port_mux"
)


def parse_dumpbin(text: str) -> tuple[set[str], set[str]]:
    """Return (defined, referenced) external symbol names.

    A dumpbin symbol line looks like:
        010 00000000 UNDEF  notype ()    External     | esp_timer_get_time
    Everything left of the pipe is fixed-width-ish columns; the name is right
    of it. Splitting on the pipe is what makes this robust to the column
    widths changing between toolchain versions.
    """
    defined: set[str] = set()
    undef: set[str] = set()
    for line in text.splitlines():
        if "|" not in line:
            continue
        left, _, rest = line.partition("|")
        rest = rest.strip()
        if not rest:
            continue
        name = rest.split()[0]
        columns = left.split()
        if "External" not in columns:
            continue
        if "UNDEF" in columns:
            undef.add(name)
        else:
            defined.add(name)
    return defined, undef


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("lib", nargs="?", help="path to osynth_core.lib")
    ap.add_argument("--dump", help="a saved `dumpbin /SYMBOLS` listing")
    args = ap.parse_args()

    if args.dump:
        text = Path(args.dump).read_text(encoding="utf-8", errors="replace")
    else:
        lib = Path(args.lib) if args.lib else None
        if lib is None:
            for candidate in DEFAULT_LIBS:
                if candidate.is_file():
                    lib = candidate
                    break
        if lib is None or not lib.is_file():
            print("No library found. Build it first (tools\\build_host.bat), "
                  "or pass a path.", file=sys.stderr)
            return 2
        if shutil.which("dumpbin") is None:
            print("dumpbin is not on PATH -- run this from a Visual Studio "
                  "developer prompt, or use --dump with a saved listing.",
                  file=sys.stderr)
            return 2
        print(f"reading {lib}")
        text = subprocess.run(["dumpbin", "/SYMBOLS", str(lib)],
                              capture_output=True, text=True,
                              errors="replace").stdout

    defined, undef = parse_dumpbin(text)
    if not defined and not undef:
        print("no symbols parsed -- is that really a dumpbin listing?",
              file=sys.stderr)
        return 2

    missing = sorted(undef - defined)
    ours = [n for n in missing if OSYNTH_RE.search(n)]
    external = len(missing) - len(ours)

    print(f"{len(defined)} external symbols defined")
    print(f"{len(missing)} unresolved, of which {external} are toolchain/CRT/STL")
    print()
    if not ours:
        print("no osynth symbols unresolved -- the library is self-contained")
        return 0

    print(f"osynth symbols still unresolved ({len(ours)}):")
    for n in ours:
        print("   " + n)
    print()
    print("Expected while the port is staged: each names a component not yet "
          "in port/host/CMakeLists.txt. Compare against the list there before "
          "treating any of these as a fault.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
