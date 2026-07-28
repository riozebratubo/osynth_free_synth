import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// One labelled integer field in the sequencer's step/track inspectors.
//
// A SpinBox rather than a slider: these are exact quantities (a velocity, a
// ratchet count, a percent) that you set and read back, not continuous
// gestures — and on a phone a 6-pixel-wide slider region for 1..8 is unusable.
// `onEdited` fires only for user edits, so pushing a fresh value into `value`
// when the firmware reports one never echoes a write back.
ColumnLayout {
    id: root

    property string label: ""
    property int from: 0
    property int to: 127
    property int value: 0
    property string suffix: ""

    signal edited(int value)

    spacing: 0

    Label {
        text: root.label + (root.suffix !== "" ? " " + root.suffix : "")
        font.pointSize: UI.fontSize * 0.7
        opacity: 0.7
        color: Material.foreground
    }

    SpinBox {
        id: spin
        from: root.from
        to: root.to
        value: root.value
        editable: true
        onValueModified: root.edited(value)

        // The binding above only survives until the user touches the control:
        // SpinBox assigns its own `value` on spin/type, which replaces it. Push
        // later model changes in by hand, or the field silently stops tracking
        // the step after the first edit — including when another step is
        // selected, which would then show the previous step's numbers.
        Connections {
            target: root
            function onValueChanged() {
                if (spin.value !== root.value) spin.value = root.value
            }
        }
    }
}
