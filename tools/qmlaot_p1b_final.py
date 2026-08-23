#!/usr/bin/env python3
"""Phase 1b: mark properties FINAL and the singleton classes final.

qmlcachegen has to assume a QML type could derive from a C++ type and shadow
its members, so a lookup through a shadowable member cannot be compiled -- the
actual type is not known until run time. After phase 1 that showed up as:

     14  Cannot use shadowable base type for further lookups: App::theme with type Theme
      3  Cannot use shadowable base type for further lookups: call to method t, returning QString

FINAL on a Q_PROPERTY is Qt's documented answer (their own QML_SINGLETON
examples use it). `final` on the class covers the method case, which has no
per-member equivalent.

Safe here because nothing derives from App, Translator, SynthController or
Theme -- verified by grep before this was written. IBluetoothManager is left
extensible on purpose: the platform backends derive from it. Its properties
still get FINAL, because no backend redeclares one.

Purely a compile-time annotation: no runtime behaviour changes.

Idempotent. --check reports without writing.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "app_osyntho", "src")

ARGS = None

# header -> (class declaration to mark `final`, or None to leave extensible)
TARGETS = {
    "app.h": "class App : public QObject {",
    "translator.h": "class Translator : public QObject {",
    "synthcontroller.h": "class SynthController : public QObject, public DatabaseClient {",
    "theme.h": "class Theme {",
    "ibluetoothmanager.h": None,
}


def add_final_to_properties(text):
    """Append FINAL inside every Q_PROPERTY(...) that lacks it."""
    out = []
    index = 0
    count = 0
    while True:
        start = text.find("Q_PROPERTY(", index)
        if start == -1:
            out.append(text[index:])
            break
        open_paren = start + len("Q_PROPERTY(") - 1
        depth = 0
        end = None
        for pos in range(open_paren, len(text)):
            if text[pos] == "(":
                depth += 1
            elif text[pos] == ")":
                depth -= 1
                if depth == 0:
                    end = pos
                    break
        if end is None:
            sys.exit("unbalanced Q_PROPERTY near offset {}".format(start))
        body = text[open_paren + 1:end]
        out.append(text[index:end])
        if not re.search(r"\bFINAL\b", body):
            # Keep the existing wrapping: if the declaration ends on its own
            # continuation line the token still reads fine appended to it.
            out.append(" FINAL")
            count += 1
        out.append(")")
        index = end + 1
    return "".join(out), count


def make_class_final(text, decl):
    if decl is None:
        return text, False
    name = decl.split()[1]
    final_decl = decl.replace("class " + name, "class " + name + " final", 1)
    if final_decl in text:
        return text, False
    if decl not in text:
        sys.exit("class declaration not found: " + decl)
    return text.replace(decl, final_decl, 1), True


def main():
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    ARGS = parser.parse_args()

    for header, decl in TARGETS.items():
        path = os.path.join(SRC, header)
        with open(path, encoding="utf-8") as handle:
            before = handle.read()
        after, props = add_final_to_properties(before)
        after, classed = make_class_final(after, decl)
        if after == before:
            print("  {:<24} already done".format(header))
            continue
        print("  {:<24} {:2d} properties FINAL{}".format(
            header, props, ", class final" if classed else ""))
        if not ARGS.check:
            with open(path, "w", encoding="utf-8", newline="") as handle:
                handle.write(after)

    print("(dry run)" if ARGS.check else "written")


if __name__ == "__main__":
    main()
