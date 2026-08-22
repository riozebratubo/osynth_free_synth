import QtQuick
import QtQuick.Controls.Material

import org.osynth.osyntho

// Rotary control bound to a numeric synth parameter (float or int) by id.
// Maps the parameter's range onto the Dial's 0..1 position, honouring an
// exp curve, and reflects external changes (EVT_PARAMS / preset loads) unless
// the user is actively dragging.
Item {
    id: root

    property int paramId: -1
    readonly property paramMeta meta: Synth.paramMeta(root.paramId)
    readonly property bool isExp: meta.exists && meta.curve === 1 && meta.min > 0
    readonly property bool isInt: meta.exists && meta.type === 1
    property real actualValue: meta.exists ? Synth.paramValue(paramId) : 0

    visible: meta.exists
    implicitWidth: 84
    implicitHeight: 100

    function posToValue(pos: real): real {
        if (!meta.exists)
            return 0
        var v = isExp ? meta.min * Math.pow(meta.max / meta.min, pos)
                      : meta.min + pos * (meta.max - meta.min)
        if (isInt)
            v = Math.round(v)
        return v
    }
    function valueToPos(v: real): real {
        if (!meta.exists || meta.max === meta.min)
            return 0
        var p = isExp ? Math.log(v / meta.min) / Math.log(meta.max / meta.min)
                      : (v - meta.min) / (meta.max - meta.min)
        return Math.max(0, Math.min(1, p))
    }
    function fmt(v: real): string {
        if (!meta.exists)
            return ""
        if (isInt)
            return "" + Math.round(v)
        var a = Math.abs(v)
        return a >= 100 ? v.toFixed(0) : (a >= 10 ? v.toFixed(1) : v.toFixed(2))
    }
    function syncDial() {
        dial.syncing = true
        dial.value = valueToPos(root.actualValue)
        dial.syncing = false
    }

    Column {
        anchors.fill: parent
        spacing: 1

        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: root.meta.exists ? root.meta.name.split('.').pop() : ""
            font.pointSize: UI.fontSize * 0.68
            elide: Text.ElideRight
            color: Material.foreground
            opacity: 0.75
        }

        Dial {
            id: dial
            width: parent.width
            height: width * 0.78
            from: 0
            to: 1
            live: true
            property bool syncing: false
            onMoved: {
                if (syncing)
                    return
                root.actualValue = root.posToValue(value)
                Synth.setParam(root.paramId, root.actualValue)
            }
        }

        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: root.fmt(root.actualValue)
            font.pointSize: UI.fontSize * 0.72
            color: Material.foreground
        }
    }

    function refresh() {
        actualValue = meta.exists ? Synth.paramValue(paramId) : 0
        syncDial()
    }

    Component.onCompleted: refresh()

    // A paramId that resolves *after* this Knob was built — ModMatrixSlot
    // resolves its ids on paramsDiscovered, and a child's Component.onCompleted
    // runs before its parent's, so its knobs are always created on -1 — used to
    // leave the dial and the readout on the value they were built with.
    // Component.onCompleted's assignment above replaces the `actualValue`
    // binding, and syncDial() was called nowhere else, so nothing but a later
    // paramChanged could correct it. Deferred so the `meta` binding has
    // certainly re-evaluated by the time valueToPos() reads it.
    onParamIdChanged: Qt.callLater(refresh)

    Connections {
        target: Synth
        function onParamChanged(id: int, value: real): void {
            if (id === root.paramId && !dial.pressed) {
                root.actualValue = value
                root.syncDial()
            }
        }
    }
}
