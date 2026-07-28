import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// Transport + arpeggiator / sequencer parameters (0x04xx). TRANSPORT/ARP opcodes
// are conveniences; the underlying params are also editable directly below.
Item {
    id: root

    // Resolved imperatively — paramIdForName has no change signal, so as a
    // binding it would evaluate once before the synth is connected and stay
    // at -1 (see ParamValue.qml).
    property int pidTempo: -1
    function refreshIds() { pidTempo = Synth.paramIdForName("seq.tempo") }

    // The tempo field reflects seq.tempo rather than holding a number of its
    // own. It used to be a bare `value: 120`, and Play sent it along — so
    // pressing Play here silently reset the synth's tempo to whatever this
    // stale field happened to show, undoing the tempo set on the Sequencer
    // page. Play now just starts the transport; the tempo is edited here.
    ParamValue { id: tempoVal; paramId: root.pidTempo; fallback: 120 }

    Connections {
        target: Synth
        function onParamsDiscovered() { root.refreshIds() }
    }
    Component.onCompleted: refreshIds()

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
                    onClicked: Synth.transport(1)
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
                        value: tempoVal.asInt
                        enabled: root.pidTempo >= 0
                        onValueModified: if (root.pidTempo >= 0)
                                             Synth.setParam(root.pidTempo, value)
                        // A user edit assigns `value`, which breaks the
                        // binding above; re-assert it whenever the synth
                        // reports a tempo, so the field keeps following
                        // changes made elsewhere (the Sequencer page, a
                        // preset load, MIDI clock).
                        Connections {
                            target: tempoVal
                            function onValueChanged() {
                                tempoField.value = tempoVal.asInt
                            }
                        }
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
