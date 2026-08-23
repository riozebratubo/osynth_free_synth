import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// Overview: engine select, master + voice (glide/unison/bend) controls, the
// analogue output level, the current preset, the USB port and the way back to
// a blank instrument.
//
// The last two came off the old osynth page when it was removed. They belong
// with the rest of "what is this synth set to", and that page was down to
// repeating what the toolbar and the FX page already showed.
Item {
    id: screen

    // Set when a restart never completed, cleared the moment one is asked for
    // again. Shown inline rather than as a toast: toasts live in Main.qml's
    // scope and a page cannot reach one, and this is a message you want still
    // on screen while you go and look at the synth.
    property bool restartFailed: false

    // Poll the port for what is attached to it. There is no event for "a
    // controller was plugged in", so the page has to ask — but only in host
    // mode, where the answer can change behind the app's back. In device mode
    // nothing does, and unlike the osynth page this one is where the app
    // opens, so a blanket 3 s poll would run for most of a session.
    Timer {
        interval: 3000
        running: screen.visible && Synth.connected && usbCard.visible
                 && Synth.usbActiveMode === 1
        repeat: true
        triggeredOnStart: true
        onTriggered: Synth.refreshUsbStatus()
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: panels.implicitHeight + 24
        clip: true

        PanelFlow {
            id: panels
            spacing: 12

            // header + engine picker: full width, so they keep their own lines
            Label {
                width: panels.contentWidth
                text: Synth.ready ? Tr.ts("%1 engine", Tr.t(Synth.engineName)) : Tr.t("Discovering…")
                font.pointSize: UI.fontSize * 1.4
                font.bold: true
                color: Material.foreground
            }

            Row {
                width: panels.contentWidth
                spacing: 8
                Repeater {
                    // Which engines the connected firmware actually has —
                    // engine.type's enum decides the count and GRAPH_INFO
                    // decides whether Modular is among them. Built in
                    // SynthController::engineList() rather than here, because
                    // an app that hardcodes the list offers buttons that older
                    // firmware answers with ST_BAD_ARG (S38).
                    model: Synth.engineList
                    delegate: Button {
                        required property var modelData
                        text: Tr.t(modelData.n)
                        highlighted: Synth.engine === modelData.e
                        enabled: Synth.connected
                        onClicked: Synth.selectEngine(modelData.e)
                    }
                }
            }

            ParamGroup { title: "Master"; prefix: "master" }
            ParamGroup { title: "Voice"; prefix: "common" }
            // out.level — ES8388 builds only, and self-hiding elsewhere on the
            // same existence test the toolbar's copy uses. Distinct from
            // master volume: this one sets the operating point for whatever is
            // in the jack and is set once by ear, where master volume is the
            // digital level you ride.
            ParamGroup { title: "Analogue output"; prefix: "out." }

            Rectangle {
                width: panels.contentWidth
                height: presetRow.implicitHeight + 20
                radius: 8
                visible: Synth.ready
                color: Material.theme === Material.Dark ? "#1AFFFFFF" : "#0D000000"
                RowLayout {
                    id: presetRow
                    anchors.fill: parent
                    anchors.margins: 10
                    Label { text: Tr.t("Preset"); opacity: 0.7; color: Material.foreground }
                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignRight
                        color: Material.foreground
                        // The same "slot - name" the toolbar shows and the
                        // Presets page tiles are numbered with — one preset,
                        // one way of naming it.
                        text: UI.presetLabel.length > 0
                            ? (UI.presetLabel
                               + (Synth.presetIsFactory ? ("  (" + Tr.t("factory") + ")") : ""))
                            : "—"
                    }
                }
            }

            // ---- USB ------------------------------------------------------
            // Not a ParamGroup, though `usb.mode` is an ordinary enum
            // parameter and would render as one: writing it changes nothing
            // until the synth restarts, and a control that silently does
            // nothing is worse than no control. So the write is paired with
            // the state that explains it and the button that applies it.
            Rectangle {
                id: usbCard
                width: panels.contentWidth
                height: usbCol.implicitHeight + 20
                radius: 8
                // Hidden entirely on firmware that cannot host: no USB-OTG, or
                // a build where the USB sink is the audio clock and dropping
                // the device role would leave the synth silent. The firmware
                // decides that, not the app.
                visible: Synth.usbStatusKnown && Synth.usbHostSupported
                color: Material.theme === Material.Dark ? "#1AFFFFFF" : "#0D000000"

                ColumnLayout {
                    id: usbCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 10
                    spacing: 8

                    Label {
                        text: Tr.t("USB port")
                        font.bold: true
                        font.pointSize: UI.fontSize * 0.95
                        color: Material.foreground
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.7
                        color: Material.foreground
                        font.pointSize: UI.fontSize * 0.85
                        text: Tr.t("One socket, one role. As a device the synth is an audio interface and MIDI port on a computer; as a host it plays a USB MIDI controller you plug into it. Changing this restarts the synth.")
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        ParamControl {
                            paramId: Synth.paramIdForName("usb.mode")
                            enabled: !Synth.restarting
                        }

                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Material.foreground
                            font.pointSize: UI.fontSize * 0.85
                            // Three states worth telling apart: mid-restart, a
                            // pending change, and settled.
                            text: Synth.restarting
                                  ? Tr.t("Restarting…")
                                  : Synth.usbRestartRequired
                                    ? Tr.t("Restart to apply — the port is still in %1 mode.")
                                       .arg(Synth.usbActiveMode === 1 ? Tr.t("host") : Tr.t("device"))
                                    : (Synth.usbActiveMode === 1
                                       ? Tr.t("Hosting MIDI controllers.")
                                       : Tr.t("Connected to a computer as an audio + MIDI device."))
                        }
                    }

                    // Only offered when it would do something. A restart with
                    // nothing pending is just a way to interrupt yourself.
                    Button {
                        text: Synth.restarting ? Tr.t("Restarting…") : Tr.t("Restart synth")
                        enabled: Synth.usbRestartRequired && !Synth.restarting
                        visible: Synth.usbRestartRequired || Synth.restarting
                        highlighted: true
                        onClicked: restartDialog.open()
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        visible: screen.restartFailed
                        color: Material.color(Material.Red)
                        font.pointSize: UI.fontSize * 0.85
                        text: Tr.t("The synth did not come back. It may still be restarting — check its power and Bluetooth.")
                    }

                    // ---- what is attached ----
                    // Host mode only: in device mode there is nothing on the
                    // other end of the cable to enumerate. Without this the
                    // only symptom of a cable, power or descriptor problem is
                    // "no sound", which says nothing about which of the three
                    // it is.
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        visible: Synth.usbActiveMode === 1 && !Synth.restarting
                        color: Material.foreground
                        font.pointSize: UI.fontSize * 0.85
                        text: Synth.usbAttachedCount === 0
                              ? Tr.t("No controller detected. Check that it is powered and that its cable carries data.")
                              : Synth.usbAttachedCount === 1
                                ? Tr.t("Connected: %1").arg(Synth.usbAttachedName)
                                : Tr.t("Connected: %1 (+%2 more)")
                                   .arg(Synth.usbAttachedName).arg(Synth.usbAttachedCount - 1)
                    }
                }
            }

            // Back to a blank instrument. Resolved by name rather than by a
            // hardcoded id, and hidden entirely when the connected firmware
            // does not have it — the same existence test the toolbar's
            // out.level strip uses, and for the same reason: older firmware
            // answers a write to an unregistered id with nothing at all, so a
            // button that is always there would look broken instead of absent.
            Rectangle {
                id: resetCard
                property int resetId: -1
                width: panels.contentWidth
                height: resetRow.implicitHeight + 20
                radius: 8
                visible: Synth.ready && resetId >= 0
                color: Material.theme === Material.Dark ? "#1AFFFFFF" : "#0D000000"

                function refresh() { resetId = Synth.paramIdForName("state.reset") }
                Component.onCompleted: refresh()
                Connections {
                    target: Synth
                    function onParamsDiscovered() { resetCard.refresh() }
                }

                RowLayout {
                    id: resetRow
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 8
                    Column {
                        Layout.fillWidth: true
                        Label {
                            text: Tr.t("Start from scratch")
                            color: Material.foreground
                            opacity: 0.7
                        }
                        Label {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            text: Tr.t("The synth remembers how you left it and comes back that way. This puts it back to the sound it had out of the box.")
                            color: Material.foreground
                            opacity: 0.5
                            font.pointSize: Math.max(8, UI.fontSize * 0.75)
                        }
                    }
                    Button {
                        text: Tr.t("Reset…")
                        enabled: Synth.connected
                        onClicked: resetDialog.open()
                    }
                }
            }
        }
    }

    Dialog {
        id: resetDialog
        anchors.centerIn: Overlay.overlay
        modal: true
        focus: true
        title: Tr.t("Reset the synth?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: Math.min(parent ? parent.width - 32 : 420, 420)

        // The ColumnLayout is not here for the layout — it is one label. A
        // wrapping Label used directly as a Dialog's contentItem closes a
        // ring: the dialog measures its implicitHeight from the label, and the
        // label decides its height from the width the dialog hands down inside
        // that same evaluation. Qt reported a binding loop on implicitHeight
        // for both dialogs on this page. A Layout re-arranges on its own
        // polish pass instead, which is why PlockDialog and SeqSetDialog —
        // wrapping labels in a ColumnLayout, same style, same build — never
        // warned.
        contentItem: ColumnLayout {
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Material.foreground
                // Says exactly what goes and what stays. The firmware draws
                // the same line: the working state is the patch, the graph and
                // the sequencer; the NVS settings and the looper are not in it.
                text: Tr.t("Every sound setting goes back to its default, and the sequencer patterns and the modular patch are cleared. Your saved presets, your local presets, the looper and the volume and input settings are left alone.")
            }
        }

        onAccepted: {
            if (resetCard.resetId >= 0) Synth.setParamNow(resetCard.resetId, 1)
        }
    }

    Dialog {
        id: restartDialog
        anchors.centerIn: Overlay.overlay
        modal: true
        focus: true
        title: Tr.t("Restart the synth?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: Math.min(parent ? parent.width - 32 : 420, 420)

        // Same reason as resetDialog above.
        contentItem: ColumnLayout {
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Material.foreground
                // Presets, sequencer patterns and the looper all persist; live
                // notes and an unsaved take do not, and that is the part worth
                // saying out loud before the box goes away.
                text: Tr.t("The synth will restart to change its USB role. Audio stops and the app reconnects on its own. Saved presets, patterns and loops are kept.")
            }
        }

        onAccepted: {
            screen.restartFailed = false
            Synth.restartSynth()
        }
    }

    Connections {
        target: Synth
        function onRestartTimedOut() { screen.restartFailed = true }
    }

    Label {
        anchors.centerIn: parent
        // Not while the synth is deliberately away: a restart drops the link,
        // and "Not connected" over the page you just pressed Restart on reads
        // as a failure rather than as the thing you asked for.
        visible: !Synth.connected && !Synth.restarting
        text: Tr.t("Not connected")
        opacity: 0.5
        color: Material.foreground
    }
}
