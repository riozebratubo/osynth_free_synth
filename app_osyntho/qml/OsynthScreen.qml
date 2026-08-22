import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts

import org.osynth.osyntho

// The synth itself, rather than the patch loaded into it (S35).
//
// Everything here is a *persisted* firmware parameter — the ones main.cpp
// lists in kPersisted, which survive a power cycle and which presets
// deliberately skip. They describe the box and what is plugged into it, not
// the sound: master volume is the room's gain, out.level is set once by ear
// for whatever is in the output jack, the line-input trio describes the source
// feeding it, and usb.mode picks what the OTG port is for.
//
// The volume and headphone controls are also on the toolbar, where they belong
// for performance. Repeating them here is not an oversight: this page is the
// one place that answers "what is this synth set to", and a settings page
// missing the two settings everyone changes would send you back to the toolbar
// to read them.
Item {
    id: root

    // Poll the port while the page is on screen. There is no event for "a
    // controller was plugged in" — a notification the app could only act on
    // with this page visible is worth less than the page asking for itself —
    // and 3 s is well under the time it takes to notice a keyboard is dead.
    Timer {
        interval: 3000
        running: root.visible && Synth.connected
        repeat: true
        triggeredOnStart: true
        onTriggered: Synth.refreshUsbStatus()
    }

    // Set when a restart never completed, cleared the moment one is asked for
    // again. Shown inline rather than as a toast: toasts live in Main.qml's
    // scope and a page cannot reach one, and this is a message you want still
    // on screen while you go and look at the synth.
    property bool restartFailed: false

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: col.implicitHeight + 24
        clip: true

        // A plain Column, not a ColumnLayout: PanelFlow binds its own
        // `width: parent.width`, which a Layout would fight over.
        Column {
            id: col
            width: parent.width
            spacing: 4

            PanelFlow {
                // master.volume — always registered, on every build.
                ParamGroup { title: "Master"; prefix: "master." }
                // out.level — ES8388 builds only; self-hides elsewhere, which
                // is the same existence test the toolbar's copy uses.
                ParamGroup { title: "Analogue output"; prefix: "out." }
                // in.route / in.gain / in.pga — input builds only. On a
                // build with a microphone as well, in.source and in.micgain
                // join them: the source picks line, mic or both, and the
                // micgain is the mic's own level trim so "both" is usable.
                // Hence the title is no longer "Line input" — the firmware
                // decides how many sockets there are, and the group renders
                // whatever it registered.
                ParamGroup { title: "Audio input"; prefix: "in." }
            }

            // ---- USB ------------------------------------------------------
            // Not a ParamGroup, though `usb.mode` is an ordinary enum
            // parameter and would render as one: writing it changes nothing
            // until the synth restarts, and a control that silently does
            // nothing is worse than no control. So the write is paired with
            // the state that explains it and the button that applies it.
            Rectangle {
                x: 12
                width: parent.width - 24
                radius: 8
                color: Material.theme === Material.Dark ? "#1AFFFFFF" : "#0D000000"
                implicitHeight: usbCol.implicitHeight + 16
                height: visible ? implicitHeight : 0
                // Hidden entirely on firmware that cannot host: no USB-OTG, or
                // a build where the USB sink is the audio clock and dropping
                // the device role would leave the synth silent. The firmware
                // decides that, not the app.
                visible: Synth.usbStatusKnown && Synth.usbHostSupported

                ColumnLayout {
                    id: usbCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 8
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
                        visible: root.restartFailed
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
        }
    }

    Dialog {
        id: restartDialog
        anchors.centerIn: parent
        modal: true
        title: Tr.t("Restart the synth?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: {
            root.restartFailed = false
            Synth.restartSynth()
        }

        Label {
            width: Math.min(root.width * 0.8, 420)
            wrapMode: Text.WordWrap
            color: Material.foreground
            // Presets, sequencer patterns and the looper all persist; live
            // notes and an unsaved take do not, and that is the part worth
            // saying out loud before the box goes away.
            text: Tr.t("The synth will restart to change its USB role. Audio stops and the app reconnects on its own. Saved presets, patterns and loops are kept.")
        }
    }

    Connections {
        target: Synth
        function onRestartTimedOut() { root.restartFailed = true }
    }

    Label {
        anchors.centerIn: parent
        visible: !Synth.ready && !Synth.restarting
        text: Synth.connected ? Tr.t("Discovering parameters…") : Tr.t("Not connected")
        opacity: 0.5
        color: Material.foreground
    }
}
