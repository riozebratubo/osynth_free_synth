#!/usr/bin/env python3
"""Check every Tr.t("...") literal on the chord surfaces has a pt_BR entry.

The translator falls back to the source string, so a missing entry is silent:
the page simply stays in English for a pt_BR user. Kept per the project's
intermediary-artifacts policy — rerun it after editing either file.

Usage: python check_chord_translations.py [qml-file ...]
"""
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

KEY_RE = re.compile(r'pt\[\"((?:[^\"\\]|\\.)*)\"\]')
CALL_RE = re.compile(r'Tr\.t\(\"((?:[^\"\\]|\\.)*)\"\)')

DEFAULT = [
    "app_osyntho/qml/ChordScreen.qml",
    "app_osyntho/qml/TrackSheet.qml",
]

# Strings the regex cannot see, because they reach Tr.t() as a property rather
# than a literal: the Chord page's pad rows take their label and their option
# names from properties on the page. Listed here so they are still checked.
DYNAMIC = [
    "Inversion", "Voicing", "Notes in the chord", "Root", "Scale",
    "When a setting changes", "Play what changed", "Play the whole chord",
    "Root pos.", "1st", "2nd", "3rd",
    # Scale names come from PARAM_INFO and are shown as the firmware
    # spells them, here and on the track sheet; not listed.
    "Single", "Triad", "7th", "9th", "11th", "13th",
]

# Universal terms the file deliberately leaves untranslated (they read the same
# in pt_BR), so they are not reported as gaps.
ALLOWED_UNTRANSLATED = {
    "Free", "Scale", "Chord", "Key", "Root", "Voicing", "Strum", "Transpose",
    "Pattern", "Generate", "Not connected", "Discovering parameters\u2026",
}


def main(argv):
    tr = (REPO / "app_osyntho/src/translator.cpp").read_text(encoding="utf-8")
    keys = set(KEY_RE.findall(tr))

    files = argv[1:] or DEFAULT
    gaps = 0
    for rel in files:
        src = (REPO / rel).read_text(encoding="utf-8")
        calls = list(dict.fromkeys(CALL_RE.findall(src)))
        if rel.endswith("ChordScreen.qml"):
            calls += [d for d in DYNAMIC if d not in calls]
        missing = [c for c in calls
                   if c not in keys and c not in ALLOWED_UNTRANSLATED]
        print("%s: %d string(s), %d without a pt_BR entry"
              % (rel, len(calls), len(missing)))
        for m in missing:
            print("    MISSING: %s" % m[:100])
            gaps += 1
    return 1 if gaps else 0


sys.exit(main(sys.argv))
