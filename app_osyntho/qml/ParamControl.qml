import QtQuick
import QtQuick.Controls.Material

import org.osynth.osyntho

// Picks the right widget for a parameter from its discovered type:
//   float / int -> Knob, enum -> EnumSelector, bool -> Switch.
Item {
    id: pc

    property int paramId: -1

    // paramMeta() is a plain call, so QML has no way to know its answer changed
    // and the binding below would evaluate once, at creation. That is fine on a
    // page whose ParamGroup rebuilds its whole control set when ids arrive, and
    // it is not fine on the patch editor: a node's parameters are registered
    // when its slot takes a kind, long after discovery, and the Repeater's model
    // (the kind's parameter count) does not change when their metadata lands —
    // so the delegates are never recreated and every control stayed invisible.
    // Bumping this re-evaluates `meta` in place.
    property int metaRevision: 0
    readonly property var meta: {
        const dep = pc.metaRevision  // read only to register the dependency
        void dep
        return pc.paramId >= 0 ? Synth.paramMeta(pc.paramId) : ({ exists: false })
    }

    // paramsDiscovered fires several times a second for the length of a
    // discovery pass, so this re-evaluates only when the answer can actually
    // have moved: nothing drawn yet, or the id now describes something else.
    // The second case is the patch editor again — node parameter ids are
    // positional, so a slot that changes kind keeps its ids and changes what
    // they mean, and the number of controls need not change with it (a mixer
    // and an envelope both declare four).
    Connections {
        target: Synth
        function onParamsDiscovered() {
            if (!pc.meta.exists) { pc.metaRevision++; return }
            const now = pc.paramId >= 0 ? Synth.paramMeta(pc.paramId) : null
            if (!now || !now.exists || now.name !== pc.meta.name) pc.metaRevision++
        }
    }

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
