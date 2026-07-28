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
    readonly property var meta: paramId >= 0 ? Synth.paramMeta(paramId) : ({ exists: false })
    readonly property bool isExp: meta.exists && meta.curve === 1 && meta.min > 0
    readonly property bool isInt: meta.exists && meta.type === 1
    property real actualValue: meta.exists ? Synth.paramValue(paramId) : 0

    visible: meta.exists
    implicitWidth: 84
    implicitHeight: 100

    function posToValue(pos) {
        if (!meta.exists)
            return 0
        var v = isExp ? meta.min * Math.pow(meta.max / meta.min, pos)
                      : meta.min + pos * (meta.max - meta.min)
        if (isInt)
            v = Math.round(v)
        return v
    }
    function valueToPos(v) {
        if (!meta.exists || meta.max === meta.min)
            return 0
        var p = isExp ? Math.log(v / meta.min) / Math.log(meta.max / meta.min)
                      : (v - meta.min) / (meta.max - meta.min)
        return Math.max(0, Math.min(1, p))
    }
    function fmt(v) {
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
            text: meta.exists ? meta.name.split('.').pop() : ""
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

    Component.onCompleted: {
        actualValue = meta.exists ? Synth.paramValue(paramId) : 0
        syncDial()
    }

    Connections {
        target: Synth
        function onParamChanged(id, value) {
            if (id === root.paramId && !dial.pressed) {
                root.actualValue = value
                root.syncDial()
            }
        }
    }
}
