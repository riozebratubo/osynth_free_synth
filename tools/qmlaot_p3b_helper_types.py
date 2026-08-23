#!/usr/bin/env python3
"""Phase 3b: type-annotate the plain QML helper functions.

The companion to qmlaot_p3a_handler_types.py. Signal handlers get their types
from the signal; these do not -- each signature below was read out of the
function's body and its call sites, so the table is the record of that reading
rather than something a regex could have produced.

Deliberately NOT annotated:

  Toast.show / ToastManager.show
      Both take optional arguments and test them with
      `typeof duration !== "undefined"`. Annotating the parameters makes QML
      coerce a missing argument to 0 / "" instead of leaving it undefined, so
      every one of those tests would flip and the defaults would stop applying.
      A silent behaviour change for two AOT entries is not a trade worth making.

  DrumPads.trackHold / Keyboard.trackHold
      `points` is the touch-point list from MultiPointTouchArea, which has no
      QML type name to write here.

  SpinBoxDouble.textFromValue / valueFromText
      Property assignments of anonymous function expressions, not `function`
      declarations -- there is no annotation syntax for them, and `locale` is a
      QLocale besides.

Idempotent. --check reports without writing.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QML = os.path.join(ROOT, "app_osyntho", "qml")

# file -> { function: (["param: type", ...], return type or None) }
# A return type of None means the value is a JS object or an untyped container
# that only phase 2 (real value types) can name.
TABLE = {
    "Main.qml": {
        "setImmersive": (["on: bool"], "void"),
        "writeBackupTo": (["path: string"], "void"),
        "safeFileName": (["name: string", "fallback: string"], "string"),
        "exportJson": (["text: string", "suggestedName: string"], "void"),
        "writeExportTo": (["path: string", "text: string"], "void"),
        "exportWav": (["suggestedName: string"], "void"),
        "writeWavExportTo": (["path: string"], "void"),
        "importJson": (["page: string"], "void"),
        "loadImportFrom": (["path: string"], "void"),
    },
    "Keyboard.qml": {
        "setKeyHeight": (["h: real"], "void"),
        "whiteMidi": (["i: int"], "int"),
        "noteName": (["midi: int", "withOctave: bool"], "string"),
        "isActive": (["n: int"], "bool"),
        "noteOn": (["n: int"], "void"),
        "noteOff": (["n: int"], "void"),
        "pickNote": (["n: int"], "void"),
        "setHeld": (["n: int", "on: bool"], "void"),
        "setActive": (["n: int", "on: bool"], "void"),
        "toggleLatch": (["n: int"], "void"),
        "setComputerKeys": (["on: bool"], "void"),
        "setTopRowDrums": (["on: bool"], "void"),
        "noteForPos": (["x: real", "y: real"], "int"),
    },
    "DrumPads.qml": {
        "slotFor": (["row: int", "col: int"], "int"),
        "slotName": (["slot: int"], "string"),
        "velocityAt": (["yFraction: real"], "int"),
        "setLit": (["slot: int", "on: bool"], "void"),
        "padAt": (["x: real", "y: real"], "int"),
        "fractionInPad": (["y: real"], "real"),
        "slotNote": (["slot: int"], "int"),
        "pickPad": (["slot: int"], "void"),
        "hit": (["slot: int", "velocity: int"], "void"),
    },
    "GraphScreen.qml": {
        # nodeAt / outJackPos / inJackPos hand back a graph node or a {x, y}
        # literal, both QVariantMap today. `var` is the honest return type
        # until phase 2 gives them a real one; without it qmllint infers
        # `undefined` from the `return null` branch and every caller's field
        # access is then reported missing.
        "nodeAt": (["slot: int"], "var"),
        "outJackPos": (["slot: int"], "var"),
        "inJackPos": (["slot: int", "port: int"], "var"),
        "kindName": (["kind: int"], "string"),
        "isAudio": (["kind: int"], "bool"),
        "tapOutput": (["slot: int"], "void"),
        "tapInput": (["slot: int", "port: int"], "void"),
    },
    "Knob.qml": {
        "posToValue": (["pos: real"], "real"),
        "valueToPos": (["v: real"], "real"),
        "fmt": (["v: real"], "string"),
    },
    "EnumSelector.qml": {"syncFrom": (["value: real"], "void")},
    "LooperScreen.qml": {"predictedMax": (["mono: bool", "four: bool"], "real")},
    "PlockDialog.qml": {
        "openFor": (["stepIndex: int"], "void"),
        "choose": (["index: int"], "void"),
    },
    "SequencerScreen.qml": {"drumNameFor": (["note: int"], "string")},
    "TransportStrip.qml": {"go": (["m: int"], "void")},
}

ARGS = None


def annotate(path, functions):
    with open(path, encoding="utf-8") as handle:
        before = handle.read()
    after = before
    done = 0
    for name, (params, ret) in sorted(functions.items()):
        pattern = re.compile(
            r"\bfunction\s+" + re.escape(name) + r"\s*\(([^)]*)\)(\s*)(:\s*[A-Za-z_<>]+\s*)?\{")
        matches = list(pattern.finditer(after))
        if not matches:
            sys.exit("{}: function {} not found".format(os.path.basename(path), name))
        if len(matches) > 1:
            sys.exit("{}: function {} declared {} times".format(
                os.path.basename(path), name, len(matches)))
        match = matches[0]
        declared = [p.strip() for p in match.group(1).split(",") if p.strip()]
        if any(":" in p for p in declared):
            continue  # already annotated
        bare = [p.split(":")[0].strip() for p in params]
        if declared != bare:
            sys.exit("{}: {} declares ({}) but the table says ({})".format(
                os.path.basename(path), name, ", ".join(declared), ", ".join(bare)))
        suffix = ": {} ".format(ret) if ret else " "
        after = (after[:match.start()]
                 + "function {}({}){}{{".format(name, ", ".join(params), suffix)
                 + after[match.end():])
        done += 1
    if after != before and not ARGS.check:
        with open(path, "w", encoding="utf-8", newline="") as handle:
            handle.write(after)
    return done


def main():
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    ARGS = parser.parse_args()

    total = 0
    for name, functions in sorted(TABLE.items()):
        count = annotate(os.path.join(QML, name), functions)
        total += count
        print("  {:<24} {}/{} annotated".format(name, count, len(functions)))
    print("{} {} helpers".format("would annotate" if ARGS.check else "annotated", total))


if __name__ == "__main__":
    main()
