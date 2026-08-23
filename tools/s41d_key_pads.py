#!/usr/bin/env python3
"""Chord page: the Key card's root and scale become button pads (S41d).

Same reasoning as the inversion and voicing rows: a dropdown you have to open
to read is the wrong control for a value you change while a chord is sounding,
now that the firmware re-voices what is held the moment it arrives.

Root goes with it. It is the other half of "the key", it was a knob for twelve
pitch classes — drag past the one you wanted — and leaving it beside a row of
scale pads would have been the odd one out.

Scale names come from PARAM_INFO, capitalised for display only: the registered
spelling is the firmware's identifier and is never touched, exactly as
UI.paramLabel() does it everywhere else.

Kept per the project's intermediary-artifacts policy. Idempotent.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
path = REPO / "app_osyntho/qml/ChordScreen.qml"

EDITS = [
    ("""    property list<string> voicingLabels: []""",
     """    property list<string> voicingLabels: []
    property list<string> scaleLabels: []"""),

    ("""        root.voicingLabels = Synth.paramMeta(root.pidVoicing).enumNames""",
     """        root.voicingLabels = Synth.paramMeta(root.pidVoicing).enumNames
        // Capitalised for the pads only. The registered spelling is the
        // firmware's identifier and every lookup still uses it, which is the
        // rule UI.paramLabel() follows for every other control in the app.
        const scales = Synth.paramMeta(root.pidScale).enumNames
        const shown = []
        for (let s = 0; s < scales.length; ++s)
            shown.push(UI.capitalized(scales[s]))
        root.scaleLabels = shown"""),

    ("""                Flow {
                    width: parent.width
                    spacing: 10
                    ParamControl { paramId: root.pidScale }
                    ParamControl { paramId: root.pidRoot }
                    ParamControl { paramId: root.pidKeymap }
                    ParamControl { paramId: root.pidFollow }
                }""",
     """                PadRow {
                    width: parent.width
                    paramId: root.pidRoot
                    label: "Root"
                    // The twelve pitch classes, so the key is one tap rather
                    // than a knob drag past the one you wanted.
                    options: root.pitchNames
                }

                PadRow {
                    width: parent.width
                    paramId: root.pidScale
                    label: "Scale"
                    options: root.scaleLabels
                }

                Flow {
                    width: parent.width
                    spacing: 10
                    ParamControl { paramId: root.pidKeymap }
                    ParamControl { paramId: root.pidFollow }
                }"""),
]


def main():
    src = path.read_text(encoding="utf-8")
    if "root.scaleLabels" in src:
        print("already patched")
        return
    for old, new in EDITS:
        if src.count(old) != 1:
            sys.exit("anchor appears %d times:\n%s" % (src.count(old), old[:90]))
        src = src.replace(old, new, 1)
    path.write_text(src, encoding="utf-8")
    print("patched")


main()
