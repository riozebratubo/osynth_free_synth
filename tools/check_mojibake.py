#!/usr/bin/env python3
"""Find UTF-8 text that has been round-tripped through cp1252 (mojibake).

Why this exists: a patch script that reads a file with Python's *locale*
encoding and writes it back as UTF-8 turns every em-dash into "\u00e2\u20ac\u201d" and
every curly quote into similar three-character rubble. The result is still
valid UTF-8, so a decode check passes and nothing complains until someone
reads the file. This repo's comments are full of em-dashes, so the blast
radius of one careless script is large and invisible.

Kept per the project's intermediary-artifacts policy. Run it over the working
tree after any scripted edit:

    python check_mojibake.py                # every tracked text file
    python check_mojibake.py path [path...]
"""
import pathlib
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

# The UTF-8-through-cp1252 signatures that actually occur in this repo's text:
# em-dash, en-dash, curly quotes, ellipsis, non-breaking space, degree sign.
SIGNATURES = [
    "\u00e2\u20ac\u201d",  # em dash
    "\u00e2\u20ac\u201c",  # en dash
    "\u00e2\u20ac\u0153",  # left double quote
    "\u00e2\u20ac\u009d",  # right double quote
    "\u00e2\u20ac\u2122",  # right single quote
    "\u00e2\u20ac\u00a6",  # ellipsis
    "\u00c2\u00a0",        # non-breaking space
    "\u00c3\u00a9",        # e-acute
    "\u00c3\u00a7",        # c-cedilla
    "\u00c3\u00b5",        # o-tilde
]

TEXT_SUFFIXES = {".c", ".h", ".cpp", ".hpp", ".qml", ".py", ".md", ".txt",
                 ".json", ".cmake", ".ts", ".qrc", ".csv"}


def tracked_files():
    out = subprocess.run(["git", "ls-files"], cwd=REPO, capture_output=True,
                         text=True).stdout.split("\n")
    return [f for f in out if f and pathlib.Path(f).suffix in TEXT_SUFFIXES]


def main(argv):
    files = argv[1:] or tracked_files()
    bad = 0
    for rel in files:
        path = REPO / rel
        if not path.is_file():
            continue
        raw = path.read_bytes()
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError as exc:
            print("NOT UTF-8   %s  (%s)" % (rel, exc))
            bad += 1
            continue
        hits = [s for s in SIGNATURES if s in text]
        if hits:
            line = next(i + 1 for i, l in enumerate(text.split("\n"))
                        if any(s in l for s in hits))
            print("MOJIBAKE    %s:%d  %r" % (rel, line, hits))
            bad += 1
    print("checked %d file(s), %d damaged" % (len(files), bad))
    return 1 if bad else 0


sys.exit(main(sys.argv))
