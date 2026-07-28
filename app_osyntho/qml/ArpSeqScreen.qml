import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// Transport + arpeggiator / sequencer parameters (0x04xx). TRANSPORT/ARP opcodes
// are conveniences; the underlying params are also editable directly below.
Item {
    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: panels.implicitHeight + 24
        clip: true

        PanelFlow {
            id: panels
            spacing: 12

            Row {
                width: panels.contentWidth  // transport keeps its own line
                spacing: 8
                Button {
                    text: t.t("Stop")
                    enabled: Synth.connected
                    onClicked: Synth.transport(0)
                }
                Button {
                    text: t.t("Play")
                    highlighted: true
                    enabled: Synth.connected
                    onClicked: Synth.transport(1, Math.round(tempoField.value))
                }
                Button {
                    text: t.t("Rec")
                    enabled: Synth.connected
                    onClicked: Synth.transport(2)
                }
                RowLayout {
                    spacing: 4
                    Label { text: t.t("Tempo"); color: Material.foreground; Layout.alignment: Qt.AlignVCenter }
                    SpinBox {
                        id: tempoField
                        from: 20
                        to: 300
                        value: 120
                    }
                }
            }

            ParamGroup { title: "Arpeggiator"; prefix: "arp" }
            ParamGroup { title: "Sequencer"; prefix: "seq" }
        }
    }

    Label {
        anchors.centerIn: parent
        visible: !Synth.ready
        text: Synth.connected ? t.t("Discovering parameters…") : t.t("Not connected")
        opacity: 0.5
        color: Material.foreground
    }
}
