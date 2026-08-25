#!/usr/bin/env python3
"""Report which Tr.t() strings in the QML have no pt_BR entry in translator.cpp.

Why this exists: Translator::t() falls back to the source string when a key is
missing, so a typo'd or drifted key is invisible in English and silently
untranslated in Portuguese. Nothing fails, nothing logs, and the only way to
notice is to switch languages and read every screen. Adding a card with seven
button captions is exactly when that goes wrong.

The C++ side is not a plain literal list: adjacent string literals concatenate,
so `pt["a long key " "split over two lines"]` is one key and has to be joined
before it can be compared. That is most of what this script does.

Not every miss is a bug. Universal audio terms are deliberately left out of the
table (see the note at the top of translator.cpp) so that t() falls through to
the source string -- "Reverb", "LFO", "Mixer" and friends read the same in
pt_BR. The output is a list to read, not a gate; there is no non-zero exit for
a missing key, only for a file that could not be parsed.

Usage:  python tools/check_translator_keys.py [file.qml ...]
        (no arguments: every .qml under app_osyntho/qml)
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QML_DIR = os.path.join(ROOT, "app_osyntho", "qml")
TRANSLATOR = os.path.join(ROOT, "app_osyntho", "src", "translator.cpp")

# One double-quoted C/QML literal, honouring backslash escapes.
LITERAL = r'"(?:[^"\\]|\\.)*"'
# pt["..."] / pt["..." "..."], i.e. one key, possibly spelled as adjacent
# literals across several lines.
PT_ENTRY = re.compile(r"pt\[\s*((?:" + LITERAL + r"\s*)+)\]")
# Tr.t("...") and Tr.ts("...", ...) -- only the first argument is a key.
TR_CALL = re.compile(r"\bTr\.ts?\(\s*(" + LITERAL + r")")
# The `name:` / `hint:` / `title:` fields of a card's data tables, which reach
# t() through a binding rather than as a literal argument. Heuristic, and the
# reason a miss here is worth reading rather than trusting.
FIELD = re.compile(r"\b(?:name|hint|title|idleText|downText|activeText)\s*:\s*("
                   + LITERAL + r")")


def unquote(text):
    """Join one or more adjacent literals into the string they denote."""
    out = []
    for lit in re.findall(LITERAL, text):
        body = lit[1:-1]
        out.append(body.replace('\\"', '"').replace("\\\\", "\\"))
    return "".join(out)


def translator_keys():
    with open(TRANSLATOR, encoding="utf-8") as f:
        src = f.read()
    return {unquote(m.group(1)) for m in PT_ENTRY.finditer(src)}


def qml_keys(path):
    with open(path, encoding="utf-8") as f:
        src = f.read()
    keys = set()
    for m in TR_CALL.finditer(src):
        keys.add(unquote(m.group(1)))
    for m in FIELD.finditer(src):
        keys.add(unquote(m.group(1)))
    return keys


def main(argv):
    files = argv[1:]
    if not files:
        files = sorted(
            os.path.join(QML_DIR, n)
            for n in os.listdir(QML_DIR)
            if n.endswith(".qml")
        )
    known = translator_keys()
    print("translator.cpp: %d pt_BR keys" % len(known))
    total = 0
    for path in files:
        missing = sorted(k for k in qml_keys(path) if k and k not in known)
        if not missing:
            continue
        total += len(missing)
        print("\n%s" % os.path.relpath(path, ROOT))
        for k in missing:
            print("    %s" % k)
    print("\n%d string(s) with no pt_BR entry (not necessarily an error --"
          " see the docstring)" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
