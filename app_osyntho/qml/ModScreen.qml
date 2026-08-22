import QtQuick
import QtQuick.Controls.Material

import org.osynth.osyntho

// LFOs + the 8-slot mod matrix (gated on the MODMATRIX cap).
Item {
    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: panels.implicitHeight + 24
        clip: true

        PanelFlow {
            id: panels

            ParamGroup { title: "LFO 1"; prefix: "lfo1"; capBit: 4 }
            ParamGroup { title: "LFO 2"; prefix: "lfo2"; capBit: 8 }
            ParamGroup { title: "Vibrato LFO"; prefix: "fm.lfo" }

            // the matrix rows need the full width, so this card never tiles
            Rectangle {
                width: panels.contentWidth
                visible: (Synth.caps & 32) !== 0
                implicitHeight: matrixCol.implicitHeight + 16
                height: implicitHeight
                radius: 8
                color: Material.theme === Material.Dark ? "#1AFFFFFF" : "#0D000000"

                Column {
                    id: matrixCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 8
                    spacing: 6

                    Label {
                        text: Tr.t("Mod matrix")
                        font.bold: true
                        font.pointSize: UI.fontSize * 0.95
                        color: Material.foreground
                    }
                    Repeater {
                        model: 8
                        delegate: ModMatrixSlot {
                            required property int index
                            slot: index + 1
                            width: parent.width
                        }
                    }
                }
            }
        }
    }

    Label {
        anchors.centerIn: parent
        visible: !Synth.ready
        text: Synth.connected ? Tr.t("Discovering parameters…") : Tr.t("Not connected")
        opacity: 0.5
        color: Material.foreground
    }
}
