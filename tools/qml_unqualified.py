#!/usr/bin/env python3
"""Group qmllint's [unqualified] warnings by the identifier that triggered them.

qmllint prints one warning per site with the offending token underlined on the
following line; this reads the token back out so the sites can be tallied by
name. That split matters for the AOT work: a name that belongs to a C++
singleton is fixed once, in C++, while a name that is an outer-scope `id` has
to be qualified at each site.

Note that until the app has been rebuilt after the singleton conversion, the
module's generated osyntho.qmltypes is stale (it was 0 bytes before any C++
type carried QML_ELEMENT), so App/Synth/Tr/BluetoothManager still show up here.
That is the stale type file talking, not the QML.

Usage:
    python tools/qml_unqualified.py [--build-dir DIR] [--sites] [--json OUT]
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "app_osyntho")
QMLLINT = r"C:/Qt/6.11.0/msvc2022_64/bin/qmllint.exe"
DEFAULT_BUILD = "build/STATIC_Desktop_Qt_6_11_0_MSVC2022_64bit-Release"

# Registered as QML singletons in phase 1; they are not QML ids and are not
# fixed by qualifying anything.
SINGLETONS = ("App", "Synth", "Tr", "BluetoothManager")

WARNING = re.compile(r"Warning: (.+?):(\d+):(\d+): Unqualified access")
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def run_qmllint(build_dir):
    files = sorted(glob.glob(os.path.join(APP, "qml", "*.qml")))
    if not files:
        sys.exit("no QML files found under app_osyntho/qml")
    if not os.path.exists(QMLLINT):
        sys.exit("qmllint not found at " + QMLLINT)
    result = subprocess.run([QMLLINT, "-I", build_dir] + files,
                            capture_output=True, text=True, cwd=APP)
    return (result.stdout + result.stderr).splitlines()


def collect(lines):
    sites = []
    for index, line in enumerate(lines):
        match = WARNING.match(line)
        if not match:
            continue
        source = lines[index + 1] if index + 1 < len(lines) else ""
        column = int(match.group(3))
        token = IDENT.match(source[column - 1:])
        path = match.group(1).replace("\\", "/")
        sites.append({
            "file": path[path.rfind("qml/"):] if "qml/" in path else path,
            "line": int(match.group(2)),
            "column": column,
            "name": token.group(0) if token else "?",
        })
    return sites


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default=DEFAULT_BUILD,
                        help="import path for the module's generated type info")
    parser.add_argument("--sites", action="store_true", help="list every local site")
    parser.add_argument("--json", help="write the local (non-singleton) sites here")
    args = parser.parse_args()

    sites = collect(run_qmllint(args.build_dir))
    singleton_sites = [s for s in sites if s["name"] in SINGLETONS]
    local_sites = [s for s in sites if s["name"] not in SINGLETONS]

    print("{} unqualified sites total".format(len(sites)))
    print("  {:5d} singleton names (fixed in C++, pending a rebuild): {}".format(
        len(singleton_sites),
        dict(Counter(s["name"] for s in singleton_sites))))
    print("  {:5d} local ids / root properties, to qualify at each site".format(
        len(local_sites)))

    by_name = Counter(s["name"] for s in local_sites)
    for name, count in by_name.most_common(25):
        print("      {:<18} {}".format(name, count))

    print("\n  per file:")
    for path, count in Counter(s["file"] for s in local_sites).most_common():
        print("      {:<40} {}".format(path, count))

    if args.sites:
        print("\n  sites:")
        for site in local_sites:
            print("      {}:{}:{}  {}".format(
                site["file"], site["line"], site["column"], site["name"]))

    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(local_sites, handle, indent=1)
        print("\nwrote {} ({} sites)".format(args.json, len(local_sites)))


if __name__ == "__main__":
    main()
