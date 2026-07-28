import QtQuick
import QtQuick.Controls.Material

import org.osynth.osyntho

// Two-octave playable keyboard with multi-touch (chords + glissando) via a
// MultiPointTouchArea over the key area. Sends NOTE_ON / NOTE_OFF test notes
// (which pass the arp/sequencer tap like played keys). Mouse acts as a single
// touch point on desktop; the computer keyboard also plays notes on desktop.
// Velocity and the optional latch/hold come from settings; octave shifts and the
// key-area height are adjusted here (height is persisted).
//
// KNOWN ISSUE + RESOLUTION (multi-finger chords on some Android devices):
// On Xiaomi (MIUI/HyperOS), spaced 3-finger chords (e.g. C-E-G) failed to sound
// while clustered/6-finger presses worked. A temporary on-screen touch-point
// readout proved the cause: Qt briefly received all 3 points, then Android sent
// a touch-CANCEL and grabbed them for a system 3-finger gesture. It is NOT a
// digitizer/hardware limit and NOT an app-logic bug — the points are taken away
// below the app. Fixes are device-side: turn off ALL MIUI 3-finger gestures
// (the screenshot toggle alone is insufficient; a reboot is often needed), or
// enable per-app full screen / Game Turbo. The app also requests immersive
// fullscreen (App.setAndroidImmersiveMode, gated by the android_immersive
// setting), which makes MIUI stop intercepting. The user confirmed disabling the
// Xiaomi optimizations resolved it.
Rectangle {
    id: root
    implicitHeight: dragHandle.height + controlBar.height + keyHeight
    color: Material.theme === Material.Dark ? "#101010" : "#DDDDDD"

    property int baseOctave: parseInt(App.setting("keyboard_octave"))
    readonly property int velocity: Math.max(1, Math.min(127, parseInt(App.setting("keyboard_velocity"))))
    readonly property bool hold: App.settingIsTrue("keyboard_hold")
    // Height of the playable key area (not counting the control strip). Persisted.
    property int keyHeight: Math.max(60, parseInt(App.setting("keyboard_height")) || 118)
    // How the user resizes the key area: a top drag "divider" (default) or a
    // "size" slider in the control strip. Reactive within the component.
    property string resizeMode: App.setting("keyboard_resize_mode") === "slider" ? "slider" : "divider"
    property int dividerThickness: Math.max(2, parseInt(App.setting("keyboard_divider_thickness")) || 5)
    property bool showNoteNames: App.settingIsTrue("keyboard_show_note_names")
    // Computer-keyboard wiring, both toggleable from the control strip (and
    // from Settings ▸ Keyboard). computerKeys gates the whole key capture;
    // topRowDrums decides whether the top two rows fire drum pads or play a
    // second octave. Desktop-only concerns — the buttons are hidden elsewhere.
    property bool computerKeys: App.settingIsTrue("keyboard_computer_keys")
    property bool topRowDrums: App.settingIsTrue("keyboard_top_row_drums")
    // App.setting() isn't reactive; re-read when the keyboard is reshown so a
    // Settings change applies without an app restart.
    onVisibleChanged: if (visible) {
        resizeMode = App.setting("keyboard_resize_mode") === "slider" ? "slider" : "divider"
        dividerThickness = Math.max(2, parseInt(App.setting("keyboard_divider_thickness")) || 5)
        showNoteNames = App.settingIsTrue("keyboard_show_note_names")
        computerKeys = App.settingIsTrue("keyboard_computer_keys")
        topRowDrums = App.settingIsTrue("keyboard_top_row_drums")
    }
    readonly property int minKeyHeight: 70
    readonly property int maxKeyHeight: 340
    function setKeyHeight(h) {
        keyHeight = Math.round(Math.max(minKeyHeight, Math.min(maxKeyHeight, h)))
        App.saveSetting("keyboard_height", keyHeight)
    }

    // Momentary notes currently sounding (note -> true), reassigned for bindings.
    property var activeNotes: ({})
    // Latched notes (hold mode), reassigned for bindings.
    property var held: ({})

    readonly property var whiteSemis: [0, 2, 4, 5, 7, 9, 11]
    // Black keys within an octave: [white index it sits after, semitone].
    readonly property var blackDefs: [[0, 1], [1, 3], [3, 6], [4, 8], [5, 10]]

    function whiteMidi(i) {
        var oct = Math.floor(i / 7)
        return 12 * (baseOctave + 1) + 12 * oct + whiteSemis[i % 7]
    }
    readonly property var semiNames: ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    // White keys get name + octave ("C4"); black keys just the sharp ("C#"),
    // since they are too narrow for the octave number too.
    function noteName(midi, withOctave) {
        var name = semiNames[midi % 12]
        return withOctave ? name + (Math.floor(midi / 12) - 1) : name
    }
    function isActive(n) { return activeNotes[n] === true || held[n] === true }

    function noteOn(n) { Synth.noteOn(n, velocity) }
    function noteOff(n) { Synth.noteOff(n) }

    // Right-click (desktop) / long-press (touch): choose the note the
    // sequencer grid writes into an empty step. Deliberately does not sound
    // the key on the right-click path — picking is not playing.
    function pickNote(n) {
        if (n < 0 || n > 127) return
        UI.paintNote = n
    }

    function setHeld(n, on) {
        var h = {}
        for (var k in held) h[k] = held[k]
        if (on) h[n] = true; else delete h[n]
        held = h
    }
    function setActive(n, on) {
        var a = {}
        for (var k in activeNotes) a[k] = activeNotes[k]
        if (on) a[n] = true; else delete a[n]
        activeNotes = a
    }
    function toggleLatch(n) {
        if (held[n]) { setHeld(n, false); noteOff(n) }
        else { setHeld(n, true); noteOn(n) }
    }

    // A sounding note is ended by the matching key-up / touch-release. Rewiring
    // the computer keys under a held key eats that release (capture stops, or
    // the key becomes a drum pad), which would strand the note — so drop what
    // is sounding on every wiring change. Latched notes are left alone: outliving
    // the key that started them is the whole point of latch.
    function releaseSounding() {
        for (var n in activeNotes) noteOff(parseInt(n))
        activeNotes = ({})
    }

    function setComputerKeys(on) {
        if (computerKeys === on) return
        releaseSounding()
        computerKeys = on
        App.saveSetting("keyboard_computer_keys", on ? "true" : "false")
    }

    // Read back by App's key filter on every keystroke, so the switch is live.
    function setTopRowDrums(on) {
        if (topRowDrums === on) return
        releaseSounding()
        topRowDrums = on
        App.saveSetting("keyboard_top_row_drums", on ? "true" : "false")
    }

    // Computer-keyboard piano (desktop): App's global key filter emits semitone
    // offsets; apply the base octave + velocity here. Capture is enabled only
    // while this keyboard is visible on a desktop platform, and only while the
    // user leaves the computer-keys toggle on.
    Component.onCompleted: App.keyboardCaptureEnabled = Qt.binding(function() {
        return root.visible && App.isDesktop() && root.computerKeys
    })
    Component.onDestruction: App.keyboardCaptureEnabled = false

    Connections {
        target: App
        enabled: root.visible
        function onComputerKeyPressed(semitone) {
            var n = 12 * (root.baseOctave + 1) + semitone
            if (root.hold) {
                root.toggleLatch(n)
            } else if (!root.activeNotes[n]) {
                root.noteOn(n)
                root.setActive(n, true)
            }
        }
        function onComputerKeyReleased(semitone) {
            if (root.hold) return
            var n = 12 * (root.baseOctave + 1) + semitone
            root.noteOff(n)
            root.setActive(n, false)
        }
        // Focus went to a text field, so the releases are no longer coming.
        function onComputerKeysAllReleased() { root.releaseSounding() }
    }

    // Which note is under a point in the key area's local coords.
    function noteForPos(x, y) {
        var wkW = keyArea.width / 14
        if (y < keyArea.height * 0.62) {
            for (var i = 0; i < 10; i++) {
                var oct = Math.floor(i / 5)
                var d = blackDefs[i % 5]
                var gw = oct * 7 + d[0]
                var bw = wkW * 0.62
                var bx = (gw + 1) * wkW - bw / 2
                if (x >= bx && x <= bx + bw)
                    return 12 * (baseOctave + 1) + 12 * oct + d[1]
            }
        }
        var wi = Math.floor(x / wkW)
        if (wi < 0) wi = 0
        if (wi > 13) wi = 13
        return whiteMidi(wi)
    }

    // Drag divider: grab the top edge to resize the key area. Present only in
    // "divider" mode; collapses to zero height in "slider" mode.
    Rectangle {
        id: dragHandle
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: root.resizeMode === "divider" ? root.dividerThickness : 0
        visible: height > 0
        color: dragArea.pressed ? Material.accent
                                : (Material.theme === Material.Dark ? "#3A3A3A" : "#B4B4B4")

        // Center grip line (a thin bar leaves no room for stacked lines).
        Rectangle {
            anchors.centerIn: parent
            width: 40
            height: Math.max(1, Math.min(2, parent.height - 2))
            radius: height / 2
            color: Material.theme === Material.Dark ? "#999999" : "#666666"
        }

        // A slightly taller invisible hit target so a thin divider is still easy
        // to grab (especially on touch). Extends downward from the top edge only
        // (never up into the SwipeView content above); the control-strip buttons
        // sit on top, so they keep priority where they overlap.
        MouseArea {
            id: dragArea
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width
            height: Math.max(parent.height, 20)
            cursorShape: Qt.SizeVerCursor
            property real startSceneY: 0
            property int startHeight: 0
            onPressed: (mouse) => {
                startSceneY = mapToItem(null, mouse.x, mouse.y).y
                startHeight = root.keyHeight
            }
            onPositionChanged: (mouse) => {
                // Scene coords are stable while the keyboard resizes; dragging up
                // (smaller y) grows the key area.
                var sceneY = mapToItem(null, mouse.x, mouse.y).y
                root.setKeyHeight(startHeight + (startSceneY - sceneY))
            }
        }
    }

    // Control strip: octave shift (left) and key-area size slider (right, only in
    // "slider" resize mode).
    Item {
        id: controlBar
        height: 30
        anchors.top: dragHandle.bottom
        anchors.left: parent.left
        anchors.right: parent.right

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6

            ToolButton {
                text: "−"  // minus
                font.bold: true
                width: 34
                onClicked: if (root.baseOctave > 0) { root.baseOctave--; App.saveSetting("keyboard_octave", root.baseOctave) }
            }
            // Mini piano pictogram (FontAwesome Free has no piano glyph, so
            // it is drawn: 5 white keys with the C#/D#/F# black-key pattern).
            // Fits the fixed-height control strip.
            Item {
                id: pianoIcon
                width: 26
                height: 15
                anchors.verticalCenter: parent.verticalCenter
                Row {
                    anchors.fill: parent
                    Repeater {
                        model: 5
                        Rectangle {
                            width: pianoIcon.width / 5
                            height: pianoIcon.height
                            radius: 1
                            color: "white"
                            border.color: "#666666"
                            border.width: 1
                        }
                    }
                }
                Repeater {
                    // white-key boundaries carrying a black key (the gap at 3
                    // is what makes it read as a piano octave)
                    model: [1, 2, 4]
                    Rectangle {
                        required property var modelData
                        x: modelData * pianoIcon.width / 5 - width / 2
                        width: pianoIcon.width / 9
                        height: pianoIcon.height * 0.62
                        color: "#111111"
                    }
                }
            }
            Label {
                text: root.baseOctave
                anchors.verticalCenter: parent.verticalCenter
                color: Material.foreground
                font.pointSize: UI.fontSize * 0.75
            }
            ToolButton {
                text: "+"
                font.bold: true
                width: 34
                onClicked: if (root.baseOctave < 8) { root.baseOctave++; App.saveSetting("keyboard_octave", root.baseOctave) }
            }

            // Computer-keyboard wiring, next to the octave controls because it
            // is the other thing you rewire mid-take. Desktop only — the key
            // capture itself is desktop-gated, so on a phone these would be two
            // buttons that do nothing.
            //
            // Deliberately not `checkable`: a Button assigning its own `checked`
            // would break the binding to the root property, and the setting is
            // also editable from Settings ▸ Keyboard. The property stays the one
            // source of truth and the buttons only render it.
            Rectangle {
                visible: App.isDesktop()
                width: 1
                height: 18
                anchors.verticalCenter: parent.verticalCenter
                color: Material.foreground
                opacity: 0.2
            }
            ToolButton {
                visible: App.isDesktop()
                width: 34
                text: "\uf11c"  // keyboard
                font.family: App.fontAwesomeName
                font.weight: Font.Black  // solid face
                font.pointSize: UI.fontSize * 0.95
                highlighted: root.computerKeys
                opacity: root.computerKeys ? 1.0 : 0.45
                onClicked: root.setComputerKeys(!root.computerKeys)
                ToolTip.visible: hovered
                ToolTip.text: root.computerKeys
                              ? t.t("Computer keys play the synth. Click to release them to the app.")
                              : t.t("Computer keys are off. Click to play the synth from them.")
            }
            ToolButton {
                visible: App.isDesktop()
                width: 34
                text: "\uf569"  // drum
                font.family: App.fontAwesomeName
                font.weight: Font.Black  // solid face
                font.pointSize: UI.fontSize * 0.95
                // Nothing reaches the drum mapping while capture is off.
                enabled: root.computerKeys  // the style dims a disabled button
                highlighted: root.topRowDrums && root.computerKeys
                opacity: root.topRowDrums ? 1.0 : 0.45
                onClicked: root.setTopRowDrums(!root.topRowDrums)
                ToolTip.visible: hovered
                ToolTip.text: root.topRowDrums
                              ? t.t("Q…I and 1…8 fire the drum pads. Click to play a second octave instead.")
                              : t.t("Q…I and 1…8 play a second octave. Click to fire the drum pads instead.")
            }

            Label {
                text: root.hold ? "· " + t.t("hold") : ""
                anchors.verticalCenter: parent.verticalCenter
                color: Material.accent
                font.pointSize: UI.fontSize * 0.75
            }
        }

        Row {
            visible: root.resizeMode === "slider"
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4

            Label {
                text: t.t("size")
                anchors.verticalCenter: parent.verticalCenter
                color: Material.foreground
                opacity: 0.7
                font.pointSize: UI.fontSize * 0.72
            }
            Slider {
                id: sizeSlider
                width: 120
                anchors.verticalCenter: parent.verticalCenter
                from: root.minKeyHeight
                to: root.maxKeyHeight
                value: root.keyHeight
                onMoved: root.setKeyHeight(value)
            }
        }
    }

    // Playable key area.
    Item {
        id: keyArea
        anchors.top: controlBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        // White keys
        Row {
            anchors.fill: parent
            Repeater {
                model: 14
                delegate: Rectangle {
                    required property int index
                    readonly property int midi: root.whiteMidi(index)
                    width: keyArea.width / 14
                    height: keyArea.height
                    color: root.isActive(midi) ? Material.accent : "white"
                    border.color: "#888888"
                    border.width: 1

                    Text {
                        visible: root.showNoteNames
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 4
                        text: root.noteName(midi, true)
                        color: root.isActive(midi) ? "white" : "#777777"
                        font.pointSize: Math.max(7, UI.fontSize * 0.68)
                    }

                    // Marks the note the sequencer grid will write.
                    Rectangle {
                        visible: UI.paintNote === parent.midi
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: parent.top
                        anchors.topMargin: 3
                        width: 7; height: 7; radius: 3.5
                        color: Material.accent
                        border.width: 1
                        border.color: "#ffffff"
                    }
                }
            }
        }

        // Black keys (overlaid)
        Repeater {
            model: 10
            delegate: Rectangle {
                required property int index
                readonly property int oct: Math.floor(index / 5)
                readonly property var def: root.blackDefs[index % 5]
                readonly property int globalWhite: oct * 7 + def[0]
                readonly property int midi: 12 * (root.baseOctave + 1) + 12 * oct + def[1]
                readonly property real wk: keyArea.width / 14
                width: wk * 0.62
                height: keyArea.height * 0.62
                x: (globalWhite + 1) * wk - width / 2
                y: 0
                z: 2
                radius: 2
                color: root.isActive(midi) ? Material.accent : "#111111"
                border.color: "#000000"

                Text {
                    visible: root.showNoteNames
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 3
                    text: root.noteName(midi, false)
                    color: "white"
                    opacity: 0.85
                    font.pointSize: Math.max(6, UI.fontSize * 0.55)
                }

                Rectangle {
                    visible: UI.paintNote === parent.midi
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.topMargin: 3
                    width: 7; height: 7; radius: 3.5
                    color: Material.accent
                    border.width: 1
                    border.color: "#ffffff"
                }
            }
        }

        // Right-click picks the sequencer's paint note without sounding the
        // key. It sits above the play area but accepts *only* the right
        // button, so left presses are not accepted here and fall through to
        // the MultiPointTouchArea below. Touch never reaches this (a touch is
        // synthesised as a left press) — the hold gesture in the touch area
        // covers that case.
        MouseArea {
            anchors.fill: parent
            z: 4
            acceptedButtons: Qt.RightButton
            onPressed: (mouse) => root.pickNote(root.noteForPos(mouse.x, mouse.y))
        }

        MultiPointTouchArea {
            id: mpta
            anchors.fill: parent
            z: 3
            mouseEnabled: true
            minimumTouchPoints: 1
            maximumTouchPoints: 10

            // pointId -> midi note currently produced by that touch point.
            property var pointNotes: ({})

            // Long-press on touch = the right-click gesture (Android/iOS have
            // no second button). Armed only for a single stationary point, so
            // it never fires during a chord or a glissando; the note sounds
            // normally first, which doubles as the audition.
            property int holdNote: -1
            Timer {
                id: holdTimer
                interval: 500
                onTriggered: if (mpta.holdNote >= 0) root.pickNote(mpta.holdNote)
            }

            function trackHold(points) {
                if (points.length !== 1) {
                    holdNote = -1
                    holdTimer.stop()
                    return
                }
                const n = root.noteForPos(points[0].x, points[0].y)
                if (n !== holdNote) {   // moved to another key: restart the clock
                    holdNote = n
                    holdTimer.restart()
                }
            }

            // Latch mode (optional, via the keyboard_hold setting): toggle each
            // newly pressed point's note; ignore motion.
            onPressed: (points) => {
                if (!root.hold) return
                for (var i = 0; i < points.length; i++)
                    root.toggleLatch(root.noteForPos(points[i].x, points[i].y))
            }

            // Momentary play: maintain the sounding set from all active points,
            // supporting chords and glissando (a point sliding to a new key).
            onTouchUpdated: (points) => {
                mpta.trackHold(points)   // both modes, before the latch bail-out
                if (root.hold) return
                var newActive = {}
                var newPointNotes = {}
                for (var i = 0; i < points.length; i++) {
                    var n = root.noteForPos(points[i].x, points[i].y)
                    newPointNotes[points[i].pointId] = n
                    newActive[n] = true
                }
                for (var na in newActive)
                    if (!root.activeNotes[na]) root.noteOn(parseInt(na))
                for (var oa in root.activeNotes)
                    if (!newActive[oa]) root.noteOff(parseInt(oa))
                root.activeNotes = newActive
                pointNotes = newPointNotes
            }
        }
    }
}
