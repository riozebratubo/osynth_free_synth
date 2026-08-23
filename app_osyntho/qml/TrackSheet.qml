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

    property trackConfig cfg
    property patternConfig pat

    function reload() {
        cfg = Synth.trackConfig()
        pat = Synth.patternConfig()
    }

    onAboutToShow: reload()
    Connections {
        target: Synth
        function onTrackConfigChanged() { if (root.visible) root.reload() }
    }

    title: Tr.t("Track") + " " + (Synth.editTrack + 1) + " — "
           + Tr.t("Pattern") + " " + (Synth.editPattern + 1)
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
                text: Tr.t("Output")
                font.bold: true
                color: Material.foreground
            }
            Flow {
                Layout.fillWidth: true
                spacing: 10

                ColumnLayout {
                    spacing: 0
                    Label {
                        text: Tr.t("Target")
                        font.pointSize: UI.fontSize * 0.7
                        opacity: 0.7
                        color: Material.foreground
                    }
                    SyncedComboBox {
                        model: Synth.targetNames()
                        modelIndex: root.cfg.target
                        onActivated: Synth.setTrackField("target", currentIndex)
                    }
                }

                // Drum tracks pick a kit slot; "from note" lets one lane play
                // the whole kit, addressed by each step's note.
                ColumnLayout {
                    spacing: 0
                    visible: root.cfg.target === 1
                    Label {
                        text: Tr.t("Drum slot")
                        font.pointSize: UI.fontSize * 0.7
                        opacity: 0.7
                        color: Material.foreground
                    }
                    SyncedComboBox {
                        id: slotBox
                        // Index 0 is the "note picks it" entry; the rest are
                        // the current kit's POPULATED slots. KIT_INFO reports
                        // every slot the build compiles in, empty ones
                        // included — and an empty one is a lane that makes no
                        // sound, listed here as a bare number with nothing
                        // after it. DrumsScreen drops those from its cards for
                        // the same reason; the two now agree on what a kit is.
                        readonly property var slotList: {
                            var out = []
                            var slots = Synth.kitSlots
                            for (var i = 0; i < slots.length; ++i)
                                if (slots[i].name !== "") out.push(slots[i])
                            return out
                        }
                        model: {
                            var out = [Tr.t("from step note")]
                            for (var i = 0; i < slotBox.slotList.length; ++i)
                                out.push((slotBox.slotList[i].slot + 1) + "  "
                                         + slotBox.slotList[i].name)
                            return out
                        }
                        // A lane stores a firmware slot NUMBER, which is only
                        // the same thing as a position in the list above while
                        // the list holds every slot. Resolve it instead of
                        // offsetting by one. -1 (nothing selected) when the
                        // lane points at a slot this kit leaves empty, or
                        // before the kit has arrived: showing "from step note"
                        // there would misreport what the lane actually does.
                        modelIndex: {
                            if (root.cfg.noteToSlot) return 0
                            const s = root.cfg.slot
                            for (var i = 0; i < slotBox.slotList.length; ++i)
                                if (slotBox.slotList[i].slot === s) return i + 1
                            return -1
                        }
                        onActivated: {
                            if (currentIndex === 0) {
                                Synth.setTrackField("slot", 255)
                            } else if (currentIndex > 0
                                       && currentIndex <= slotBox.slotList.length) {
                                Synth.setTrackField(
                                    "slot", slotBox.slotList[currentIndex - 1].slot)
                            }
                        }
                    }
                }
            }

            // ---- time ------------------------------------------------------
            Label {
                text: Tr.t("Time")
                font.bold: true
                color: Material.foreground
            }
            Flow {
                Layout.fillWidth: true
                spacing: 10

                StepField {
                    label: Tr.t("Length")
                    from: 1
                    to: Math.max(1, Synth.seqMaxSteps)
                    suffix: Tr.t("steps")
                    value: root.cfg.length
                    onEdited: (v) => Synth.setTrackField("length", v)
                }

                ColumnLayout {
                    spacing: 0
                    Label {
                        text: Tr.t("Division")
                        font.pointSize: UI.fontSize * 0.7
                        opacity: 0.7
                        color: Material.foreground
                    }
                    SyncedComboBox {
                        model: Synth.divNames()
                        modelIndex: root.cfg.div
                        onActivated: Synth.setTrackField("div", currentIndex)
                    }
                }

                ColumnLayout {
                    spacing: 0
                    Label {
                        text: Tr.t("Direction")
                        font.pointSize: UI.fontSize * 0.7
                        opacity: 0.7
                        color: Material.foreground
                    }
                    SyncedComboBox {
                        model: Synth.dirNames()
                        modelIndex: root.cfg.dir
                        onActivated: Synth.setTrackField("dir", currentIndex)
                    }
                }

                StepField {
                    label: Tr.t("Swing")
                    // 0 means "follow the pattern" in this control; the wire
                    // encoding for that is 0xFF, mapped by the controller.
                    from: 0; to: 75
                    suffix: root.cfg.followsPatternSwing ? Tr.t("(pattern)") : "%"
                    value: root.cfg.followsPatternSwing ? 0 : root.cfg.swing
                    onEdited: (v) => Synth.setTrackField("swing", v === 0 ? -1 : v)
                }
            }

            // ---- feel ------------------------------------------------------
            Label {
                text: Tr.t("Feel")
                font.bold: true
                color: Material.foreground
            }
            Flow {
                Layout.fillWidth: true
                spacing: 10

                StepField {
                    label: Tr.t("Transpose")
                    from: -24; to: 24
                    suffix: Tr.t("semitones")
                    value: root.cfg.transpose
                    onEdited: (v) => Synth.setTrackField("transpose", v)
                }
                StepField {
                    label: Tr.t("Gate scale")
                    from: 0; to: 200
                    suffix: "%"
                    value: root.cfg.gateScale
                    onEdited: (v) => Synth.setTrackField("gateScale", v)
                }
                // The same control as the sequencer page's "Level" slider —
                // named for what it does rather than what it is for, since
                // this page is where you set the exact number.
                StepField {
                    label: Tr.t("Track level (velocity)")
                    from: 0; to: 200
                    // Above 100 % the boost runs out early: step velocities
                    // are capped at 127, so a track already played hard has
                    // little headroom left to gain.
                    suffix: root.cfg.velScale > 100 ? "% " + Tr.t("(caps at 127)") : "%"
                    value: root.cfg.velScale
                    onEdited: (v) => Synth.setTrackField("velScale", v)
                }
                StepField {
                    label: Tr.t("Probability scale")
                    from: 0; to: 100
                    suffix: "%"
                    value: root.cfg.probScale
                    onEdited: (v) => Synth.setTrackField("probScale", v)
                }
                StepField {
                    label: Tr.t("Humanize")
                    from: 0; to: 100
                    suffix: "%"
                    value: root.cfg.humanize
                    onEdited: (v) => Synth.setTrackField("humanize", v)
                }
            }

            // ---- pattern (shared by every track) ---------------------------
            Label {
                text: Tr.t("Pattern")
                font.bold: true
                color: Material.foreground
            }
            Flow {
                Layout.fillWidth: true
                spacing: 10

                ColumnLayout {
                    spacing: 0
                    Label {
                        text: Tr.t("Scale")
                        font.pointSize: UI.fontSize * 0.7
                        opacity: 0.7
                        color: Material.foreground
                    }
                    SyncedComboBox {
                        model: Synth.scaleNames()
                        modelIndex: root.pat.scale
                        onActivated: Synth.setPatternField("scale", currentIndex)
                    }
                }
                ColumnLayout {
                    spacing: 0
                    Label {
                        text: Tr.t("Root")
                        font.pointSize: UI.fontSize * 0.7
                        opacity: 0.7
                        color: Material.foreground
                    }
                    SyncedComboBox {
                        model: ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
                        modelIndex: root.pat.root
                        onActivated: Synth.setPatternField("root", currentIndex)
                    }
                }
                StepField {
                    label: Tr.t("Pattern swing")
                    from: 25; to: 75
                    suffix: "%"
                    value: root.pat.swing
                    onEdited: (v) => Synth.setPatternField("swing", v)
                }
            }

            // ---- generators ------------------------------------------------
            Label {
                text: Tr.t("Generate")
                font.bold: true
                color: Material.foreground
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                SpinBox {
                    id: pulses
                    from: 0
                    to: Math.max(1, root.cfg.length)
                    value: 4
                }
                Label {
                    text: Tr.t("hits over")
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
                    text: Tr.t("Euclid")
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
                    text: Tr.t("Rotate ←")
                    onClicked: Synth.rotateTrack(-1)
                }
                Button {
                    text: Tr.t("Rotate →")
                    onClicked: Synth.rotateTrack(1)
                }
                Button {
                    text: Tr.t("Humanize now")
                    // Bakes jitter into the stored velocities/micro-timing,
                    // unlike the per-track `humanize` above, which re-rolls on
                    // every pass.
                    onClicked: Synth.humanizeTrack(35)
                }
                Button {
                    text: Tr.t("Clear track")
                    onClicked: Synth.clearTrack()
                }
                Button {
                    text: Tr.t("Clear pattern")
                    onClicked: Synth.clearPattern()
                }
            }
        }
    }
}
