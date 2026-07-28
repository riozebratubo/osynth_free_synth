import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// The multitrack step sequencer (firmware S23).
//
// One column: transport, track selector, the step grid, then an inspector for
// the selected step. The grid is the centre of gravity — everything else is
// sized to leave it room, because on a phone the grid is the only part you
// touch while playing.
//
// The controller owns all pattern state; this file caches nothing but the
// snapshot it is drawing. Taps go straight to Synth.* and the view rebuilds
// from stepsChanged, which keeps the display honest when the firmware edits a
// pattern by itself (a loaded sequence slot, a song-chain pattern change).
Item {
    id: root

    // Which step the inspector is editing; -1 = none.
    property int selectedStep: -1
    // Note written into an empty step. It lives on the UI singleton because
    // the surfaces that pick it — the on-screen keyboard and the drum pads —
    // are siblings of the SwipeView and cannot reach into this page.
    // Right-click (desktop) or long-press (touch) a key or pad to set it.

    property var stepData: []
    property var cfg: ({})
    readonly property bool isDrumTrack: cfg.target === 1
    // A drum lane whose slot is chosen by each step's note, rather than fixed.
    readonly property bool noteToSlotLane: isDrumTrack && cfg.noteToSlot === true

    // note -> drum name for the current kit. Built once per kit change rather
    // than scanned per cell, and reading Synth.kitSlots (a notifying property)
    // is what makes every binding below re-evaluate when the kit is swapped —
    // an invokable call would not create that dependency.
    readonly property var kitNoteMap: {
        var m = ({})
        var slots = Synth.kitSlots
        for (var i = 0; i < slots.length; ++i) m[slots[i].note] = slots[i].name
        return m
    }
    function drumNameFor(note) {
        const n = kitNoteMap[note]
        return n !== undefined ? n : ""
    }

    // The pick that applies to this lane: the pads' choice on a lane whose
    // drum comes from the note, the keyboard's everywhere else.
    readonly property int paintNote: noteToSlotLane ? UI.drumNote : UI.paintNote
    readonly property int stepCount: cfg.length !== undefined ? cfg.length : 64
    readonly property var sel: (selectedStep >= 0 && selectedStep < stepData.length)
                               ? stepData[selectedStep] : ({})

    // Locks on the selected step. Held as a property rather than read straight
    // into the Repeater's model, because plocksForStep() is a plain invokable
    // with no change signal: bound directly it re-evaluated only when
    // selectedStep moved, so adding a lock in PlockDialog or deleting one from
    // the list below left the list showing its previous contents.
    property var stepPlocks: []
    function reloadPlocks() {
        stepPlocks = selectedStep >= 0 ? Synth.plocksForStep(selectedStep) : []
    }
    onSelectedStepChanged: reloadPlocks()

    function reload() {
        cfg = Synth.trackConfig()
        stepData = Synth.steps()
        if (selectedStep >= stepData.length) selectedStep = -1
        reloadPlocks()
    }

    Connections {
        target: Synth
        function onStepsChanged() { root.reload() }
        function onTrackConfigChanged() { root.reload() }
        function onEditTargetChanged() { root.selectedStep = -1; root.reload() }
        function onSeqInfoChanged() { root.reload() }
        function onPlocksChanged() { root.reload() }
        function onReadyChanged() { if (Synth.ready) Synth.refreshSequencer() }
        function onParamsDiscovered() { root.refreshIds() }
        // Switching a drum lane to "from step note" makes the note meaningful;
        // the controller suggests the lane's outgoing drum so placement keeps
        // going with it instead of the melodic default, which maps to nothing.
        function onPaintNoteSuggested(note) { UI.paintDrumNote = note }
    }
    Component.onCompleted: {
        refreshIds()
        if (Synth.ready) Synth.refreshSequencer()
        reload()
    }

    // Transport param ids. Resolved imperatively (paramIdForName has no
    // change signal) so they update when discovery completes; as bindings they
    // would evaluate once, before the synth is connected, and stay at -1.
    property int pidMode: -1
    property int pidTempo: -1
    property int pidPattern: -1
    property int pidFill: -1
    property int pidSong: -1
    property int pidCountIn: -1
    // Reactive reads of the transport state (see ParamValue.qml).
    ParamValue { id: modeVal;    paramId: root.pidMode }
    ParamValue { id: tempoVal;   paramId: root.pidTempo; fallback: 120 }
    ParamValue { id: songVal;    paramId: root.pidSong }
    ParamValue { id: countInVal; paramId: root.pidCountIn }

    function refreshIds() {
        pidMode = Synth.paramIdForName("seq.mode")
        pidTempo = Synth.paramIdForName("seq.tempo")
        pidPattern = Synth.paramIdForName("seq.pattern")
        pidFill = Synth.paramIdForName("seq.fill")
        pidSong = Synth.paramIdForName("seq.song")
        pidCountIn = Synth.paramIdForName("seq.countin")
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8
        visible: Synth.ready && Synth.seqAvailable

        // ---- transport ---------------------------------------------------
        Flow {
            Layout.fillWidth: true
            spacing: 6

            Button {
                text: "\uf04b"  // play
                font.family: App.fontAwesomeName
                font.weight: Font.Black
                highlighted: Synth.playing
                onClicked: Synth.transport(1)
            }
            Button {
                text: "\uf04d"  // stop
                font.family: App.fontAwesomeName
                font.weight: Font.Black
                onClicked: Synth.transport(0)
            }
            Button {
                text: "\uf111"  // circle: record arms live/step input
                font.family: App.fontAwesomeName
                font.weight: Font.Black
                highlighted: modeVal.asInt === 2
                onClicked: Synth.transport(2)
            }

            Row {
                spacing: 4
                Label {
                    text: t.t("BPM")
                    color: Material.foreground
                    anchors.verticalCenter: parent.verticalCenter
                }
                SpinBox {
                    id: tempoBox
                    from: 30
                    to: 300
                    value: tempoVal.asInt
                    onValueModified: if (root.pidTempo > 0) Synth.setParam(root.pidTempo, value)
                    // A user edit assigns `value`, which replaces the binding
                    // above for good; push later synth-side changes in by hand
                    // or the field stops following a preset load, the Arp
                    // page's tempo field or MIDI clock. Same fix as
                    // StepField.qml and ArpSeqScreen.qml.
                    Connections {
                        target: tempoVal
                        function onValueChanged() { tempoBox.value = tempoVal.asInt }
                    }
                }
            }

            Row {
                spacing: 4
                Label {
                    text: t.t("Pattern")
                    color: Material.foreground
                    anchors.verticalCenter: parent.verticalCenter
                }
                SpinBox {
                    id: patternBox
                    from: 1
                    to: Math.max(1, Synth.seqPatterns)
                    value: Synth.editPattern + 1
                    onValueModified: {
                        Synth.editPattern = value - 1
                        // Also move playback there; while the transport runs
                        // the firmware queues the switch to the next bar.
                        if (root.pidPattern > 0) Synth.setParam(root.pidPattern, value - 1)
                    }
                    // Re-assert after a user edit breaks the binding, so the
                    // box keeps following editPattern when the firmware moves
                    // it (a song chain advancing, a loaded set).
                    Connections {
                        target: Synth
                        function onEditTargetChanged() {
                            patternBox.value = Synth.editPattern + 1
                        }
                    }
                }
            }

            Button {
                text: t.t("Fill")
                // Momentary: fill is a gesture, not a mode.
                onPressedChanged: if (root.pidFill > 0) Synth.setParam(root.pidFill, pressed ? 1 : 0)
            }
            Button {
                text: t.t("Song")
                checkable: true
                checked: songVal.on
                onToggled: if (root.pidSong > 0) Synth.setParam(root.pidSong, checked ? 1 : 0)
            }
            Button {
                text: t.t("Track…")
                flat: true
                onClicked: trackSheet.open()
            }
            // Whole-sequencer save/load, the counterpart of the looper's
            // set slots: patterns, song chain and arrangement parameters in
            // one file on the synth. Behind a button because it is a
            // between-takes action, and the grid needs the vertical space.
            Button {
                text: t.t("Set…")
                flat: true
                visible: seqSetDialog.available
                onClicked: seqSetDialog.open()
            }

            // Four clicked beats before the first step, so you can come in on
            // time. The firmware owns the timing — it counts on the same clock
            // that runs the sequencer, which the app could not do accurately
            // over BLE.
            Button {
                text: t.t("Count-in")
                visible: root.pidCountIn > 0
                checkable: true
                checked: countInVal.on
                onToggled: Synth.setParam(root.pidCountIn, checked ? 1 : 0)
            }

            // The note a tap on an empty step writes. Shown here because it is
            // set from another surface entirely (right-click / long-press a
            // key or pad), and an invisible mode is a confusing one.
            Row {
                spacing: 4
                visible: !root.isDrumTrack || root.cfg.noteToSlot === true
                Label {
                    text: t.t("Paint")
                    color: Material.foreground
                    opacity: 0.7
                    anchors.verticalCenter: parent.verticalCenter
                }
                // On a note-picks-the-drum lane, name the drum rather than the
                // note — and say plainly when the picked note maps to nothing,
                // because placing it would write a step that is simply silent.
                Label {
                    readonly property string drum: root.drumNameFor(root.paintNote)
                    text: !root.noteToSlotLane ? Synth.noteName(root.paintNote)
                          : drum !== "" ? drum
                          : Synth.noteName(root.paintNote) + " — " + t.t("no drum")
                    color: (root.noteToSlotLane && drum === "") ? "#FF5252"
                                                                : Material.accent
                    font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }
                Label {
                    text: t.t("(right-click or hold a key)")
                    color: Material.foreground
                    opacity: 0.45
                    font.pointSize: UI.fontSize * 0.7
                    anchors.verticalCenter: parent.verticalCenter
                    visible: UI.desktopLayout
                }
            }
        }

        // ---- track selector ----------------------------------------------
        Flow {
            Layout.fillWidth: true
            spacing: 4

            Repeater {
                model: Synth.seqTracks
                delegate: Button {
                    id: trackBtn
                    required property int index
                    // Safe as bindings, unlike the page-level ids above: a
                    // delegate is only created once its model has content, and
                    // both models (Synth.kitSlots, Synth.seqTracks) are filled
                    // from responses requested at finishDiscovery() — i.e.
                    // after every parameter's metadata is in. Any later model
                    // change recreates the delegates and re-resolves these.
                    readonly property int pidMute: Synth.paramIdForName("trk" + (index + 1) + ".mute")
                    ParamValue { id: muteVal; paramId: trackBtn.pidMute }
                    readonly property bool muted: muteVal.on

                    text: (index + 1) + (muted ? " ✕" : "")
                    highlighted: Synth.editTrack === index
                    opacity: muted ? 0.45 : 1.0
                    padding: 8
                    onClicked: Synth.editTrack = index
                    // Long-press mutes: the fastest live gesture, and it keeps
                    // a second row of mute buttons off a phone screen.
                    onPressAndHold: if (pidMute > 0) Synth.setParam(pidMute, muted ? 0 : 1)
                }
            }

            // Level of the edited track, kept beside the track buttons because
            // it is a mixing control you reach for constantly — it was only in
            // the Track sheet before, where nobody found it.
            //
            // It works by scaling every step's velocity, which is the only
            // per-track level this architecture can offer: all tracks feed one
            // engine and one voice bus, so there is no separate audio path to
            // attenuate. On a drum lane velocity maps linearly to amplitude,
            // so it is exactly a volume; on a synth lane it is velocity, which
            // on some engines also opens the timbre up as it rises.
            Row {
                spacing: 6

                Label {
                    text: t.t("Level")
                    color: Material.foreground
                    opacity: 0.7
                    anchors.verticalCenter: parent.verticalCenter
                }
                Slider {
                    id: levelSlider
                    width: 110
                    anchors.verticalCenter: parent.verticalCenter
                    from: 0
                    to: 200
                    stepSize: 5
                    snapMode: Slider.SnapAlways

                    // Same guard as the master volume: a Slider replaces its
                    // own `value` binding the moment the user drags it, so the
                    // model has to be pushed in by hand afterwards — and that
                    // push must not look like a user edit.
                    property bool syncing: false
                    function sync() {
                        const v = root.cfg.velScale !== undefined ? root.cfg.velScale : 100
                        if (value === v) return
                        syncing = true
                        value = v
                        syncing = false
                    }
                    Component.onCompleted: sync()
                    onMoved: if (!syncing) Synth.setTrackField("velScale", value)

                    Connections {
                        target: root
                        function onCfgChanged() { levelSlider.sync() }
                    }
                }
                Label {
                    text: Math.round(levelSlider.value) + "%"
                    color: Material.foreground
                    font.bold: Math.round(levelSlider.value) !== 100
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }

        // ---- the grid ------------------------------------------------------
        Frame {
            Layout.fillWidth: true
            Layout.fillHeight: true
            padding: 4

            GridView {
                id: grid
                anchors.fill: parent
                clip: true
                // 16 columns keeps bars visually aligned at every usual length.
                readonly property int columns: 16
                cellWidth: Math.floor(width / columns)
                cellHeight: Math.min(cellWidth, 46)
                model: root.stepCount
                cacheBuffer: 4000

                delegate: Item {
                    id: cell
                    required property int index
                    width: grid.cellWidth
                    height: grid.cellHeight

                    readonly property var s: index < root.stepData.length
                                             ? root.stepData[index] : ({})
                    readonly property bool filled: s.filled === true
                    readonly property bool onBeat: index % 4 === 0
                    readonly property string drumName:
                        (filled && root.noteToSlotLane)
                        ? (root.kitNoteMap[s.note] !== undefined
                           ? root.kitNoteMap[s.note] : "") : ""

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 2
                        radius: 3
                        color: cell.filled
                               ? (cell.s.muted ? Qt.darker(Material.accent, 2.0) : Material.accent)
                               : Qt.rgba(Material.foreground.r, Material.foreground.g,
                                         Material.foreground.b, cell.onBeat ? 0.14 : 0.06)
                        // Velocity reads as opacity — the point of a grid is
                        // seeing dynamics without opening anything.
                        opacity: cell.filled ? 0.45 + 0.55 * (cell.s.vel / 127.0) : 1.0

                        border.width: (root.selectedStep === cell.index
                                       || Synth.playhead === cell.index) ? 2 : 0
                        border.color: root.selectedStep === cell.index
                                      ? Material.foreground : "#ffffff"

                        // Corner marks for the per-step features, so a glance
                        // shows which steps carry more than a plain trig.
                        Rectangle {  // parameter lock
                            visible: cell.s.hasPlock === true
                            width: 5; height: 5; radius: 2.5
                            color: "#ffffff"
                            anchors.top: parent.top
                            anchors.left: parent.left
                            anchors.margins: 2
                        }
                        Label {  // ratchet count
                            visible: cell.filled && cell.s.ratchet > 1
                            text: cell.filled ? "x" + cell.s.ratchet : ""
                            font.pointSize: UI.fontSize * 0.5
                            color: "#ffffff"
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.margins: 1
                        }
                        Label {  // conditional trig
                            visible: cell.filled && cell.s.cond > 0
                            text: "?"
                            font.pointSize: UI.fontSize * 0.55
                            font.bold: true
                            color: "#ffffff"
                            anchors.bottom: parent.bottom
                            anchors.right: parent.right
                            anchors.margins: 1
                        }
                        // `visible` does not gate a binding — these evaluate on
                        // every cell, including the empty ones whose `s` is {},
                        // so each has to tolerate an undefined field.
                        Label {  // note name on synth tracks
                            visible: cell.filled && !root.isDrumTrack
                            text: cell.filled ? Synth.noteName(cell.s.note) : ""
                            font.pointSize: UI.fontSize * 0.5
                            color: "#ffffff"
                            anchors.centerIn: parent
                        }
                        // On a lane that picks its drum from the note, the note
                        // decides what you hear — so name the drum, and flag a
                        // note no kit slot answers to, which is silent and
                        // would otherwise look like any other step.
                        Label {
                            visible: cell.filled && root.noteToSlotLane
                            text: cell.filled && root.noteToSlotLane
                                  ? (cell.drumName !== "" ? cell.drumName : "?") : ""
                            font.pointSize: UI.fontSize * 0.45
                            font.bold: cell.drumName === ""
                            color: cell.drumName === "" ? "#FF5252" : "#ffffff"
                            elide: Text.ElideRight
                            width: parent.width - 4
                            horizontalAlignment: Text.AlignHCenter
                            anchors.centerIn: parent
                        }
                        Label {  // probability under 100 %
                            visible: cell.filled && root.isDrumTrack
                                     && !root.noteToSlotLane && cell.s.prob < 100
                            text: cell.filled ? String(cell.s.prob) : ""
                            font.pointSize: UI.fontSize * 0.5
                            color: "#ffffff"
                            anchors.centerIn: parent
                        }
                    }

                    // Faint step number every 4 steps on empty cells.
                    Label {
                        visible: !cell.filled && cell.onBeat
                        text: cell.index + 1
                        font.pointSize: UI.fontSize * 0.5
                        opacity: 0.5
                        color: Material.foreground
                        anchors.centerIn: parent
                    }

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton | Qt.RightButton

                        // Tap / left-click only places or clears the step. The
                        // inspector deliberately does *not* open here: writing
                        // a pattern is the fast path, and a panel appearing
                        // under your finger on every tap fights it.
                        //
                        // Right-click opens the inspector without toggling —
                        // so you can edit a step you do not want to erase.
                        onClicked: (mouse) => {
                            if (mouse.button === Qt.RightButton) {
                                root.selectedStep = cell.index
                            } else {
                                Synth.toggleStep(cell.index, Synth.noteForNewStep(root.paintNote))
                            }
                        }

                        // Touch has no second button; long-press is the same
                        // gesture. Qt suppresses `clicked` once this fires, so
                        // a hold never also toggles the step.
                        onPressAndHold: (mouse) => root.selectedStep = cell.index
                    }
                }
            }
        }

        // Discoverability for the gesture above; disappears once a step is
        // selected, i.e. as soon as it has been learned.
        Label {
            Layout.fillWidth: true
            visible: root.selectedStep < 0
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            opacity: 0.5
            font.pointSize: UI.fontSize * 0.75
            color: Material.foreground
            text: UI.desktopLayout
                  ? t.t("Click a step to place it · right-click one to edit it")
                  : t.t("Tap a step to place it · hold one to edit it")
        }

        // ---- step inspector -------------------------------------------------
        Frame {
            Layout.fillWidth: true
            visible: root.selectedStep >= 0 && root.sel.filled !== undefined
            padding: 6

            ColumnLayout {
                anchors.fill: parent
                spacing: 4

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: t.t("Step") + " " + (root.selectedStep + 1)
                             + (root.sel.filled ? "" : "  " + t.t("(empty)"))
                        font.bold: true
                        color: Material.foreground
                        Layout.fillWidth: true
                    }
                    Button {
                        text: t.t("Lock…")
                        flat: true
                        enabled: root.sel.filled === true
                        onClicked: plockDialog.openFor(root.selectedStep)
                    }
                    ToolButton {
                        text: "\uf00d"  // times
                        font.family: App.fontAwesomeName
                        font.weight: Font.Black
                        onClicked: root.selectedStep = -1
                    }
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 10
                    visible: root.sel.filled === true

                    // Always shown. The firmware stores a note on every step
                    // whatever the lane's target, so editing it is never
                    // destructive — and on a drum lane bound to a fixed slot
                    // the note is simply inert, which the suffix says rather
                    // than the control disappearing.
                    StepField {
                        label: t.t("Note")
                        from: 0; to: 127
                        suffix: (root.isDrumTrack && root.cfg.noteToSlot !== true)
                                ? t.t("(lane plays a fixed slot)")
                                : Synth.noteName(root.sel.note !== undefined ? root.sel.note : 60)
                        value: root.sel.note !== undefined ? root.sel.note : 60
                        onEdited: (v) => {
                            Synth.setStepField(root.selectedStep, "note", v)
                            if (root.noteToSlotLane) UI.paintDrumNote = v
                            else if (!root.isDrumTrack) UI.paintNote = v
                        }
                    }
                    StepField {
                        label: t.t("Velocity")
                        from: 1; to: 127
                        value: root.sel.vel !== undefined ? root.sel.vel : 100
                        onEdited: (v) => Synth.setStepField(root.selectedStep, "vel", v)
                    }
                    StepField {
                        label: t.t("Gate")
                        from: 1; to: 255
                        // 16 = exactly one step; show it in step-lengths.
                        suffix: "(" + ((root.sel.gate !== undefined ? root.sel.gate : 16) / 16.0).toFixed(2) + "x)"
                        value: root.sel.gate !== undefined ? root.sel.gate : 16
                        onEdited: (v) => Synth.setStepField(root.selectedStep, "gate", v)
                    }
                    StepField {
                        label: t.t("Probability")
                        from: 0; to: 100
                        suffix: "%"
                        value: root.sel.prob !== undefined ? root.sel.prob : 100
                        onEdited: (v) => Synth.setStepField(root.selectedStep, "prob", v)
                    }
                    StepField {
                        label: t.t("Micro-timing")
                        from: -50; to: 50
                        suffix: "%"
                        value: root.sel.micro !== undefined ? root.sel.micro : 0
                        onEdited: (v) => Synth.setStepField(root.selectedStep, "micro", v)
                    }
                    StepField {
                        label: t.t("Ratchet")
                        from: 1; to: 8
                        value: root.sel.ratchet !== undefined ? root.sel.ratchet : 1
                        onEdited: (v) => Synth.setStepField(root.selectedStep, "ratchet", v)
                    }

                    ColumnLayout {
                        spacing: 0
                        Label {
                            text: t.t("Condition")
                            font.pointSize: UI.fontSize * 0.7
                            opacity: 0.7
                            color: Material.foreground
                        }
                        SyncedComboBox {
                            model: Synth.condNames()
                            modelIndex: root.sel.cond !== undefined ? root.sel.cond : 0
                            onActivated: Synth.setStepField(root.selectedStep, "cond", currentIndex)
                        }
                    }
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    visible: root.sel.filled === true

                    Button {
                        text: t.t("Accent")
                        checkable: true
                        checked: root.sel.accent === true
                        onToggled: Synth.setStepField(root.selectedStep, "accent", checked ? 1 : 0)
                    }
                    Button {
                        text: t.t("Slide")
                        checkable: true
                        visible: !root.isDrumTrack
                        checked: root.sel.slide === true
                        onToggled: Synth.setStepField(root.selectedStep, "slide", checked ? 1 : 0)
                    }
                    Button {
                        text: t.t("Mute step")
                        checkable: true
                        checked: root.sel.muted === true
                        onToggled: Synth.setStepField(root.selectedStep, "mute", checked ? 1 : 0)
                    }
                }

                // Locks already on this step.
                Repeater {
                    model: root.stepPlocks
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        Label {
                            text: modelData.name + " = " + Number(modelData.value).toFixed(3)
                            font.pointSize: UI.fontSize * 0.8
                            color: Material.foreground
                            Layout.fillWidth: true
                            elide: Label.ElideRight
                        }
                        ToolButton {
                            text: "\uf2ed"  // trash
                            font.family: App.fontAwesomeName
                            font.weight: Font.Black
                            onClicked: Synth.clearPlock(root.selectedStep, modelData.pid)
                        }
                    }
                }
            }
        }
    }

    // ---- empty / unsupported states ---------------------------------------
    Label {
        anchors.centerIn: parent
        visible: !Synth.ready || !Synth.seqAvailable
        width: parent.width - 48
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        opacity: 0.6
        color: Material.foreground
        text: !Synth.connected ? t.t("Not connected")
              : !Synth.ready ? t.t("Discovering parameters…")
              : t.t("This firmware has no sequencer, or its pattern store could not be allocated.")
    }

    TrackSheet { id: trackSheet }
    PlockDialog { id: plockDialog }
    SeqSetDialog { id: seqSetDialog }
}
