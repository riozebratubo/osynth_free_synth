#!/usr/bin/env python3
"""Bisect Keyboard.qml: are the type annotations to blame, or Phase 1?

Skipping Keyboard.qml with QT_QML_SKIP_CACHEGEN proved the hang lives in that
document's generated code. The generated code has no loops (no backwards jumps)
and all 239 lookup retry loops carry a hasError() exit, so it is not spinning
inside one function -- it is re-entrancy between compiled bindings.

Two candidate causes, and they need different fixes:

  A. One of my type annotations is wrong, so the generated code coerces a value
     the interpreter left alone. Fix: correct the annotation.

  B. A binding that had always been there only became *compilable* once phase 1
     made App / Synth / Tr typed singletons -- 122 entries in this file compile
     now. Nothing of mine is wrong; qmlcachegen mis-compiles something. Fix:
     leave QT_QML_SKIP_CACHEGEN on this one file.

--strip removes every type annotation this session added to Keyboard.qml while
leaving the Tr rename and everything else intact. Rebuild with cachegen ON for
that file:

    hang gone      -> cause A, and I bisect the annotations
    hang persists  -> cause B, and the skip is the permanent answer

--restore re-applies them (identical to re-running p3a/p3b/p2b for this file).
"""
import argparse
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEYBOARD = os.path.join(ROOT, "app_osyntho", "qml", "Keyboard.qml")

# Annotations this session added to Keyboard.qml, as (annotated, bare) pairs.
FUNCS = [
    ("function onSettingChanged(name: string): void {", "function onSettingChanged(name) {"),
    ("function onComputerKeyPressed(semitone: int): void {", "function onComputerKeyPressed(semitone) {"),
    ("function onComputerKeyReleased(semitone: int): void {", "function onComputerKeyReleased(semitone) {"),
    ("function setKeyHeight(h: real): void {", "function setKeyHeight(h) {"),
    ("function whiteMidi(i: int): int {", "function whiteMidi(i) {"),
    ("function noteName(midi: int, withOctave: bool): string {", "function noteName(midi, withOctave) {"),
    ("function isActive(n: int): bool {", "function isActive(n) {"),
    ("function noteOn(n: int): void {", "function noteOn(n) {"),
    ("function noteOff(n: int): void {", "function noteOff(n) {"),
    ("function pickNote(n: int): void {", "function pickNote(n) {"),
    ("function setHeld(n: int, on: bool): void {", "function setHeld(n, on) {"),
    ("function setActive(n: int, on: bool): void {", "function setActive(n, on) {"),
    ("function toggleLatch(n: int): void {", "function toggleLatch(n) {"),
    ("function setComputerKeys(on: bool): void {", "function setComputerKeys(on) {"),
    ("function setTopRowDrums(on: bool): void {", "function setTopRowDrums(on) {"),
    ("function noteForPos(x: real, y: real): int {", "function noteForPos(x, y) {"),
]

LISTS = [
    ("readonly property list<int> whiteSemis:", "readonly property var whiteSemis:"),
    ("readonly property list<string> semiNames:", "readonly property var semiNames:"),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--strip", action="store_true", help="remove the annotations")
    group.add_argument("--restore", action="store_true", help="put them back")
    parser.add_argument("--which", help="1-based indices/ranges into --list, e.g. 1-8 or 4,9,15")
    parser.add_argument("--list", action="store_true", help="number the annotations and exit")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    all_pairs = FUNCS + LISTS
    if args.list:
        for i, (annotated, _) in enumerate(all_pairs, 1):
            print("{:3d}  {}".format(i, annotated.rstrip(" {")))
        return

    chosen = range(1, len(all_pairs) + 1)
    if args.which:
        chosen = []
        for part in args.which.split(","):
            if "-" in part:
                lo, hi = part.split("-")
                chosen.extend(range(int(lo), int(hi) + 1))
            else:
                chosen.append(int(part))
    selected = [all_pairs[i - 1] for i in chosen]

    text = io.open(KEYBOARD, encoding="utf-8").read()
    before = text
    pairs = [(a, b) for a, b in selected] if args.strip else [(b, a) for a, b in selected]

    done = 0
    for old, new in pairs:
        if old in text:
            text = text.replace(old, new, 1)
            done += 1
        elif new not in text:
            sys.exit("Keyboard.qml: neither form found for:\n  " + old)

    verb = "stripped" if args.strip else "restored"
    print("{} {} of {} annotations".format(verb, done, len(pairs)))
    if text != before and not args.check:
        io.open(KEYBOARD, "w", encoding="utf-8", newline="").write(text)
        print("written -- rebuild with cachegen ON for Keyboard.qml "
              "(set OSYNTHO_QML_SKIP_CACHEGEN back to \"\")")


if __name__ == "__main__":
    main()
