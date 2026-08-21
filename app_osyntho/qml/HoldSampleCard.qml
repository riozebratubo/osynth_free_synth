import QtQuick
import QtQuick.Controls.Material

import org.osynth.osyntho

// Momentary "hold to sample" button for a freeze parameter (S38).
//
// The firmware side of a freeze is a *switch*: 1 holds the buffer, 0 lets it
// record. That is the right thing to store in a preset and the wrong thing to
// play with, because capturing a phrase means flipping it off, waiting, and
// flipping it back on — three actions, two of them timing-critical, on a
// touchscreen. This inverts it into one gesture: press and the buffer records,
// release and it holds whatever just went in.
//
// It writes the same parameter the switch does and nothing else, so the two
// stay consistent in both directions — the switch still shows the state this
// button leaves behind, and moving the switch is still a perfectly good way to
// work. Neither is a mode.
//
// setParamNow(), not setParam(): a press and a release are two values of one id
// milliseconds apart, and setParam()'s ~40 ms coalescing window keeps only the
// last — a quick tap would arrive as the release alone and the synth would
// never see the record. See the note on setParamNow in synthcontroller.h, which
// was written after the sequencer's Fill hit exactly this.
Rectangle {
    id: card

    // Registered name of the bool freeze parameter, e.g. "buf.freeze".
    property string paramName: ""
    property string title: ""
    // One line under the button saying what gets captured.
    property string hint: ""

    // Resolved by refresh(), never by a binding: paramIdForName() is a plain
    // call, so QML has no way to know its answer changed and a binding would
    // evaluate once, at creation — before discovery has registered anything.
    // Same trap ParamControl documents for paramMeta(), and the same fix the
    // rest of this app uses: a function plus paramsDiscovered.
    property int paramId: -1
    // Mirrored from the synth rather than read back on demand, for the same
    // reason: paramValue() is a call, and paramChanged is what says it moved.
    property bool frozen: false

    readonly property bool available: paramId >= 0

    function refresh() {
        const id = Synth.paramIdForName(paramName)
        if (id !== paramId)
            paramId = id
        if (paramId >= 0)
            frozen = Math.round(Synth.paramValue(paramId)) === 1
    }

    Component.onCompleted: refresh()

    Connections {
        target: Synth
        function onParamsDiscovered() { card.refresh() }
        // The switch beside this button writes the same id, and so does a
        // preset load; both arrive here.
        function onParamChanged(id, value) {
            if (id === card.paramId)
                card.frozen = Math.round(value) === 1
        }
    }

    property real maxWidth: {
        if (!parent)
            return 320
        const cw = parent.contentWidth
        return cw !== undefined && cw > 0 ? cw : parent.width
    }

    // Same sizing contract as ParamGroup: ask for what the content needs in
    // tiled mode, never more than the container gives.
    readonly property real tileWidth:
        Math.max(button.implicitWidth, titleLabel.implicitWidth) + 16

    width: Math.min(UI.tiledPanels ? tileWidth : maxWidth, maxWidth)
    implicitHeight: available ? (col.implicitHeight + 16) : 0
    height: implicitHeight
    visible: available
    radius: 8
    color: Material.theme === Material.Dark ? "#1AFFFFFF" : "#0D000000"

    Column {
        id: col
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 8
        spacing: 6

        Label {
            id: titleLabel
            text: t.t(card.title)
            font.bold: true
            font.pointSize: UI.fontSize * 0.95
            color: Material.foreground
        }

        Button {
            id: button
            enabled: Synth.connected && card.available
            // Deliberately not `checkable`. The state this shows belongs to the
            // synth, not to the button — the switch beside it can change the
            // same parameter, and a preset load can too.
            highlighted: down || !card.frozen
            implicitWidth: 150
            text: down ? t.t("Recording…")
                       : (card.frozen ? t.t("Hold to sample") : t.t("Live"))

            onPressed: Synth.setParamNow(card.paramId, 0)
            onReleased: Synth.setParamNow(card.paramId, 1)
            // A press that leaves the button still ends the gesture: without
            // this the buffer would be left recording forever after a drag-off,
            // which looks exactly like the button having done nothing.
            onCanceled: Synth.setParamNow(card.paramId, 1)
        }

        Label {
            visible: card.hint !== ""
            text: t.t(card.hint)
            font.pointSize: UI.fontSize * 0.8
            color: Material.foreground
            opacity: 0.7
            // Wraps inside the card's width. Deliberately not part of
            // tileWidth: a hint is prose, its natural width is far wider than
            // the controls, and letting it size the card would make this panel
            // hog a tiled row.
            width: col.width
            wrapMode: Text.WordWrap
        }
    }
}
