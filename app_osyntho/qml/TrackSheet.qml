import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// Per-track and per-pattern settings for the sequencer, plus the generators.
//
// A dialog rather than a page: these are set-and-forget (what the track plays,
// how long it is, which direction it runs) and would otherwise crowd out the
// grid, which is what you actually touch while playing.
Dialog {
    id: root

    property var cfg: ({})
    property var pat: ({})

    function reload() {
        cfg = Synth.trackConfig()
        pat = Synth.patternConfig()
    }

    onAboutToShow: reload()
    Connections {
        target: Synth
        function onTrackConfigChanged() { if (root.visible) root.reload() }
    }

    title: t.t("Track") + " " + (Synth.editTrack + 1) + " — "
           + t.t("Pattern") + " " + (Synth.editPattern + 1)
    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(parent ? parent.width - 32 : 420, 560)
    standardButtons: Dialog.Close

    contentItem: Flickable {
        implicitHeight: Math.min(content.implicitHeight, UI.window ? UI.window.height * 0.62 : 480)
        contentHeight: content.implicitHeight
        clip: true

        ColumnLayout {
            id: content
            width: parent.width
            spacing: 10

            // ---- what this track plays -----------------------------------
            Label {
                text: t.t("Output")
                font.bold: true
                color: Material.foreground
            }
            Flow {
                Layout.fillWidth: true
                spacing: 10

                ColumnLayout {
                    spacing: 0
                    Label {
                        text: t.t("Target")
                        font.pointSize: UI.fontSize * 0.7
                        opacity: 0.7
                        color: Material.foreground
                    }
                    SyncedComboBox {
                        model: Synth.targetNames()
                        modelIndex: root.cfg.target !== undefined ? root.cfg.target : 0
                        onActivated: Synth.setTrackField("target", currentIndex)
                    }
                }

                // Drum tracks pick a kit slot; "from note" lets one lane play
                // the whole kit, addressed by each step's note.
                ColumnLayout {
                    spacing: 0
                    visible: root.cfg.target === 1
                    Label {
                        text: t.t("Drum slot")
                        font.pointSize: UI.fontSize * 0.7
                        opacity: 0.7
                        color: Material.foreground
                    }
                    SyncedComboBox {
                        id: slotBox
                        // Index 0 is the "note picks it" entry; the rest are
                        // the current kit's slots, labelled with their names.
                        model: {
                            var out = [t.t("from step note")]
                            var slots = Synth.kitSlots
                            for (var i = 0; i < slots.length; ++i)
                                out.push((i + 1) + "  " + slots[i].name)
                            return out
                        }
                        modelIndex: root.cfg.noteToSlot ? 0
                                    : ((root.cfg.slot !== undefined ? root.cfg.slot : 0) + 1)
                        onActivated: Synth.setTrackField("slot", currentIndex === 0 ? 255 : currentIndex - 1)
                    }
                }
            }

            // ---- time ------------------------------------------------------
            Label {
                text: t.t("Time")
                font.bold: true
                color: Material.foreground
            }
            Flow {
                Layout.fillWidth: true
                spacing: 10

                StepField {
                    label: t.t("Length")
                    from: 1
                    to: Math.max(1, Synth.seqMaxSteps)
                    suffix: t.t("steps")
                    value: root.cfg.length !== undefined ? root.cfg.length : 64
                    onEdited: (v) => Synth.setTrackField("length", v)
                }

                ColumnLayout {
                    spacing: 0
                    Label {
                        text: t.t("Division")
                        font.pointSize: UI.fontSize * 0.7
                        opacity: 0.7
                        color: Material.foreground
                    }
                    SyncedComboBox {
                        model: Synth.divNames()
                        modelIndex: root.cfg.div !== undefined ? root.cfg.div : 6
                        onActivated: Synth.setTrackField("div", currentIndex)
                    }
                }

                ColumnLayout {
                    spacing: 0
                    Label {
                        text: t.t("Direction")
                        font.pointSize: UI.fontSize * 0.7
                        opacity: 0.7
                        color: Material.foreground
                    }
                    SyncedComboBox {
                        model: Synth.dirNames()
                        modelIndex: root.cfg.dir !== undefined ? root.cfg.dir : 0
                        onActivated: Synth.setTrackField("dir", currentIndex)
                    }
                }

                StepField {
                    label: t.t("Swing")
                    // 0 means "follow the pattern" in this control; the wire
                    // encoding for that is 0xFF, mapped by the controller.
                    from: 0; to: 75
                    suffix: root.cfg.followsPatternSwing ? t.t("(pattern)") : "%"
                    value: root.cfg.followsPatternSwing ? 0
                           : (root.cfg.swing !== undefined ? root.cfg.swing : 50)
                    onEdited: (v) => Synth.setTrackField("swing", v === 0 ? -1 : v)
                }
            }

            // ---- feel ------------------------------------------------------
            Label {
                text: t.t("Feel")
                font.bold: true
                color: Material.foreground
            }
            Flow {
                Layout.fillWidth: true
                spacing: 10

                StepField {
                    label: t.t("Transpose")
                    from: -24; to: 24
                    suffix: t.t("semitones")
                    value: root.cfg.transpose !== undefined ? root.cfg.transpose : 0
                    onEdited: (v) => Synth.setTrackField("transpose", v)
                }
                StepField {
                    label: t.t("Gate scale")
                    from: 0; to: 200
                    suffix: "%"
                    value: root.cfg.gateScale !== undefined ? root.cfg.gateScale : 100
                    onEdited: (v) => Synth.setTrackField("gateScale", v)
                }
                // The same control as the sequencer page's "Level" slider —
                // named for what it does rather than what it is for, since
                // this page is where you set the exact number.
                StepField {
                    label: t.t("Track level (velocity)")
                    from: 0; to: 200
                    // Above 100 % the boost runs out early: step velocities
                    // are capped at 127, so a track already played hard has
                    // little headroom left to gain.
                    suffix: root.cfg.velScale > 100 ? "% " + t.t("(caps at 127)") : "%"
                    value: root.cfg.velScale !== undefined ? root.cfg.velScale : 100
                    onEdited: (v) => Synth.setTrackField("velScale", v)
                }
                StepField {
                    label: t.t("Probability scale")
                    from: 0; to: 100
                    suffix: "%"
                    value: root.cfg.probScale !== undefined ? root.cfg.probScale : 100
                    onEdited: (v) => Synth.setTrackField("probScale", v)
                }
                StepField {
                    label: t.t("Humanize")
                    from: 0; to: 100
                    suffix: "%"
                    value: root.cfg.humanize !== undefined ? root.cfg.humanize : 0
                    onEdited: (v) => Synth.setTrackField("humanize", v)
                }
            }

            // ---- pattern (shared by every track) ---------------------------
            Label {
                text: t.t("Pattern")
                font.bold: true
                color: Material.foreground
            }
            Flow {
                Layout.fillWidth: true
                spacing: 10

                ColumnLayout {
                    spacing: 0
                    Label {
                        text: t.t("Scale")
                        font.pointSize: UI.fontSize * 0.7
                        opacity: 0.7
                        color: Material.foreground
                    }
                    SyncedComboBox {
                        model: Synth.scaleNames()
                        modelIndex: root.pat.scale !== undefined ? root.pat.scale : 0
                        onActivated: Synth.setPatternField("scale", currentIndex)
                    }
                }
                ColumnLayout {
                    spacing: 0
                    Label {
                        text: t.t("Root")
                        font.pointSize: UI.fontSize * 0.7
                        opacity: 0.7
                        color: Material.foreground
                    }
                    SyncedComboBox {
                        model: ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
                        modelIndex: root.pat.root !== undefined ? root.pat.root : 0
                        onActivated: Synth.setPatternField("root", currentIndex)
                    }
                }
                StepField {
                    label: t.t("Pattern swing")
                    from: 25; to: 75
                    suffix: "%"
                    value: root.pat.swing !== undefined ? root.pat.swing : 50
                    onEdited: (v) => Synth.setPatternField("swing", v)
                }
            }

            // ---- generators ------------------------------------------------
            Label {
                text: t.t("Generate")
                font.bold: true
                color: Material.foreground
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                SpinBox {
                    id: pulses
                    from: 0
                    to: Math.max(1, root.cfg.length !== undefined ? root.cfg.length : 16)
                    value: 4
                }
                Label {
                    text: t.t("hits over")
                    color: Material.foreground
                    Layout.alignment: Qt.AlignVCenter
                }
                SpinBox {
                    id: euclidSteps
                    from: 1
                    to: Math.max(1, Synth.seqMaxSteps)
                    value: 16
                }
                Button {
                    text: t.t("Euclid")
                    // Spreads `pulses` as evenly as possible over `steps` — the
                    // most useful generator to have on a device with no
                    // keyboard, and it sets the track length to match. Same
                    // note rule as a grid tap, so the two never disagree about
                    // what a drum lane's steps should carry.
                    onClicked: Synth.euclidFill(
                                   pulses.value, euclidSteps.value, 0,
                                   Synth.noteForNewStep(
                                       root.cfg.noteToSlot === true ? UI.drumNote
                                                                    : UI.paintNote),
                                   100)
                }
            }
            Flow {
                Layout.fillWidth: true
                spacing: 6
                Button {
                    text: t.t("Rotate ←")
                    onClicked: Synth.rotateTrack(-1)
                }
                Button {
                    text: t.t("Rotate →")
                    onClicked: Synth.rotateTrack(1)
                }
                Button {
                    text: t.t("Humanize now")
                    // Bakes jitter into the stored velocities/micro-timing,
                    // unlike the per-track `humanize` above, which re-rolls on
                    // every pass.
                    onClicked: Synth.humanizeTrack(35)
                }
                Button {
                    text: t.t("Clear track")
                    onClicked: Synth.clearTrack()
                }
                Button {
                    text: t.t("Clear pattern")
                    onClicked: Synth.clearPattern()
                }
            }
        }
    }
}
