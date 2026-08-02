import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// 8-track loop recorder (firmware 0x06xx params). Track selection, transport
// (stop/play/rec), per-track filled indicators, clears, mix levels, a live
// position/time display and the cap-policy toggles: mono record (S19) and
// 4-track mode (S20) — each doubles the max loop length for the next set.
// The firmware drives loop.mode back to play when a punch-in completes and
// mirrors loop.filled / loop.len / loop.maxlen — plus loop.pos /
// loop.rectrk ~4x/s while the transport runs (S18) — all as regular param
// events, so this screen only ever reflects the synth's actual state.
Item {
    id: root

    property int idTrack: -1
    property int idMode: -1
    property int idClear: -1
    property int idFilled: -1
    property int idLen: -1
    property int idSave: -1
    property int idLoad: -1
    property int idPos: -1
    property int idRecTrk: -1

    property int idMono: -1
    property int idMaxLen: -1
    property int idTracks: -1
    // Start alignment (S24): both delay the moment a take actually begins.
    property int idSync: -1
    property int idCountIn: -1
    property int idArmed: -1
    // Beats left before an armed take starts; 0 when not waiting.
    property int armedBeats: 0
    property bool syncOn: false
    // Seeded to the firmware's default so the toggle does not flash the wrong
    // state in the window before loop.countin arrives; rebind() and the
    // paramChanged handler below take over as soon as it does.
    property bool countInOn: true

    property int curTrack: 1
    property int curMode: 0    // 0 stop, 1 play, 2 rec
    property int filledMask: 0
    property real loopLen: 0.0
    // Max loop length in seconds. S19+ firmware mirrors the live cap in
    // loop.maxlen (S20: a base cap, x2 for mono, x2 in 4-track mode); older
    // firmware registers its fixed cap as loop.len's max.
    //
    // The base is not a fixed 40 s any more: since the P4 target the firmware
    // sizes it from the free PSRAM it measures at boot, so it differs per
    // board (~38 s on an 8 MB S3, 160 s on a 32 MB P4, less on a 2 MB S3).
    // Nothing here hardcodes it — every text below reads this property, which
    // is fed by loop.maxlen's live value and by predictedMax() off its
    // registered default. The 12.0 is only a placeholder until a synth
    // connects and reports its own.
    property real maxLen: 12.0
    // Cap policies for the *next* loop set, latched when its first take
    // starts: mono record mode (loop.mono, S19; a load adopts the set's
    // format) and 4-track mode (loop.tracks, S20 — trades slots for cap).
    // (both default on in S30+ firmware; rebind() adopts whatever the
    // connected synth reports, so older firmware still shows stereo/8)
    property bool monoOn: true
    property bool fourTracks: true
    // Track the firmware is actually writing (loop.rectrk, S18): 0 while a
    // punch-in is armed but hasn't reached the loop start yet.
    property int recTrack: 0
    // Loop position (loop.pos, S18): the firmware reports it ~4x/s; between
    // reports a local clock advances from the last report's arrival time.
    // BLE batching makes the anchor lag a roughly constant ~0.1 s — fine
    // for a time display, and every report re-anchors (no drift).
    property real posBase: 0.0
    property double posAnchorMs: 0
    property real dispPos: 0.0
    readonly property bool posAvailable: idPos >= 0
    // Highest save/load slot (from the discovered param range: flash = 0,
    // SD = 7). Persistence row hidden when the firmware lacks the params.
    property int slotMax: 0
    readonly property bool canPersist: idSave >= 0

    // Firmware without the looper (classic ESP32: no PSRAM) registers none of
    // the loop.* params — the screen then shows a hint instead of controls.
    readonly property bool available: idMode >= 0

    function rebind() {
        idTrack = Synth.paramIdForName("loop.track")
        idMode = Synth.paramIdForName("loop.mode")
        idClear = Synth.paramIdForName("loop.clear")
        idFilled = Synth.paramIdForName("loop.filled")
        idLen = Synth.paramIdForName("loop.len")
        idSave = Synth.paramIdForName("loop.save")
        idLoad = Synth.paramIdForName("loop.load")
        idPos = Synth.paramIdForName("loop.pos")
        idRecTrk = Synth.paramIdForName("loop.rectrk")
        idMono = Synth.paramIdForName("loop.mono")
        idMaxLen = Synth.paramIdForName("loop.maxlen")
        idTracks = Synth.paramIdForName("loop.tracks")
        idSync = Synth.paramIdForName("loop.sync")
        idCountIn = Synth.paramIdForName("loop.countin")
        idArmed = Synth.paramIdForName("loop.armed")
        if (idArmed >= 0 && Synth.paramValueKnown(idArmed))
            armedBeats = Math.round(Synth.paramValue(idArmed))
        if (idSync >= 0 && Synth.paramValueKnown(idSync))
            syncOn = Synth.paramValue(idSync) > 0.5
        if (idCountIn >= 0 && Synth.paramValueKnown(idCountIn))
            countInOn = Synth.paramValue(idCountIn) > 0.5
        // Only adopt values the synth has actually reported. rebind() runs on
        // every paramsDiscovered, and paramValue() falls back to the
        // registered default when a value is still unknown — which for these
        // firmware-written status params is a plausible-looking 0. Without the
        // guard a late discovery pass silently resets loop.filled to "empty"
        // and the clear buttons grey out over a loop that exists.
        if (idTrack >= 0 && Synth.paramValueKnown(idTrack))
            curTrack = Math.round(Synth.paramValue(idTrack))
        if (idMode >= 0 && Synth.paramValueKnown(idMode))
            curMode = Math.round(Synth.paramValue(idMode))
        if (idFilled >= 0 && Synth.paramValueKnown(idFilled))
            filledMask = Math.round(Synth.paramValue(idFilled))
        if (idLen >= 0 && Synth.paramValueKnown(idLen))
            loopLen = Synth.paramValue(idLen)
        if (idMono >= 0) monoOn = Synth.paramValue(idMono) > 0.5
        if (idTracks >= 0) fourTracks = Synth.paramValue(idTracks) > 0.5
        if (idMaxLen >= 0) {
            maxLen = Synth.paramValue(idMaxLen)
        } else if (idLen >= 0) {
            // pre-S19 firmware: the fixed cap is loop.len's registered max
            const m = Synth.paramMeta(idLen)
            if (m.exists && m.max > 0) maxLen = m.max
        }
        if (idRecTrk >= 0 && Synth.paramValueKnown(idRecTrk))
            recTrack = Math.round(Synth.paramValue(idRecTrk))
        if (idPos >= 0) {
            posBase = Synth.paramValue(idPos)
            posAnchorMs = Date.now()
        }
        slotMax = idSave >= 0 ? Math.round(Synth.paramMeta(idSave).max) : 0
        refreshPos()
    }

    // Authoritative re-read of the two status values that decide whether the
    // clear buttons are live. Cheap (two ids, one reliable frame) and only
    // fired on a transport edge, not periodically.
    function verifyState() {
        if (idFilled >= 0) Synth.refreshParam(idFilled)
        if (idLen >= 0) Synth.refreshParam(idLen)
        if (idMode >= 0) Synth.refreshParam(idMode)
        if (idTrack >= 0) Synth.refreshParam(idTrack)
        if (idRecTrk >= 0) Synth.refreshParam(idRecTrk)
    }

    function refreshPos() {
        var p = posBase
        if (curMode !== 0 && posAnchorMs > 0)
            p += (Date.now() - posAnchorMs) / 1000.0
        if (loopLen > 0) p = p % loopLen
        var den = loopLen > 0 ? loopLen : maxLen
        dispPos = Math.min(Math.max(p, 0), den)
    }

    // Instant local estimate of the cap while the firmware's re-mirrored
    // loop.maxlen is still in flight: its discovered default is the base
    // cap (stereo, 8 tracks) and each enabled toggle doubles it. Also
    // correct on S19 firmware, where default/max are the stereo/mono caps.
    function predictedMax(mono, four) {
        const m = Synth.paramMeta(idMaxLen)
        if (!m.exists || m.def <= 0) return maxLen
        return m.def * (mono ? 2 : 1) * (four ? 2 : 1)
    }

    Component.onCompleted: rebind()

    // Re-read the status the moment the page is shown. loop.filled and
    // loop.len only change on transport edges, so the app learns them from
    // single notifications — and a notification is not a guaranteed delivery.
    // Losing one leaves the clears, the save button and the track lights
    // wrong with no way back; this makes simply looking at the page fix it.
    onVisibleChanged: if (visible && Synth.connected) verifyState()

    Connections {
        target: Synth
        function onConnectedChanged() { if (Synth.connected) root.verifyState() }
    }

    // A user tap on a switch breaks its `checked` binding; re-assert the
    // synth state so firmware-driven changes (e.g. a load adopting the
    // stored set's format) keep moving them.
    onMonoOnChanged: monoSwitch.checked = monoOn
    onFourTracksChanged: fourSwitch.checked = fourTracks

    Connections {
        target: Synth
        function onParamsDiscovered() { root.rebind() }
        function onParamChanged(id, value) {
            if (id === root.idTrack) root.curTrack = Math.round(value)
            else if (id === root.idMode) {
                const mode = Math.round(value)
                const moved = mode !== root.curMode
                root.curMode = mode
                // Leaving rec is exactly when loop.filled and loop.len change.
                // The firmware mirrors them and they arrive in the ~20 Hz
                // EVT_PARAMS batch — but a notification is not a guaranteed
                // delivery, and losing this one leaves the clear buttons
                // disabled over a loop that plainly exists. Re-read the two
                // rather than trusting a single event.
                //
                // Only on an actual transport edge. verifyState() re-reads
                // loop.mode itself, and paramChanged fires for every value the
                // synth reports back whether or not it moved — so re-verifying
                // on every report made this handler answer its own GET_PARAM
                // with five more, forever: a permanent write storm on the link
                // that starts with the first loop.mode value of the session.
                if (moved) root.verifyState()
            }
            else if (id === root.idFilled) root.filledMask = Math.round(value)
            else if (id === root.idLen) root.loopLen = value
            else if (id === root.idPos) {
                root.posBase = value
                root.posAnchorMs = Date.now()
                root.refreshPos()
            }
            else if (id === root.idRecTrk) {
                const was = root.recTrack
                root.recTrack = Math.round(value)
                // rectrk 1..8 -> 0 means the audio task just stopped writing a
                // track, i.e. a take closed. Same reasoning as above.
                if (was > 0 && root.recTrack === 0) root.verifyState()
            }
            else if (id === root.idMono) root.monoOn = value > 0.5
            else if (id === root.idTracks) root.fourTracks = value > 0.5
            else if (id === root.idMaxLen) root.maxLen = value
            else if (id === root.idArmed) root.armedBeats = Math.round(value)
            else if (id === root.idSync) root.syncOn = value > 0.5
            else if (id === root.idCountIn) root.countInOn = value > 0.5
        }
    }

    // Smooth display between the firmware's ~4 Hz position reports.
    Timer {
        interval: 100
        repeat: true
        running: root.available && Synth.connected && root.posAvailable
                 && root.curMode !== 0
        onTriggered: root.refreshPos()
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: panels.implicitHeight + 24
        clip: true
        visible: root.available

        PanelFlow {
            id: panels
            spacing: 12

            // ---- transport ----
            Row {
                width: panels.contentWidth
                spacing: 8
                Button {
                    text: "\uf04d"  // stop
                    font.family: App.fontAwesomeName
                    font.weight: Font.Black  // solid face
                    enabled: Synth.connected
                    highlighted: root.curMode === 0
                    onClicked: Synth.setParam(root.idMode, 0)
                }
                Button {
                    text: "\uf04b"  // play
                    font.family: App.fontAwesomeName
                    font.weight: Font.Black  // solid face
                    enabled: Synth.connected
                    highlighted: root.curMode === 1
                    onClicked: Synth.setParam(root.idMode, 1)
                }
                Button {
                    text: "\uf111"  // circle (record)
                    font.family: App.fontAwesomeName
                    font.weight: Font.Black  // solid face
                    enabled: Synth.connected
                    highlighted: root.curMode === 2
                    Material.accent: "#B00020"
                    onClicked: Synth.setParam(root.idMode, 2)
                }
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    color: root.curMode === 2 && root.recTrack > 0
                        ? "#B00020" : Material.foreground
                    opacity: root.curMode === 2 && root.recTrack > 0 ? 1.0 : 0.8
                    text: {
                        // stopped (or old firmware without loop.pos): static
                        if (!root.posAvailable || root.curMode === 0) {
                            return root.loopLen > 0
                                ? t.ts("loop %1 s", root.loopLen.toFixed(2))
                                : t.ts("no loop — rec records the first track and sets the length (max %1 s)",
                                       root.maxLen.toFixed(0))
                        }
                        // rec armed: punch lands at the next loop start
                        if (root.curMode === 2 && root.recTrack === 0
                                && root.loopLen > 0) {
                            return t.ts("punch at loop start — %1 s",
                                Math.max(0, root.loopLen - root.dispPos).toFixed(1))
                        }
                        var den = root.loopLen > 0 ? root.loopLen : root.maxLen
                        var pos = root.dispPos.toFixed(1) + " / "
                                + den.toFixed(1) + " s"
                        return root.curMode === 2 ? t.ts("● rec %1", pos) : pos
                    }
                }
            }

            // ---- loop position (S18: loop.pos telemetry; hidden on
            //      firmware without it) ----
            ProgressBar {
                width: panels.contentWidth
                visible: root.posAvailable && root.curMode !== 0
                         && (root.loopLen > 0 || root.curMode === 2)
                from: 0
                to: root.loopLen > 0 ? root.loopLen : root.maxLen
                value: root.dispPos
                Material.accent: root.curMode === 2 && root.recTrack > 0
                    ? "#B00020" : App.theme.materialAccent
            }

            // ---- cap policies (S19 mono, S20 4-track mode; each switch
            //      hidden on firmware without its param). Both are latched
            //      when a loop's first take starts, so with a loop present
            //      they only name the *next* set; each doubles the cap.
            //      The firmware re-mirrors loop.maxlen on toggle; the
            //      predictedMax() estimate just makes the max-time texts
            //      move in the same frame instead of one BLE round-trip
            //      later.
            Column {
                width: panels.contentWidth
                spacing: 0
                visible: root.idMono >= 0 || root.idTracks >= 0
                Flow {
                    width: parent.width
                    spacing: 8
                    Switch {
                        id: monoSwitch
                        text: t.t("Record mono")
                        visible: root.idMono >= 0
                        enabled: Synth.connected
                        checked: root.monoOn
                        onToggled: {
                            Synth.setParam(root.idMono, checked ? 1 : 0)
                            root.maxLen = root.predictedMax(
                                checked, fourSwitch.checked)
                        }
                    }
                    Switch {
                        id: fourSwitch
                        text: t.t("4 tracks")
                        visible: root.idTracks >= 0
                        enabled: Synth.connected
                        checked: root.fourTracks
                        onToggled: {
                            Synth.setParam(root.idTracks, checked ? 1 : 0)
                            // tracks 5-8 leave the grid; park the selection
                            if (checked && root.curTrack > 4)
                                Synth.setParam(root.idTrack, 1)
                            root.maxLen = root.predictedMax(
                                monoSwitch.checked, checked)
                        }
                    }
                }
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: Material.foreground
                    opacity: 0.55
                    font.pointSize: UI.fontSize * 0.75
                    text: root.loopLen > 0
                        ? t.t("applies to the next loop (after clear all)")
                        : t.ts("max loop %1 s", root.maxLen.toFixed(0))
                }
            }

            // ---- track selector ----
            Rectangle {
                width: panels.contentWidth
                implicitHeight: trackCol.implicitHeight + 16
                radius: 8
                color: Material.theme === Material.Dark ? "#1AFFFFFF" : "#0D000000"

                Column {
                    id: trackCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 8
                    spacing: 6

                    Label {
                        text: t.t("Tracks")
                        font.bold: true
                        font.pointSize: UI.fontSize * 0.95
                        color: Material.foreground
                    }

                    Flow {
                        width: parent.width
                        spacing: 6
                        Repeater {
                            // 4-track mode narrows the grid (a policy for
                            // the next set — an already-loaded 8-track set
                            // keeps playing all of its tracks)
                            model: root.fourTracks ? 4 : 8
                            delegate: Button {
                                required property int index
                                readonly property bool selected: root.curTrack === index + 1
                                readonly property bool filled: (root.filledMask & (1 << index)) !== 0
                                readonly property bool recording: selected && root.curMode === 2
                                width: 64
                                text: (index + 1)
                                enabled: Synth.connected
                                highlighted: selected
                                onClicked: Synth.setParam(root.idTrack, index + 1)

                                // filled / recording indicator
                                Rectangle {
                                    width: 8; height: 8; radius: 4
                                    anchors.top: parent.top
                                    anchors.right: parent.right
                                    anchors.margins: 6
                                    color: parent.recording ? "#B00020"
                                         : parent.filled ? "#2E7D32"
                                         : "transparent"
                                    border.width: parent.filled || parent.recording ? 0 : 1
                                    border.color: Material.foreground
                                    opacity: parent.filled || parent.recording ? 1.0 : 0.35
                                }
                            }
                        }
                    }

                    // Start alignment. Hidden on firmware without the
                    // params, so an older synth just does not show them.
                    Row {
                        spacing: 8
                        visible: root.idSync >= 0 || root.idCountIn >= 0

                        // SyncedButton for the same reason the two switches
                        // above are re-asserted by hand: a press replaces a
                        // plain `checked` binding, and a loaded set adopting
                        // the stored format moves these from the firmware side.
                        SyncedButton {
                            text: t.t("Sync to sequencer")
                            visible: root.idSync >= 0
                            modelChecked: root.syncOn
                            onToggled: Synth.setParam(root.idSync, checked ? 1 : 0)
                            // The clock free-runs, so this works whether or not
                            // the sequencer is playing.
                            ToolTip.visible: hovered
                            ToolTip.text: t.t("Recording starts on the next "
                                              + "downbeat of the sequencer clock.")
                        }
                        SyncedButton {
                            text: t.t("Count-in")
                            visible: root.idCountIn >= 0
                            modelChecked: root.countInOn
                            onToggled: Synth.setParam(root.idCountIn, checked ? 1 : 0)
                            ToolTip.visible: hovered
                            ToolTip.text: t.t("Four clicked beats before "
                                              + "recording begins.")
                        }
                        Label {
                            visible: root.armedBeats > 0
                            text: t.t("waiting") + " " + root.armedBeats
                            color: "#FF5252"
                            font.bold: true
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    Row {
                        spacing: 8
                        Button {
                            text: t.t("Clear track")
                            // The track guard is not decoration: JS shifts by
                            // (count & 31), so a curTrack of 0 would make
                            // `1 << -1` the sign bit and light this button over
                            // a track that has nothing in it.
                            enabled: Synth.connected && root.curTrack >= 1
                                     && (root.filledMask & (1 << (root.curTrack - 1))) !== 0
                            onClicked: Synth.setParam(root.idClear, 1)
                        }
                        Button {
                            text: t.t("Clear all")
                            enabled: Synth.connected && root.filledMask !== 0
                            onClicked: clearAllDialog.open()
                        }
                    }
                }
            }

            // ---- persistence (S16; hidden when the firmware lacks it) ----
            Rectangle {
                width: panels.contentWidth
                implicitHeight: persistRow.implicitHeight + 16
                radius: 8
                visible: root.canPersist
                color: Material.theme === Material.Dark ? "#1AFFFFFF" : "#0D000000"

                RowLayout {
                    id: persistRow
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 8
                    spacing: 8

                    Label {
                        text: t.t("Slot")
                        color: Material.foreground
                        opacity: 0.7
                    }
                    SpinBox {
                        id: slotBox
                        from: 0
                        to: root.slotMax
                        value: 0
                        visible: root.slotMax > 0  // flash backend: only slot 0
                    }
                    Button {
                        text: t.t("Save set")
                        enabled: Synth.connected && root.filledMask !== 0
                        // trigger param: writing the slot number performs the save
                        onClicked: Synth.setParam(root.idSave, slotBox.value)
                    }
                    Button {
                        text: t.t("Load set")
                        enabled: Synth.connected
                        onClicked: Synth.setParam(root.idLoad, slotBox.value)
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Material.foreground
                        opacity: 0.55
                        font.pointSize: UI.fontSize * 0.75
                        text: t.t("Flash backend needs the loop stopped; loading replaces the current set.")
                    }
                }
            }

            ParamGroup { title: "Track levels"; prefix: "loop.lvl" }
        }
    }

    Dialog {
        id: clearAllDialog
        anchors.centerIn: parent
        modal: true
        title: t.t("Clear all tracks?")
        standardButtons: Dialog.Yes | Dialog.No
        Label {
            text: t.t("All recorded tracks are discarded and the loop length is reset.")
            color: Material.foreground
        }
        onAccepted: Synth.setParam(root.idClear, 2)
    }

    Label {
        anchors.centerIn: parent
        visible: Synth.ready && !root.available
        text: t.t("Looper not available on this synth (needs PSRAM).")
        opacity: 0.5
        color: Material.foreground
    }

    Label {
        anchors.centerIn: parent
        visible: !Synth.ready
        text: Synth.connected ? t.t("Discovering parameters…") : t.t("Not connected")
        opacity: 0.5
        color: Material.foreground
    }
}
