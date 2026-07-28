import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// The drum bus (firmware S22): kit selection, the bus controls, and a card per
// kit slot.
//
// The slot cards tile through PanelFlow like every other page's panels, so
// 16 slots read as a grid instead of 16 full-width rows you have to scroll
// through. Each card is the width its four knobs need in a 2x2 block, which
// is what makes the packing come out even.
//
// Slot parameters are named generically in the firmware (drum1.level …)
// because kits are swappable; the human names come from the kit itself, so
// every card is labelled from Synth.kitSlots rather than from the parameter
// name. Tapping a pad auditions the slot — also how you check a kit after
// loading one from the SD card.
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
    ParamValue { id: chokeVal; paramId: root.pidChoke }

    // KIT_INFO reports every slot the build compiles in, empty ones included —
    // they come back with no name and note 0 (see SynthController::noteForSlot).
    // A card for one of those is a blank title over "C-1" and four knobs that
    // drive nothing, so they are dropped here, exactly as DrumPads greys them
    // out. Reading Synth.kitSlots (a notifying property) is what makes this
    // re-evaluate when the kit is swapped.
    readonly property var populatedSlots: {
        var out = []
        var slots = Synth.kitSlots
        for (var i = 0; i < slots.length; ++i) {
            if (slots[i].name !== "") out.push(slots[i])
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
                            text: t.t("Kit")
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
                        ToolButton {
                            text: "\uf021"  // rotate: rescan the card for kits
                            font.family: App.fontAwesomeName
                            font.weight: Font.Black
                            onClicked: Synth.refreshKit()
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
                            text: t.t("Choke groups")
                            modelChecked: chokeVal.on
                            onToggled: if (root.pidChoke > 0) Synth.setParam(root.pidChoke, checked ? 1 : 0)
                        }
                    }
                }
            }

            Label {
                visible: root.populatedSlots.length === 0
                width: panels.contentWidth
                wrapMode: Text.WordWrap
                opacity: 0.6
                color: Material.foreground
                text: t.t("This firmware has no drum kit built in. Build one with "
                          + "tools/gen_drumkit.py and reflash, or put a kit on the "
                          + "SD card.")
            }

            // ---- one card per populated slot, tiled -----------------------
            Repeater {
                model: root.populatedSlots

                delegate: Frame {
                    id: strip
                    required property var modelData
                    readonly property int slot: modelData.slot
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

                        // The pad: name, MIDI note, audition on tap.
                        Button {
                            Layout.fillWidth: true
                            onClicked: Synth.triggerDrum(strip.slot, 100)
                            contentItem: Column {
                                spacing: 0
                                Label {
                                    width: parent.width
                                    horizontalAlignment: Text.AlignHCenter
                                    text: strip.modelData.name
                                    font.bold: true
                                    font.pointSize: UI.fontSize * 0.85
                                    color: Material.foreground
                                    elide: Label.ElideRight
                                }
                                Label {
                                    width: parent.width
                                    horizontalAlignment: Text.AlignHCenter
                                    text: Synth.noteName(strip.modelData.note)
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
        text: Synth.connected ? t.t("Discovering parameters…") : t.t("Not connected")
    }
}
