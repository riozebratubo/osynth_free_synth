#!/usr/bin/env python3
"""Phase 0: the CMakeLists edits the other scripts assume.

Run this FIRST. The per-phase scripts only touch .qml/.cpp/.h sources; these
are the build-file changes that make the new types reach qmltyperegistrar, plus
the removal of two dead QML files.

  * src/qmlforeign.h/.cpp -- the QML_FOREIGN singleton wrappers for Synth and
    BluetoothManager. Both must be registered from outside their own classes;
    see the header for why (singletonConstructionMode tests
    default-constructibility before it looks for a create() factory).
  * src/paramtypes.h -- the ParamMeta value type.
  * SettingsItemNumber.qml / SettingsItemToggable.qml are listed in QML_FILES
    but instantiated nowhere. Removed from the module and deleted.

The three new source files are not generated here -- they are hand-written and
already on disk. This only wires them into the build.

Idempotent. --check reports without writing.
"""
import argparse
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "app_osyntho")
CMAKE = os.path.join(APP, "CMakeLists.txt")

DEAD_QML = ("SettingsItemNumber.qml", "SettingsItemToggable.qml")

ARGS = None


def main():
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    ARGS = parser.parse_args()

    for name in ("qmlforeign.h", "qmlforeign.cpp", "paramtypes.h"):
        if not os.path.exists(os.path.join(APP, "src", name)):
            sys.exit("missing hand-written source: src/" + name)

    text = io.open(CMAKE, encoding="utf-8").read()
    before = text
    notes = []

    anchor = "    SOURCES src/ibluetoothmanager.h\n"
    addition = (anchor +
                "    # Registers SynthController and IBluetoothManager as QML singletons from\n"
                "    # outside their own classes. See src/qmlforeign.h for why that is required.\n"
                "    SOURCES src/qmlforeign.h src/qmlforeign.cpp\n")
    if "src/qmlforeign.h" not in text:
        if text.count(anchor) != 1:
            sys.exit("CMakeLists: ibluetoothmanager anchor not unique")
        text = text.replace(anchor, addition, 1)
        notes.append("qmlforeign")

    anchor = "    SOURCES src/ble/synthprotocol.h\n"
    addition = (anchor +
                "    # QML value types for the parameter API (see src/paramtypes.h).\n"
                "    SOURCES src/paramtypes.h\n")
    if "src/paramtypes.h" not in text:
        if text.count(anchor) != 1:
            sys.exit("CMakeLists: synthprotocol anchor not unique")
        text = text.replace(anchor, addition, 1)
        notes.append("paramtypes")

    for name in DEAD_QML:
        line = "    QML_FILES qml/{}\n".format(name)
        if line in text:
            text = text.replace(line, "", 1)
            notes.append("-" + name)

    if text != before:
        print("  CMakeLists.txt: " + ", ".join(notes))
        if not ARGS.check:
            io.open(CMAKE, "w", encoding="utf-8", newline="").write(text)

    for name in DEAD_QML:
        path = os.path.join(APP, "qml", name)
        if os.path.exists(path):
            print("  delete qml/" + name)
            if not ARGS.check:
                os.remove(path)

    print("(dry run)" if ARGS.check else "done")


if __name__ == "__main__":
    main()
