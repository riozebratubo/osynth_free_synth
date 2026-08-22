import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// Overview: engine select, master + voice (glide/unison/bend) controls, the
// current preset, and the way back to a blank instrument.
Item {
    id: screen
    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: panels.implicitHeight + 24
        clip: true

        PanelFlow {
            id: panels
            spacing: 12

            // header + engine picker: full width, so they keep their own lines
            Label {
                width: panels.contentWidth
                text: Synth.ready ? t.ts("%1 engine", t.t(Synth.engineName)) : t.t("Discovering…")
                font.pointSize: UI.fontSize * 1.4
                font.bold: true
                color: Material.foreground
            }

            Row {
                width: panels.contentWidth
                spacing: 8
                Repeater {
                    // Which engines the connected firmware actually has —
                    // engine.type's enum decides the count and GRAPH_INFO
                    // decides whether Modular is among them. Built in
                    // SynthController::engineList() rather than here, because
                    // an app that hardcodes the list offers buttons that older
                    // firmware answers with ST_BAD_ARG (S38).
                    model: Synth.engineList
                    delegate: Button {
                        required property var modelData
                        text: t.t(modelData.n)
                        highlighted: Synth.engine === modelData.e
                        enabled: Synth.connected
                        onClicked: Synth.selectEngine(modelData.e)
                    }
                }
            }

            ParamGroup { title: "Master"; prefix: "master" }
            ParamGroup { title: "Voice"; prefix: "common" }

            Rectangle {
                width: panels.contentWidth
                height: presetRow.implicitHeight + 20
                radius: 8
                visible: Synth.ready
                color: Material.theme === Material.Dark ? "#1AFFFFFF" : "#0D000000"
                RowLayout {
                    id: presetRow
                    anchors.fill: parent
                    anchors.margins: 10
                    Label { text: t.t("Preset"); opacity: 0.7; color: Material.foreground }
                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignRight
                        color: Material.foreground
                        text: Synth.presetSlot >= 0
                            ? (Synth.presetSlot + (Synth.presetName ? (" · " + Synth.presetName) : "")
                               + (Synth.presetIsFactory ? ("  (" + t.t("factory") + ")") : ""))
                            : "—"
                    }
                }
            }

            // Back to a blank instrument. Resolved by name rather than by a
            // hardcoded id, and hidden entirely when the connected firmware
            // does not have it — the same existence test the toolbar's
            // out.level strip uses, and for the same reason: older firmware
            // answers a write to an unregistered id with nothing at all, so a
            // button that is always there would look broken instead of absent.
            Rectangle {
                id: resetCard
                property int resetId: -1
                width: panels.contentWidth
                height: resetRow.implicitHeight + 20
                radius: 8
                visible: Synth.ready && resetId >= 0
                color: Material.theme === Material.Dark ? "#1AFFFFFF" : "#0D000000"

                function refresh() { resetId = Synth.paramIdForName("state.reset") }
                Component.onCompleted: refresh()
                Connections {
                    target: Synth
                    function onParamsDiscovered() { resetCard.refresh() }
                }

                RowLayout {
                    id: resetRow
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 8
                    Column {
                        Layout.fillWidth: true
                        Label {
                            text: t.t("Start from scratch")
                            color: Material.foreground
                            opacity: 0.7
                        }
                        Label {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            text: t.t("The synth remembers how you left it and comes back that way. This puts it back to the sound it had out of the box.")
                            color: Material.foreground
                            opacity: 0.5
                            font.pointSize: Math.max(8, UI.fontSize * 0.75)
                        }
                    }
                    Button {
                        text: t.t("Reset…")
                        enabled: Synth.connected
                        onClicked: resetDialog.open()
                    }
                }
            }
        }
    }

    Dialog {
        id: resetDialog
        anchors.centerIn: Overlay.overlay
        modal: true
        focus: true
        title: t.t("Reset the synth?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: Math.min(parent ? parent.width - 32 : 420, 420)

        contentItem: Label {
            wrapMode: Text.WordWrap
            color: Material.foreground
            // Says exactly what goes and what stays. The firmware draws the
            // same line: the working state is the patch, the graph and the
            // sequencer; the NVS settings and the looper are not in it.
            text: t.t("Every sound setting goes back to its default, and the sequencer patterns and the modular patch are cleared. Your saved presets, the patch library, the looper and the volume and input settings are left alone.")
        }

        onAccepted: {
            if (resetCard.resetId >= 0) Synth.setParamNow(resetCard.resetId, 1)
        }
    }

    Label {
        anchors.centerIn: parent
        visible: !Synth.connected
        text: t.t("Not connected")
        opacity: 0.5
        color: Material.foreground
    }
}
