#!/usr/bin/env python3
"""One-shot retype of qml/GraphScreen.qml onto the graphSlot / graphKindDesc
value types (src/graphtypes.h).

Kept rather than deleted because it is the exact record of what the QML
conversion changed, substitution by substitution: the QVariantMap field reads
that became typed property reads, the `in` -> `sources` rename forced by `in`
being a JavaScript operator, and the null checks that became `.valid` checks
once the model stopped being able to hand back null.

Idempotent only in the sense that it asserts: every substitution must match
exactly once, so re-running it on an already-converted file fails loudly
instead of corrupting it.

    python tools/retype_graph_qml.py
"""

import io
import os
import sys

PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "..", "app_osyntho", "qml", "GraphScreen.qml")

SUBS = [
    # --- the not-a-slot sentinel ------------------------------------------
    ("""    property int pendingSource: -1   // slot whose output jack is armed, or -1
    property int selectedSlot: -1    // node whose parameter panel is open
""",
     """    property int pendingSource: -1   // slot whose output jack is armed, or -1
    property int selectedSlot: -1    // node whose parameter panel is open

    // The "no such slot" answer nodeAt() hands back, and the only reason it
    // exists: a graphSlot is a value type, so it cannot be null, and a
    // default-constructed one already *is* the not-a-slot state — valid false,
    // kind 0, no sources. Never assigned; not readonly only because a
    // read-only property has to be initialised and the default is the point.
    property graphSlot emptySlot
"""),

    # --- geometry helpers --------------------------------------------------
    ("""    function nodeAt(slot: int): var {
        var list = Synth.graphNodes
        return (slot >= 0 && slot < list.length) ? list[slot] : null
    }
    function outJackPos(slot: int): var {
        var n = nodeAt(slot)
        if (!n) return null
        return { x: n.x + nodeW, y: n.y + nodeH / 2 }
    }
    function inJackPos(slot: int, port: int): var {
        var n = nodeAt(slot)
        if (!n) return null
        var k = Synth.graphKind(n.kind)
        var count = k.inputs ? k.inputs.length : 0
        if (count <= 0) return null""",
     """    function nodeAt(slot: int): graphSlot {
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
        if (count <= 0) return null"""),

    ("""    function kindName(kind: int): string {
        var k = Synth.graphKind(kind)
        return k && k.name ? k.name : "—"
    }
    function isAudio(kind: int): bool {
        var k = Synth.graphKind(kind)
        return k && k.rate === 1
    }""",
     """    function kindName(kind: int): string {
        var k = Synth.graphKind(kind)
        return k.name.length > 0 ? k.name : "—"
    }
    function isAudio(kind: int): bool {
        return Synth.graphKind(kind).rate === 1
    }"""),

    ("""        var n = nodeAt(slot)
        if (!n) return
        if (pendingSource < 0) {
            // Nothing armed: a tap on a patched jack unpatches it. This is the
            // only way to remove a cable, and it mirrors pulling one out.
            if (n.in[port] >= 0) Synth.graphConnect(slot, port, -1)""",
     """        var n = nodeAt(slot)
        if (!n.valid) return
        if (pendingSource < 0) {
            // Nothing armed: a tap on a patched jack unpatches it. This is the
            // only way to remove a cable, and it mirrors pulling one out.
            if (n.sources[port] >= 0) Synth.graphConnect(slot, port, -1)"""),

    # --- cable painter -----------------------------------------------------
    ("""                        var k = Synth.graphKind(n.kind)
                        var nin = k.inputs ? k.inputs.length : 0
                        for (var port = 0; port < nin; ++port) {
                            var src = n.in[port]
                            if (src === undefined || src < 0) continue""",
     """                        var k = Synth.graphKind(n.kind)
                        // The kind declares the ports and the slot carries a
                        // source for each one it knows about; a kind table that
                        // landed before the model can name more of them than
                        // the slot has entries for, which used to be what the
                        // `undefined` test here was standing in for.
                        var nin = Math.min(k.inputs.length, n.sources.length)
                        for (var port = 0; port < nin; ++port) {
                            var src = n.sources[port]
                            if (src < 0) continue"""),

    # --- node delegate -----------------------------------------------------
    ("""                delegate: Rectangle {
                    id: node
                    required property var modelData
                    required property int index

                    readonly property var kindDesc: Synth.graphKind(modelData.kind)""",
     """                delegate: Rectangle {
                    id: node
                    required property graphSlot modelData
                    required property int index

                    readonly property graphKindDesc kindDesc:
                        Synth.graphKind(modelData.kind)"""),

    ("""                            text: node.kindDesc.name ? node.kindDesc.name : "?\"""",
     """                            text: node.kindDesc.name.length > 0
                                  ? node.kindDesc.name : "?\""""),

    ("""                        model: node.kindDesc.inputs ? node.kindDesc.inputs : []
                        delegate: Item {
                            required property var modelData
                            required property int index""",
     """                        model: node.kindDesc.inputs
                        delegate: Item {
                            required property string modelData
                            required property int index"""),

    ("""                            readonly property bool patched:
                                node.modelData.in[index] >= 0""",
     """                            readonly property bool patched:
                                node.modelData.sources[index] >= 0"""),

    # --- parameter panel ---------------------------------------------------
    ("""                    model: {
                        if (screen.selectedSlot < 0) return []
                        var n = screen.nodeAt(screen.selectedSlot)
                        if (!n) return []
                        var k = Synth.graphKind(n.kind)
                        return k.params ? k.params.length : 0
                    }""",
     """                    model: {
                        if (screen.selectedSlot < 0) return 0
                        var n = screen.nodeAt(screen.selectedSlot)
                        if (!n.valid) return 0
                        return Synth.graphKind(n.kind).params.length
                    }"""),

    # --- kind picker -------------------------------------------------------
    ("""            delegate: ItemDelegate {
                required property var modelData
                required property int index""",
     """            delegate: ItemDelegate {
                required property graphKindDesc modelData
                required property int index"""),

    ("""                text: (modelData.name ? modelData.name : "?")""",
     """                text: (modelData.name.length > 0 ? modelData.name : "?")"""),
]


def main():
    with io.open(PATH, encoding="utf-8", newline="") as f:
        text = f.read()
    eol = "\r\n" if text.count("\r\n") else "\n"
    for old, new in SUBS:
        o = old.replace("\n", eol)
        if text.count(o) != 1:
            sys.exit("no unique match (%d) for: %s" % (text.count(o),
                                                       old.strip()[:70]))
        text = text.replace(o, new.replace("\n", eol))
    with io.open(PATH, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    print("GraphScreen.qml: %d substitutions applied" % len(SUBS))


if __name__ == "__main__":
    main()
