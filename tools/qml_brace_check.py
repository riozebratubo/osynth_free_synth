#!/usr/bin/env python3
"""Cheap structural sanity check for the QML sources.

Not a parser and no substitute for building — it walks the file once, skipping
comments, string literals and template literals, and checks that braces
balance. That is enough to catch the one mistake hand-editing QML actually
produces: a block opened or closed twice.

A single regex pass is NOT enough here, because stripping strings before
comments turns an apostrophe in a `// doesn't` comment into the start of a
string literal and swallows everything up to the next quote. Hence the scanner.

Usage:  python tools/qml_brace_check.py [file ...]
        (no arguments: every .qml under app_osyntho/qml)
Exit status is non-zero when any file is unbalanced.
"""
import glob
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QMLDIR = os.path.join(ROOT, "app_osyntho", "qml")


def brace_balance(text):
    """(open_count, close_count, depth_went_negative) ignoring comments/strings."""
    i, n = 0, len(text)
    opened = closed = depth = 0
    went_negative = False
    while i < n:
        c = text[i]
        # comments
        if c == "/" and i + 1 < n:
            if text[i + 1] == "/":
                i = text.find("\n", i)
                if i < 0:
                    break
                continue
            if text[i + 1] == "*":
                end = text.find("*/", i + 2)
                i = n if end < 0 else end + 2
                continue
        # string / template literals
        if c in "\"'`":
            quote = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == "{":
            opened += 1
            depth += 1
        elif c == "}":
            closed += 1
            depth -= 1
            if depth < 0:
                went_negative = True
        i += 1
    return opened, closed, went_negative


def main(argv):
    files = argv[1:] or sorted(glob.glob(os.path.join(QMLDIR, "*.qml")))
    bad = 0
    for path in files:
        opened, closed, negative = brace_balance(io.open(path, encoding="utf-8").read())
        ok = opened == closed and not negative
        if not ok:
            bad += 1
        print("%-34s {=%-4d }=%-4d %s"
              % (os.path.basename(path), opened, closed,
                 "OK" if ok else "*** MISMATCH ***"))
    print("\n%d file(s) checked, %d unbalanced." % (len(files), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
