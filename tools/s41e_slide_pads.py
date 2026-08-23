#!/usr/bin/env python3
"""Chord page: pads respond to a slide, the way the keyboard does (S41e).

A Button grabs its own press, so a finger dragged across a row of them acts on
the first one and nothing else. Keyboard.qml has the same problem and solves it
the same way: the keys are plain rectangles and a single input surface sits
over all of them. This does that for the chord page's pads.

Two behaviours over one mechanism:

  * the key board is momentary — entering a pad sounds it, leaving it stops it,
    so a drag across the board is a glissando of chords;
  * the parameter rows are selection — the value follows the finger, and since
    the firmware re-voices a held chord as the value lands (S41b), sliding
    across the qualities auditions every one of them under a chord you are
    already holding.

Kept per the project's intermediary-artifacts policy. Idempotent.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
path = REPO / "app_osyntho/qml/ChordScreen.qml"

SLIDE_AREA = '''    // One input surface over a container of pads.
    //
    // Buttons cannot do this: each grabs its own press, so a finger dragged
    // across a row acts on the first pad and never reaches its neighbours.
    // Keyboard.qml has the same problem and answers it the same way — the keys
    // are plain rectangles and a single area listens for all of them.
    //
    // Deliberately no preventStealing: the page is a long Flickable, so a
    // vertical drag has to keep scrolling it. Losing the grab that way arrives
    // as onCanceled, which releases whatever was sounding — the same thing a
    // finger leaving the keyboard does. Horizontal drags are never stolen
    // (contentWidth is the viewport width), so the slide itself is unaffected.
    component SlideArea: MouseArea {
        id: slide
        // The Flow or Grid holding the pads. Hit-tested with childAt() rather
        // than arithmetic because a Flow wraps where it likes, and the gaps
        // between pads answer null — which reads as "no pad", correctly.
        property Item pads: null
        property int active: -1

        signal padEntered(index: int)
        signal padExited(index: int)

        anchors.fill: slide.pads

        function indexAt(px: real, py: real): int {
            if (!slide.pads)
                return -1
            const item = slide.pads.childAt(px, py)
            // An untyped read, and the one in this file: childAt() answers a
            // bare Item and only the pad delegates carry padIndex.
            return (item && item.padIndex !== undefined) ? item.padIndex : -1
        }

        function moveTo(px: real, py: real): void {
            const i = slide.indexAt(px, py)
            if (i === slide.active)
                return
            if (slide.active >= 0)
                slide.padExited(slide.active)
            slide.active = i
            if (i >= 0)
                slide.padEntered(i)
        }

        function endHere(): void {
            if (slide.active >= 0)
                slide.padExited(slide.active)
            slide.active = -1
        }

        onPressed: (m) => slide.moveTo(m.x, m.y)
        onPositionChanged: (m) => { if (slide.pressed) slide.moveTo(m.x, m.y) }
        onReleased: slide.endHere()
        onCanceled: slide.endHere()
    }

    // The look every pad shares: a plain rectangle, because the input belongs
    // to the SlideArea above and a Button would take it back.
    component Pad: Rectangle {
        id: padItem
        property int padIndex: 0
        property bool on: false
        property string topText: ""
        property string mainText: ""

        radius: 4
        color: padItem.on
               ? Material.accent
               : (Material.theme === Material.Dark ? "#22FFFFFF" : "#14000000")
        border.width: 1
        border.color: Material.theme === Material.Dark ? "#33FFFFFF" : "#1F000000"

        Column {
            anchors.centerIn: parent
            spacing: 1
            Label {
                width: padItem.width - 8
                visible: padItem.topText.length > 0
                text: padItem.topText
                font.pointSize: UI.fontSize * 0.6
                opacity: padItem.on ? 0.85 : 0.6
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
                color: padItem.on ? "white" : Material.foreground
            }
            Label {
                width: padItem.width - 8
                text: padItem.mainText
                font.pointSize: UI.fontSize * 0.8
                font.bold: true
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
                color: padItem.on ? "white" : Material.foreground
            }
        }
    }

'''

PAD_ROW = '''    // One parameter as a row of pads: the whole range visible, one tap away,
    // and the value follows a finger dragged across it. With the firmware
    // re-voicing a held chord as each value lands (S41b), that slide is how
    // you audition every quality under a chord you are already holding.
    //
    // setParamNow() rather than setParam(): the ordinary path batches knob
    // drags at ~20 Hz, which on a single discrete tap is felt as lag.
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

        Item {
            width: padRow.width
            height: padFlow.implicitHeight

            Flow {
                id: padFlow
                width: parent.width
                spacing: 4
                Repeater {
                    model: padRow.options
                    delegate: Pad {
                        required property string modelData
                        required property int index
                        padIndex: index
                        on: padValue.asInt === index
                        mainText: Tr.t(modelData)
                        implicitWidth: Math.max(44, mainText.length * UI.fontSize * 0.62 + 16)
                        width: implicitWidth
                        height: Math.round(UI.fontSize * 2.4)
                    }
                }
            }

            SlideArea {
                pads: padFlow
                onPadEntered: (index) => Synth.setParamNow(padRow.paramId, index)
            }
        }
    }

'''


def main():
    src = path.read_text(encoding="utf-8")
    if "component SlideArea" in src:
        print("already patched")
        return

    # 1. SlideArea + Pad go in ahead of PadRow, which now uses both.
    anchor = "    // One parameter as a row of buttons: the whole range visible and one tap\n"
    if src.count(anchor) != 1:
        sys.exit("PadRow comment anchor appears %d times" % src.count(anchor))
    start = src.index(anchor)
    end = src.index("    // A titled card, so the six of them below do not repeat sixteen lines of")
    src = src[:start] + SLIDE_AREA + PAD_ROW + src[end:]

    # 2. The free-mode quality grid *is* a PadRow. Say so once instead of
    #    keeping a second hand-built copy of the same control.
    old_grid = """                Flow {
                    width: parent.width
                    spacing: 6
                    Repeater {
                        model: root.qualityNames
                        delegate: Button {
                            required property string modelData
                            required property int index
                            text: modelData
                            highlighted: typeVal.asInt === index
                            flat: !highlighted
                            padding: 6
                            onClicked: if (root.pidType >= 0)
                                           Synth.setParamNow(root.pidType, index)
                        }
                    }
                }"""
    new_grid = """                PadRow {
                    width: parent.width
                    paramId: root.pidType
                    options: root.qualityNames
                }"""
    if src.count(old_grid) != 1:
        sys.exit("quality grid anchor appears %d times" % src.count(old_grid))
    src = src.replace(old_grid, new_grid, 1)

    # 3. The board: pads plus one area, momentary.
    old_board_start = "                Grid {\n                    id: board\n"
    if src.count(old_board_start) != 1:
        sys.exit("board anchor appears %d times" % src.count(old_board_start))
    bstart = src.index(old_board_start)
    bend = src.index("            }\n\n            // ---- user mode", bstart)
    new_board = '''                Item {
                    width: parent.width
                    height: board.implicitHeight

                    Grid {
                        id: board
                        width: parent.width
                        columns: Math.max(1, root.degreesPerOctave)
                        spacing: 4
                        // Pads share the row evenly, so the board fits a phone
                        // in portrait and a desktop window without a second
                        // layout.
                        readonly property real cellWidth:
                            Math.max(40, (width - spacing * (columns - 1)) / columns)

                        Repeater {
                            model: root.degreeKeys
                            delegate: Pad {
                                required property int modelData
                                required property int index
                                padIndex: index
                                width: board.cellWidth
                                height: Math.round(UI.fontSize * 3.6)
                                // Lit while sounding, so a slide across the
                                // board reads back as the chords it played.
                                on: boardSlide.active === index
                                topText: Synth.noteName(modelData)
                                mainText: index < root.degreeLabels.length
                                          ? root.degreeLabels[index] : ""
                            }
                        }
                    }

                    // Momentary: entering a pad sounds it and leaving it stops
                    // it, so dragging across the board is a glissando of
                    // chords rather than one chord and eleven dead pads.
                    SlideArea {
                        id: boardSlide
                        pads: board
                        enabled: Synth.connected
                        onPadEntered: (index) => {
                            if (index < root.degreeKeys.length)
                                Synth.noteOn(root.degreeKeys[index], 100)
                        }
                        onPadExited: (index) => {
                            if (index < root.degreeKeys.length)
                                Synth.noteOff(root.degreeKeys[index])
                        }
                    }
                }
'''
    src = src[:bstart] + new_board + src[bend:]
    path.write_text(src, encoding="utf-8")
    print("patched")


main()
