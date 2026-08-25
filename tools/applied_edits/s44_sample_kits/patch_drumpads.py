"""S44: give DrumPads.qml a top action row and the arm-then-pad record gesture."""
import io

P = 'app_osyntho/qml/DrumPads.qml'
s = io.open(P, encoding='utf-8').read()
if 'padAction' in s:
    raise SystemExit('already patched')

# ---- 1. the header comment -------------------------------------------------
old = """// 4x4 velocity-sensitive drum pads, sitting to the left of the on-screen
// keyboard so a beat and a part can be played without changing pages."""
new = """// 4x4 velocity-sensitive sample pads, sitting to the left of the on-screen
// keyboard so a beat and a part can be played without changing pages.
//
// Since S44 there is a row of actions above them: Record, Erase and Undo. They
// are here and not only on the Sample kits page because sampling is something
// you do *while playing* -- the whole point of the pre-roll ring in the
// firmware is that you can catch something the moment you hear it, and having
// to change pages first would defeat that as thoroughly as a slow button.
//
// Record and Erase arm rather than act: press one, then press the pad it
// applies to. That second press is the confirmation for an operation that
// overwrites a sample, and for Record it is also the take itself -- the pad is
// held for as long as you want to capture. The armed state lives in the UI
// singleton, so pressing Record here lights the button on the Sample kits page
// too."""
assert old in s, 'header'
s = s.replace(old, new, 1)

# ---- 2. leave room for the action row --------------------------------------
old = """    Grid {
        id: grid
        anchors.fill: parent
        anchors.margins: 3"""
new = """    // The action row. Hidden entirely on a firmware with no recorder, so the
    // pads keep their full height on a build that cannot sample.
    Row {
        id: actions
        visible: UI.samplerAvailable
        height: visible ? Math.max(22, root.height * 0.13) : 0
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 3
        spacing: 3

        readonly property real cellW: (width - spacing * 2) / 3

        // Record. Lights amber while armed and waiting for a pad, red while a
        // take is actually running -- two different states that a single
        // colour would have conflated, and the difference matters because the
        // firmware may still be waiting on smp.thresh.
        Rectangle {
            width: actions.cellW
            height: actions.height
            radius: 4
            color: UI.samplerRecording
                   ? "#c62828"
                   : (UI.padAction === "record"
                      ? "#ef6c00"
                      : Qt.rgba(Material.foreground.r, Material.foreground.g,
                                Material.foreground.b, 0.13))
            border.width: 1
            border.color: Qt.rgba(Material.foreground.r, Material.foreground.g,
                                  Material.foreground.b, 0.18)
            Text {
                anchors.centerIn: parent
                text: "\\uf111"  // circle
                font.family: App.fontAwesomeName
                font.weight: Font.Black
                font.pointSize: Math.max(6, UI.fontSize * 0.62)
                color: (UI.samplerRecording || UI.padAction === "record")
                       ? "#ffffff" : Material.foreground
                opacity: 0.9
            }
            MouseArea {
                anchors.fill: parent
                onClicked: {
                    if (UI.samplerRecording) { UI.stopRecord(); return }
                    UI.padAction = UI.padAction === "record" ? "" : "record"
                }
            }
        }

        // Erase, same arming gesture.
        Rectangle {
            width: actions.cellW
            height: actions.height
            radius: 4
            color: UI.padAction === "erase"
                   ? "#ef6c00"
                   : Qt.rgba(Material.foreground.r, Material.foreground.g,
                             Material.foreground.b, 0.13)
            border.width: 1
            border.color: Qt.rgba(Material.foreground.r, Material.foreground.g,
                                  Material.foreground.b, 0.18)
            Text {
                anchors.centerIn: parent
                text: "\\uf55a"  // eraser
                font.family: App.fontAwesomeName
                font.weight: Font.Black
                font.pointSize: Math.max(6, UI.fontSize * 0.62)
                color: UI.padAction === "erase" ? "#ffffff" : Material.foreground
                opacity: 0.9
            }
            MouseArea {
                anchors.fill: parent
                onClicked: UI.padAction = UI.padAction === "erase" ? "" : "erase"
            }
        }

        // Undo needs no pad: it puts back whatever the last record, erase or
        // copy displaced, wherever that was.
        Rectangle {
            width: actions.cellW
            height: actions.height
            radius: 4
            color: Qt.rgba(Material.foreground.r, Material.foreground.g,
                           Material.foreground.b, 0.13)
            border.width: 1
            border.color: Qt.rgba(Material.foreground.r, Material.foreground.g,
                                  Material.foreground.b, 0.18)
            Text {
                anchors.centerIn: parent
                text: "\\uf0e2"  // arrow-rotate-left
                font.family: App.fontAwesomeName
                font.weight: Font.Black
                font.pointSize: Math.max(6, UI.fontSize * 0.62)
                color: Material.foreground
                opacity: 0.9
            }
            MouseArea {
                anchors.fill: parent
                onClicked: {
                    UI.setSmp("smp.undo", 1)
                    UI.padAction = ""
                }
            }
        }
    }

    Grid {
        id: grid
        anchors.top: actions.visible ? actions.bottom : parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 3"""
