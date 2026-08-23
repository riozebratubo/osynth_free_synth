#!/usr/bin/env python3
"""S41 SD-rail power change: structural checks for the files it touched.

Compiles nothing. It proves three things that a config-plus-#if edit can get
wrong without any compiler noticing until the board is in front of you:

  1. #if/#endif balance in the two files that gained the LDO block.
  2. Every OSYNTH_SD_* symbol the C code reads is declared in the Kconfig.
     A misspelt CONFIG_ name is not an error in C — it is silently 0, which
     here means "no LDO" and a card that never answers.
  3. The live ./sdkconfig actually carries the P4 SD block. sdkconfig is
     gitignored and wins over the defaults files once it exists, so a correct
     defaults file is not evidence that the build will use it
     (tools/check_sdkconfig_drift.py is the general form of this).

Usage:  python tools/s41_check_sd_pwr.py
Exit status 1 if anything fails.
"""
import io
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
C_FILES = ["components/looper/loop_store.cpp", "components/drums/drum_kit.cpp"]
KCONFIG = ROOT / "components" / "synth_core" / "Kconfig.projbuild"

OPEN = re.compile(r"^\s*#\s*(if|ifdef|ifndef)\b")
CLOSE = re.compile(r"^\s*#\s*endif\b")

# What the P4 defaults claim, and what the live sdkconfig therefore has to say
# for the card to come up. The pin roles are the SD-to-SPI mapping of the
# SDMMC slot-1 IOMUX block (D3/CLK/CMD/D0), not the SDMMC roles.
EXPECTED = {
    "CONFIG_OSYNTH_LOOP_STORE_SD": "y",
    "CONFIG_OSYNTH_SD_CS_GPIO": "42",
    "CONFIG_OSYNTH_SD_SCK_GPIO": "43",
    "CONFIG_OSYNTH_SD_MOSI_GPIO": "44",
    "CONFIG_OSYNTH_SD_MISO_GPIO": "39",
    "CONFIG_OSYNTH_SD_PWR_LDO_CHAN": "4",
}


def pp_balance(path):
    depth, bad = 0, []
    for n, line in enumerate(io.open(path, encoding="utf-8"), 1):
        if OPEN.match(line):
            depth += 1
        elif CLOSE.match(line):
            depth -= 1
            if depth < 0:
                bad.append(f"{path}:{n}: #endif with no matching #if")
                depth = 0
    if depth:
        bad.append(f"{path}: {depth} unclosed #if block(s)")
    return bad


def main():
    fails = []

    for rel in C_FILES:
        fails += pp_balance(ROOT / rel)

    kconf = KCONFIG.read_text(encoding="utf-8")
    used = set()
    for rel in C_FILES:
        used |= set(re.findall(r"CONFIG_(OSYNTH_SD_\w+)",
                               (ROOT / rel).read_text(encoding="utf-8")))
    for sym in sorted(used):
        if not re.search(rf"^\s*config\s+{sym}\s*$", kconf, re.M):
            fails.append(f"{sym}: read by the C code, not declared in Kconfig")

    live = ROOT / "sdkconfig"
    if not live.is_file():
        print("no ./sdkconfig yet — it will be generated from the defaults")
    else:
        text = live.read_text(encoding="utf-8")
        for sym, want in EXPECTED.items():
            m = re.search(rf"^{sym}=(.*)$", text, re.M)
            got = m.group(1) if m else "<absent>"
            if got != want:
                fails.append(f"{sym}: sdkconfig has {got}, expected {want}")

    for f in fails:
        print(f"  FAIL: {f}")
    print("s41 SD-rail checks:", "ok" if not fails else f"{len(fails)} problem(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
