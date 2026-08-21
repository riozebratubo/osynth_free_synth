import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// Overview: engine select, master + voice (glide/unison/bend) controls, and the
// current preset.
Item {
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
