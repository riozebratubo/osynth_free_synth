#!/usr/bin/env python3
"""Phase 3c: the zero-risk half of the unqualified-access work.

The remaining [unqualified] warnings are three different problems wearing one
message, and only two of them are safe to fix without running the app:

  1. A root property read from inside the same document without its id --
     `x: margin` in Toast.qml. Prefixing with the root id is always correct and
     changes no scoping rules. Done here.

  2. An id of the *same* document reached from inside a Component or delegate
     -- `pc.paramId` inside ParamControl's inline Components, `presetGrid` and
     `screen` inside PresetsScreen's GridView delegate. `pragma
     ComponentBehavior: Bound` binds those Components to the context they are
     written in, which is what makes the id statically resolvable. Done here,
     but ONLY for files whose delegates use no injected index/model/modelData,
     or already declare them required -- under Bound a view can no longer
     inject those, and a delegate still reading one silently gets undefined.
     tools/qml_delegate_audit.py is what checks that.

  3. An id belonging to a *different* document -- Toolbar.qml reading
     `swipeView`, `mainWindow` and `mainStackView`, which are ids in Main.qml.
     Those resolve only because Toolbar happens to be created in Main's
     context. No pragma fixes this: Toolbar's root cannot see Main's ids
     statically. The real fix is to give Toolbar declared properties and let
     Main bind them, which is an API change with real call sites -- NOT done
     here. 16 sites, listed in the roadmap.

Idempotent. --check reports without writing.
"""
import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QML = os.path.join(ROOT, "app_osyntho", "qml")

ARGS = None

# file -> [(old, new), ...] -- root properties read without their id.
QUALIFY = {
    "Knob.qml": [
        ('text: meta.exists ? meta.name.split(\'.\').pop() : ""',
         'text: root.meta.exists ? root.meta.name.split(\'.\').pop() : ""'),
    ],
    "Toast.qml": [
        ("        x: margin\n        y: margin",
         "        x: root.margin\n        y: root.margin"),
        ("            id: theIcon\n            color: desiredForegroundColor",
         "            id: theIcon\n            color: root.desiredForegroundColor"),
        ("            id: theText\n            color: desiredForegroundColor",
         "            id: theText\n            color: root.desiredForegroundColor"),
        ("        NumberAnimation{\n            to: 0.9\n            duration: fadeTime\n        }\n"
         "        PauseAnimation{\n            duration: time - 2*fadeTime\n        }\n"
         "        NumberAnimation{\n            to: 0\n            duration: fadeTime\n        }",
         "        NumberAnimation{\n            to: 0.9\n            duration: root.fadeTime\n        }\n"
         "        PauseAnimation{\n            duration: root.time - 2*root.fadeTime\n        }\n"
         "        NumberAnimation{\n            to: 0\n            duration: root.fadeTime\n        }"),
        ("            if(!running && selfDestroying)",
         "            if(!running && root.selfDestroying)"),
    ],
    "Toolbar.qml": [
        ("readonly property real spare: t1.width - titleFloor - fixedChromeWidth",
         "readonly property real spare: t1.width - titleFloor - t1.fixedChromeWidth"),
    ],
    "WindowStateSaver.qml": [
        # The root Item had no id at all -- `s` is the Settings object nested
        # inside it -- so windowName had nothing to qualify against until the
        # root got one.
        ("Item {\n    property Window window",
         "Item {\n    id: root\n\n    property Window window"),
        ("        category: windowName", "        category: root.windowName"),
    ],
}

# Files whose same-document ids are reached from a Component/delegate. Verified
# delegate-safe by tools/qml_delegate_audit.py before being listed here.
BIND = ("ParamControl.qml", "PresetsScreen.qml")

PRAGMA = "pragma ComponentBehavior: Bound\n"


def add_pragma(text):
    if PRAGMA.strip() in text:
        return text
    lines = text.split("\n")
    # After any existing pragma (pragma Singleton must stay first), before the
    # imports.
    at = 0
    for index, line in enumerate(lines):
        if line.startswith("pragma "):
            at = index + 1
        elif line.startswith("import "):
            break
    return "\n".join(lines[:at] + [PRAGMA.strip()] + lines[at:])


def main():
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    ARGS = parser.parse_args()

    changed = 0
    for name in sorted(set(QUALIFY) | set(BIND)):
        path = os.path.join(QML, name)
        with open(path, encoding="utf-8") as handle:
            before = handle.read()
        after = before
        notes = []
        applied = 0
        for old, new in QUALIFY.get(name, []):
            if after.count(old) == 1:
                after = after.replace(old, new, 1)
                applied += 1
            elif new in after:
                continue
            else:
                sys.exit("{}: could not match\n---\n{}\n---".format(name, old))
        if applied:
            notes.append("{} qualified".format(applied))
        if name in BIND:
            with_pragma = add_pragma(after)
            if with_pragma != after:
                notes.append("ComponentBehavior: Bound")
            after = with_pragma
        if after != before:
            changed += 1
            print("  {:<24} {}".format(name, ", ".join(notes)))
            if not ARGS.check:
                with open(path, "w", encoding="utf-8", newline="") as handle:
                    handle.write(after)
    print("{} {} files".format("would change" if ARGS.check else "changed", changed))


if __name__ == "__main__":
    main()
