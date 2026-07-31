import QtQuick
import QtQuick.Controls.Material

import org.osynth.osyntho

// A titled card holding every discovered parameter whose name starts with
// `prefix`, as a wrapping Flow of ParamControls. Hidden when the group is empty
// or its `capBit` module is absent from the active engine's caps mask. Rebuilds
// on each discovery pass (engine switch / preset load).
//
// Width: in tiled mode (UI.tiledPanels, the default) the card takes only what
// its controls need on a single line, so the parent PanelFlow can pack several
// side by side; otherwise it fills the row. Either way it never exceeds the
// container's content width — a card with more controls than fit simply wraps
// them and, being full width, ends up alone on its line.
//
// caps bits: FILTER=1 ENV2=2 LFO1=4 LFO2=8 MIXER=16 MODMATRIX=32
Rectangle {
    id: grp

    property string title: ""
    property string prefix: ""
    property int capBit: 0
    property var ids: []

    // Width the container can give us. A PanelFlow parent publishes it as
    // contentWidth (its own width minus padding); in any other container we
    // fall back to the parent's full width. (qmllint types `parent` as a bare
    // Item and flags the read — it resolves fine at runtime.)
    property real maxWidth: {
        if (!parent)
            return 320
        const cw = parent.contentWidth
        return cw !== undefined && cw > 0 ? cw : parent.width
    }

    readonly property bool available: ids.length > 0 && (capBit === 0 || (Synth.caps & capBit) !== 0)

    // Width of the controls side by side on one line, plus the card's margins:
    // "just the width this panel needs". Control implicit widths are fixed
    // (knob 84 / enum 128 / switch 110), so this settles as soon as the
    // Repeater has built them; the binding re-runs on childrenChanged and
    // whenever a control's implicit width does. Until there is something to
    // measure the card claims the full row rather than shrinking to its title.
    readonly property real tileWidth: {
        let w = 0
        let n = 0
        for (let i = 0; i < flow.children.length; ++i) {
            const c = flow.children[i]
            // skips the Repeater itself (implicitWidth 0) and absent params
            if (!c.visible || c.implicitWidth <= 0)
                continue
            w += c.implicitWidth
            n++
        }
        if (n === 0)
            return maxWidth
        if (n > 1)
            w += (n - 1) * flow.spacing
        return Math.max(w, titleLabel.implicitWidth) + 16
    }

    width: Math.min(UI.tiledPanels ? tileWidth : maxWidth, maxWidth)
    implicitHeight: available ? (col.implicitHeight + 16) : 0
    height: implicitHeight
    visible: available
    radius: 8
    color: Material.theme === Material.Dark ? "#1AFFFFFF" : "#0D000000"

    // Only assign when the set actually changed. `ids` is the Repeater's model,
    // and assigning a list to a var property always fires the change signal
    // even when the contents are identical — which tears down and rebuilds
    // every ParamControl in the card. paramsDiscovered arrives up to ~7×/s for
    // the length of a discovery pass, so an unguarded refresh had every visible
    // group rebuilding its whole control set that often, on the GUI thread, for
    // the seconds the pass takes. For groups outside the engine range (the
    // common case on an engine switch) the answer is unchanged every time.
    function refresh() {
        const next = Synth.paramIdsByPrefix(prefix)
        if (next.length === ids.length) {
            let same = true
            for (let i = 0; i < next.length; ++i) {
                if (next[i] !== ids[i]) {
                    same = false
                    break
                }
            }
            if (same)
                return
        }
        ids = next
    }
    Component.onCompleted: refresh()

    Connections {
        target: Synth
        function onParamsDiscovered() { grp.refresh() }
    }

    Column {
        id: col
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 8
        spacing: 6

        Label {
            id: titleLabel
            text: grp.title !== "" ? t.t(grp.title) : grp.prefix
            font.bold: true
            font.pointSize: UI.fontSize * 0.95
            color: Material.foreground
        }

        Flow {
            id: flow
            width: parent.width
            spacing: 10
            Repeater {
                model: grp.ids
                delegate: ParamControl {
                    required property var modelData
                    paramId: modelData
                }
            }
        }
    }
}