assert old in s, 'grid anchors'
s = s.replace(old, new, 1)

# ---- 3. mark the armed pad, and show empty pads as recordable --------------
old = """                // A kit can declare fewer slots than there are pads.
                readonly property bool populated: label !== \"\""""
new = """                // A kit can declare fewer slots than there are pads. On a
                // recordable kit an empty pad is still a destination, so it is
                // drawn as one rather than as a hole.
                readonly property bool populated: label !== ""
                readonly property bool armed: UI.armedPad === slot && UI.samplerAvailable"""
assert old in s, 'populated'
s = s.replace(old, new, 1)

old = """                border.width: lit ? 0 : 1
                border.color: Qt.rgba(Material.foreground.r, Material.foreground.g,
                                      Material.foreground.b, 0.18)"""
new = """                border.width: pad.armed ? 2 : (lit ? 0 : 1)
                border.color: pad.armed
                              ? (UI.samplerRecording ? "#c62828" : "#ef6c00")
                              : Qt.rgba(Material.foreground.r, Material.foreground.g,
                                        Material.foreground.b, 0.18)"""
assert old in s, 'border'
s = s.replace(old, new, 1)

# ---- 4. the gesture --------------------------------------------------------
old = """        // A drum is a one-shot: it fires on touch-down and there is nothing to
        // release, so only *new* points trigger. Dragging onto another pad
        // fires that one too (a roll), which is the useful behaviour.
        onPressed: (points) => {
            var m = mpta.pointSlots
            for (var i = 0; i < points.length; ++i) {
                const slot = mpta.padAt(points[i].x, points[i].y)
                if (slot < 0) continue
                m[points[i].pointId] = slot
                root.hit(slot, root.velocityAt(mpta.fractionInPad(points[i].y)))
            }
            mpta.pointSlots = m
        }"""
new = """        // Which point, if any, is holding a take open. Only one can: the
        // firmware has one recorder, and a second finger landing mid-take must
        // not silently re-aim it.
        property int recPoint: -1

        // A drum is a one-shot: it fires on touch-down and there is nothing to
        // release, so only *new* points trigger. Dragging onto another pad
        // fires that one too (a roll), which is the useful behaviour.
        //
        // Unless an action is armed, in which case the first press is consumed
        // by it instead of sounding: you are aiming at a destination, and
        // hearing the sample you are about to replace is not helpful.
        onPressed: (points) => {
            var m = mpta.pointSlots
            for (var i = 0; i < points.length; ++i) {
                const slot = mpta.padAt(points[i].x, points[i].y)
                if (slot < 0) continue
                if (UI.padAction === "record" && mpta.recPoint < 0) {
                    mpta.recPoint = points[i].pointId
                    UI.startRecordInto(slot)
                    continue
                }
                if (UI.padAction === "erase") {
                    UI.erasePad(slot)
                    continue
                }
                m[points[i].pointId] = slot
                root.hit(slot, root.velocityAt(mpta.fractionInPad(points[i].y)))
            }
            mpta.pointSlots = m
        }

        // Touch-up. Two jobs, and neither existed before S44: close a take
        // that a held pad opened, and let go of a gate or loop pad. The second
        // is a no-op on a one-shot, which is every pad of every kit that
        // predates the play modes, so it can be sent unconditionally.
        onReleased: (points) => {
            for (var i = 0; i < points.length; ++i) {
                const id = points[i].pointId
                if (id === mpta.recPoint) {
                    mpta.recPoint = -1
                    UI.stopRecord()
                    continue
                }
                const held = mpta.pointSlots[id]
                if (held !== undefined) {
                    Synth.releaseDrum(held)
                    delete mpta.pointSlots[id]
                }
            }
            mpta.holdSlot = -1
            holdTimer.stop()
        }

        // A canceled gesture (the OS taking the touch, a call arriving) has to
        // close an open take as well -- otherwise the recorder runs to its
        // ceiling and commits whatever it caught.
        onCanceled: (points) => {
            if (mpta.recPoint >= 0) {
                mpta.recPoint = -1
                UI.stopRecord()
            }
            for (var k in mpta.pointSlots) Synth.releaseDrum(mpta.pointSlots[k])
            mpta.pointSlots = ({})
        }"""
