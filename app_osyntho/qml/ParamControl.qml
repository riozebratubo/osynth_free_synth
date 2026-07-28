import QtQuick
import QtQuick.Controls.Material

import org.osynth.osyntho

// Picks the right widget for a parameter from its discovered type:
//   float / int -> Knob, enum -> EnumSelector, bool -> Switch.
Item {
    id: pc

    property int paramId: -1
    readonly property var meta: paramId >= 0 ? Synth.paramMeta(paramId) : ({ exists: false })

    visible: meta.exists
    implicitWidth: loader.item ? loader.item.implicitWidth : 0
    implicitHeight: loader.item ? loader.item.implicitHeight : 0

    Loader {
        id: loader
        anchors.centerIn: parent
        sourceComponent: !pc.meta.exists ? null
                       : pc.meta.type === 2 ? enumComp
                       : pc.meta.type === 3 ? boolComp
                       : knobComp
    }

    Component { id: knobComp; Knob { paramId: pc.paramId } }
    Component { id: enumComp; EnumSelector { paramId: pc.paramId } }

    // Bool switch. Root is an Item (settable implicit size) wrapping the Column.
    Component {
        id: boolComp
        Item {
            id: boolRoot
            implicitWidth: 110
            implicitHeight: boolCol.implicitHeight
            Column {
                id: boolCol
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                spacing: 1
                Label {
                    width: parent.width
                    text: pc.meta.exists ? pc.meta.name.split('.').pop() : ""
                    font.pointSize: UI.fontSize * 0.68
                    elide: Text.ElideRight
                    color: Material.foreground
                    opacity: 0.75
                }
                Switch {
                    id: sw
                    property bool syncing: false
                    Component.onCompleted: {
                        syncing = true
                        checked = Math.round(Synth.paramValue(pc.paramId)) === 1
                        syncing = false
                    }
                    onToggled: if (!syncing) Synth.setParam(pc.paramId, checked ? 1 : 0)
                    Connections {
                        target: Synth
                        function onParamChanged(id, value) {
                            if (id === pc.paramId) {
                                sw.syncing = true
                                sw.checked = Math.round(value) === 1
                                sw.syncing = false
                            }
                        }
                    }
                }
            }
        }
    }
}
