#!/usr/bin/env python3
"""Score qmlcachegen's ahead-of-time compilation coverage for app_osyntho.

qt_add_qml_module runs qmlcachegen with --dump-aot-stats, which drops one
<File>_qml.cpp.aotstats JSON file per QML document into
    <build>/.rcc/qmlcache/osyntho_qml/
Each entry is one binding or function; codegenResult 0 means it was compiled
to C++, anything else means it fell back to interpreted byte code.

This is the measurement harness for the "raise AOT coverage" work: run it
against a build dir before and after a phase to see what actually moved.

Usage:
    python tools/qml_aot_score.py <build-dir> [--top N] [--by-file] [--json out.json]
    python tools/qml_aot_score.py --compare before.json after.json
"""
import argparse
import collections
import json
import os
import re
import sys

# Failure messages carry file paths and generated component names; fold those
# away so the same underlying cause tallies as one bucket.
_NORMALISERS = [
    (re.compile(r"\(component in [^)]*\)::"), "<component>::"),
    (re.compile(r"\bD:/[^\s\"]*/"), ""),
]


def normalise(message):
    for pattern, replacement in _NORMALISERS:
        message = pattern.sub(replacement, message)
    return message.strip().rstrip(".")


def collect(build_dir):
    """Walk a build dir and return (per_file, entries).

    per_file maps the QML document name to (compiled, fallback).
    entries is the flat list of every fallback entry, for reason tallies.
    """
    root = os.path.join(build_dir, ".rcc", "qmlcache")
    if not os.path.isdir(root):
        sys.exit("no .rcc/qmlcache under {} - is this a configured build dir?".format(build_dir))

    per_file = {}
    fallbacks = []
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in sorted(filenames):
            if not name.endswith(".aotstats"):
                continue
            with open(os.path.join(dirpath, name), encoding="utf-8") as handle:
                data = json.load(handle)
            document = name[: -len("_qml.cpp.aotstats")] if name.endswith("_qml.cpp.aotstats") else name
            compiled = failed = 0
            for module in data.get("modules", []):
                for module_file in module.get("moduleFiles", []):
                    for entry in module_file.get("entries", []):
                        if entry.get("codegenResult") == 0:
                            compiled += 1
                        else:
                            failed += 1
                            fallbacks.append(
                                {
                                    "file": document,
                                    "line": entry.get("line"),
                                    "function": entry.get("functionName"),
                                    "message": normalise(entry.get("message", "")),
                                }
                            )
            got = per_file.get(document, (0, 0))
            per_file[document] = (got[0] + compiled, got[1] + failed)
    return per_file, fallbacks


def summarise(per_file, fallbacks):
    compiled = sum(c for c, _ in per_file.values())
    failed = sum(f for _, f in per_file.values())
    reasons = collections.Counter(entry["message"] for entry in fallbacks)
    return {"compiled": compiled, "fallback": failed, "reasons": dict(reasons),
            "per_file": {k: list(v) for k, v in per_file.items()}}


def rate(compiled, failed):
    total = compiled + failed
    return 100.0 * compiled / total if total else 0.0


def report(summary, top, by_file):
    compiled, failed = summary["compiled"], summary["fallback"]
    total = compiled + failed
    print("AOT coverage: {}/{} bindings compiled to C++ ({:.1f}%), {} fell back".format(
        compiled, total, rate(compiled, failed), failed))
    reasons = collections.Counter(summary["reasons"])
    print("\nTop fallback reasons:")
    for message, count in reasons.most_common(top):
        print("  {:5d}  {}".format(count, message[:130]))
    if by_file:
        print("\nPer file (worst first):")
        rows = sorted(summary["per_file"].items(), key=lambda kv: -kv[1][1])
        for document, (ok, bad) in rows:
            if not bad:
                continue
            print("  {:5d} fallback  {:5d} ok   {}".format(bad, ok, document))


def compare(before_path, after_path):
    with open(before_path, encoding="utf-8") as handle:
        before = json.load(handle)
    with open(after_path, encoding="utf-8") as handle:
        after = json.load(handle)
    for label, summary in (("before", before), ("after", after)):
        print("{:>7}: {}/{} compiled ({:.1f}%)".format(
            label, summary["compiled"], summary["compiled"] + summary["fallback"],
            rate(summary["compiled"], summary["fallback"])))
    delta = after["compiled"] - before["compiled"]
    print("  delta: {:+d} bindings compiled".format(delta))

    before_reasons = collections.Counter(before["reasons"])
    after_reasons = collections.Counter(after["reasons"])
    moved = [(after_reasons.get(m, 0) - c, m) for m, c in before_reasons.items()]
    moved += [(c, m) for m, c in after_reasons.items() if m not in before_reasons]
    print("\nReason movement (negative = fixed):")
    for change, message in sorted(moved)[:25]:
        if change:
            print("  {:+5d}  {}".format(change, message[:130]))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build_dir", nargs="?", help="a configured build directory")
    parser.add_argument("--top", type=int, default=20, help="how many fallback reasons to list")
    parser.add_argument("--by-file", action="store_true", help="also break the fallbacks down per document")
    parser.add_argument("--json", help="write the summary here, for a later --compare")
    parser.add_argument("--compare", nargs=2, metavar=("BEFORE", "AFTER"),
                        help="diff two summaries written with --json")
    args = parser.parse_args()

    if args.compare:
        compare(*args.compare)
        return
    if not args.build_dir:
        parser.error("a build dir is required unless --compare is given")

    per_file, fallbacks = collect(args.build_dir)
    summary = summarise(per_file, fallbacks)
    report(summary, args.top, args.by_file)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(summary, handle, indent=2, sort_keys=True)
        print("\nwrote {}".format(args.json))


if __name__ == "__main__":
    main()
