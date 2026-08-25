import QtQuick
import QtQuick.Controls.Material

import org.osynth.osyntho

// 4x4 velocity-sensitive sample pads, sitting to the left of the on-screen
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
// too.
//
// Layout is MPC order — **slot 0 is bottom-left**, rows filling upward. That
// is not decoration: the factory kit is ordered kick, kick.tight, snare,
// snare.tight, stick, clap, hats…, toms…, crash, ride, so bottom-up puts the
// kick and snare under your thumbs and the cymbals at the top, which is how a
// kit is actually laid out and how anyone who has touched a groovebox expects
// to find it.
//
// Velocity comes from where in the pad you hit: the lower edge is soft, the
// top is hard. A touchscreen reports no pressure, and a fixed velocity makes
// programmed drums sound like a machine — this gives dynamics with no extra
// control to set up. Hitting dead centre gives the keyboard's velocity
// setting, so the two surfaces agree by default.
Rectangle {
    id: root

    property int columns: 4
    property int rows: 4
    readonly property int padCount: columns * rows

    // The same setting the keyboard uses, so both surfaces feel alike; it is
    // the velocity a centre hit produces.
    //
    // App.setting() is a plain invokable, so this captures nothing and would
    // otherwise be sampled once at creation — a velocity change in Settings
    // never reached the pads until the app was restarted. Re-read on the write
    // signal rather than on visibility: the pads are played from the computer
    // keys while hidden, which is the whole point of having them there.
    property int baseVelocity:
        Math.max(1, Math.min(127, parseInt(App.setting("keyboard_velocity")) || 100))

    Connections {
        target: App
        function onSettingChanged(name: string): void {
            if (name === "keyboard_velocity")
                root.baseVelocity = Math.max(1, Math.min(127,
                    parseInt(App.setting("keyboard_velocity")) || 100))
        }
    }

    color: Material.theme === Material.Dark ? "#101010" : "#DDDDDD"

    // Slot for a pad position. Row 0 is the *top* row visually, so the bottom
    // row (rows-1) must map to the lowest slots.
    function slotFor(row: int, col: int): int {
        return (root.rows - 1 - row) * root.columns + col
    }

    function slotName(slot: int): string {
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
    }

    // Vertical position within a pad -> velocity. Top edge = 127, bottom edge
    // scales down to a quarter of the base, centre = the base velocity.
    function velocityAt(yFraction: real): int {
        const f = Math.max(0, Math.min(1, 1 - yFraction))  // 0 bottom, 1 top
        const v = f < 0.5 ? root.baseVelocity * (0.25 + 1.5 * f)
                          : root.baseVelocity + (127 - root.baseVelocity) * (2 * f - 1)
        return Math.max(1, Math.min(127, Math.round(v)))
    }

    // Pads lit by a touch point or a recent hit, as {slot: true}. Reassigned
    // rather than mutated so the delegates' bindings re-evaluate.
    property var litPads: ({})

    // Whether the bound kit can be recorded into. Drives how an empty pad is
    // drawn: a hole on the read-only factory kit, a numbered outline on one
    // you can sample into.
    readonly property bool kitRecordable: {
        if (!UI.samplerAvailable) return false
        const list = Synth.kits
        for (let i = 0; i < list.length; ++i) {
            if (list[i].index === Synth.currentKit) return list[i].user === true
        }
        return false
    }

    function setLit(slot: int, on: bool): void {
        var m = {}
        for (var k in root.litPads) m[k] = root.litPads[k]
        if (on) m[slot] = true
        else delete m[slot]
        root.litPads = m
    }

    // The action row. Hidden entirely on a firmware with no recorder, so the
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
                text: "\uf111"  // circle
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
                text: "\uf55a"  // eraser
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
                text: "\uf0e2"  // arrow-rotate-left
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
        anchors.margins: 3
        columns: root.columns
        rows: root.rows
        spacing: 3

        readonly property real cellW: (width - spacing * (root.columns - 1)) / root.columns
        readonly property real cellH: (height - spacing * (root.rows - 1)) / root.rows

        Repeater {
            model: root.padCount

            delegate: Rectangle {
                id: pad
                required property int index
                readonly property int row: Math.floor(index / root.columns)
                readonly property int col: index % root.columns
                readonly property int slot: root.slotFor(row, col)
                readonly property string label: root.slotName(slot)
                readonly property bool lit: root.litPads[slot] === true
                // A kit can declare fewer slots than there are pads. On a
                // recordable kit an empty pad is still a destination, so it is
                // drawn as one rather than as a hole.
                readonly property bool populated: root.slotFilled(slot)
                readonly property bool armed: UI.armedPad === slot && UI.samplerAvailable

                width: grid.cellW
                height: grid.cellH
                radius: 4
                color: !populated
                       ? Qt.rgba(Material.foreground.r, Material.foreground.g,
                                 Material.foreground.b,
                                 root.kitRecordable ? 0.07 : 0.04)
                       : lit ? Material.accent
                             : Qt.rgba(Material.foreground.r, Material.foreground.g,
                                       Material.foreground.b, 0.13)
                border.width: pad.armed ? 2 : (lit ? 0 : 1)
                border.color: pad.armed
                              ? (UI.samplerRecording ? "#c62828" : "#ef6c00")
                              : Qt.rgba(Material.foreground.r, Material.foreground.g,
                                        Material.foreground.b, 0.18)

                Behavior on color {
                    ColorAnimation { duration: pad.lit ? 0 : 140 }
                }

                Text {
                    anchors.centerIn: parent
                    width: parent.width - 6
                    horizontalAlignment: Text.AlignHCenter
                    text: pad.populated
                          ? pad.label
                          : (root.kitRecordable ? String(pad.slot + 1) : "")
                    elide: Text.ElideRight
                    font.pointSize: Math.max(6, UI.fontSize * 0.6)
                    font.bold: pad.lit
                    color: pad.lit ? "#ffffff" : Material.foreground
                    opacity: pad.populated ? 0.9 : 0.3
                }

                // Marks the pad whose note the sequencer grid will write on a
                // drum lane. Falls back to the kit's first slot, so there is
                // always exactly one marked pad rather than none.
                Rectangle {
                    visible: pad.populated && root.slotNote(pad.slot) === UI.drumNote
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.margins: 3
                    width: 6; height: 6; radius: 3
                    color: pad.lit ? "#ffffff" : Material.accent
                }
            }
        }
    }

    // One touch area over the whole grid rather than a MouseArea per pad: that
    // is what makes multi-pad hits (and drags across pads) work, exactly like
    // the keyboard's key area.
    // Right-click picks the paint note instead of firing the pad. Accepts only
    // the right button, so left presses are not accepted here and fall through
    // to the touch area below; touch never reaches it (a touch arrives as a
    // left press), which the hold gesture below covers.
    MouseArea {
        anchors.fill: grid
        z: 4
        acceptedButtons: Qt.RightButton
        onPressed: (mouse) => {
            const slot = mpta.padAt(mouse.x, mouse.y)
            if (slot >= 0) root.pickPad(slot)
        }
    }

    MultiPointTouchArea {
        id: mpta
        anchors.fill: grid
        z: 3
        mouseEnabled: true
        minimumTouchPoints: 1
        maximumTouchPoints: 10

        // pointId -> slot that point is currently holding down.
        property var pointSlots: ({})

        // Long-press = the right-click gesture on Android/iOS. Armed only for
        // a single stationary point, so a roll or a two-hand hit never trips
        // it. The pad has already sounded by then, which doubles as an
        // audition of what you are selecting.
        property int holdSlot: -1
        Timer {
            id: holdTimer
            interval: 500
            onTriggered: if (mpta.holdSlot >= 0) root.pickPad(mpta.holdSlot)
        }

        function trackHold(points) {
            if (points.length !== 1) {
                holdSlot = -1
                holdTimer.stop()
                return
            }
            const s = mpta.padAt(points[0].x, points[0].y)
            if (s !== holdSlot) {   // moved to another pad: restart the clock
                holdSlot = s
                holdTimer.restart()
            }
        }

        function padAt(x: real, y: real): int {
            const col = Math.floor(x / (grid.cellW + grid.spacing))
            const row = Math.floor(y / (grid.cellH + grid.spacing))
            if (col < 0 || col >= root.columns || row < 0 || row >= root.rows) return -1
            return root.slotFor(row, col)
        }

        function fractionInPad(y: real): real {
            const pitch = grid.cellH + grid.spacing
            const row = Math.floor(y / pitch)
            return Math.max(0, Math.min(1, (y - row * pitch) / grid.cellH))
        }

        // Which point, if any, is holding a take open. Only one can: the
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
        }

        onTouchUpdated: (points) => {
            mpta.trackHold(points)
            var m = {}
            for (var i = 0; i < points.length; ++i) {
                const p = points[i]
                // The finger holding a take open may wander off its pad; the
                // recording follows the button, not the position.
                if (p.pointId === mpta.recPoint) continue
                const slot = mpta.padAt(p.x, p.y)
                if (slot < 0) continue
                m[p.pointId] = slot
                // Only fire when a point crosses into a different pad.
                if (mpta.pointSlots[p.pointId] !== slot) {
                    root.hit(slot, root.velocityAt(mpta.fractionInPad(p.y)))
                }
            }
            mpta.pointSlots = m
        }
    }

    signal padHit(int slot, int velocity)

    // MIDI note a slot answers to, from the kit's note map, or -1.
    function slotNote(slot: int): int {
        const slots = Synth.kitSlots
        for (let i = 0; i < slots.length; ++i) {
            if (slots[i].slot === slot) return slots[i].note
        }
        return -1
    }

    // Right-click (desktop) / long-press (touch): pick the note the sequencer
    // grid writes, using the slot's own note from the kit map. On a drum lane
    // bound to a fixed slot the note is ignored, but on a lane set to "the
    // step's note picks the slot" this is exactly how you choose which drum a
    // step plays.
    function pickPad(slot: int): void {
        const n = root.slotNote(slot)
        if (n >= 0) UI.paintDrumNote = n
    }

    function hit(slot: int, velocity: int): void {
        // A pad the current kit leaves empty makes no sound and does not
        // flash. But an *unknown* kit — the slot list has not arrived yet —
        // must still play: silence with no explanation is worse than a hit on
        // a slot that turns out to be empty.
        if (Synth.kitSlots.length > 0 && !root.slotFilled(slot)) return
        Synth.triggerDrum(slot, velocity)
        root.setLit(slot, true)
        unlight.restart()
        root.padHit(slot, velocity)
    }

    // Pads light on hit and fade back; there is no note-off to key it to.
    Timer {
        id: unlight
        interval: 90
        onTriggered: root.litPads = ({})
    }

    // Computer keyboard (desktop): the top two rows fire pads when the
    // keyboard_top_row_drums setting is on. App does the key mapping and the
    // gating, so this just plays what it is told — including while the pads
    // are hidden, which is the point of having them on the keys.
    Connections {
        target: App
        function onComputerDrumPadPressed(pad: int): void {
            root.hit(pad, root.baseVelocity)
        }
    }
}
