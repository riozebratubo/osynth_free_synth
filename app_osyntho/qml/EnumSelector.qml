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
    readonly property paramMeta meta: Synth.paramMeta(root.paramId)

    visible: meta.exists
    implicitWidth: 128
    implicitHeight: col.implicitHeight

    // A paramId that resolves after this selector was built (ModMatrixSlot
    // resolves its ids on paramsDiscovered, and a child completes before its
    // parent). Deferred so the `meta` binding — and therefore the model — has
    // re-evaluated before the value is pushed into it.
    function resync() { combo.resync() }
    onParamIdChanged: Qt.callLater(resync)

    Column {
        id: col
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 1

        Label {
            width: parent.width
            text: root.meta.exists ? UI.paramLabel(root.meta.name) : ""
            font.pointSize: UI.fontSize * 0.68
            elide: Text.ElideRight
            color: Material.foreground
            opacity: 0.75
        }

        ComboBox {
            id: combo
            width: parent.width
            model: root.meta.enumNames
            property bool syncing: false
            onActivated: if (!syncing) Synth.setParam(root.paramId, currentIndex)

            // Clamped into the model. The synth's own store clamps an enum to
            // its registered range, so an out-of-range value should not reach
            // us — but a firmware whose enum has grown or shrunk since the
            // metadata was read, or a parameter lock carrying an older value,
            // would otherwise park the box on an index that does not exist,
            // where it shows no text at all and reports one on the next write.
            function syncFrom(value: real): void {
                const n = combo.count
                if (n <= 0) return
                combo.syncing = true
                combo.currentIndex = Math.max(0, Math.min(n - 1, Math.round(value)))
                combo.syncing = false
            }

            function resync() { syncFrom(Synth.paramValue(root.paramId)) }

            Component.onCompleted: resync()
            // A ComboBox resets its own currentIndex when the model is
            // replaced, and this model only fills in once the parameter's
            // metadata has arrived — so a selector built before discovery
            // (ModMatrixSlot's source, whose paramId resolves later still) sat
            // on entry 0 whatever the synth had, until the value happened to
            // change. Both moments re-read it; `count` settling is the reliable
            // signal that the new list is in place.
            onCountChanged: resync()

            Connections {
                target: Synth
                function onParamChanged(id: int, value: real): void {
                    if (id === root.paramId) combo.syncFrom(value)
                }
            }
        }
    }
}
