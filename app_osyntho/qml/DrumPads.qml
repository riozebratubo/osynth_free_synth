import QtQuick
import QtQuick.Controls.Material

import org.osynth.osyntho

// 4x4 velocity-sensitive drum pads, sitting to the left of the on-screen
// keyboard so a beat and a part can be played without changing pages.
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
    readonly property int baseVelocity:
        Math.max(1, Math.min(127, parseInt(App.setting("keyboard_velocity")) || 100))

    color: Material.theme === Material.Dark ? "#101010" : "#DDDDDD"

    // Slot for a pad position. Row 0 is the *top* row visually, so the bottom
    // row (rows-1) must map to the lowest slots.
    function slotFor(row, col) {
        return (root.rows - 1 - row) * root.columns + col
    }

    function slotName(slot) {
        const slots = Synth.kitSlots
        for (let i = 0; i < slots.length; ++i) {
            if (slots[i].slot === slot) return slots[i].name
        }
        return ""
    }

    // Vertical position within a pad -> velocity. Top edge = 127, bottom edge
    // scales down to a quarter of the base, centre = the base velocity.
    function velocityAt(yFraction) {
        const f = Math.max(0, Math.min(1, 1 - yFraction))  // 0 bottom, 1 top
        const v = f < 0.5 ? root.baseVelocity * (0.25 + 1.5 * f)
                          : root.baseVelocity + (127 - root.baseVelocity) * (2 * f - 1)
        return Math.max(1, Math.min(127, Math.round(v)))
    }

    // Pads lit by a touch point or a recent hit, as {slot: true}. Reassigned
    // rather than mutated so the delegates' bindings re-evaluate.
    property var litPads: ({})

    function setLit(slot, on) {
        var m = {}
        for (var k in root.litPads) m[k] = root.litPads[k]
        if (on) m[slot] = true
        else delete m[slot]
        root.litPads = m
    }

    Grid {
        id: grid
        anchors.fill: parent
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
                // A kit can declare fewer slots than there are pads.
                readonly property bool populated: label !== ""

                width: grid.cellW
                height: grid.cellH
                radius: 4
                color: !populated
                       ? Qt.rgba(Material.foreground.r, Material.foreground.g,
                                 Material.foreground.b, 0.04)
                       : lit ? Material.accent
                             : Qt.rgba(Material.foreground.r, Material.foreground.g,
                                       Material.foreground.b, 0.13)
                border.width: lit ? 0 : 1
                border.color: Qt.rgba(Material.foreground.r, Material.foreground.g,
                                      Material.foreground.b, 0.18)

                Behavior on color {
                    ColorAnimation { duration: pad.lit ? 0 : 140 }
                }

                Text {
                    anchors.centerIn: parent
                    width: parent.width - 6
                    horizontalAlignment: Text.AlignHCenter
                    text: pad.label
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

        function padAt(x, y) {
            const col = Math.floor(x / (grid.cellW + grid.spacing))
            const row = Math.floor(y / (grid.cellH + grid.spacing))
            if (col < 0 || col >= root.columns || row < 0 || row >= root.rows) return -1
            return root.slotFor(row, col)
        }

        function fractionInPad(y) {
            const pitch = grid.cellH + grid.spacing
            const row = Math.floor(y / pitch)
            return Math.max(0, Math.min(1, (y - row * pitch) / grid.cellH))
        }

        // A drum is a one-shot: it fires on touch-down and there is nothing to
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
        }

        onTouchUpdated: (points) => {
            mpta.trackHold(points)
            var m = {}
            for (var i = 0; i < points.length; ++i) {
                const p = points[i]
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
    function slotNote(slot) {
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
    function pickPad(slot) {
        const n = root.slotNote(slot)
        if (n >= 0) UI.paintDrumNote = n
    }

    function hit(slot, velocity) {
        // A pad the current kit leaves empty makes no sound and does not
        // flash. But an *unknown* kit — the slot list has not arrived yet —
        // must still play: silence with no explanation is worse than a hit on
        // a slot that turns out to be empty.
        if (Synth.kitSlots.length > 0 && root.slotName(slot) === "") return
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
        function onComputerDrumPadPressed(pad) {
            root.hit(pad, root.baseVelocity)
        }
    }
}
