import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

import org.osynth.osyntho

// Modular patch graph (S28): the node canvas.
//
// Node *values* are ordinary parameters, so the per-node panel is built from
// the same ParamControl widgets as every other page — what is special here is
// only the wiring, which is not parameter space and comes over the graph
// opcodes.
//
// Connections are made by tapping, not dragging. A drag has to compete with
// panning the canvas and with moving the node itself, and on a phone the jacks
// are barely bigger than a fingertip; tap-a-jack-then-tap-a-jack has none of
// that ambiguity and is undoable by tapping the same jack twice.
Item {
    id: screen

    readonly property bool ready: Synth.connected && Synth.graphAvailable
    readonly property bool onModular: Synth.engine === Synth.graphEngineIndex

    // Lowest empty slot, or -1 when the graph is full — what "Add node" needs.
    // graphFreeSlot() is a plain invokable, so a binding that only calls it has
    // nothing to re-evaluate on: it would keep the answer from page load, taken
    // before the first GRAPH_NODES reply, when the model is empty and the call
    // returns -1. Reading graphNodes here gives the binding its dependency —
    // that property notifies on graphChanged, which is precisely when the free
    // slot can differ — and an empty model genuinely has no slot to offer.
    readonly property int freeSlot: {
        var nodes = Synth.graphNodes
        return nodes.length > 0 ? Synth.graphFreeSlot() : -1
    }

    // Node geometry. Fixed rather than content-sized: cables are drawn to jack
    // centres computed from these numbers, and a node that changed height
    // when its name got longer would drag its cables off the jacks.
    readonly property int nodeW: Math.round(96 * UI.fontSize / 10)
    readonly property int nodeH: Math.round(56 * UI.fontSize / 10)
    readonly property int jackR: Math.round(7 * UI.fontSize / 10)

    property int pendingSource: -1   // slot whose output jack is armed, or -1
    property int selectedSlot: -1    // node whose parameter panel is open

    // The "no such slot" answer nodeAt() hands back, and the only reason it
    // exists: a graphSlot is a value type, so it cannot be null, and a
    // default-constructed one already *is* the not-a-slot state — valid false,
    // kind 0, no sources. Never assigned; not readonly only because a
    // read-only property has to be initialised and the default is the point.
    property graphSlot emptySlot

    // The theme accent, captured on an item that does not override it. The
    // cost meter recolours itself near the budget, and reading Material.accent
    // inside its own binding is a self-reference: on that item the name now
    // resolves to the overridden value, so it loops. Holding the default here
    // gives the binding a source outside itself.
    readonly property color accentColor: Material.accent
    readonly property color warnColor: "#E5793B"


    // --- geometry helpers, shared by the delegates and the cable painter ---

    function nodeAt(slot: int): graphSlot {
        var list = Synth.graphNodes
        return (slot >= 0 && slot < list.length) ? list[slot] : screen.emptySlot
    }
    function outJackPos(slot: int): var {
        var n = nodeAt(slot)
        if (!n.valid) return null
        return { x: n.x + nodeW, y: n.y + nodeH / 2 }
    }
    function inJackPos(slot: int, port: int): var {
        var n = nodeAt(slot)
        if (!n.valid) return null
        var k = Synth.graphKind(n.kind)
        var count = k.inputs.length
        if (count <= 0) return null
        // Ports spread down the node's left edge, centred on it.
        var step = nodeH / (count + 1)
        return { x: n.x, y: n.y + step * (port + 1) }
    }

    function kindName(kind: int): string {
        var k = Synth.graphKind(kind)
        return k.name.length > 0 ? k.name : "—"
    }
    function isAudio(kind: int): bool {
        return Synth.graphKind(kind).rate === 1
    }

    function tapOutput(slot: int): void {
        pendingSource = (pendingSource === slot) ? -1 : slot
        cables.requestPaint()
    }
    function tapInput(slot: int, port: int): void {
        var n = nodeAt(slot)
        if (!n.valid) return
        if (pendingSource < 0) {
            // Nothing armed: a tap on a patched jack unpatches it. This is the
            // only way to remove a cable, and it mirrors pulling one out.
            if (n.sources[port] >= 0) Synth.graphConnect(slot, port, -1)
            return
        }
        Synth.graphConnect(slot, port, pendingSource)
        pendingSource = -1
    }

    onReadyChanged: if (ready) cables.requestPaint()

    Connections {
        target: Synth
        // Fires for wiring changes *and* for a node being dragged (layout is
        // applied locally — see graphSetNodePos), so it must not clear the
        // armed jack: moving a node out of the way mid-patch is exactly when
        // you would lose it.
        function onGraphChanged() { cables.requestPaint() }
        function onGraphKindsChanged() { cables.requestPaint() }
        function onGraphErrorChanged() {
            if (Synth.graphError.length > 0) {
                // The edit was refused and the model rolled back, so an armed
                // jack may now point at a node that never got created.
                screen.pendingSource = -1
                errorBar.text = Synth.graphError
                errorBar.visible = true
                errorTimer.restart()
            }
        }
    }

    Timer {
        id: errorTimer
        interval: 4000
        onTriggered: { errorBar.visible = false; Synth.clearGraphError() }
    }

    // --- not available / wrong engine -------------------------------------

    ColumnLayout {
        anchors.centerIn: parent
        width: parent.width * 0.8
        spacing: UI.fontSize
        visible: !screen.ready || !screen.onModular

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            font.pointSize: UI.fontSize
            text: !Synth.connected ? Tr.t("Not connected")
                : !Synth.graphAvailable
                    ? Tr.t("This firmware was built without the modular engine.")
                    : Tr.t("The modular engine is not the active engine.")
        }
        Button {
            Layout.alignment: Qt.AlignHCenter
            visible: screen.ready && !screen.onModular
            text: Tr.t("Switch to modular")
            onClicked: Synth.selectEngine(Synth.graphEngineIndex)
        }
    }

    // --- the canvas -------------------------------------------------------

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        visible: screen.ready && screen.onModular

        // Toolbar: add a node, and the cost meter. The meter is not decoration
        // — the firmware refuses an over-budget patch, and seeing the number
        // climb is what turns that from a surprise into a constraint.
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 6
            spacing: 8

            Button {
                text: Tr.t("Add node")
                enabled: screen.freeSlot >= 0
                onClicked: kindPicker.open()
            }
            Item { Layout.fillWidth: true }
            Label {
                text: Tr.t("CPU")
                font.pointSize: UI.fontSize * 0.8
                opacity: 0.7
            }
            ProgressBar {
                Layout.preferredWidth: Math.round(120 * UI.fontSize / 10)
                from: 0
                to: Math.max(1, Synth.graphCostBudget)
                value: Synth.graphCost
                Material.accent: value > to * 0.85 ? screen.warnColor
                                                   : screen.accentColor
            }
            Label {
                text: Synth.graphCost + " / " + Synth.graphCostBudget
                font.pointSize: UI.fontSize * 0.75
                opacity: 0.7
            }
        }

        // Where the signal actually goes after Out. The graph is the *voice*
        // path; reverb, delay and chorus are a master-bus stage that runs on
        // the mix (render_chain in main.cpp: voices, then fx_process), shared
        // by every engine and with no node kind of its own here. Without this
        // line, a patch of Osc → Out that comes out wet reads as a bug in the
        // graph rather than as the FX page doing its job.
        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            Layout.bottomMargin: 4
            text: Tr.t("Master FX (reverb, delay, chorus) run after Out, on the mix — they are shared with every engine and set on the FX page.")
            wrapMode: Text.WordWrap
            font.pointSize: UI.fontSize * 0.75
            opacity: 0.6
        }

        Rectangle {
            id: errorBar
            property alias text: errorLabel.text
            Layout.fillWidth: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            visible: false
            Layout.preferredHeight: errorLabel.implicitHeight + 10
            radius: 4
            color: "#33E5793B"
            Label {
                id: errorLabel
                anchors.centerIn: parent
                font.pointSize: UI.fontSize * 0.8
                color: "#E5793B"
            }
        }

        Flickable {
            id: canvas
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            // Room to lay a patch out beyond the viewport; the firmware stores
            // these coordinates, so the space has to be the same on every
            // device that opens the patch.
            contentWidth: Math.max(width, 760)
            contentHeight: Math.max(height, 520)
            boundsBehavior: Flickable.StopAtBounds

            // Cables under the nodes, so a jack is never hidden by a wire.
            Canvas {
                id: cables
                anchors.fill: parent
                renderStrategy: Canvas.Cooperative

                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()
                    ctx.lineWidth = Math.max(2, Math.round(UI.fontSize / 5))
                    ctx.lineCap = "round"

                    var nodes = Synth.graphNodes
                    for (var dst = 0; dst < nodes.length; ++dst) {
                        var n = nodes[dst]
                        if (n.kind === 0) continue
                        var k = Synth.graphKind(n.kind)
                        // The kind declares the ports and the slot carries a
                        // source for each one it knows about; a kind table that
                        // landed before the model can name more of them than
                        // the slot has entries for, which used to be what the
                        // `undefined` test here was standing in for.
                        var nin = Math.min(k.inputs.length, n.sources.length)
                        for (var port = 0; port < nin; ++port) {
                            var src = n.sources[port]
                            if (src < 0) continue
                            var a = screen.outJackPos(src)
                            var b = screen.inJackPos(dst, port)
                            if (!a || !b) continue
                            // Audio cables read heavier than modulation ones:
                            // in a patch of a dozen wires, telling signal from
                            // control at a glance is most of the readability.
                            ctx.strokeStyle = screen.isAudio(nodes[src].kind)
                                ? "#4FA3E3" : "#B07CD8"
                            ctx.beginPath()
                            ctx.moveTo(a.x, a.y)
                            // Horizontal-tangent bezier: the curve leaves and
                            // arrives level with the jacks, so a cable never
                            // looks like it lands on the wrong port.
                            var dx = Math.max(30, Math.abs(b.x - a.x) * 0.5)
                            ctx.bezierCurveTo(a.x + dx, a.y, b.x - dx, b.y, b.x, b.y)
                            ctx.stroke()
                        }
                    }
                }
            }

            Repeater {
                model: Synth.graphNodes
                delegate: Rectangle {
                    id: node
                    required property graphSlot modelData
                    required property int index

                    readonly property graphKindDesc kindDesc:
                        Synth.graphKind(modelData.kind)
                    readonly property bool empty: modelData.kind === 0
                    readonly property bool isOut: index === Synth.graphOutSlot

                    visible: !empty
                    x: modelData.x
                    y: modelData.y
                    width: screen.nodeW
                    height: screen.nodeH
                    radius: 6
                    color: Material.theme === Material.Dark ? "#242A33" : "#F1F3F6"
                    border.width: screen.selectedSlot === index ? 2 : 1
                    border.color: screen.selectedSlot === index
                        ? Material.accent
                        : (screen.isAudio(modelData.kind) ? "#4FA3E3" : "#B07CD8")

                    Column {
                        anchors.centerIn: parent
                        spacing: 1
                        Label {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: node.kindDesc.name.length > 0
                                  ? node.kindDesc.name : "?"
                            font.pointSize: UI.fontSize * 0.8
                            font.bold: true
                        }
                        Label {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "n" + node.index
                            font.pointSize: UI.fontSize * 0.6
                            opacity: 0.55
                        }
                    }

                    // Drag to move, tap to open the parameter panel. The
                    // position is pushed on release, not continuously: it is a
                    // BLE write per update otherwise, and the firmware treats
                    // it as canvas-only anyway.
                    MouseArea {
                        anchors.fill: parent
                        drag.target: node
                        drag.minimumX: 0
                        drag.minimumY: 0
                        drag.maximumX: canvas.contentWidth - node.width
                        drag.maximumY: canvas.contentHeight - node.height
                        onPositionChanged: if (drag.active) cables.requestPaint()
                        onReleased: {
                            if (Math.abs(node.x - node.modelData.x) > 1 ||
                                Math.abs(node.y - node.modelData.y) > 1) {
                                Synth.graphSetNodePos(node.index,
                                                      Math.round(node.x),
                                                      Math.round(node.y))
                            }
                        }
                        onClicked: {
                            screen.selectedSlot =
                                (screen.selectedSlot === node.index) ? -1 : node.index
                        }
                    }

                    // Output jack. The Out node has none — it is the sink.
                    Rectangle {
                        visible: !node.isOut
                        width: screen.jackR * 2
                        height: width
                        radius: width / 2
                        x: parent.width - width / 2
                        y: parent.height / 2 - height / 2
                        color: screen.pendingSource === node.index
                            ? Material.accent
                            : (screen.isAudio(node.modelData.kind) ? "#4FA3E3" : "#B07CD8")
                        border.width: 1
                        border.color: Material.background
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -6   // fingertip target
                            onClicked: screen.tapOutput(node.index)
                        }
                    }

                    // Input jacks, one per declared port.
                    Repeater {
                        model: node.kindDesc.inputs
                        delegate: Item {
                            required property string modelData
                            required property int index
                            readonly property var pos:
                                screen.inJackPos(node.index, index)
                            readonly property bool patched:
                                node.modelData.sources[index] >= 0

                            x: -screen.jackR
                            y: (pos ? pos.y - node.modelData.y : 0) - screen.jackR
                            width: screen.jackR * 2
                            height: screen.jackR * 2

                            Rectangle {
                                anchors.fill: parent
                                radius: width / 2
                                color: parent.patched ? Material.foreground : "transparent"
                                opacity: parent.patched ? 0.75 : 1.0
                                border.width: 2
                                border.color: screen.pendingSource >= 0
                                    ? Material.accent : Material.foreground
                            }
                            Label {
                                anchors.right: parent.left
                                anchors.rightMargin: 3
                                anchors.verticalCenter: parent.verticalCenter
                                text: parent.modelData
                                font.pointSize: UI.fontSize * 0.55
                                opacity: 0.6
                            }
                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -6
                                onClicked: screen.tapInput(node.index, parent.index)
                            }
                        }
                    }
                }
            }
        }

        // --- parameter panel for the selected node ------------------------

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: paramFlow.implicitHeight + headerRow.height + 16
            visible: screen.selectedSlot >= 0
            color: Material.theme === Material.Dark ? "#1A1F26" : "#EEF1F5"

            RowLayout {
                id: headerRow
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: 6
                Label {
                    text: screen.selectedSlot >= 0
                        ? (screen.kindName(screen.nodeAt(screen.selectedSlot).kind)
                           + "  ·  n" + screen.selectedSlot)
                        : ""
                    font.bold: true
                    font.pointSize: UI.fontSize * 0.85
                }
                Item { Layout.fillWidth: true }
                Button {
                    text: Tr.t("Remove")
                    flat: true
                    // Slot 0 is pinned to the output node: a graph with no sink
                    // renders nothing, so the firmware refuses to clear it.
                    visible: screen.selectedSlot !== Synth.graphOutSlot
                    onClicked: {
                        Synth.graphSetKind(screen.selectedSlot, 0)
                        screen.selectedSlot = -1
                    }
                }
                ToolButton {
                    text: ""  // xmark
                    font.family: App.fontAwesomeName
                    onClicked: screen.selectedSlot = -1
                }
            }

            Flow {
                id: paramFlow
                anchors.top: headerRow.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: 6
                spacing: 8

                Repeater {
                    // One control per parameter the selected node's kind
                    // declares; ParamControl picks the widget from the
                    // discovered type exactly as on every other page.
                    model: {
                        if (screen.selectedSlot < 0) return 0
                        var n = screen.nodeAt(screen.selectedSlot)
                        if (!n.valid) return 0
                        return Synth.graphKind(n.kind).params.length
                    }
                    delegate: ParamControl {
                        required property int index
                        paramId: Synth.graphNodeParamId(screen.selectedSlot, index)
                    }
                }
            }
        }
    }

    // --- node kind picker -------------------------------------------------

    Dialog {
        id: kindPicker
        anchors.centerIn: parent
        width: Math.min(screen.width * 0.9, Math.round(320 * UI.fontSize / 10))
        title: Tr.t("Add node")
        modal: true
        standardButtons: Dialog.Cancel

        ListView {
            implicitHeight: Math.min(screen.height * 0.5, contentHeight)
            width: parent.width
            clip: true
            model: Synth.graphKinds
            delegate: ItemDelegate {
                required property graphKindDesc modelData
                required property int index
                width: ListView.view.width
                // Kind 0 is "empty" and the out node is pinned to slot 0, so
                // neither is something to add.
                visible: index > 0 && modelData.name !== "out"
                height: visible ? implicitHeight : 0
                text: (modelData.name.length > 0 ? modelData.name : "?")
                      + "   ·   " + (modelData.rate === 1 ? Tr.t("audio") : Tr.t("control"))
                      + "   ·   " + modelData.cost
                onClicked: {
                    // Read at click time, not from screen.freeSlot: the picker
                    // stays open across the edit round trip, so the binding may
                    // already have moved on to the next slot.
                    var slot = Synth.graphFreeSlot()
                    if (slot >= 0) Synth.graphSetKind(slot, modelData.kind)
                    kindPicker.close()
                }
            }
        }
    }
}
