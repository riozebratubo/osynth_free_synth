#!/usr/bin/env python3
"""Third and last follow-up to tools/ui_preset_marks.py: the comments added by
the first two scripts used ASCII hyphens where this codebase uses em dashes.
Targets those exact lines only, so pre-existing " - " is untouched.
"""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

LINES = [
    ("app_osyntho/qml/UI.qml",
     '    // page tiles are labelled with. Empty - not "-1", not a placeholder -',
     '    // page tiles are labelled with. Empty — not "-1", not a placeholder —'),

    ("app_osyntho/qml/Main.qml",
     "                // picker are for, while *what the sound is* - engine, slot,",
     "                // picker are for, while *what the sound is* — engine, slot,"),
    ("app_osyntho/qml/Main.qml",
     "                // name - is the thing you look up mid-session, and it was",
     "                // name — is the thing you look up mid-session, and it was"),

    ("app_osyntho/qml/HomeScreen.qml",
     "                        // Presets page tiles are numbered with - one preset,",
     "                        // Presets page tiles are numbered with — one preset,"),

    ("app_osyntho/qml/PatchLibraryScreen.qml",
     "                // records this - see SynthController::libraryPatchId - so it is",
     "                // records this — see SynthController::libraryPatchId — so it is"),

    ("app_osyntho/src/synthcontroller.h",
     "  // so nothing but this can say a stored patch is what is playing - and that is",
     "  // so nothing but this can say a stored patch is what is playing — and that is"),

    ("app_osyntho/src/synthcontroller.cpp",
     "    // another one, and that switch must not clear the mark it has just set -",
     "    // another one, and that switch must not clear the mark it has just set —"),
    ("app_osyntho/src/synthcontroller.cpp",
     "  // The row *is* the live sound - it was just taken from it - so the library",
     "  // The row *is* the live sound — it was just taken from it — so the library"),
]

files = {}
for rel, old, new in LINES:
    path = os.path.join(ROOT, rel)
    if path not in files:
        with io.open(path, encoding="utf-8") as fh:
            files[path] = fh.read()

failed = []
for rel, old, new in LINES:
    path = os.path.join(ROOT, rel)
    if files[path].count(old) != 1:
        failed.append((rel, files[path].count(old), old.strip()[:64]))
        continue
    files[path] = files[path].replace(old, new, 1)

if failed:
    for rel, n, head in failed:
        print("ANCHOR x%d in %s: %s" % (n, rel, head))
    sys.exit(1)

for path, text in sorted(files.items()):
    with io.open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("patched", os.path.relpath(path, ROOT))
