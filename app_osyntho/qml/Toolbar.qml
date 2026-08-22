import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// Top toolbar: page prev/next for the horizontal SwipeView, a connection
// indicator, and the overflow menu (Backup / Device / Firmware / Settings).
// The SwipeView it drives arrives as the `pager` property, and navigation
// requests go out through the UI singleton's signals — so this does not
// depend on Main's ids resolving through the context chain. It still reads
// mainWindow for the keyboard/drum-pad toggles, which do.
//
// Icons are Font Awesome 6 Free solid glyphs written as "\uXXXX" escapes; the
// weight pin (Font.Black) selects the solid face over the regular one.
ToolBar {
    id: t1

    property string title: "Osyntho"
    property string subtitle: ""
    // The SwipeView the prev/next arrows drive. Named `pager` rather than
    // `swipeView` so that `pager: swipeView` at the call site cannot resolve
    // its right-hand side to this property and bind it to itself.
    property SwipeView pager

    Material.foreground: App.theme.primaryColor
    Material.background: App.theme.primaryBgColor

    // Toolbar width spent on chrome that is never dropped. Deliberately built
    // from constants and from items whose width cannot depend on the
    // transports — measuring the (fillWidth) title column instead would make
    // the fit test oscillate: hide the strip, gain room, show it again.
    readonly property real fixedChromeWidth:
        (prevButton.visible ? prevButton.width : 8)
        + (masterVol.visible ? 76 : 0)
        + (outLevel.visible ? 76 : 0)
        + plugLabel.width + 4
        + menuButton.width
        + (nextButton.visible ? nextButton.width : 0)
        + 24  // RowLayout spacing and margins

    RowLayout {
        anchors.fill: parent

        ToolButton {
            id: prevButton
            text: "\uf104"  // angle-left
            font.family: App.fontAwesomeName
            font.weight: Font.Black  // solid face
            font.pointSize: UI.fontSize * 1.2
            visible: t1.pager && t1.pager.currentIndex > 0
            onClicked: if (t1.pager.currentIndex > 0) t1.pager.currentIndex--
        }
        // Stands in for the hidden prev button so the title does not jump to the
        // edge on the first page. Layout.preferredWidth, not width: a RowLayout
        // sets its children's width itself, from their preferred/implicit size,
        // so a plain `width: 8` was overwritten with the Item's implicitWidth of
        // 0 — the spacer was not there at all, while fixedChromeWidth below has
        // always charged 8 px for it.
        Item { visible: !prevButton.visible; Layout.preferredWidth: 8 }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: -4
            Label {
                text: t1.title
                font.family: "Open Sans"
                font.pointSize: UI.fontSize * 1.4
                color: App.theme.primaryColor
                elide: Label.ElideRight
                Layout.fillWidth: true
            }
            Label {
                text: t1.subtitle
                font.family: "Open Sans"
                font.pointSize: UI.fontSize * 0.8
                color: App.theme.primaryColor
                elide: Label.ElideRight
                visible: t1.subtitle !== ""
                Layout.fillWidth: true
            }
        }

        BusyIndicator {
            running: BluetoothManager.scanning
            visible: running
            Layout.maximumHeight: parent.height - 8
        }

        // Master volume: bound to the master.volume param (id 0x0000). Visible
        // once discovered; reflects external changes (presets/MIDI) unless the
        // user is dragging it. meta is refreshed imperatively because
        // Synth.paramMeta() has no change signal (a plain binding would stay at
        // its pre-discovery {exists:false} value forever).
        RowLayout {
            id: masterVol
            readonly property int volId: 0  // master.volume == 0x0000
            property paramMeta meta
            visible: masterVol.meta.exists
            spacing: 4
            Layout.rightMargin: 6
            Layout.preferredWidth: 70  // reserve space so the slider isn't squeezed to 0

            function refresh() {
                meta = Synth.paramMeta(volId)
                if (meta.exists) {
                    volSlider.syncing = true
                    volSlider.value = Synth.paramValue(volId)
                    volSlider.syncing = false
                }
            }
            Component.onCompleted: refresh()

            Connections {
                target: Synth
                function onParamsDiscovered() { masterVol.refresh() }
                function onParamChanged(id: int, v: real): void {
                    if (id === masterVol.volId && !volSlider.pressed) {
                        volSlider.syncing = true
                        volSlider.value = v
                        volSlider.syncing = false
                    }
                }
            }

            Label {
                text: "\uf028"  // volume-up
                font.family: App.fontAwesomeName
                font.weight: Font.Black  // solid face
                font.pointSize: UI.fontSize
                color: App.theme.primaryColor
                Layout.alignment: Qt.AlignVCenter
            }
            Slider {
                id: volSlider
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                // Contrast against the coloured toolbar: the default accent equals
                // the toolbar background (purple-on-purple = invisible handle).
                Material.accent: App.theme.primaryColor
                from: masterVol.meta.exists ? masterVol.meta.min : 0
                to: masterVol.meta.exists ? masterVol.meta.max : 1
                property bool syncing: false
                onMoved: if (!syncing) Synth.setParam(masterVol.volId, value)
            }
        }

        // Analogue output level (out.level), on firmware with a codec whose
        // output driver has one \u2014 an ES8388 build; there is no such register
        // on a discrete PCM5102A, and the param is then never registered, so
        // the whole strip self-hides on the same existence test masterVol
        // uses. Resolved by name rather than by a hardcoded id because it is
        // optional: paramIdForName returns -1 until discovery finds it.
        //
        // Distinct from master volume on purpose: this one sets the operating
        // point for what is plugged in (0 dB into a line input, lower for
        // headphones) and is set once by ear, while master.volume is the
        // digital one you ride. Its 1.5 dB hardware steps also mean dragging
        // it can be heard stepping \u2014 expected, and why it is not the control
        // bound to performance.
        RowLayout {
            id: outLevel
            property int levelId: -1
            property paramMeta meta
            visible: outLevel.meta.exists
            spacing: 4
            Layout.rightMargin: 6
            Layout.preferredWidth: 70

            function refresh() {
                levelId = Synth.paramIdForName("out.level")
                meta = Synth.paramMeta(levelId)
                if (meta.exists) {
                    outSlider.syncing = true
                    outSlider.value = Synth.paramValue(levelId)
                    outSlider.syncing = false
                }
            }
            Component.onCompleted: refresh()

            Connections {
                target: Synth
                function onParamsDiscovered() { outLevel.refresh() }
                function onParamChanged(id: int, v: real): void {
                    if (id === outLevel.levelId && outLevel.levelId >= 0
                            && !outSlider.pressed) {
                        outSlider.syncing = true
                        outSlider.value = v
                        outSlider.syncing = false
                    }
                }
            }

            Label {
                text: "\uf025"  // headphones
                font.family: App.fontAwesomeName
                font.weight: Font.Black  // solid face
                font.pointSize: UI.fontSize
                color: App.theme.primaryColor
                Layout.alignment: Qt.AlignVCenter

                ToolTip.visible: outMouse.containsMouse
                ToolTip.text: Tr.t("Output level: %1 dB").arg(outSlider.value.toFixed(1))
                MouseArea {
                    id: outMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.NoButton
                }
            }
            Slider {
                id: outSlider
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                Material.accent: App.theme.primaryColor
                from: outLevel.meta.exists ? outLevel.meta.min : -45
                to: outLevel.meta.exists ? outLevel.meta.max : 4.5
                // The codec quantises to 1.5 dB anyway, so snapping the handle
                // to the same grid stops the slider from reporting a value the
                // hardware cannot actually take.
                stepSize: 1.5
                snapMode: Slider.SnapAlways
                property bool syncing: false
                onMoved: if (!syncing) Synth.setParam(outLevel.levelId, value)
            }
        }

        // Transports: looper and sequencer. Both when there is room, looper
        // only when there is not \u2014 it is the one you reach for mid-take, and
        // the sequencer has its own transport on its page anyway.
        //
        // The fit test is measured, not guessed at a breakpoint: each group
        // reports its own implicitWidth, and the title column is allowed to
        // shrink to `titleFloor` before either group is dropped. A hard-coded
        // width threshold would be wrong at a different font size, and
        // UI.fontSize is user-adjustable.
        Item {
            id: transports

            // Resolved imperatively (paramIdForName has no change signal), so
            // they update when discovery completes rather than staying at
            // their pre-discovery value of -1 and hiding both groups forever.
            property int seqModeId: -1
            property int loopModeId: -1
            property int loopArmedId: -1
            function refreshIds() {
                seqModeId = Synth.paramIdForName("seq.mode")
                loopModeId = Synth.paramIdForName("loop.mode")
                loopArmedId = Synth.paramIdForName("loop.armed")
            }
            Component.onCompleted: refreshIds()
            Connections {
                target: Synth
                function onParamsDiscovered() { transports.refreshIds() }
            }

            readonly property real titleFloor: UI.fontSize * 5
            readonly property real spare: t1.width - titleFloor - t1.fixedChromeWidth
            readonly property bool showLooper: loopTransport.present
                                               && spare >= loopTransport.estimatedWidth
            // Room the looper strip is actually taking, so the sequencer's fit
            // test asks for what is left rather than assuming a looper exists.
            readonly property real looperWidth: showLooper ? loopTransport.estimatedWidth + 8 : 0
            // The looper keeps priority where both are present — it is the one
            // you reach for mid-take. But gating this on showLooper meant a
            // firmware without a looper (no PSRAM) hid the sequencer transport
            // too, however much room the toolbar had.
            readonly property bool showSeq: seqTransport.present
                                            && spare >= looperWidth + seqTransport.estimatedWidth

            // The 8 is the RowLayout's spacing between the two strips, so it is
            // only spent when both are actually shown — matching looperWidth
            // above, which is what the fit test above charges for.
            implicitWidth: (showSeq ? seqTransport.estimatedWidth + (showLooper ? 8 : 0) : 0)
                           + (showLooper ? loopTransport.estimatedWidth : 0)
            implicitHeight: Math.max(seqTransport.implicitHeight,
                                     loopTransport.implicitHeight)
            Layout.alignment: Qt.AlignVCenter

            RowLayout {
                anchors.fill: parent
                spacing: 8

                TransportStrip {
                    id: seqTransport
                    caption: Tr.t("SEQ")
                    modeId: transports.seqModeId
                    visible: transports.showSeq
                }
                TransportStrip {
                    id: loopTransport
                    caption: Tr.t("LOOP")
                    modeId: transports.loopModeId
                    armedId: transports.loopArmedId
                    visible: transports.showLooper
                }
            }
        }

        // Connection indicator (plug icon).
        Label {
            id: plugLabel
            text: "\uf1e6"  // plug
            font.family: App.fontAwesomeName
            font.weight: Font.Black  // solid face
            font.pointSize: UI.fontSize * 1.1
            color: Synth.connected ? "#69F0AE" : Qt.rgba(1, 1, 1, 0.35)
            Layout.rightMargin: 4
        }

        ToolButton {
            id: menuButton
            text: "\uf0c9"  // bars
            font.family: App.fontAwesomeName
            font.weight: Font.Black  // solid face
            font.pointSize: UI.fontSize * 1.15
            onClicked: mainMenu.open()

            Menu {
                id: mainMenu
                y: menuButton.height

                Menu {
                    title: Tr.t("Backup")
                    MenuItem { text: Tr.t("Save data..."); onTriggered: UI.shareBackupRequested() }
                    MenuItem { text: Tr.t("Restore data..."); onTriggered: UI.restoreBackupRequested() }
                }

                MenuItem {
                    text: Tr.t("Show keyboard")
                    checkable: true
                    checked: mainWindow.keyboardVisible
                    onTriggered: mainWindow.keyboardVisible = !mainWindow.keyboardVisible
                }

                MenuItem {
                    text: Tr.t("Show drum pads")
                    checkable: true
                    checked: mainWindow.drumPadsVisible
                    onTriggered: mainWindow.drumPadsVisible = !mainWindow.drumPadsVisible
                }

                MenuSeparator {}

                // Panic. Reachable from every page because a stuck note is not
                // something you want to go looking for: note-offs go out
                // write-without-response, so one the synth's command queue
                // drops leaves a note sounding that nothing is tracking.
                MenuItem {
                    text: Tr.t("All notes off")
                    enabled: Synth.connected
                    onTriggered: Synth.allNotesOff()
                }

                MenuSeparator {}

                MenuItem {
                    text: Tr.t("Select device...")
                    // Off means off: the selector's scan would otherwise put
                    // the radio back to work behind the Settings toggle.
                    enabled: App.bluetoothEnabled
                    onTriggered: UI.selectDeviceRequested()
                }

                MenuItem {
                    text: Tr.t("Update firmware...")
                    enabled: Synth.firmwareUpdateSupported
                    onTriggered: UI.updateFirmwareRequested("bin")
                }

                MenuSeparator {}

                MenuItem {
                    text: Tr.t("Settings")
                    onTriggered: UI.settingsRequested()
                }
            }
        }

        ToolButton {
            id: nextButton
            text: "\uf105"  // angle-right
            font.family: App.fontAwesomeName
            font.weight: Font.Black  // solid face
            font.pointSize: UI.fontSize * 1.2
            visible: t1.pager && t1.pager.currentIndex < t1.pager.count - 1
            onClicked: if (t1.pager.currentIndex < t1.pager.count - 1) t1.pager.currentIndex++
        }
    }
}
