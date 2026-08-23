#!/usr/bin/env python3
"""Declare SynthController's chord helpers and dispatch OP_CHORD_SET.

Second half of the chord-mode app wiring (see splice_chord.py). Idempotent.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

EDITS = [
    (
        "app_osyntho/src/synthcontroller.h",
        "  void handleSeqInfo(const QByteArray& payload);",
        "  void handleSeqInfo(const QByteArray& payload);\n"
        "  void handleChordSet(const QByteArray& payload);\n"
        "  // Live value of one chord.* parameter, by name, falling back to\n"
        "  // `fallback` before discovery has run. The display-side chord math\n"
        "  // reads every setting through this, so \"not connected yet\" is one\n"
        "  // answer in one place rather than a guard at every call site.\n"
        "  double chordParam(const char* leaf, double fallback) const;\n"
        "  void chordScaleRoot(int* scale, int* root) const;",
    ),
    (
        "app_osyntho/src/synthcontroller.cpp",
        "    case OP_SEQ_INFO:\n      handleSeqInfo(f.payload);\n      break;",
        "    case OP_SEQ_INFO:\n      handleSeqInfo(f.payload);\n      break;\n"
        "    case OP_CHORD_SET:\n"
        "      // A refusal is the normal answer from firmware that predates\n"
        "      // chord mode, not an error: chordAvailable stays false and the\n"
        "      // page keeps its user-set editor off the screen.\n"
        "      if (f.status == 0) handleChordSet(f.payload);\n"
        "      break;",
    ),
]

for rel, needle, replacement in EDITS:
    path = REPO / rel
    src = path.read_text(encoding="utf-8")
    if replacement.split("\n")[1] in src and "handleChordSet" in src and needle not in src:
        print("%s: already patched" % rel)
        continue
    if "handleChordSet" in src and replacement in src:
        print("%s: already patched" % rel)
        continue
    if src.count(needle) != 1:
        sys.exit("%s: needle appears %d times" % (rel, src.count(needle)))
    path.write_text(src.replace(needle, replacement, 1), encoding="utf-8")
    print("%s: patched" % rel)
