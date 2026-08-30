import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// The sample-kit rack (firmware S22 for the bus, S44 for the recorder): kit
// selection, the bus controls, the recorder, and a card per pad.
//
// The file is still called DrumsScreen because the page it draws is still the
// drum bus — what changed in S44 is that the bus grew eight recordable kits
// beside the factory one, so the *page* is "Sample kits" and the tab is "Kit"
// (UI.qml). Renaming the file would have churned the QML module listing and
// the resource paths for no behavioural gain; the name is internal and this
// comment is the map between the two.
//
// Slot parameters are named generically in the firmware (drum1.level ...)
// because kits are swappable; the human names come from the kit itself, so
// every card is labelled from Synth.kitSlots rather than from the parameter
// name. Tapping a pad auditions it.
//
// Two kinds of per-pad control sit on the same card and they are not the same
// kind of thing, which is worth knowing before editing this:
//
//   drumN.level / pan / tune / decay   ordinary parameters. Automatable,
//                                      p-lockable, saved in a preset. Drawn
//                                      with ParamControl like anything else.
//   mode / reverse / start / choke     kit data, sent over OP_KIT_EDIT and
//                                      saved with the kit, not the patch.
//                                      They follow a kit switch; a parameter
//                                      would not. Drawn by hand, because there
//                                      is no parameter behind them to describe
//                                      them.
Item {
    id: root

    // Resolved imperatively (paramIdForName has no change signal): as
    // bindings these evaluate once, before discovery, and every control gated
    // on them would stay hidden for the life of the app.
    property int pidLevel: -1
    property int pidSend: -1
    property int pidChoke: -1
    property int pidMidiCh: -1
    // Metronome level. It belongs on the drum bus because that is what plays
    // it (drums_click), and it had no control anywhere — so turning Count-in
    // on, in the Looper or on the Sequencer page, gave you a click you could
    // not turn down or, if the kit had left it at zero, hear at all.
    property int pidClick: -1

    // The theme accent, captured on an item that does not override it. The
    // record-level meter recolours itself as it approaches clipping, and
    // reading Material.accent inside its own binding is a self-reference: on
    // that item the name resolves to the overridden value, so it loops.
    // Holding the default here gives the binding a source outside itself.
    // Same idiom, and the same reason, as GraphScreen's cost meter.
    readonly property color accentColor: Material.accent

    ParamValue { id: chokeVal; paramId: root.pidChoke }

    // Whether the bound kit accepts recording. The factory kit is flash-mapped
    // and read-only, so the whole recorder half of this page is hidden for it
    // rather than drawn and then refusing — a control that cannot work is
    // worse than one that is not there.
    readonly property bool kitRecordable: {
        if (!UI.samplerAvailable) return false
        const list = Synth.kits
        for (let i = 0; i < list.length; ++i) {
            if (list[i].index === Synth.currentKit) return list[i].user === true
        }
        return false
    }

    readonly property bool canSave: Synth.kitStorage !== "none"

    // Which pads get a card.
    //
    // On the factory kit, only the populated ones: KIT_INFO reports every slot
    // the build compiles in, and a card for an empty one is a blank title over
    // four knobs that drive nothing.
    //
    // On a recordable kit, all of them — an empty pad there is a *destination*,
    // and hiding the sixteen places you can record into would be hiding the
    // feature. Reading Synth.kitSlots (a notifying property) is what makes this
    // re-evaluate when a kit is swapped or a pad is recorded.
    readonly property var visibleSlots: {
        var out = []
        var slots = Synth.kitSlots
        for (var i = 0; i < slots.length; ++i) {
            const filled = slots[i].filled !== undefined ? slots[i].filled
                                                         : slots[i].name !== ""
            if (filled || root.kitRecordable) out.push(slots[i])
        }
        return out
    }

    function refreshIds() {
        pidLevel = Synth.paramIdForName("drums.level")
        pidSend = Synth.paramIdForName("drums.send")
        pidChoke = Synth.paramIdForName("drums.choke")
        pidMidiCh = Synth.paramIdForName("drums.midich")
        pidClick = Synth.paramIdForName("drums.click")
    }

    Connections {
        target: Synth
        function onReadyChanged() { if (Synth.ready) Synth.refreshKit() }
        function onParamsDiscovered() { root.refreshIds() }
    }
    Component.onCompleted: {
        refreshIds()
        if (Synth.ready) Synth.refreshKit()
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: panels.implicitHeight + 24
        clip: true
        visible: Synth.ready

        PanelFlow {
            id: panels
            spacing: 10

            // ---- kit + bus: its own line ---------------------------------
            Frame {
                width: panels.contentWidth
                padding: 8

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Label {
                            text: Tr.t("Kit")
                            color: Material.foreground
                            Layout.alignment: Qt.AlignVCenter
                        }
                        SyncedComboBox {
                            Layout.fillWidth: true
                            model: {
                                var out = []
                                for (var i = 0; i < Synth.kits.length; ++i)
                                    out.push(Synth.kits[i].name)
                                return out
                            }
                            modelIndex: Synth.currentKit
                            enabled: Synth.kits.length > 1
                            onActivated: Synth.selectKit(currentIndex)
                        }
                        // Rename. Only a recordable kit has a name of its own
                        // to change — the factory kit's comes from the image.
                        ToolButton {
                            visible: root.kitRecordable
                            text: "\uf304"  // pen
                            font.family: App.fontAwesomeName
                            font.weight: Font.Black
                            onClicked: renameDialog.open()
                            ToolTip.visible: hovered
                            ToolTip.text: Tr.t("Rename this kit")
                        }
                        ToolButton {
                            text: "\uf021"  // rotate: re-read the kit
                            font.family: App.fontAwesomeName
                            font.weight: Font.Black
                            onClicked: Synth.refreshKit()
                        }
                    }

                    // Where kits are being kept, and what that costs. Shown
                    // rather than assumed: "nowhere" is a real answer on a
                    // board with no card, and the moment to learn it is now,
                    // not after a power cycle takes an afternoon's sampling
                    // with it.
                    RowLayout {
                        Layout.fillWidth: true
                        visible: UI.samplerAvailable
                        spacing: 8

                        Label {
                            color: Material.foreground
                            opacity: 0.65
                            font.pointSize: UI.fontSize * 0.75
                            text: {
                                const free = Math.round(UI.samplerFreeKb)
                                const where = Synth.kitStorage === "sd"
                                        ? Tr.t("Saving to SD card")
                                        : Synth.kitStorage === "lfs"
                                          ? Tr.t("Saving to internal flash (small)")
                                          : Tr.t("No storage — kits are lost at power off")
                                return where + "  ·  " + free + " KB " + Tr.t("free")
                            }
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            visible: root.kitRecordable && root.canSave
                            text: Tr.t("Save kit")
                            onClicked: UI.setSmp("smp.save", 1)
                        }
                    }

                    Flow {
                        Layout.fillWidth: true
                        spacing: 12

                        ParamControl {
                            visible: root.pidLevel > 0
                            paramId: root.pidLevel
                        }
                        ParamControl {
                            visible: root.pidSend > 0
                            paramId: root.pidSend
                        }
                        ParamControl {
                            visible: root.pidClick > 0
                            paramId: root.pidClick
                        }
                        ParamControl {
                            visible: root.pidMidiCh > 0
                            paramId: root.pidMidiCh
                        }
                        // SyncedButton: a press replaces a plain `checked`
                        // binding, after which a preset load moving drums.choke
                        // no longer moves the button. See SyncedButton.qml.
                        SyncedButton {
                            text: Tr.t("Choke groups")
                            modelChecked: chokeVal.on
                            onToggled: if (root.pidChoke > 0) Synth.setParam(root.pidChoke, checked ? 1 : 0)
                        }
                    }
                }
            }

            // ---- the recorder --------------------------------------------
            Frame {
                width: panels.contentWidth
                visible: root.kitRecordable
                padding: 8

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    // The transport row. Record and Erase arm and wait for a
                    // pad — here or on the pad strip at the bottom of the
                    // screen, which shares the armed state through UI.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Button {
                            id: recBtn
                            text: UI.samplerRecording ? Tr.t("Stop")
                                : (UI.padAction === "record" ? Tr.t("Pick a pad")
                                                             : Tr.t("Record"))
                            highlighted: UI.padAction === "record" || UI.samplerRecording
                            Material.background: UI.samplerRecording ? "#c62828"
                                               : (UI.padAction === "record" ? "#ef6c00"
                                                                            : undefined)
                            onClicked: {
                                if (UI.samplerRecording) { UI.stopRecord(); return }
                                UI.padAction = UI.padAction === "record" ? "" : "record"
                            }
                        }
                        Button {
                            text: UI.padAction === "erase" ? Tr.t("Pick a pad")
                                                           : Tr.t("Erase")
                            highlighted: UI.padAction === "erase"
                            Material.background: UI.padAction === "erase" ? "#ef6c00"
                                                                          : undefined
                            enabled: !UI.samplerRecording
                            onClicked: UI.padAction = UI.padAction === "erase" ? "" : "erase"
                        }
                        Button {
                            text: Tr.t("Undo")
                            enabled: !UI.samplerRecording
                            onClicked: { UI.setSmp("smp.undo", 1); UI.padAction = "" }
                            ToolTip.visible: hovered
                            ToolTip.text: Tr.t("Put back whatever the last record, erase or copy replaced")
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            color: Material.foreground
                            opacity: 0.7
                            font.pointSize: UI.fontSize * 0.75
                            text: {
                                switch (UI.samplerState) {
                                case 1: return Tr.t("Armed")
                                case 2: return Tr.t("Waiting for sound…")
                                case 3: return Tr.t("Recording")
                                case 4: return Tr.t("Saving…")
                                default: return ""
                                }
                            }
                        }
                    }

                    // Level and length while a take runs. Both come from the
                    // firmware — see UI.samplerState on why nothing here is
                    // inferred from what the app last sent.
                    ProgressBar {
                        Layout.fillWidth: true
                        visible: UI.samplerRecording
                        from: 0; to: 1
                        value: UI.samplerPos
                    }
                    ProgressBar {
                        Layout.fillWidth: true
                        visible: UI.samplerRecording
                        from: 0; to: 1
                        value: UI.samplerPeak
                        Material.accent: UI.samplerPeak > 0.98 ? "#c62828"
                                       : (UI.samplerPeak > 0.8 ? "#ef6c00"
                                                               : root.accentColor)
                    }

                    Flow {
                        Layout.fillWidth: true
                        spacing: 12

                        ParamControl { paramId: Synth.paramIdForName("smp.src") }
                        ParamControl { paramId: Synth.paramIdForName("smp.gain") }
                        ParamControl { paramId: Synth.paramIdForName("smp.thresh") }
                        ParamControl { paramId: Synth.paramIdForName("smp.preroll") }
                        ParamControl { paramId: Synth.paramIdForName("smp.maxsec") }
                        ParamControl { paramId: Synth.paramIdForName("smp.countin") }
                        ParamControl { paramId: Synth.paramIdForName("smp.slices") }
                        ParamControl { paramId: Synth.paramIdForName("smp.slicemode") }
                        ParamControl { paramId: Synth.paramIdForName("smp.trim") }
                        ParamControl { paramId: Synth.paramIdForName("smp.norm") }
                        ParamControl { paramId: Synth.paramIdForName("smp.monitor") }
                    }
                }
            }

            Label {
                visible: root.visibleSlots.length === 0
                width: panels.contentWidth
                wrapMode: Text.WordWrap
                opacity: 0.6
                color: Material.foreground
                text: Tr.t("This firmware has no drum kit built in. Build one with "
                          + "tools/gen_drumkit.py and reflash, or put a kit on the "
                          + "SD card.")
            }

            // ---- one card per pad, tiled ---------------------------------
            Repeater {
                model: root.visibleSlots

                delegate: Frame {
                    id: strip
                    required property var modelData
                    readonly property int slot: modelData.slot
                    readonly property bool filled: modelData.filled !== undefined
                                                   ? modelData.filled
                                                   : modelData.name !== ""
                    readonly property bool armed: UI.armedPad === strip.slot
                                                  && UI.samplerAvailable
                    // Safe as bindings, unlike the page-level ids above: a
                    // delegate is only created once its model has content, and
                    // both models (Synth.kitSlots, Synth.seqTracks) are filled
                    // from responses requested at finishDiscovery() — i.e.
                    // after every parameter's metadata is in. Any later model
                    // change recreates the delegates and re-resolves these.
                    readonly property int pidSlotLevel: Synth.paramIdForName("drum" + (slot + 1) + ".level")
                    readonly property int pidSlotPan: Synth.paramIdForName("drum" + (slot + 1) + ".pan")
                    readonly property int pidSlotTune: Synth.paramIdForName("drum" + (slot + 1) + ".tune")
                    readonly property int pidSlotDecay: Synth.paramIdForName("drum" + (slot + 1) + ".decay")

                    // Two knobs wide (Knob is 84 + a little air) plus the
                    // frame's own padding, capped at the flow's content width
                    // so a narrow phone gets one card per line rather than a
                    // clipped one.
                    padding: 6
                    width: Math.min(2 * 88 + 2 * padding + 8, panels.contentWidth)

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 6

                        // The pad: name, MIDI note or length, audition on tap.
                        // While an action is armed the tap performs it instead
                        // of auditioning — you are aiming at a destination, and
                        // hearing what you are about to replace does not help.
                        Button {
                            Layout.fillWidth: true
                            highlighted: strip.armed
                            Material.background: strip.armed
                                    ? (UI.samplerRecording ? "#c62828" : "#ef6c00")
                                    : undefined
                            onPressed: {
                                if (UI.padAction === "record") {
                                    UI.startRecordInto(strip.slot)
                                } else if (UI.padAction === "erase") {
                                    UI.erasePad(strip.slot)
                                } else if (strip.filled) {
                                    Synth.triggerDrum(strip.slot, 100)
                                }
                            }
                            onReleased: {
                                if (UI.samplerRecording && strip.armed) UI.stopRecord()
                                else if (strip.filled) Synth.releaseDrum(strip.slot)
                            }
                            contentItem: Column {
                                spacing: 0
                                Label {
                                    width: parent.width
                                    horizontalAlignment: Text.AlignHCenter
                                    text: strip.filled
                                          ? strip.modelData.name
                                          : Tr.t("empty")
                                    font.bold: true
                                    font.pointSize: UI.fontSize * 0.85
                                    opacity: strip.filled ? 1.0 : 0.5
                                    color: Material.foreground
                                    elide: Label.ElideRight
                                }
                                Label {
                                    width: parent.width
                                    horizontalAlignment: Text.AlignHCenter
                                    // Length is the thing you want on a pad you
                                    // recorded; the note is the thing you want
                                    // on one you are sequencing. Show both,
                                    // length first when there is one.
                                    text: {
                                        const n = Synth.noteName(strip.modelData.note)
                                        if (!strip.filled) return Tr.t("pad") + " " + (strip.slot + 1)
                                        const secs = strip.modelData.seconds
                                        return secs > 0
                                               ? n + "  ·  " + secs.toFixed(2) + " s"
                                               : n
                                    }
                                    font.pointSize: UI.fontSize * 0.65
                                    opacity: 0.6
                                    color: Material.foreground
                                }
                            }
                        }

                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            columnSpacing: 4
                            rowSpacing: 2

                            ParamControl {
                                visible: strip.pidSlotLevel > 0
                                paramId: strip.pidSlotLevel
                            }
                            ParamControl {
                                visible: strip.pidSlotPan > 0
                                paramId: strip.pidSlotPan
                            }
                            ParamControl {
                                visible: strip.pidSlotTune > 0
                                paramId: strip.pidSlotTune
                            }
                            ParamControl {
                                visible: strip.pidSlotDecay > 0
                                paramId: strip.pidSlotDecay
                            }
                        }

                        // ---- kit data, not parameters (S44) --------------
                        //
                        // Only on a recordable kit: the factory kit is
                        // flash-resident and the firmware refuses these edits,
                        // so offering them would be offering a control that
                        // silently forgets.
                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: root.kitRecordable && strip.filled
                            spacing: 2

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 4

                                Label {
                                    text: Tr.t("Play")
                                    font.pointSize: UI.fontSize * 0.7
                                    opacity: 0.7
                                    color: Material.foreground
                                }
                                ComboBox {
                                    Layout.fillWidth: true
                                    font.pointSize: UI.fontSize * 0.7
                                    model: [Tr.t("one-shot"), Tr.t("gate"), Tr.t("loop")]
                                    currentIndex: strip.modelData.mode !== undefined
                                                  ? strip.modelData.mode : 0
                                    onActivated: Synth.setPadField(strip.slot, 0, currentIndex)
                                }
                                // Reverse: a toggle on the kit rather than a
                                // second copy of the audio, so it costs
                                // nothing and can be flipped while playing.
                                ToolButton {
                                    text: "\uf0e2"  // arrow-rotate-left
                                    font.family: App.fontAwesomeName
                                    font.weight: Font.Black
                                    font.pointSize: UI.fontSize * 0.8
                                    highlighted: strip.modelData.reverse === true
                                    opacity: strip.modelData.reverse === true ? 1.0 : 0.45
                                    onClicked: Synth.setPadField(
                                        strip.slot, 1,
                                        strip.modelData.reverse === true ? 0 : 1)
                                    ToolTip.visible: hovered
                                    ToolTip.text: Tr.t("Play this pad backwards")
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 4

                                Label {
                                    text: Tr.t("Start")
                                    font.pointSize: UI.fontSize * 0.7
                                    opacity: 0.7
                                    color: Material.foreground
                                }
                                Slider {
                                    id: startSlider
                                    Layout.fillWidth: true
                                    from: 0; to: 0.999
                                    // Not a two-way binding: the firmware
                                    // clamps, and a slider that wrote and then
                                    // read its own write would fight it. The
                                    // value follows the kit; dragging sends.
                                    value: strip.modelData.start !== undefined
                                           ? strip.modelData.start : 0
                                    onMoved: Synth.setPadField(strip.slot, 2, value)
                                }
                                Label {
                                    text: Tr.t("Choke")
                                    font.pointSize: UI.fontSize * 0.7
                                    opacity: 0.7
                                    color: Material.foreground
                                }
                                SpinBox {
                                    from: 0; to: 7
                                    font.pointSize: UI.fontSize * 0.7
                                    implicitWidth: 96
                                    value: strip.modelData.choke !== undefined
                                           ? strip.modelData.choke : 0
                                    onValueModified: Synth.setPadField(strip.slot, 3, value)
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 4

                                Button {
                                    Layout.fillWidth: true
                                    text: Tr.t("Copy to…")
                                    font.pointSize: UI.fontSize * 0.7
                                    onClicked: {
                                        UI.setSmp("smp.copyfrom", strip.slot)
                                        UI.setSmp("smp.copykit", -1)
                                        copyDialog.sourceSlot = strip.slot
                                        copyDialog.open()
                                    }
                                }
                                Button {
                                    Layout.fillWidth: true
                                    text: Tr.t("Erase")
                                    font.pointSize: UI.fontSize * 0.7
                                    enabled: !UI.samplerRecording
                                    onClicked: UI.erasePad(strip.slot)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- dialogs -----------------------------------------------------------

    Dialog {
        id: renameDialog
        anchors.centerIn: parent
        title: Tr.t("Rename kit")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: nameField.text = Synth.kits.length > Synth.currentKit
                  ? Synth.kits[Synth.currentKit].name : ""
        onAccepted: Synth.renameKit(Synth.currentKit, nameField.text)

        TextField {
            id: nameField
            width: 240
            maximumLength: 23
            placeholderText: Tr.t("Kit name")
        }
    }

    Dialog {
        id: copyDialog
        property int sourceSlot: -1
        anchors.centerIn: parent
        title: Tr.t("Copy pad to")
        modal: true
        standardButtons: Dialog.Cancel

        // A grid of destinations rather than two spin boxes: copying a pad is
        // a spatial act and the pads are already a grid everywhere else.
        Grid {
            columns: 4
            spacing: 4
            Repeater {
                model: 16
                delegate: Button {
                    required property int index
                    width: 56
                    text: String(index + 1)
                    enabled: index !== copyDialog.sourceSlot
                    onClicked: {
                        UI.setSmp("smp.copyto", index)
                        copyDialog.close()
                    }
                }
            }
        }
    }

    Label {
        anchors.centerIn: parent
        visible: !Synth.ready
        opacity: 0.5
        color: Material.foreground
        text: Synth.connected ? Tr.t("Discovering parameters…") : Tr.t("Not connected")
    }
}
