#!/usr/bin/env python3
"""Phase 3a: type-annotate the Connections signal handlers.

qmlcachegen will not compile a function it cannot type, and reports
"Functions without type annotations won't be compiled". 84 functions hit that
after phase 1; most of them are `Connections` handlers, whose parameter types
are not a judgement call at all -- they are fixed by the C++ signal (or the
QML signal in UI.qml) being handled.

So those are done here, mechanically, from the signal declarations:

  src/synthcontroller.h  paramChanged(int id, double value)  ...
  src/app.h              settingChanged(QString name)        ...
  qml/UI.qml             jsonImported(string page, string text)

The plain helper functions (posToValue, noteForPos, slotFor, ...) are NOT
touched here: their types have to be read out of each body, so they are
annotated by hand.

A handler may declare fewer parameters than the signal carries -- QML allows
that -- so only the parameters actually declared get annotated.

Idempotent. --check reports without writing.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QML = os.path.join(ROOT, "app_osyntho", "qml")

# handler name -> parameter types, in order, from the emitting signal.
HANDLERS = {
    # SynthController
    "onParamChanged": ["int", "real"],
    "onPresetsChanged": ["int"],
    "onPresetJsonReady": ["string", "string"],
    "onLoopExportReady": ["string"],
    "onLoopExportFailed": ["string"],
    "onPaintNoteSuggested": ["int"],
    "onShowError": ["string"],
    "onShowInfo": ["string"],
    # App
    "onSettingChanged": ["string"],
    "onRestoreFailed": ["string"],
    "onSelectFileSelected": ["string"],
    "onComputerKeyPressed": ["int"],
    "onComputerKeyReleased": ["int"],
    "onComputerDrumPadPressed": ["int"],
    # UI.qml
    "onExportJsonRequested": ["string", "string"],
    "onImportJsonRequested": ["string"],
    "onJsonImported": ["string", "string"],
    "onUpdateFirmwareRequested": ["string"],
}

DECL = re.compile(
    r"\bfunction\s+(" + "|".join(HANDLERS) + r")\s*\(([^)]*)\)(\s*)(:\s*[A-Za-z_<>]+\s*)?\{")

ARGS = None


def annotate(text, stats):
    def repl(match):
        name, params, gap, ret = match.group(1), match.group(2), match.group(3), match.group(4)
        types = HANDLERS[name]
        names = [p.strip() for p in params.split(",") if p.strip()]
        if any(":" in n for n in names):
            return match.group(0)  # already annotated
        if len(names) > len(types):
            sys.exit("{}: declares {} params, signal carries {}".format(
                name, len(names), len(types)))
        typed = ", ".join("{}: {}".format(n, t) for n, t in zip(names, types))
        stats[0] += 1
        # Handlers return nothing; saying so lets the compiler skip the
        # undefined-return path.
        return "function {}({}){}: void {{".format(name, typed, gap or " ")
    return DECL.sub(repl, text)


def main():
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    ARGS = parser.parse_args()

    total = 0
    for name in sorted(os.listdir(QML)):
        if not name.endswith(".qml"):
            continue
        path = os.path.join(QML, name)
        with open(path, encoding="utf-8") as handle:
            before = handle.read()
        stats = [0]
        after = annotate(before, stats)
        if after == before:
            continue
        total += stats[0]
        print("  {:<32} {} handlers".format(name, stats[0]))
        if not ARGS.check:
            with open(path, "w", encoding="utf-8", newline="") as handle:
                handle.write(after)
    print("{} {} handlers".format("would annotate" if ARGS.check else "annotated", total))


if __name__ == "__main__":
    main()
