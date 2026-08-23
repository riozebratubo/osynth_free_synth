#!/usr/bin/env python3
"""Splice the chord-mode block into app_osyntho/src/synthcontroller.cpp.

Kept per the project's intermediary-artifacts policy: this is the edit that
added SynthController's chord helpers and CHORD_SET decode, and re-running it
on an already-patched file is a no-op rather than a duplicate.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
ROOT = REPO / "tools"

MARKER = "\n/* ------------------------------------------------- sequencer frame decode */\n"

block = (ROOT / "s41_chord_block.cpp.txt").read_text(encoding="utf-8")
target = REPO / "app_osyntho/src/synthcontroller.cpp"
src = target.read_text(encoding="utf-8")

if "SynthController::chordNotesFor" in src:
    print("already patched")
    sys.exit(0)

if src.count(MARKER) != 1:
    sys.exit("marker appears %d times, expected 1" % src.count(MARKER))

target.write_text(src.replace(MARKER, block, 1), encoding="utf-8")
print("patched")
