import QtQuick

import org.osynth.osyntho

// A reactive read of one synth parameter.
//
// `Synth.paramValue()` is a plain invokable: a binding onto it captures no
// property, so it evaluates once and never again. The parameter changes and
// the UI silently does not — a record button that stays grey, a toggle that
// never reflects the synth. (Same trap as `paramIdForName`; see
// AdsrEnvelope.qml.)
//
// This republishes the value as a real property, updated from `paramChanged`,
// so ordinary bindings work:
//
//     ParamValue { id: mode; paramId: root.pidMode }
//     ...
//     highlighted: mode.asInt === 2
//
// Non-visual, so it can be dropped into a Layout or a positioner without
// taking part in it.
QtObject {
    id: root

    property int paramId: -1
    // Value to report until the synth has actually told us one — paramValue()
    // falls back to the parameter's *default*, which for a status parameter is
    // a plausible-looking lie.
    property real fallback: 0

    property real value: fallback
    property bool valueKnown: false

    readonly property int asInt: Math.round(value)
    readonly property bool on: value >= 0.5   // bool/enum params
    readonly property bool available: paramId >= 0

    function refresh() {
        if (paramId >= 0 && Synth.paramValueKnown(paramId)) {
            value = Synth.paramValue(paramId)
            valueKnown = true
        } else {
            value = fallback
            valueKnown = false
        }
    }

    onParamIdChanged: refresh()
    Component.onCompleted: refresh()

    // Held in a property because QtObject has no default property to parent a
    // child element to.
    property Connections _conn: Connections {
        target: Synth
        function onParamChanged(id: int, v: real): void {
            if (id === root.paramId) {
                root.value = v
                root.valueKnown = true
            }
        }
        // Ids resolve and values arrive during discovery; a reconnect clears
        // everything and starts again.
        function onParamsDiscovered() { root.refresh() }
        function onConnectedChanged() { root.refresh() }
    }
}
