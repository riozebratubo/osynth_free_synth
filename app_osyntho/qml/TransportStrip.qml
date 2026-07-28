import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// One compact stop/play/rec group for the toolbar, driving either the
// sequencer (seq.mode) or the looper (loop.mode).
//
// Both transports are the same three-state enum — 0 stop, 1 play, 2 rec — so
// one component serves both and the two groups look identical. That sameness
// is what makes a double transport readable at a glance; the caption is the
// only thing telling them apart.
//
// Icons are Font Awesome 6 Free solid glyphs written as "\uXXXX" escapes, as
// everywhere else in this app — a literal private-use character does not
// survive every tool that touches the file, and a lost one renders as a blank
// button with no other symptom.
RowLayout {
    id: root

    property string caption: ""
    property int modeId: -1   // the mode parameter this group drives
    property int armedId: -1  // optional: the looper's count-in countdown

    readonly property bool present: modeId >= 0
    readonly property int mode: modeVal.asInt
    // Beats left on an armed looper take; 0 when not counting in.
    readonly property int armed: armedVal.asInt

    ParamValue { id: modeVal;  paramId: root.modeId }
    ParamValue { id: armedVal; paramId: root.armedId }
    // Drives the record button's blink while counting in.
    property bool blinkOn: true

    // Width this group will need, predicted from the font size rather than
    // measured from the laid-out children. The toolbar's fit test decides
    // `visible` from this, and a Layout's implicit size can depend on its own
    // visibility — so measuring would let the decision feed back into its own
    // input and either oscillate or latch at hidden.
    readonly property real estimatedWidth:
        Math.round(UI.fontSize * 2.1) * 3          // the three buttons
        + caption.length * UI.fontSize * 0.5       // the caption
        + 12                                       // spacing + margins

    readonly property color idleColor: App.theme.primaryColor
    readonly property color activeColor: "#69F0AE"
    readonly property color recColor: "#FF5252"

    spacing: 1
    visible: present && Synth.connected

    function go(m) { if (modeId >= 0) Synth.setParam(modeId, m) }

    Label {
        text: root.caption
        font.pointSize: UI.fontSize * 0.62
        color: App.theme.primaryColor
        opacity: 0.65
        Layout.alignment: Qt.AlignVCenter
        Layout.rightMargin: 2
    }

    ToolButton {
        text: "\uf04d"  // stop
        font.family: App.fontAwesomeName
        font.weight: Font.Black
        font.pointSize: UI.fontSize * 0.95
        padding: 3
        implicitWidth: Math.round(UI.fontSize * 2.1)
        Layout.alignment: Qt.AlignVCenter
        Material.foreground: root.mode === 0 ? root.activeColor : root.idleColor
        onClicked: root.go(0)
    }
    ToolButton {
        text: "\uf04b"  // play
        font.family: App.fontAwesomeName
        font.weight: Font.Black
        font.pointSize: UI.fontSize * 0.95
        padding: 3
        implicitWidth: Math.round(UI.fontSize * 2.1)
        Layout.alignment: Qt.AlignVCenter
        Material.foreground: root.mode === 1 ? root.activeColor : root.idleColor
        onClicked: root.go(1)
    }
    ToolButton {
        text: "\uf111"  // circle (record)
        font.family: App.fontAwesomeName
        font.weight: Font.Black
        font.pointSize: UI.fontSize * 0.95
        padding: 3
        implicitWidth: Math.round(UI.fontSize * 2.1)
        Layout.alignment: Qt.AlignVCenter
        // Counting in blinks; actually recording is solid red.
        Material.foreground: root.mode !== 2 ? root.idleColor
                             : root.armed > 0
                               ? (root.blinkOn ? root.recColor : root.idleColor)
                               : root.recColor
        onClicked: root.go(2)
    }

    // The beats remaining, so a count-in reads as a countdown rather than a
    // record button that appears stuck.
    Label {
        visible: root.armed > 0
        text: root.armed
        font.pointSize: UI.fontSize * 0.75
        font.bold: true
        color: root.recColor
        Layout.alignment: Qt.AlignVCenter
        Layout.leftMargin: 1
    }

    Timer {
        interval: 250
        repeat: true
        running: root.armed > 0
        onTriggered: root.blinkOn = !root.blinkOn
        onRunningChanged: if (!running) root.blinkOn = true
    }
}