assert old in s, 'onPressed'
s = s.replace(old, new, 1)

# The roll-across-pads path must not re-aim a running take either.
old = """        onTouchUpdated: (points) => {
            mpta.trackHold(points)
            var m = {}
            for (var i = 0; i < points.length; ++i) {
                const p = points[i]
                const slot = mpta.padAt(p.x, p.y)
                if (slot < 0) continue
                m[p.pointId] = slot"""
new = """        onTouchUpdated: (points) => {
            mpta.trackHold(points)
            var m = {}
            for (var i = 0; i < points.length; ++i) {
                const p = points[i]
                // The finger holding a take open may wander off its pad; the
                // recording follows the button, not the position.
                if (p.pointId === mpta.recPoint) continue
                const slot = mpta.padAt(p.x, p.y)
                if (slot < 0) continue
                m[p.pointId] = slot"""
assert old in s, 'onTouchUpdated'
s = s.replace(old, new, 1)

# ---- 5. an empty pad on a recordable kit is a destination, not silence -----
old = """    function hit(slot: int, velocity: int): void {
        // A pad the current kit leaves empty makes no sound and does not
        // flash. But an *unknown* kit — the slot list has not arrived yet —
        // must still play: silence with no explanation is worse than a hit on
        // a slot that turns out to be empty.
        if (Synth.kitSlots.length > 0 && root.slotName(slot) === "") return"""
new = """    function hit(slot: int, velocity: int): void {
        // A pad the current kit leaves empty makes no sound and does not
        // flash. But an *unknown* kit — the slot list has not arrived yet —
        // must still play: silence with no explanation is worse than a hit on
        // a slot that turns out to be empty.
        if (Synth.kitSlots.length > 0 && !root.slotFilled(slot)) return"""
assert old in s, 'hit'
s = s.replace(old, new, 1)

old = """    function slotName(slot: int): string {
        const slots = Synth.kitSlots
        for (let i = 0; i < slots.length; ++i) {
            if (slots[i].slot === slot) return slots[i].name
        }
        return ""
    }"""
new = """    function slotName(slot: int): string {
        const slots = Synth.kitSlots
        for (let i = 0; i < slots.length; ++i) {
            if (slots[i].slot === slot) return slots[i].name
        }
        return ""
    }

    // Whether the pad has audio in it. Since S44 the firmware says so directly
    // (`filled`), which matters because a *recorded* pad can be named and a
    // named one can be empty -- the two stopped being the same question the
    // moment pads became destinations you erase. Falls back to the name on a
    // firmware that does not report it.
    function slotFilled(slot: int): bool {
        const slots = Synth.kitSlots
        for (let i = 0; i < slots.length; ++i) {
            if (slots[i].slot === slot) {
                return slots[i].filled !== undefined ? slots[i].filled
                                                     : slots[i].name !== ""
            }
        }
        return false
    }"""
assert old in s, 'slotName'
s = s.replace(old, new, 1)

io.open(P, 'w', encoding='utf-8').write(s)
print('ok')
