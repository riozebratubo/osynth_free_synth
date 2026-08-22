#!/usr/bin/env python3
"""Round-5 audit probe: cross-check the app's ParamGroup panels against the
parameter names the firmware actually registers.

A ParamGroup shows every discovered parameter whose name startswith() its
`prefix` (SynthController::paramIdsByPrefix). Three things can go wrong and
none of them is visible without a synth attached:

  * a prefix that matches nothing  -> the card is permanently hidden (dead panel)
  * a name matched by two prefixes -> the same knob is drawn on two panels
  * a name matched by no prefix    -> the parameter has no control anywhere,
                                      unless a hand-built screen covers it

Usage:  python tools/audit_paramgroup_coverage.py
Reads:  tools/out/param_names.txt  (regenerate with the grep below)
    grep -rhoE '"[a-z][a-zA-Z0-9]*\\.[a-zA-Z0-9._]*"' components/ main/ --include=*.cpp \\
      | sed -E 's/"//g' | sort -u > tools/out/param_names.txt
"""
import os
import re
import glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QML = os.path.join(ROOT, "app_osyntho", "qml")
NAMES = os.path.join(ROOT, "tools", "out", "param_names.txt")

# Screens that build their controls by hand rather than through a ParamGroup;
# a parameter owned by one of these is not an orphan.
HAND_BUILT = {
    "arp.": "ArpSeqScreen",
    "seq.": "SequencerScreen",
    "loop.": "LooperScreen",
    "drums.": "DrumsScreen / DrumPads",
    "preset.": "PresetsScreen (actions)",
    "engine.": "HomeScreen engine buttons",
    # state.reset (S40) is the Home page's "Start from scratch" card: one
    # button behind a confirmation, not a control anyone drags.
    "state.": "HomeScreen reset card",
}


def param_names():
    out = []
    with open(NAMES, encoding="utf-8") as fh:
        for line in fh:
            n = line.strip()
            # the grep also catches #include "foo.h" spellings
            if n and not n.endswith(".h"):
                out.append(n)
    return out


def paramgroup_prefixes():
    found = {}
    for path in sorted(glob.glob(os.path.join(QML, "*.qml"))):
        text = open(path, encoding="utf-8", errors="replace").read()
        for m in re.finditer(r"ParamGroup\s*\{([^}]*)\}", text):
            body = m.group(1)
            pre = re.search(r'prefix:\s*"([^"]*)"', body)
            title = re.search(r'title:\s*"([^"]*)"', body)
            if pre:
                found.setdefault(pre.group(1), []).append(
                    (os.path.basename(path), title.group(1) if title else "?"))
    return found


def main():
    names = param_names()
    prefixes = paramgroup_prefixes()

    print("=== ParamGroup prefixes in use ===")
    for p, uses in sorted(prefixes.items()):
        where = ", ".join(f"{f}:{t}" for f, t in uses)
        print(f"  {p:12} {where}")

    print("\n=== dead panels (prefix matches no registered parameter) ===")
    dead = False
    for p, uses in sorted(prefixes.items()):
        if not [n for n in names if n.startswith(p)]:
            dead = True
            print(f"  {p:12} '{uses[0][1]}' in {uses[0][0]}  -> always hidden")
    if not dead:
        print("  (none)")

    print("\n=== drawn on more than one panel ===")
    dup = False
    for n in names:
        hits = [p for p in prefixes if n.startswith(p)]
        if len(hits) > 1:
            dup = True
            print(f"  {n:18} -> {hits}")
    if not dup:
        print("  (none)")

    print("\n=== no ParamGroup panel ===")
    for n in sorted(names):
        if [p for p in prefixes if n.startswith(p)]:
            continue
        owner = next((v for k, v in HAND_BUILT.items() if n.startswith(k)), None)
        print(f"  {n:18} {'-> ' + owner if owner else '** NO SCREEN AT ALL **'}")


if __name__ == "__main__":
    main()
