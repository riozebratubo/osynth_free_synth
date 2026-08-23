#!/usr/bin/env python3
"""Phase 2a: move the QML consumers of paramMeta() onto the ParamMeta value type.

SynthController::paramMeta() returns a ParamMeta gadget (src/paramtypes.h)
instead of a QVariantMap, so the properties holding its result can be declared
`paramMeta` rather than `var` and every `meta.exists` / `meta.min` / `meta.max`
read becomes a typed member access the compiler can emit C++ for.

Two shapes disappear along the way, both of which existed only because the old
call could not be trusted with a bad id:

    paramId >= 0 ? Synth.paramMeta(paramId) : ({ exists: false })
    property var meta: ({ exists: false })

paramMeta() is range-checked now and answers a default ParamMeta (exists false)
for anything it does not know, so the guard and the hand-built stand-in object
are both redundant.

What is deliberately NOT changed:

  * the metaRevision dependency in ParamControl.qml and the imperative
    refresh() calls in Toolbar.qml. paramMeta() still has no change signal --
    it is a plain call -- so those workarounds are load-bearing. Their comments
    explain what breaks without them (node parameters registered after
    discovery, whose delegates are never recreated).

  * paramPickerList(), still a QVariantList. PlockDialog feeds it to a ComboBox
    with textRole/valueRole, and whether those resolve against a QList<gadget>
    needs testing before the picker is rebuilt on top of it.

Idempotent. --check reports without writing.
"""
import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QML = os.path.join(ROOT, "app_osyntho", "qml")

ARGS = None

EDITS = {
    "Knob.qml": [(
        "    readonly property var meta: paramId >= 0 ? Synth.paramMeta(paramId) : ({ exists: false })",
        "    readonly property paramMeta meta: Synth.paramMeta(root.paramId)",
    )],
    "EnumSelector.qml": [(
        "    readonly property var meta: paramId >= 0 ? Synth.paramMeta(paramId) : ({ exists: false })",
        "    readonly property paramMeta meta: Synth.paramMeta(root.paramId)",
    )],
    "ParamControl.qml": [
        (
            "    readonly property var meta: {\n"
            "        const dep = pc.metaRevision  // read only to register the dependency\n"
            "        void dep\n"
            "        return pc.paramId >= 0 ? Synth.paramMeta(pc.paramId) : ({ exists: false })\n"
            "    }",
            "    readonly property paramMeta meta: {\n"
            "        const dep = pc.metaRevision  // read only to register the dependency\n"
            "        void dep\n"
            "        return Synth.paramMeta(pc.paramId)\n"
            "    }",
        ),
        (
            "            const now = pc.paramId >= 0 ? Synth.paramMeta(pc.paramId) : null\n"
            "            if (!now || !now.exists || now.name !== pc.meta.name) pc.metaRevision++",
            "            const now = Synth.paramMeta(pc.paramId)\n"
            "            if (!now.exists || now.name !== pc.meta.name) pc.metaRevision++",
        ),
    ],
    "Toolbar.qml": [
        (
            "            property var meta: ({ exists: false })\n"
            "            visible: meta.exists\n"
            "            spacing: 4\n"
            "            Layout.rightMargin: 6\n"
            "            Layout.preferredWidth: 70  // reserve space so the slider isn't squeezed to 0",
            "            property paramMeta meta\n"
            "            visible: masterVol.meta.exists\n"
            "            spacing: 4\n"
            "            Layout.rightMargin: 6\n"
            "            Layout.preferredWidth: 70  // reserve space so the slider isn't squeezed to 0",
        ),
        (
            "            property var meta: ({ exists: false })\n"
            "            visible: meta.exists\n"
            "            spacing: 4\n"
            "            Layout.rightMargin: 6\n"
            "            Layout.preferredWidth: 70\n",
            "            property paramMeta meta\n"
            "            visible: outLevel.meta.exists\n"
            "            spacing: 4\n"
            "            Layout.rightMargin: 6\n"
            "            Layout.preferredWidth: 70\n",
        ),
        (
            "                meta = levelId >= 0 ? Synth.paramMeta(levelId) : ({ exists: false })",
            "                meta = Synth.paramMeta(levelId)",
        ),
    ],
    "PlockDialog.qml": [
        (
            "    property int pid: -1\n"
            "    property var meta: ({})",
            "    property int pid: -1\n"
            "    // Derived rather than assigned: `pid` is the only thing that moves it,\n"
            "    // and binding to it drops the two hand-written resets this used to need.\n"
            "    readonly property paramMeta meta: Synth.paramMeta(root.pid)",
        ),
        (
            "        pid = -1\n"
            "        meta = ({})\n"
            "        open()",
            "        pid = -1\n"
            "        open()",
        ),
        (
            "        pid = choices[index].id\n"
            "        meta = Synth.paramMeta(pid)\n",
            "        pid = choices[index].id\n",
        ),
    ],
}


def main():
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    ARGS = parser.parse_args()

    changed = 0
    for name, pairs in sorted(EDITS.items()):
        path = os.path.join(QML, name)
        with open(path, encoding="utf-8") as handle:
            before = handle.read()
        after = before
        applied = 0
        for old, new in pairs:
            if after.count(old) == 1:
                after = after.replace(old, new, 1)
                applied += 1
            elif new in after:
                continue  # already applied
            else:
                sys.exit("{}: could not match\n---\n{}\n---".format(name, old))
        if after != before:
            changed += 1
            print("  {:<22} {} edits".format(name, applied))
            if not ARGS.check:
                with open(path, "w", encoding="utf-8", newline="") as handle:
                    handle.write(after)
    print("{} {} files".format("would change" if ARGS.check else "changed", changed))


if __name__ == "__main__":
    main()
