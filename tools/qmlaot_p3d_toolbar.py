#!/usr/bin/env python3
"""Phase 3d: stop Toolbar reaching into Main.qml's ids.

Toolbar.qml reads `swipeView`, `mainStackView` and `mainWindow`, which are ids
in *Main.qml*. They resolve only because Toolbar happens to be created in Main's
context -- its own header comment says so. No pragma fixes that: Toolbar's root
cannot see another document's ids statically, so every binding touching one is
uncompilable.

Two of the three are addressed here.

`mainStackView` (2 sites). Main already declares handlers for
UI.settingsRequested() and UI.selectDeviceRequested() that do exactly the push
Toolbar was doing inline -- and nothing was emitting them. The menu's own
"Update firmware..." item next door already goes through UI, so this is the
established pattern in this very file, not a new one. Toolbar emits; Main's
existing handlers do the push.

`swipeView` (10 sites) becomes a declared property. It is called `pager`, not
`swipeView`, on purpose: `swipeView: swipeView` inside a Toolbar declaration
would resolve the right-hand side to the Toolbar's own property first and bind
it to itself.

`mainWindow` (6 sites) is NOT addressed. Toolbar both reads and writes
`keyboardVisible` / `drumPadsVisible`, so it needs either two-way plumbing or
for that state to move onto the UI singleton -- a design decision about where
session state lives, not a mechanical fix.

Idempotent. --check reports without writing.
"""
import argparse
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QML = os.path.join(ROOT, "app_osyntho", "qml")

ARGS = None

TOOLBAR = [
    # route the two pushes through the bus Main already listens on
    ('onTriggered: mainStackView.push("BluetoothDeviceSelectorScreen.qml", {})',
     "onTriggered: UI.selectDeviceRequested()"),
    ('onTriggered: mainStackView.push("SettingsScreen.qml", {})',
     "onTriggered: UI.settingsRequested()"),
    # the SwipeView arrives as a property now
    ("    property string subtitle: \"\"",
     "    property string subtitle: \"\"\n"
     "    // The SwipeView the prev/next arrows drive. Named `pager` rather than\n"
     "    // `swipeView` so that `pager: swipeView` at the call site cannot resolve\n"
     "    // its right-hand side to this property and bind it to itself.\n"
     "    property SwipeView pager"),
    ("            visible: swipeView.currentIndex > 0\n"
     "            onClicked: if (swipeView.currentIndex > 0) swipeView.currentIndex--",
     "            visible: t1.pager && t1.pager.currentIndex > 0\n"
     "            onClicked: if (t1.pager.currentIndex > 0) t1.pager.currentIndex--"),
    ("            visible: swipeView.currentIndex < swipeView.count - 1\n"
     "            onClicked: if (swipeView.currentIndex < swipeView.count - 1) swipeView.currentIndex++",
     "            visible: t1.pager && t1.pager.currentIndex < t1.pager.count - 1\n"
     "            onClicked: if (t1.pager.currentIndex < t1.pager.count - 1) t1.pager.currentIndex++"),
    # the header comment described the old arrangement
    ("// Instantiated inside Main.qml, so it resolves Main's ids (swipeView,\n"
     "// mainStackView) through the context chain.",
     "// The SwipeView it drives arrives as the `pager` property, and navigation\n"
     "// requests go out through the UI singleton's signals — so this does not\n"
     "// depend on Main's ids resolving through the context chain. It still reads\n"
     "// mainWindow for the keyboard/drum-pad toggles, which do."),
]

MAIN = [
    ("            header: Toolbar {\n                subtitle:",
     "            header: Toolbar {\n                pager: swipeView\n                subtitle:"),
]


def apply(path, pairs, label):
    text = io.open(path, encoding="utf-8").read()
    before = text
    applied = 0
    for old, new in pairs:
        # Insertion pairs (old is a prefix of new) leave `old` in place, so
        # testing it first would re-apply on every run. Check the result first.
        if len(new) > len(old) and new in text:
            continue
        if text.count(old) == 1:
            text = text.replace(old, new, 1)
            applied += 1
        elif new in text:
            continue
        else:
            sys.exit("{}: could not match\n---\n{}\n---".format(label, old))
    if text != before:
        print("  {:<16} {} edits".format(label, applied))
        if not ARGS.check:
            io.open(path, "w", encoding="utf-8", newline="").write(text)
        return 1
    return 0


def main():
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    ARGS = parser.parse_args()
    changed = apply(os.path.join(QML, "Toolbar.qml"), TOOLBAR, "Toolbar.qml")
    changed += apply(os.path.join(QML, "Main.qml"), MAIN, "Main.qml")
    print("{} {} files".format("would change" if ARGS.check else "changed", changed))


if __name__ == "__main__":
    main()
