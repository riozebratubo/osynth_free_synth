pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// Chord mode (S41): one key in, a chord out.
//
// Every control here is an ordinary 0x044x parameter, so most of the page is
// ParamControl — which means the enum labels are the firmware's own and cannot
// drift from the values they write. The hand-built parts are only the ones a
// generic card does badly: the mode tabs (the mode decides which half of the
// page is even meaningful, so it should not be a dropdown you have to open to
// read), the quality grid, the degree strip and the user-set editor.
//
// The strip and the readouts ask Synth.chordNameFor() / chordNotesFor(), which
// compute app-side. See the comment over chordNotesFor() in
// synthcontroller.cpp for why that is a copy of the firmware's construction
// and what it deliberately does not copy: it names chords, it does not claim
// to voice them.
//
// Everything reactive here is refreshed imperatively rather than bound.
// Synth's parameter accessors are plain invokables — a binding onto one
// captures no property and evaluates exactly once, before the synth is even
// connected (see ParamValue.qml, which exists for the same reason).
Item {
    id: root

    property int pidEnable: -1
    property int pidMode: -1
    property int pidType: -1
    property int pidScale: -1
    property int pidRoot: -1
    property int pidKeymap: -1
    property int pidSize: -1
    property int pidFollow: -1
    property int pidInv: -1
    property int pidVoicing: -1
    property int pidBass: -1
    property int pidRange: -1
    property int pidVel: -1
    property int pidLead: -1
    property int pidStrum: -1
    property int pidStrumDir: -1
    property int pidRoute: -1
    property int pidKeys: -1
    property int pidRestrike: -1

    // Note names without an octave number. Spelled out rather than trimmed off
    // Synth.noteName(): a regex in a binding is one of the constructs
    // qmlcachegen cannot compile, and this page has one in a Repeater.
    readonly property list<string> pitchNames:
        ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

    // Refreshed, not bound — see the header. `qualityNames` comes from
    // PARAM_INFO on chord.type, so the grid's buttons and the indices they
    // write can never describe different chords.
    property list<string> qualityNames: []
    // The same list with "silent" in front, for the user-set pickers. Built
    // once here rather than with Array.concat at each delegate: `qualityNames`
    // is a QML sequence type, not a JS array, and concat does not flatten one.
    property list<string> slotChoices: []
    // The board: `boardOctaves` octaves of scale degrees, one row each.
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
    property list<string> scaleLabels: []
    readonly property list<string> invLabels: ["Root pos.", "1st", "2nd", "3rd"]
    readonly property list<string> sizeLabels:
        ["Single", "3rd", "Triad", "7th", "9th", "11th", "13th"]
    // chord.restrike's two enum labels are "changed" and "all", which are
    // exact but say nothing on a pad. These name what each one does; the
    // values written are still the enum's indices.
    readonly property list<string> restrikeLabels:
        ["Play what changed", "Play the whole chord"]
    // What the tonic currently plays, spelled out. The one readout that turns
    // "major, 4 notes" into something you can check at a glance.
    property string rootChord: ""

    ParamValue { id: enableVal; paramId: root.pidEnable }
    ParamValue { id: modeVal; paramId: root.pidMode }

    readonly property int mode: modeVal.asInt   // 0 free, 1 scale, 2 user
    // The page needs only the parameters. `Synth.chordAvailable` is a narrower
    // question — it is about the *user set*, which travels over its own
    // opcode — so it gates that one card and nothing else.
    readonly property bool available: root.pidEnable >= 0

    function refreshIds(): void {
        root.pidEnable = Synth.paramIdForName("chord.enable")
        root.pidMode = Synth.paramIdForName("chord.mode")
        root.pidType = Synth.paramIdForName("chord.type")
        root.pidScale = Synth.paramIdForName("chord.scale")
        root.pidRoot = Synth.paramIdForName("chord.root")
        root.pidKeymap = Synth.paramIdForName("chord.keymap")
        root.pidSize = Synth.paramIdForName("chord.size")
        root.pidFollow = Synth.paramIdForName("chord.follow")
        root.pidInv = Synth.paramIdForName("chord.inv")
        root.pidVoicing = Synth.paramIdForName("chord.voicing")
        root.pidBass = Synth.paramIdForName("chord.bass")
        root.pidRange = Synth.paramIdForName("chord.range")
        root.pidVel = Synth.paramIdForName("chord.vel")
        root.pidLead = Synth.paramIdForName("chord.lead")
        root.pidStrum = Synth.paramIdForName("chord.strum")
        root.pidStrumDir = Synth.paramIdForName("chord.strumdir")
        root.pidRoute = Synth.paramIdForName("chord.route")
        root.pidKeys = Synth.paramIdForName("chord.keys")
        root.pidRestrike = Synth.paramIdForName("chord.restrike")
        const qualities = Synth.chordQualityNames()
        const choices = [Tr.t("silent")]
        for (let i = 0; i < qualities.length; ++i) choices.push(qualities[i])
        root.qualityNames = qualities
        root.slotChoices = choices
        root.voicingLabels = Synth.paramMeta(root.pidVoicing).enumNames
        // Capitalised for the pads only. The registered spelling is the
        // firmware's identifier and every lookup still uses it, which is the
        // rule UI.paramLabel() follows for every other control in the app.
        const scales = Synth.paramMeta(root.pidScale).enumNames
        const shown = []
        for (let s = 0; s < scales.length; ++s)
            shown.push(UI.capitalized(scales[s]))
        root.scaleLabels = shown
        root.refreshPreview()
    }

    function refreshPreview(): void {
        root.degreesPerOctave = Math.max(1, Synth.chordDegreeKeys(1).length)
        const keys = Synth.chordDegreeKeys(root.boardOctaves)
        const labels = []
        for (let i = 0; i < keys.length; ++i)
            labels.push(Synth.chordNameFor(keys[i]))
        root.degreeKeys = keys
        root.degreeLabels = labels
        const tonic = keys.length > 0 ? keys[0] : 60
        const notes = Synth.chordNotesFor(tonic)
        const spelled = []
        for (let n = 0; n < notes.length; ++n)
            spelled.push(root.pitchNames[notes[n] % 12])
        root.rootChord = spelled.length > 0
            ? (Synth.chordNameFor(tonic) + "   " + spelled.join(" "))
            : ""
    }

    Connections {
        target: Synth
        function onParamsDiscovered(): void { root.refreshIds() }
        // Only the parameters this page draws conclusions from. Without the
        // name test every knob drag anywhere in the app would rebuild the
        // strip, and a drag is a stream of writes.
        function onParamChanged(id: int, value: real): void {
            const name = Synth.paramName(id)
            if (name.startsWith("chord.") || name === "seq.scale"
                    || name === "seq.root")
                root.refreshPreview()
        }
        function onChordSetChanged(): void { root.refreshPreview() }
    }
    Component.onCompleted: root.refreshIds()

    // One input surface over a container of pads.
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

        // Wide enough for the label it carries. Text.implicitWidth is the
        // natural width and does not follow the assigned one (no wrapping,
        // elide instead), so this cannot feed back into itself. The board
        // overrides `width` outright, where every pad has to be the same size.
        implicitWidth: Math.max(44, mainLabel.implicitWidth + 16)
        implicitHeight: Math.round(UI.fontSize * 2.4)

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
                id: mainLabel
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

    // One parameter as a row of pads: the whole range visible, one tap away,
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
                    }
                }
            }

            SlideArea {
                pads: padFlow
                onPadEntered: (index) => Synth.setParamNow(padRow.paramId, index)
            }
        }
    }

    // A titled card, so the six of them below do not repeat sixteen lines of
    // geometry each. Matches ParamGroup's look deliberately: the auto-built
    // cards on every other page and the hand-built ones here should not read
    // as two different kinds of thing.
    component Card: Rectangle {
        id: card
        property string title: ""
        default property alias content: cardCol.data
        property real cardWidth: 0

        width: card.cardWidth
        implicitHeight: card.visible ? cardCol.implicitHeight + 16 : 0
        height: card.implicitHeight
        radius: 8
        color: Material.theme === Material.Dark ? "#1AFFFFFF" : "#0D000000"

        Column {
            id: cardCol
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 8
            spacing: 8

            Label {
                text: card.title.length > 0 ? Tr.t(card.title) : ""
                visible: card.title.length > 0
                font.bold: true
                font.pointSize: UI.fontSize * 0.95
                color: Material.foreground
            }
        }
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: panels.implicitHeight + 24
        clip: true
        visible: root.available

        PanelFlow {
            id: panels
            spacing: 12

            // ---- the toggle and the mode ---------------------------------
            Card {
                cardWidth: panels.contentWidth

                RowLayout {
                    width: parent.width
                    spacing: 12

                    Switch {
                        id: enableSwitch
                        text: Tr.t("Chord mode")
                        enabled: root.pidEnable >= 0
                        property bool syncing: false
                        checked: enableVal.on
                        onToggled: if (!enableSwitch.syncing && root.pidEnable >= 0)
                                       Synth.setParam(root.pidEnable,
                                                      enableSwitch.checked ? 1 : 0)
                        // A tap assigns `checked` and breaks the binding above;
                        // re-assert it whenever the synth reports the value, so
                        // a preset load, a MIDI CC or the mod matrix still moves
                        // the switch.
                        Connections {
                            target: enableVal
                            function onValueChanged(): void {
                                enableSwitch.syncing = true
                                enableSwitch.checked = enableVal.on
                                enableSwitch.syncing = false
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: enableVal.on ? Tr.t("Keys play chords.")
                                           : Tr.t("Keys play single notes.")
                        opacity: 0.6
                        elide: Text.ElideRight
                        color: Material.foreground
                    }
                }

                TabBar {
                    id: modeTabs
                    width: parent.width
                    enabled: root.pidMode >= 0
                    property bool syncing: false
                    currentIndex: modeVal.asInt
                    // The binding above is broken the moment a tab is tapped
                    // (TabBar assigns currentIndex itself), and it also fires
                    // this handler when the *synth* moves the mode. The value
                    // test is what stops the second case echoing straight back
                    // out as a write.
                    onCurrentIndexChanged: {
                        if (modeTabs.syncing || root.pidMode < 0) return
                        if (Math.round(Synth.paramValue(root.pidMode))
                                !== modeTabs.currentIndex)
                            Synth.setParam(root.pidMode, modeTabs.currentIndex)
                    }
                    Connections {
                        target: modeVal
                        function onValueChanged(): void {
                            modeTabs.syncing = true
                            modeTabs.currentIndex = modeVal.asInt
                            modeTabs.syncing = false
                        }
                    }
                    TabButton { text: Tr.t("Free") }
                    TabButton { text: Tr.t("Scale") }
                    TabButton { text: Tr.t("User set") }
                }

                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    opacity: 0.6
                    font.pointSize: UI.fontSize * 0.85
                    color: Material.foreground
                    text: root.mode === 0
                          ? Tr.t("One chord shape under every key, transposed.")
                          : root.mode === 1
                            ? Tr.t("The key picks a scale degree and the chord is stacked in scale thirds, so the quality follows the degree — there is no wrong chord to play.")
                            : Tr.t("Twelve slots, one per key of the octave above the root.")
                }
            }

            // ---- free mode: the quality grid -----------------------------
            Card {
                title: "Chord"
                cardWidth: panels.contentWidth
                visible: root.mode === 0 && root.pidType >= 0

                PadRow {
                    width: parent.width
                    paramId: root.pidType
                    options: root.qualityNames
                }

                Label {
                    width: parent.width
                    visible: root.rootChord.length > 0
                    text: Tr.t("C plays") + ":  " + root.rootChord
                    opacity: 0.7
                    color: Material.foreground
                }
            }

            // ---- scale mode: the key, and the chords it gives you ---------
            Card {
                title: "Key"
                cardWidth: panels.contentWidth
                visible: root.mode === 1

                PadRow {
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
                }

                PadRow {
                    width: parent.width
                    paramId: root.pidSize
                    label: "Notes in the chord"
                    // A knob for seven discrete steps meant dragging past the
                    // one you wanted; these also name what each count *is*,
                    // which "4" on a knob never did.
                    options: root.sizeLabels
                }

                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    opacity: 0.6
                    font.pointSize: UI.fontSize * 0.85
                    color: Material.foreground
                    text: Tr.t("Tap a chord to hear it. In 'degrees' the keys run one scale step apart from middle C, so no key is dead or doubled; in 'chromatic' they keep their own pitch and are snapped into the scale.")
                }

                // The board: the actual keys of the keyboard, laid out one
                // octave of the scale per row, each labelled with the key you
                // press and the chord it plays. The most useful thing this
                // page can show — it turns "major, 4 notes" into a picture of
                // Cmaj7 Dm7 Em7 Fmaj7 G7 Am7 Bm7b5 sitting under your hand.
                Item {
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
            }

            // ---- user mode: the twelve slots ------------------------------
            Card {
                title: "User chord set"
                cardWidth: panels.contentWidth
                visible: root.mode === 2

                Label {
                    width: parent.width
                    visible: !Synth.chordAvailable
                    wrapMode: Text.WordWrap
                    opacity: 0.6
                    color: Material.foreground
                    text: Synth.connected
                          ? Tr.t("This firmware has no user chord set.")
                          : Tr.t("Not connected")
                }

                Repeater {
                    model: Synth.chordAvailable ? Synth.chordSet : []
                    delegate: RowLayout {
                        id: slotRow
                        required property chordSlot modelData
                        required property int index
                        width: parent ? parent.width : 0
                        spacing: 8

                        // Slot 0 is the root itself, so the label is the key
                        // relative to chord.root rather than an absolute note.
                        Label {
                            Layout.preferredWidth: 36
                            text: root.pitchNames[slotRow.index % 12]
                            font.bold: true
                            color: Material.foreground
                        }

                        ComboBox {
                            id: qualityBox
                            Layout.preferredWidth: 140
                            // Index 0 is "silent" and the qualities follow, so
                            // a quality sits one past its chord.type number.
                            model: root.slotChoices
                            // The slot's real label rather than the row the
                            // list happens to be on: an interval list that
                            // matches no standard quality shows as its numbers,
                            // which is the honest answer and one the list
                            // cannot contain.
                            displayText: slotRow.modelData.label
                            onActivated: slotRow.applyQuality(qualityBox.currentIndex)
                        }

                        function applyQuality(choice: int): void {
                            if (choice <= 0)
                                Synth.clearChordSlot(slotRow.index)
                            else
                                Synth.setChordSlotQuality(slotRow.index, choice - 1,
                                                          slotRow.modelData.transpose)
                        }

                        Label {
                            text: Tr.t("Transpose")
                            opacity: 0.7
                            color: Material.foreground
                        }
                        SpinBox {
                            id: transposeBox
                            from: -24
                            to: 24
                            value: slotRow.modelData.transpose
                            editable: true
                            onValueModified: Synth.setChordSlot(
                                slotRow.index, transposeBox.value,
                                slotRow.modelData.intervals)
                        }

                        Item { Layout.fillWidth: true }

                        Button {
                            text: Tr.t("Try")
                            flat: true
                            enabled: Synth.connected && !slotRow.modelData.silent
                            onPressed: Synth.noteOn(60 + slotRow.index, 100)
                            onReleased: Synth.noteOff(60 + slotRow.index)
                            onCanceled: Synth.noteOff(60 + slotRow.index)
                        }
                    }
                }
            }

            // ---- these three apply in every mode --------------------------
            Card {
                title: "Voicing"
                cardWidth: panels.contentWidth

                PadRow {
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
                }

                PadRow {
                    width: parent.width
                    paramId: root.pidRestrike
                    label: "When a setting changes"
                    // Two pads rather than a switch: neither answer is the
                    // "off" one, and a switch would have to pick a label that
                    // names only half of what it does.
                    options: root.restrikeLabels
                }

                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    opacity: 0.6
                    font.pointSize: UI.fontSize * 0.85
                    color: Material.foreground
                    text: Tr.t("Every setting on this page takes effect on the chord you are already holding, not just the next one — the row above decides whether you hear only the notes that changed, or the whole chord played again. A change that moves no note is silent either way. Voice-leading picks the inversion nearest the chord you just played, so a progression moves by the shortest path — it overrides the inversion setting while it is on.")
                }
            }

            Card {
                title: "Strum"
                cardWidth: panels.contentWidth

                Flow {
                    width: parent.width
                    spacing: 10
                    ParamControl { paramId: root.pidStrum }
                    ParamControl { paramId: root.pidStrumDir }
                }

                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    opacity: 0.6
                    font.pointSize: UI.fontSize * 0.85
                    color: Material.foreground
                    text: Tr.t("Milliseconds between the notes of a chord. At 0 they all start together.")
                }
            }

            Card {
                title: "Routing"
                cardWidth: panels.contentWidth

                Flow {
                    width: parent.width
                    spacing: 10
                    ParamControl { paramId: root.pidRoute }
                    ParamControl { paramId: root.pidKeys }
                }

                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    opacity: 0.6
                    font.pointSize: UI.fontSize * 0.85
                    color: Material.foreground
                    text: Tr.t("Pre-arp: one key gives a running arpeggio of the chord. Post-arp: each note the arpeggiator plays comes out as a block chord. Mono releases the previous key's chord when a new key lands, which is what keeps a chord inside eight voices.")
                }
            }

            Card {
                cardWidth: panels.contentWidth

                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    opacity: 0.6
                    font.pointSize: UI.fontSize * 0.85
                    color: Material.foreground
                    text: Tr.t("Chord mode is a performance setting: it survives a power cycle, and loading a preset never changes it. Sequencer tracks are chorded one at a time — the switch is on the track sheet of the Sequencer page.")
                }
            }
        }
    }

    Label {
        anchors.centerIn: parent
        visible: !root.available
        text: Synth.connected ? Tr.t("Discovering parameters…")
                              : Tr.t("Not connected")
        opacity: 0.5
        color: Material.foreground
    }
}
