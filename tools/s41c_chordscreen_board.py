#!/usr/bin/env python3
"""Chord page: a key board in scale mode, and pad rows for the discrete
controls (S41c).

Two changes, both about immediacy:

  * scale mode draws the keyboard's keys as a board of pads, two octaves of
    scale degrees laid out one octave per row, each labelled with the key you
    press and the chord it plays.
  * inversion, voicing and chord size become rows of buttons rather than a
    knob and a dropdown, and every one of them writes with setParamNow(). The
    firmware re-voices a held chord on the spot (S41b), so a ~20 Hz coalesced
    write would show up as lag on a control whose whole point is that you hear
    it change under your fingers.

Kept per the project's intermediary-artifacts policy. Idempotent.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
path = REPO / "app_osyntho/qml/ChordScreen.qml"

EDITS = [
    # --- state the board needs -------------------------------------------
    ("""    property list<int> degreeKeys: []
    property list<string> degreeLabels: []""",
     """    // The board: `boardOctaves` octaves of scale degrees, one row each.
    // degreesPerOctave is the column count, and comes from the scale rather
    // than from a guess of seven — the pentatonics have five and blues six.
    readonly property int boardOctaves: 2
    property int degreesPerOctave: 7
    property list<int> degreeKeys: []
    property list<string> degreeLabels: []
    // Labels for the two controls drawn as pads rather than as a knob and a
    // dropdown. Voicing's come from PARAM_INFO so they cannot disagree with
    // the values they write; inversion is a plain int and has none, so its
    // four are named here.
    property list<string> voicingLabels: []
    readonly property list<string> invLabels: ["Root", "1st", "2nd", "3rd"]
    readonly property list<string> sizeLabels:
        ["1", "3rd", "Triad", "7th", "9th", "11th", "13th"]"""),

    ("""        root.qualityNames = qualities
        root.slotChoices = choices
        root.refreshPreview()""",
     """        root.qualityNames = qualities
        root.slotChoices = choices
        root.voicingLabels = Synth.paramMeta(root.pidVoicing).enumNames
        root.refreshPreview()"""),

    ("""    function refreshPreview(): void {
        const keys = Synth.chordDegreeKeys()
        const labels = []
        for (let i = 0; i < keys.length; ++i)
            labels.push(Synth.chordNameFor(keys[i]))
        root.degreeKeys = keys
        root.degreeLabels = labels""",
     """    function refreshPreview(): void {
        root.degreesPerOctave = Math.max(1, Synth.chordDegreeKeys(1).length)
        const keys = Synth.chordDegreeKeys(root.boardOctaves)
        const labels = []
        for (let i = 0; i < keys.length; ++i)
            labels.push(Synth.chordNameFor(keys[i]))
        root.degreeKeys = keys
        root.degreeLabels = labels"""),

    # --- the pad-row component -------------------------------------------
    ("""    // A titled card, so the six of them below do not repeat sixteen lines of""",
     """    // One parameter as a row of buttons: the whole range visible and one tap
    // away, instead of a dropdown you have to open to read or a knob you have
    // to drag past the value you wanted.
    //
    // setParamNow() rather than setParam(): these land on a chord that is
    // already sounding, and the firmware re-voices it the moment the value
    // arrives, so the ~20 Hz batching the ordinary path uses for knob drags
    // would be felt as lag on a control that is a single discrete tap.
    component PadRow: Column {
        id: padRow
        property int paramId: -1
        property string label: ""
        property list<string> options: []

        spacing: 2
        visible: padRow.paramId >= 0 && padRow.options.length > 0

        ParamValue { id: padValue; paramId: padRow.paramId }

        Label {
            text: padRow.label.length > 0 ? Tr.t(padRow.label) : ""
            font.pointSize: UI.fontSize * 0.68
            opacity: 0.75
            color: Material.foreground
        }

        Flow {
            width: padRow.width
            spacing: 4
            Repeater {
                model: padRow.options
                delegate: Button {
                    required property string modelData
                    required property int index
                    text: Tr.t(modelData)
                    highlighted: padValue.asInt === index
                    flat: !highlighted
                    padding: 6
                    onClicked: Synth.setParamNow(padRow.paramId, index)
                }
            }
        }
    }

    // A titled card, so the six of them below do not repeat sixteen lines of"""),

    # --- free mode: the quality grid writes immediately too ---------------
    ("""                            onClicked: if (root.pidType >= 0)
                                           Synth.setParam(root.pidType, index)""",
     """                            onClicked: if (root.pidType >= 0)
                                           Synth.setParamNow(root.pidType, index)"""),

    # --- scale card: size as pads, and the board --------------------------
    ("""                Flow {
                    width: parent.width
                    spacing: 10
                    ParamControl { paramId: root.pidScale }
                    ParamControl { paramId: root.pidRoot }
                    ParamControl { paramId: root.pidSize }
                    ParamControl { paramId: root.pidKeymap }
                    ParamControl { paramId: root.pidFollow }
                }""",
     """                Flow {
                    width: parent.width
                    spacing: 10
                    ParamControl { paramId: root.pidScale }
                    ParamControl { paramId: root.pidRoot }
                    ParamControl { paramId: root.pidKeymap }
                    ParamControl { paramId: root.pidFollow }
                }

                PadRow {
                    width: parent.width
                    paramId: root.pidSize
                    label: "Notes in the chord"
                    // A knob for seven discrete steps meant dragging past the
                    // one you wanted; these also name what each count *is*,
                    // which "4" on a knob never did.
                    options: root.sizeLabels
                }"""),

    ("""                // The degree strip: what each scale degree actually plays, by
                // name, and playable. The most useful thing this page can show
                // — it turns "major, 4 notes" into "Cmaj7 Dm7 Em7 Fmaj7 G7 Am7
                // Bm7b5".
                Flow {
                    width: parent.width
                    spacing: 6
                    Repeater {
                        model: root.degreeKeys
                        delegate: Button {
                            required property int modelData
                            required property int index
                            text: (index + 1) + "  "
                                  + (index < root.degreeLabels.length
                                     ? root.degreeLabels[index] : "")
                            flat: true
                            padding: 6
                            enabled: Synth.connected
                            // Press to sound, release to stop, so the strip
                            // behaves like the keys it is describing. onCanceled
                            // matters as much as onReleased: a press that turns
                            // into a page drag never releases, and would leave
                            // the chord droning.
                            onPressed: Synth.noteOn(modelData, 100)
                            onReleased: Synth.noteOff(modelData)
                            onCanceled: Synth.noteOff(modelData)
                        }
                    }
                }""",
     """                // The board: the actual keys of the keyboard, laid out one
                // octave of the scale per row, each labelled with the key you
                // press and the chord it plays. The most useful thing this
                // page can show — it turns "major, 4 notes" into a picture of
                // Cmaj7 Dm7 Em7 Fmaj7 G7 Am7 Bm7b5 sitting under your hand.
                Grid {
                    id: board
                    width: parent.width
                    columns: Math.max(1, root.degreesPerOctave)
                    spacing: 4
                    // Pads share the row evenly, so the board fits a phone in
                    // portrait and a desktop window without a second layout.
                    readonly property real cellWidth:
                        Math.max(40, (width - spacing * (columns - 1)) / columns)

                    Repeater {
                        model: root.degreeKeys
                        delegate: Button {
                            id: pad
                            required property int modelData
                            required property int index
                            // The tonic of each octave, which is what makes
                            // the grid readable as a scale rather than as a
                            // block of buttons.
                            readonly property bool tonic:
                                index % Math.max(1, root.degreesPerOctave) === 0

                            width: board.cellWidth
                            height: padCol.implicitHeight + 14
                            flat: !pad.tonic
                            highlighted: pad.tonic
                            enabled: Synth.connected
                            padding: 4
                            // Press to sound, release to stop, so a pad
                            // behaves like the key it stands for. onCanceled
                            // matters as much as onReleased: a press that
                            // turns into a page drag never releases, and would
                            // leave the chord droning.
                            onPressed: Synth.noteOn(pad.modelData, 100)
                            onReleased: Synth.noteOff(pad.modelData)
                            onCanceled: Synth.noteOff(pad.modelData)

                            contentItem: Column {
                                id: padCol
                                spacing: 1
                                Label {
                                    width: pad.width - 8
                                    text: Synth.noteName(pad.modelData)
                                    font.pointSize: UI.fontSize * 0.6
                                    opacity: 0.65
                                    elide: Text.ElideRight
                                    horizontalAlignment: Text.AlignHCenter
                                    color: Material.foreground
                                }
                                Label {
                                    width: pad.width - 8
                                    text: pad.index < root.degreeLabels.length
                                          ? root.degreeLabels[pad.index] : ""
                                    font.pointSize: UI.fontSize * 0.8
                                    font.bold: true
                                    elide: Text.ElideRight
                                    horizontalAlignment: Text.AlignHCenter
                                    color: Material.foreground
                                }
                            }
                        }
                    }
                }"""),

    # --- voicing card: inv and voicing as pads ---------------------------
    ("""                Flow {
                    width: parent.width
                    spacing: 10
                    ParamControl { paramId: root.pidInv }
                    ParamControl { paramId: root.pidVoicing }
                    ParamControl { paramId: root.pidBass }
                    ParamControl { paramId: root.pidRange }
                    ParamControl { paramId: root.pidVel }
                    ParamControl { paramId: root.pidLead }
                }""",
     """                PadRow {
                    width: parent.width
                    paramId: root.pidInv
                    label: "Inversion"
                    options: root.invLabels
                }

                PadRow {
                    width: parent.width
                    paramId: root.pidVoicing
                    label: "Voicing"
                    options: root.voicingLabels
                }

                Flow {
                    width: parent.width
                    spacing: 10
                    ParamControl { paramId: root.pidBass }
                    ParamControl { paramId: root.pidRange }
                    ParamControl { paramId: root.pidVel }
                    ParamControl { paramId: root.pidLead }
                }"""),

    # --- and say so, since it is now true --------------------------------
    ("""                    text: Tr.t("Voice-leading picks the inversion nearest the chord you just played, so a progression moves by the shortest path — it overrides the inversion setting while it is on.")""",
     """                    text: Tr.t("Every setting on this page takes effect on the chord you are already holding, not just the next one. Voice-leading picks the inversion nearest the chord you just played, so a progression moves by the shortest path — it overrides the inversion setting while it is on.")"""),
]


def main():
    src = path.read_text(encoding="utf-8")
    if "component PadRow" in src:
        print("already patched")
        return
    for old, new in EDITS:
        if src.count(old) != 1:
            sys.exit("anchor appears %d times:\n%s" % (src.count(old), old[:90]))
        src = src.replace(old, new, 1)
    path.write_text(src, encoding="utf-8")
    print("patched")


main()
