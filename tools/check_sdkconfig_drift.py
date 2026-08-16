#!/usr/bin/env python3
"""Check the live ./sdkconfig against the sdkconfig.defaults for its target.

`sdkconfig` is gitignored and is only generated from the defaults files when it
does not already exist — an existing value always wins. So a commit that moves a
pin in sdkconfig.defaults.<target> does not reach a tree that already has an
sdkconfig, and the build keeps driving the old pins with nothing in the log
saying so. That is not hypothetical: it is how the ESP32-P4 ES8388 bring-up came
up silent, with I2S still on the pre-b7b9782 22/23/26 while the module was wired
to the 46/47/32 the defaults file had moved to.

This is the complement to check_sdkconfig_coverage.py, which checks that every
settable symbol is *mentioned* in a defaults file. This one checks that what is
mentioned is what the build is actually using.

Only explicitly-set `CONFIG_x=y` lines in the defaults are compared; commented-out
documentation lines are what a defaults file uses to record "left at the Kconfig
default", and those are not drift.

Usage: python tools/check_sdkconfig_drift.py [path/to/sdkconfig]
Exit status 1 if anything differs.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def parse(text):
    """{symbol: value} from sdkconfig syntax. `# CONFIG_X is not set` is n."""
    out = {}
    for line in text.splitlines():
        m = re.match(r"^(CONFIG_\w+)=(.*)$", line)
        if m:
            out[m.group(1)] = m.group(2)
            continue
        m = re.match(r"^# (CONFIG_\w+) is not set$", line)
        if m:
            out[m.group(1)] = "n"
    return out


def main(argv):
    sdkconfig = Path(argv[1]) if len(argv) > 1 else ROOT / "sdkconfig"
    if not sdkconfig.is_file():
        print(f"{sdkconfig} does not exist — nothing to drift from")
        return 0

    live = parse(sdkconfig.read_text(encoding="utf-8"))
    target = live.get("CONFIG_IDF_TARGET", "").strip('"')
    if not target:
        print(f"{sdkconfig}: no CONFIG_IDF_TARGET; cannot pick a defaults file")
        return 1

    files = [ROOT / "sdkconfig.defaults", ROOT / f"sdkconfig.defaults.{target}"]
    files = [p for p in files if p.is_file()]

    drift = []
    for path in files:
        for sym, want in parse(path.read_text(encoding="utf-8")).items():
            # A defaults file writes =n for a bool; sdkconfig writes the
            # "is not set" comment, which parse() already normalised to n.
            got = live.get(sym)
            if got != want:
                drift.append((path.name, sym, want, got))

    print(f"{sdkconfig.name} (target {target}) vs {', '.join(p.name for p in files)}")
    for name, sym, want, got in drift:
        print(f"  {sym}: {name} wants {want}, build has "
              f"{got if got is not None else '<absent>'}")
    if drift:
        print("\nDelete sdkconfig and rebuild to regenerate it from the defaults,"
              "\nor set the values above in menuconfig.")
    else:
        print("  no drift")
    return 1 if drift else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
