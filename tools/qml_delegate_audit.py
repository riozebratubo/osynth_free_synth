#!/usr/bin/env python3
"""Audit what `pragma ComponentBehavior: Bound` would break in each QML file.

Bound is the fix for the remaining [unqualified] warnings -- it lets a delegate
reach the ids of the component it is written in, which is what qmlcachegen needs
before it can compile `screen.nodeW` or `root.columns` inside a Repeater.

It is not free. Under Bound a Component is created in its *own* context, so a
view can no longer inject `index`, `model` and `modelData` as context
properties. Any delegate still reading those without a `required property`
declaration silently gets `undefined` at run time -- it builds, it just
misbehaves. This finds those before the pragma goes in.

For each file it reports:
  * how many unqualified sites Bound would address
  * every use of index / model / modelData
  * whether the enclosing object already declares that name required

A file with 0 unguarded uses is safe to flip on its own. Anything else needs
the `required property` lines added in the same edit.

Usage:  python tools/qml_delegate_audit.py [file.qml ...]
"""
import collections
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QML = os.path.join(ROOT, "app_osyntho", "qml")
SITES = os.path.join(ROOT, "tools", "out", "unqualified_local.json")

INJECTED = ("index", "model", "modelData")


def strip_noise(text):
    """Blank comments and strings, keeping line structure intact."""
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', text)
    return text


def audit(path):
    with open(path, encoding="utf-8") as handle:
        raw = handle.read()
    body = strip_noise(raw)
    lines = body.splitlines()

    required = collections.Counter()
    for match in re.finditer(r"\brequired\s+property\s+[\w<>.]+\s+(\w+)", body):
        required[match.group(1)] += 1

    uses = collections.Counter()
    sites = []
    for number, line in enumerate(lines, 1):
        for name in INJECTED:
            # a bare use, not a declaration and not someone's member
            for m in re.finditer(r"(?<![\w.$])" + name + r"(?![\w:])", line):
                if re.search(r"(required\s+property|property)\s+[\w<>.]+\s+" + name, line):
                    continue
                uses[name] += 1
                sites.append((number, name, raw.splitlines()[number - 1].strip()[:74]))
                break
    return required, uses, sites


def main():
    by_file = collections.Counter()
    if os.path.exists(SITES):
        for site in json.load(open(SITES, encoding="utf-8")):
            by_file[os.path.basename(site["file"])] += 1

    names = sys.argv[1:] or sorted(
        n for n in os.listdir(QML) if n.endswith(".qml"))

    print("{:<34} {:>7} {:>9} {:>9}  {}".format(
        "file", "unqual", "injected", "required", "verdict"))
    print("-" * 86)
    detail = []
    for name in names:
        base = os.path.basename(name)
        path = os.path.join(QML, base)
        if not os.path.exists(path):
            continue
        required, uses, sites = audit(path)
        unguarded = sum(count for key, count in uses.items() if not required.get(key))
        if not by_file.get(base) and not unguarded:
            continue
        verdict = "safe to flip" if unguarded == 0 else "needs {} required decl(s)".format(unguarded)
        print("{:<34} {:>7} {:>9} {:>9}  {}".format(
            base, by_file.get(base, 0), sum(uses.values()), sum(required.values()), verdict))
        if unguarded:
            detail.append((base, [s for s in sites if not required.get(s[1])]))

    for base, sites in detail:
        print("\n  {} -- unguarded injected-property uses:".format(base))
        for number, name, text in sites:
            print("    {:>5}  {:<10} {}".format(number, name, text))


if __name__ == "__main__":
    main()
