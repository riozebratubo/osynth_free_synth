import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// Sequencer sets (firmware S27): eight slots on the synth's filesystem, each
// holding the whole sequencer — every pattern with its tracks, configuration
// and parameter locks, the song chain, and the arrangement parameters (tempo,
// swing, accent, arpeggiator, track mutes).
//
// Both controls are trigger parameters: writing the slot number *is* the
// operation, exactly like the looper's save/load set. The firmware writes the
// slot back when it finishes, which is both the confirmation shown here and
// the signal to re-read the pattern the editor is displaying.
Dialog {
    id: dlg

    property int idSave: -1
    property int idLoad: -1
    // Highest slot the firmware offers, from the discovered parameter range.
    property int slotMax: 0
    readonly property bool available: idSave >= 0 && idLoad >= 0
    // "" | "save" | "load" — the operation waiting for its acknowledgement.
    property string pending: ""
    property string statusText: ""

    title: Tr.t("Sequencer sets")
    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(parent ? parent.width - 32 : 420, 480)
    standardButtons: Dialog.Close

    function rebind() {
        idSave = Synth.paramIdForName("preset.seqset.save")
        idLoad = Synth.paramIdForName("preset.seqset.load")
        slotMax = idSave >= 0 ? Math.round(Synth.paramMeta(idSave).max) : 0
    }

    Component.onCompleted: rebind()
    onOpened: { rebind(); pending = ""; statusText = "" }

    Connections {
        target: Synth
        function onParamsDiscovered() { dlg.rebind() }
        function onParamChanged(id: int, value: real): void {
            // The firmware reflects the trigger once the preset task is done —
            // and only on success, so a silent slot means the operation failed
            // and the settle timer below has the last word.
            const slot = Math.round(value)
            if (id === dlg.idLoad && dlg.pending === "load") {
                dlg.pending = ""
                dlg.statusText = Tr.ts("Loaded set %1.", slot)
                Synth.refreshSequencer()
            } else if (id === dlg.idSave && dlg.pending === "save") {
                dlg.pending = ""
                dlg.statusText = Tr.ts("Saved to set %1.", slot)
            }
        }
    }

    // A failed load (empty or corrupt slot) reflects nothing, and a
    // notification is not a guaranteed delivery either. Re-read the sequencer
    // regardless, so the grid can never sit on a pattern the synth no longer
    // has.
    Timer {
        id: settle
        interval: 1200
        repeat: false
        onTriggered: {
            const wasLoad = dlg.pending === "load"
            if (dlg.pending !== "") {
                dlg.pending = ""
                dlg.statusText = Tr.t("No answer from the synth — the slot may be empty, or its storage full. Its log says which.")
            }
            if (wasLoad) Synth.refreshSequencer()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: dlg.available

            Label {
                text: Tr.t("Slot")
                color: Material.foreground
                opacity: 0.7
            }
            SpinBox {
                id: slotBox
                from: 0
                to: dlg.slotMax
                value: 0
            }
            Item { Layout.fillWidth: true }
            Button {
                text: Tr.t("Save set")
                enabled: Synth.connected && dlg.pending === ""
                // pending is armed *after* the write: setParam echoes the
                // value back synchronously for a responsive UI, and taking
                // that echo for the synth's answer would report a save that
                // has not happened yet.
                onClicked: {
                    dlg.statusText = Tr.t("Saving…")
                    Synth.setParam(dlg.idSave, slotBox.value)
                    dlg.pending = "save"
                    settle.restart()
                }
            }
            Button {
                text: Tr.t("Load set")
                highlighted: true
                enabled: Synth.connected && dlg.pending === ""
                onClicked: {  // see the note on Save set
                    dlg.statusText = Tr.t("Loading…")
                    Synth.setParam(dlg.idLoad, slotBox.value)
                    dlg.pending = "load"
                    settle.restart()
                }
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Material.foreground
            opacity: 0.6
            font.pointSize: UI.fontSize * 0.8
            visible: dlg.available
            text: Tr.t("A set is every pattern, the song chain and the arrangement parameters. Loading replaces all of them; saving writes to the synth's flash, which briefly interrupts the audio — do it with the transport stopped.")
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Material.accent
            visible: dlg.statusText !== ""
            text: dlg.statusText
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Material.foreground
            opacity: 0.6
            visible: !dlg.available
            text: Tr.t("This firmware has no sequencer sets.")
        }
    }
}
