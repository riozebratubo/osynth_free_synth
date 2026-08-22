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
    property int idStore: -1
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
    // Track storage for the next set (loop.store, S31): false = psram (whole
    // tracks in RAM, capped by maxLen), true = sd (streamed off the card, so
    // the cap becomes the card's). Only present on firmware built with the
    // SD store backend — the switch is hidden otherwise, like the cap
    // toggles above. Latched by the first take, so with a loop present it
    // names the next set.
    property bool sdStore: false
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

    // ---- download (S33) ----
    // Where the download picker is pointed. Index 0 is the live set; the rest
    // are the save slots, so the source enum and the slot number both fall out
    // of one index. Slots are only offered on firmware that has persistence at
    // all (canPersist), and only as many as the backend has (slotMax: flash 0,
    // SD 7).
    readonly property var exportSources: {
        var l = [Tr.t("Live set")]
        if (canPersist)
            for (var i = 0; i <= slotMax; ++i) l.push(Tr.ts("Slot %1", String(i)))
        return l
    }
    // Owned here rather than read off the ComboBox: a plain
    // `exportSrcBox.currentIndex` binding on a root property is evaluated
    // before that child exists, captures no dependency on it and then never
    // updates. The picker is a SyncedComboBox for the mirror-image reason —
    // see its own header.
    property int exportSourceIndex: 0
    property int exportTrackIndex: 0
    readonly property int exportSource: exportSourceIndex > 0 ? 1 : 0  // LOOP_SRC_*
    readonly property int exportSlot: exportSourceIndex > 0 ? exportSourceIndex - 1 : 0
    // Which tracks the last probe found recorded there, 0-based. Read off the
    // probe's own filled mask rather than loop.filled: they are the same thing
    // only for the live set, and a slot's mask is not mirrored anywhere.
    readonly property var exportTracks: {
        var out = []
        const info = Synth.loopExportInfo
        if (!info.valid) return out
        for (var i = 0; i < info.tracks; ++i)
            if ((info.filled & (1 << i)) !== 0) out.push(i)
        return out
    }
    readonly property var exportTrackLabels:
        exportTracks.map(function(i) { return Tr.ts("Track %1", String(i + 1)) })

    // Both lists are rebuilt whenever the synth answers, and a selection that
    // no longer exists would otherwise index past the end — a slot that has
    // been cleared, or a backend with fewer slots after a reconnect.
    onExportSourcesChanged:
        if (exportSourceIndex >= exportSources.length) exportSourceIndex = 0
    onExportTracksChanged:
        if (exportTrackIndex >= exportTracks.length) exportTrackIndex = 0

    // Asks the synth what is recorded in the selected source. Also what tells
    // the app the firmware has the download opcode at all — the panel stays
    // hidden until one of these is answered.
    function probeExport() {
        if (available && Synth.connected)
            Synth.probeLoopExport(exportSource, exportSlot)
    }

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
        idStore = Synth.paramIdForName("loop.store")
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
        if (idStore >= 0) sdStore = Synth.paramValue(idStore) > 0.5
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
        // The download panel hides itself until a LOOP_DUMP request has been
        // answered, so it needs one asked without waiting for the user to
        // press anything. Discovery is also when slotMax settles, which is
        // what the source picker's slot entries are built from.
        probeExport()
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
        // The download picker is stale for exactly the same reasons the five
        // above are, and on the same events: a take that just closed changed
        // what there is to download. (Ignored by the controller while a
        // transfer is running, so this cannot disturb one.)
        probeExport()
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
    function predictedMax(mono: bool, four: bool): real {
        // In SD mode the cap is the card's, so neither toggle moves it —
        // predicting a doubling here would make the hint jump and then be
        // corrected by the firmware's own loop.maxlen a moment later.
        if (sdStore) return maxLen
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
    // Same reason, and this one is not hypothetical: the firmware writes
    // loop.store back to psram whenever a streamed set cannot be opened, and
    // without this the switch a moment ago tapped to "on" stays on over a set
    // that is recording to PSRAM.
    onSdStoreChanged: sdSwitch.checked = sdStore

    Connections {
        target: Synth
        function onParamsDiscovered() { root.rebind() }
        function onParamChanged(id: int, value: real): void {
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
            // The firmware writes this back when a card is missing and a
            // streamed set falls back to psram, so the switch follows.
            else if (id === root.idStore) root.sdStore = value > 0.5
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
                                ? Tr.ts("loop %1 s", root.loopLen.toFixed(2))
                                : Tr.ts("no loop — rec records the first track and sets the length (max %1 s)",
                                       root.maxLen.toFixed(0))
                        }
                        // rec armed: punch lands at the next loop start
                        if (root.curMode === 2 && root.recTrack === 0
                                && root.loopLen > 0) {
                            return Tr.ts("punch at loop start — %1 s",
                                Math.max(0, root.loopLen - root.dispPos).toFixed(1))
                        }
                        var den = root.loopLen > 0 ? root.loopLen : root.maxLen
                        var pos = root.dispPos.toFixed(1) + " / "
                                + den.toFixed(1) + " s"
                        return root.curMode === 2 ? Tr.ts("● rec %1", pos) : pos
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
                        text: Tr.t("Record mono")
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
                        text: Tr.t("4 tracks")
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
                    // Storage for the next set (S31). Streaming off the card
                    // trades the PSRAM cap for the card's capacity; the
                    // firmware re-mirrors loop.maxlen either way, so the
                    // hint below just follows it rather than predicting.
                    Switch {
                        id: sdSwitch
                        text: Tr.t("Record to SD")
                        visible: root.idStore >= 0
                        enabled: Synth.connected
                        checked: root.sdStore
                        onToggled: Synth.setParam(root.idStore, checked ? 1 : 0)
                    }
                }
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: Material.foreground
                    opacity: 0.55
                    font.pointSize: UI.fontSize * 0.75
                    text: root.loopLen > 0
                        ? Tr.t("applies to the next loop (after clear all)")
                        : (root.sdStore
                           ? Tr.ts("streamed from the card, max loop %1 s",
                                  root.maxLen.toFixed(0))
                           : Tr.ts("max loop %1 s", root.maxLen.toFixed(0)))
                }
                // Slot save/load holds the whole set in RAM, so it cannot
                // take a streamed set — the firmware refuses it and says so
                // in its log. Saying it here too keeps the user from
                // discovering it only after a long take.
                Label {
                    width: parent.width
                    visible: root.sdStore && root.canPersist
                    wrapMode: Text.WordWrap
                    color: Material.foreground
                    opacity: 0.55
                    font.pointSize: UI.fontSize * 0.75
                    text: Tr.t("slot save/load is unavailable for SD sets — "
                              + "the tracks are /sd/osynth/liveN.olt")
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
                        text: Tr.t("Tracks")
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
                            text: Tr.t("Sync to sequencer")
                            visible: root.idSync >= 0
                            modelChecked: root.syncOn
                            onToggled: Synth.setParam(root.idSync, checked ? 1 : 0)
                            // The clock free-runs, so this works whether or not
                            // the sequencer is playing.
                            ToolTip.visible: hovered
                            ToolTip.text: Tr.t("Recording starts on the next "
                                              + "downbeat of the sequencer clock.")
                        }
                        SyncedButton {
                            text: Tr.t("Count-in")
                            visible: root.idCountIn >= 0
                            modelChecked: root.countInOn
                            onToggled: Synth.setParam(root.idCountIn, checked ? 1 : 0)
                            ToolTip.visible: hovered
                            ToolTip.text: Tr.t("Four clicked beats before "
                                              + "recording begins.")
                        }
                        Label {
                            visible: root.armedBeats > 0
                            text: Tr.t("waiting") + " " + root.armedBeats
                            color: "#FF5252"
                            font.bold: true
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    Row {
                        spacing: 8
                        // Both stay live whatever loop.filled says. The mask is
                        // a status mirror, and a lost notification used to
                        // leave the only way out of a stuck-looking loop
                        // greyed out — the one moment the user most needs it.
                        // The firmware ignores a clear on an empty track, so
                        // the worst an always-enabled button can do is
                        // nothing. The track lights below still show what is
                        // actually recorded; that is the mask's real job.
                        Button {
                            text: Tr.t("Clear track")
                            enabled: Synth.connected && root.curTrack >= 1
                            onClicked: Synth.setParam(root.idClear, 1)
                        }
                        Button {
                            text: Tr.t("Clear all")
                            enabled: Synth.connected
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
                        text: Tr.t("Slot")
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
                        text: Tr.t("Save set")
                        enabled: Synth.connected && root.filledMask !== 0
                        // trigger param: writing the slot number performs the save
                        onClicked: Synth.setParam(root.idSave, slotBox.value)
                    }
                    Button {
                        text: Tr.t("Load set")
                        enabled: Synth.connected
                        onClicked: Synth.setParam(root.idLoad, slotBox.value)
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Material.foreground
                        opacity: 0.55
                        font.pointSize: UI.fontSize * 0.75
                        text: Tr.t("Flash backend needs the loop stopped; loading replaces the current set.")
                    }
                }
            }

            // ---- download (S33) ----
            // Reads a recorded track back off the synth and writes it as a
            // WAV. Two shapes: one track as recorded, or every track summed
            // at the levels below — which is why this panel sits above them.
            //
            // The source picker exists because a track's audio can be in
            // three different places and only the user knows which one they
            // mean: the live set (in PSRAM, or streamed off the card — the app
            // cannot tell and does not need to), or a save slot. A streamed
            // set can never be saved to a slot at all (the firmware refuses
            // it), so "Live set" is the only route to those takes.
            //
            // Hidden until an OP_LOOP_DUMP request has been answered, so older
            // firmware shows nothing rather than a button that only fails.
            Rectangle {
                width: panels.contentWidth
                implicitHeight: exportCol.implicitHeight + 16
                radius: 8
                visible: Synth.loopExportSupported
                color: Material.theme === Material.Dark ? "#1AFFFFFF" : "#0D000000"

                Column {
                    id: exportCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 8
                    spacing: 6

                    Label {
                        text: Tr.t("Download")
                        font.bold: true
                        font.pointSize: UI.fontSize * 0.95
                        color: Material.foreground
                    }

                    Flow {
                        width: parent.width
                        spacing: 8

                        // SyncedComboBox: both models are rebuilt every time
                        // the synth answers a probe, and a plain ComboBox
                        // silently drops its currentIndex binding on the first
                        // pick — so the picker would go on showing a track the
                        // user had moved off. See SyncedComboBox.qml.
                        SyncedComboBox {
                            model: root.exportSources
                            modelIndex: root.exportSourceIndex
                            enabled: Synth.connected && !Synth.loopExportActive
                            onActivated: {
                                root.exportSourceIndex = currentIndex
                                root.probeExport()
                            }
                        }
                        SyncedComboBox {
                            model: root.exportTrackLabels
                            modelIndex: root.exportTrackIndex
                            visible: root.exportTracks.length > 0
                            enabled: Synth.connected && !Synth.loopExportActive
                            onActivated: root.exportTrackIndex = currentIndex
                        }
                        Button {
                            text: Tr.t("Track WAV")
                            enabled: Synth.connected && !Synth.loopExportActive
                                     && root.exportTracks.length > 0
                            onClicked: Synth.startLoopExport(
                                root.exportSource, root.exportSlot,
                                root.exportTracks[root.exportTrackIndex])
                        }
                        Button {
                            text: Tr.t("Mix WAV")
                            enabled: Synth.connected && !Synth.loopExportActive
                                     && root.exportTracks.length > 0
                            onClicked: Synth.startLoopMixExport(root.exportSource,
                                                                root.exportSlot)
                        }
                        Button {
                            text: Tr.t("Cancel")
                            visible: Synth.loopExportActive
                            onClicked: Synth.cancelLoopExport()
                        }
                    }

                    ProgressBar {
                        width: parent.width
                        visible: Synth.loopExportActive
                        from: 0
                        to: 1
                        value: Synth.loopExportProgress
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        color: Material.foreground
                        opacity: 0.55
                        font.pointSize: UI.fontSize * 0.75
                        text: {
                            if (Synth.loopExportActive)
                                return Tr.ts("downloading… %1%",
                                            String(Math.round(Synth.loopExportProgress * 100)))
                            const info = Synth.loopExportInfo
                            if (!info.valid || root.exportTracks.length === 0)
                                return Tr.t("nothing recorded there")
                            // Audio comes over the same BLE link as everything
                            // else, so a long take is a long wait — say so
                            // before the user starts one, not during.
                            return Tr.ts("%1 track(s), %2 s %3 — a download runs at BLE speed, so allow a while; the mix uses the track levels below",
                                        String(root.exportTracks.length),
                                        info.seconds.toFixed(1),
                                        info.mono ? Tr.t("mono") : Tr.t("stereo"))
                        }
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
        title: Tr.t("Clear all tracks?")
        standardButtons: Dialog.Yes | Dialog.No
        Label {
            text: Tr.t("All recorded tracks are discarded and the loop length is reset.")
            color: Material.foreground
        }
        onAccepted: Synth.setParam(root.idClear, 2)
    }

    Label {
        anchors.centerIn: parent
        visible: Synth.ready && !root.available
        // Says what the app actually knows — that loop.mode never arrived —
        // rather than guessing why. The old text asserted "needs PSRAM", which
        // the app has no way to determine: a firmware built without PSRAM
        // registers no looper params, but so does any discovery that lost them,
        // and the two are indistinguishable from here. Blaming the hardware
        // sent a real enumeration bug undiagnosed for a long time.
        text: Tr.t("Looper parameters were not received from this synth.")
        opacity: 0.5
        color: Material.foreground
    }

    Label {
        anchors.centerIn: parent
        visible: !Synth.ready
        text: Synth.connected ? Tr.t("Discovering parameters…") : Tr.t("Not connected")
        opacity: 0.5
        color: Material.foreground
    }
}
