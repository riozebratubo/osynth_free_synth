import QtQuick
import QtQuick.Controls.Material

import org.osynth.osyntho

// ComboBox bound to an enum synth parameter by id (uses the discovered enum
// names). Reflects external changes.
//
// Root is an Item (not the Column) so implicitWidth/Height are settable: this
// lets the control size itself in a Flow and be overridden by Layout.* in a
// RowLayout (ModMatrixSlot).
Item {
    id: root

    property int paramId: -1
    readonly property var meta: paramId >= 0 ? Synth.paramMeta(paramId) : ({ exists: false })

    visible: meta.exists
    implicitWidth: 128
    implicitHeight: col.implicitHeight

    Column {
        id: col
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 1

        Label {
            width: parent.width
            text: root.meta.exists ? root.meta.name.split('.').pop() : ""
            font.pointSize: UI.fontSize * 0.68
            elide: Text.ElideRight
            color: Material.foreground
            opacity: 0.75
        }

        ComboBox {
            id: combo
            width: parent.width
            model: root.meta.exists ? (root.meta.enumNames || []) : []
            property bool syncing: false
            onActivated: if (!syncing) Synth.setParam(root.paramId, currentIndex)

            // Clamped into the model. The synth's own store clamps an enum to
            // its registered range, so an out-of-range value should not reach
            // us — but a firmware whose enum has grown or shrunk since the
            // metadata was read, or a parameter lock carrying an older value,
            // would otherwise park the box on an index that does not exist,
            // where it shows no text at all and reports one on the next write.
            function syncFrom(value) {
                const n = combo.count
                if (n <= 0) return
                combo.syncing = true
                combo.currentIndex = Math.max(0, Math.min(n - 1, Math.round(value)))
                combo.syncing = false
            }

            Component.onCompleted: syncFrom(Synth.paramValue(root.paramId))
            Connections {
                target: Synth
                function onParamChanged(id, value) {
                    if (id === root.paramId) combo.syncFrom(value)
                }
            }
        }
    }
}
