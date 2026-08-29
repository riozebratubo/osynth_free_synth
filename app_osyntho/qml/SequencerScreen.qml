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
    property trackConfig cfg
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
    function drumNameFor(note: int): string {
        const n = kitNoteMap[note]
        return n !== undefined ? n : ""
    }

    // The pick that applies to this lane: the pads' choice on a lane whose
    // drum comes from the note, the keyboard's everywhere else.
    readonly property int paintNote: noteToSlotLane ? UI.drumNote : UI.paintNote
    readonly property int stepCount: cfg.length
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
        function onPaintNoteSuggested(note: int): void { UI.paintDrumNote = note }
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
                    text: Tr.t("BPM")
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
                    text: Tr.t("Pattern")
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

            // Copy another pattern over this one — the fastest way to make a
            // variation, which is nearly always "the last one, but with X
            // changed". The firmware does the copy in place (OP_SEQ_EDIT), so
            // no pattern data travels over the link for it.
            Button {
                text: Tr.t("Copy…")
                flat: true
                enabled: Synth.seqPatterns > 1
                onClicked: copyPatternDialog.open()
            }

            Button {
                text: Tr.t("Fill")
                // Momentary: fill is a gesture, not a mode — so setParamNow,
                // not setParam. The batched write keeps only the last value an
                // id was given inside its ~40 ms window, and a press and its
                // release are two values of this one id: a tap quicker than
                // that window sent the release alone and the synth never saw
                // the fill at all.
                onPressedChanged: if (root.pidFill > 0) Synth.setParamNow(root.pidFill, pressed ? 1 : 0)
            }
            // The firmware owns these two as well — a loaded set carries
            // seq.song and seq.countin — so they follow the parameter rather
            // than the last press. See SyncedButton.qml.
            SyncedButton {
                text: Tr.t("Song")
                modelChecked: songVal.on
                onToggled: if (root.pidSong > 0) Synth.setParam(root.pidSong, checked ? 1 : 0)
            }
            Button {
                text: Tr.t("Track…")
                flat: true
                onClicked: trackSheet.open()
            }
            // Whole-sequencer save/load, the counterpart of the looper's
            // set slots: patterns, song chain and arrangement parameters in
            // one file on the synth. Behind a button because it is a
            // between-takes action, and the grid needs the vertical space.
            Button {
                text: Tr.t("Set…")
                flat: true
                visible: seqSetDialog.available
                onClicked: seqSetDialog.open()
            }

            // Four clicked beats before the first step, so you can come in on
            // time. The firmware owns the timing — it counts on the same clock
            // that runs the sequencer, which the app could not do accurately
            // over BLE.
            SyncedButton {
                text: Tr.t("Count-in")
                visible: root.pidCountIn > 0
                modelChecked: countInVal.on
                onToggled: Synth.setParam(root.pidCountIn, checked ? 1 : 0)
            }

            // The note a tap on an empty step writes. Shown here because it is
            // set from another surface entirely (right-click / long-press a
            // key or pad), and an invisible mode is a confusing one.
            Row {
                spacing: 4
                visible: !root.isDrumTrack || root.cfg.noteToSlot === true
                Label {
                    text: Tr.t("Paint")
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
                          : Synth.noteName(root.paintNote) + " — " + Tr.t("no drum")
                    color: (root.noteToSlotLane && drum === "") ? "#FF5252"
                                                                : Material.accent
                    font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }
                Label {
                    text: Tr.t("(right-click or hold a key)")
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
                // A Column, so each track is a number with its mute directly
                // underneath: the mutes form their own row under the track
                // switches and can be read across in one glance, which is the
                // whole point of a mute row. Interleaved in a Row — "1 M 2 M
                // 3 M" — eight tracks read as sixteen unrelated buttons.
                //
                // One Column per track rather than two Repeaters in two Flows,
                // because the pair has to stay together: two independent rows
                // come apart at the first wrap on a narrow phone, and at any
                // width difference between a number and an M.
                delegate: Column {
                    id: trackCell
                    required property int index
                    spacing: 2

                    // Resolved imperatively, like the page-level ids above
                    // and for the same reason: paramIdForName() has no change
                    // signal, so as a binding it answers once, when the
                    // delegate is created, and keeps that answer forever.
                    //
                    // It used to be a binding, on the reasoning that delegates
                    // only exist once Synth.seqTracks has content — i.e. after
                    // SEQ_INFO, which finishDiscovery() asks for, by which
                    // point every parameter's metadata is in. That last step
                    // does not hold: finishDiscovery() is also reached when
                    // the discovery budget expires with ids still unresolved
                    // (kDiscoveryBudgetMs, synthcontroller.cpp), and
                    // trk*.mute sits at the top of the id space (0x0430+), so
                    // it is among the last the metadata pump reaches and the
                    // first to be left out. The name then resolved to -1, the
                    // M button's `visible` latched false, and the top-up that
                    // filled the metadata in seconds later changed nothing —
                    // mute was unreachable for the rest of the session, from
                    // the long-press too, since it shares this guard.
                    //
                    // paramsDiscovered is the signal to re-ask: every newly
                    // known PARAM_INFO schedules one, top-ups included.
                    property int pidMute: -1
                    function refreshPid(): void {
                        pidMute = Synth.paramIdForName(
                            "trk" + (trackCell.index + 1) + ".mute")
                    }
                    Component.onCompleted: refreshPid()
                    // Non-visual, so the Column does not position it.
                    property Connections _pidConn: Connections {
                        target: Synth
                        function onParamsDiscovered() { trackCell.refreshPid() }
                    }

                    ParamValue { id: muteVal; paramId: trackCell.pidMute }
                    readonly property bool muted: muteVal.on

                    // Both buttons take the wider of the two, so the stack is
                    // square and every mute sits exactly under its number.
                    // Reading implicitWidth to set width is not a loop: a
                    // Button's implicit size comes from its label, which never
                    // reads the width back.
                    readonly property real cellWidth:
                        Math.max(trackBtn.implicitWidth, muteBtn.implicitWidth)

                    Button {
                        id: trackBtn
                        width: trackCell.cellWidth
                        text: trackCell.index + 1
                        highlighted: Synth.editTrack === trackCell.index
                        // Dimmed rather than marked: the M under it now says
                        // *which* state this is, so a second glyph in here only
                        // competed with it for a very small button.
                        opacity: trackCell.muted ? 0.45 : 1.0
                        padding: 8
                        onClicked: Synth.editTrack = trackCell.index
                        // The long-press survives the button under it: it is
                        // the fastest gesture on a phone, where the M is a
                        // small target, and it costs nothing to keep.
                        // >= 0, matching the button under it: paramIdForName
                        // answers -1 for "no such name", and 0 is a real id.
                        onPressAndHold: if (trackCell.pidMute >= 0)
                                            Synth.setParam(trackCell.pidMute,
                                                           trackCell.muted ? 0 : 1)
                    }

                    // Mute. Not a volume control and never was: the firmware
                    // gates the whole trigger branch on this parameter
                    // (seq_play.cpp, track_audible in fire_step), so a muted
                    // lane generates no note-ons and no drum hits at all —
                    // there is no per-track audio path to attenuate, since
                    // every lane feeds one engine and one voice bus.
                    //
                    // Synced, not plain-checkable: the firmware owns this
                    // parameter and moves it on its own (a loaded sequence or
                    // set republishes every track's mute from the pattern), so
                    // a `checked` the button assigned itself would go stale.
                    // See SyncedButton.qml.
                    SyncedButton {
                        id: muteBtn
                        width: trackCell.cellWidth
                        text: "M"
                        visible: trackCell.pidMute >= 0
                        padding: 8
                        font.bold: true
                        // Red rather than the accent: mute is the one state
                        // here that means "this lane is not playing", and it
                        // has to read as different from the blue selection
                        // highlight on the button it sits under.
                        Material.accent: "#FF5252"
                        // Filled when muted. The default checked Button is a
                        // very quiet change, which is not enough for a state
                        // you need to read across eight of these mid-take.
                        highlighted: checked
                        modelChecked: trackCell.muted
                        onToggled: Synth.setParam(trackCell.pidMute, checked ? 1 : 0)
                    }
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
                    text: Tr.t("Level")
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
                        const v = root.cfg.velScale
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

        // ---- board size ----------------------------------------------------
        // The step count, on the page rather than only inside the Track sheet
        // — the same reason the Level slider above moved out. The sheet keeps
        // its free 1..256 spinbox for the odd lengths (12, 24) no button here
        // covers; these six are the ones you reach for while playing, and a
        // live edit can only afford one tap.
        //
        // Length is per track, so this resizes the *selected* lane alone: that
        // is what makes polymeter possible, and what the sheet has always done.
        Flow {
            Layout.fillWidth: true
            spacing: 4

            // The label is wrapped rather than sitting in the Flow directly:
            // a Flow positions both axes, so it refuses to lay out at all if
            // any direct child carries an anchor ("Flow will not function") —
            // which stacked every button at the origin. A Row only owns x, so
            // a vertical anchor inside one is fine. Same shape as the Paint
            // and Level rows above.
            Row {
                Label {
                    text: Tr.t("Steps")
                    color: Material.foreground
                    opacity: 0.7
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            Repeater {
                model: [8, 16, 32, 64, 128, 256]
                delegate: Button {
                    required property int modelData
                    // seqMaxSteps is 0 until SEQ_INFO arrives; every current
                    // build reports 256 (SEQ_MAX_STEPS is 256 on the classic
                    // ESP32 as well as the S3 — only tracks and patterns
                    // shrink there), and hiding the whole row until connect
                    // would make it jump into place afterwards.
                    readonly property int cap: Synth.seqMaxSteps > 0 ? Synth.seqMaxSteps : 256
                    visible: modelData <= cap
                    text: modelData
                    // Nothing is highlighted at a length no button names — 12
                    // from the Euclid generator, say. That is the honest
                    // reading of the track, not a missing state.
                    highlighted: root.stepCount === modelData
                    flat: !highlighted
                    padding: 8
                    // Shrinking does not destroy the steps past the cut: the
                    // firmware only narrows what it plays, so tapping back up
                    // brings them back. They go for good when the pattern is
                    // saved, which writes `length` steps and no more.
                    onClicked: Synth.setTrackField("length", modelData)
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
                  ? Tr.t("Click a step to place it · right-click one to edit it")
                  : Tr.t("Tap a step to place it · hold one to edit it")
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
                        text: Tr.t("Step") + " " + (root.selectedStep + 1)
                             + (root.sel.filled ? "" : "  " + Tr.t("(empty)"))
                        font.bold: true
                        color: Material.foreground
                        Layout.fillWidth: true
                    }
                    Button {
                        text: Tr.t("Lock…")
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
                        label: Tr.t("Note")
                        from: 0; to: 127
                        suffix: (root.isDrumTrack && root.cfg.noteToSlot !== true)
                                ? Tr.t("(lane plays a fixed slot)")
                                : Synth.noteName(root.sel.note !== undefined ? root.sel.note : 60)
                        value: root.sel.note !== undefined ? root.sel.note : 60
                        onEdited: (v) => {
                            Synth.setStepField(root.selectedStep, "note", v)
                            if (root.noteToSlotLane) UI.paintDrumNote = v
                            else if (!root.isDrumTrack) UI.paintNote = v
                        }
                    }
                    StepField {
                        label: Tr.t("Velocity")
                        from: 1; to: 127
                        value: root.sel.vel !== undefined ? root.sel.vel : 100
                        onEdited: (v) => Synth.setStepField(root.selectedStep, "vel", v)
                    }
                    // How long the note is held, counted in steps — the same
                    // unit as the grid's squares and as the track's own
                    // Length, which is what you are thinking in when you want
                    // a note to ring on over the next square. The firmware
                    // stores the gate in 1/16 of a step; that encoding is
                    // converted here and is not visible anywhere else.
                    //
                    // Whole steps only. The byte can express a fraction of a
                    // step (a staccato gate), but a spinner that walks in
                    // sixteenths of a square reads as broken next to a grid
                    // whose unit is the square — and nothing has asked for
                    // one yet. If something does, it belongs in this
                    // conversion, not in StepField.
                    //
                    // 16 steps maps to 255, not 256, which the byte cannot
                    // hold: the firmware calls that "a tie across the bar",
                    // landing the note-off a hair before the next bar instead
                    // of on top of it. Math.round carries 255 back to 16, so
                    // the value round-trips. A gate that is not a multiple of
                    // 16 — written by an older build, or by the firmware
                    // itself — shows as the nearest whole step and is left
                    // alone until the field is actually edited.
                    //
                    // A long note only rings over the steps that follow it
                    // while they are empty: a lane is monophonic, so the next
                    // trig on the same track cuts it short.
                    StepField {
                        label: Tr.t("Length")
                        from: 1; to: 16
                        suffix: Tr.t("steps")
                        value: Math.max(1, Math.round(
                                   (root.sel.gate !== undefined ? root.sel.gate : 16) / 16))
                        onEdited: (v) => Synth.setStepField(root.selectedStep, "gate",
                                                            Math.min(255, v * 16))
                    }
                    StepField {
                        label: Tr.t("Probability")
                        from: 0; to: 100
                        suffix: "%"
                        value: root.sel.prob !== undefined ? root.sel.prob : 100
                        onEdited: (v) => Synth.setStepField(root.selectedStep, "prob", v)
                    }
                    StepField {
                        label: Tr.t("Micro-timing")
                        from: -50; to: 50
                        suffix: "%"
                        value: root.sel.micro !== undefined ? root.sel.micro : 0
                        onEdited: (v) => Synth.setStepField(root.selectedStep, "micro", v)
                    }
                    StepField {
                        label: Tr.t("Ratchet")
                        from: 1; to: 8
                        value: root.sel.ratchet !== undefined ? root.sel.ratchet : 1
                        onEdited: (v) => Synth.setStepField(root.selectedStep, "ratchet", v)
                    }

                    ColumnLayout {
                        spacing: 0
                        Label {
                            text: Tr.t("Condition")
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

                    // SyncedButton, not Button: these three are the same three
                    // controls for every step, so a plain `checked` binding —
                    // which the first press replaces — left them showing the
                    // *previous* step's flags after the selection moved, and
                    // the next press then wrote the inverse of that stale value
                    // to the newly selected step. See SyncedButton.qml.
                    SyncedButton {
                        text: Tr.t("Accent")
                        modelChecked: root.sel.accent === true
                        onToggled: Synth.setStepField(root.selectedStep, "accent", checked ? 1 : 0)
                    }
                    SyncedButton {
                        text: Tr.t("Slide")
                        visible: !root.isDrumTrack
                        modelChecked: root.sel.slide === true
                        onToggled: Synth.setStepField(root.selectedStep, "slide", checked ? 1 : 0)
                    }
                    SyncedButton {
                        text: Tr.t("Mute step")
                        modelChecked: root.sel.muted === true
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
        text: !Synth.connected ? Tr.t("Not connected")
              : !Synth.ready ? Tr.t("Discovering parameters…")
              : Tr.t("This firmware has no sequencer, or its pattern store could not be allocated.")
    }

    TrackSheet { id: trackSheet }
    PlockDialog { id: plockDialog }
    SeqSetDialog { id: seqSetDialog }

    // Copy a pattern onto the one being edited. Destructive and not undoable —
    // the firmware overwrites in place — so it asks first, and the dialog is
    // also where the source is picked rather than adding a second control to a
    // toolbar that is already full.
    Dialog {
        id: copyPatternDialog

        // The previous pattern, because a variation is nearly always of the one
        // you just made. On the first pattern there is no previous, so the next
        // one stands in — anything but the destination, which would be a no-op.
        function defaultSource() {
            const here = Synth.editPattern
            const last = Math.max(0, Synth.seqPatterns - 1)
            return here > 0 ? here - 1 : Math.min(1, last)
        }

        title: Tr.t("Copy pattern")
        modal: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(parent ? parent.width - 32 : 380, 420)
        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: srcBox.value = defaultSource() + 1
        onAccepted: {
            const src = srcBox.value - 1
            if (src !== Synth.editPattern) {
                Synth.copyPattern(src, Synth.editPattern)
            }
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label {
                    text: Tr.t("Copy from pattern")
                    color: Material.foreground
                }
                SpinBox {
                    id: srcBox
                    from: 1
                    to: Math.max(1, Synth.seqPatterns)
                    value: copyPatternDialog.defaultSource() + 1
                }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Material.foreground
                text: Tr.ts("Everything in pattern %1 — every track's steps, "
                           + "configuration and parameter locks — is replaced "
                           + "by pattern %2. This cannot be undone.",
                           Synth.editPattern + 1, srcBox.value)
            }

            // The one input this dialog can be given that does nothing at all.
            // Better said than silently ignored on OK.
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                visible: srcBox.value - 1 === Synth.editPattern
                color: "#FF5252"
                text: Tr.t("That is the pattern being edited — pick another one.")
            }
        }
    }
}
