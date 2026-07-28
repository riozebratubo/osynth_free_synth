import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// Attach a parameter lock to one step: pick any registered parameter, then set
// the value the step should force.
//
// The picker reuses Synth.paramPickerList() — the same list the mod matrix
// uses for its destinations — because "every registered parameter" is exactly
// the right set: a lock is a scheduled parameter write, so anything the synth
// exposes is fair game (a filter cutoff on step 5, a different drum slot's
// tune on step 12, an FX mix on the last step of the bar).
Dialog {
    id: root

    property int step: -1
    property var choices: []
    property int pid: -1
    property var meta: ({})

    function openFor(stepIndex) {
        step = stepIndex
        choices = Synth.paramPickerList()
        picker.currentIndex = -1
        picker.editText = ""
        pid = -1
        meta = ({})
        open()
    }

    // Adopt the parameter at `index` in the picker list. Shared by the two ways
    // of choosing one — clicking the popup and typing the name — so they cannot
    // drift apart.
    function choose(index) {
        if (index < 0 || index >= choices.length) return
        pid = choices[index].id
        meta = Synth.paramMeta(pid)
        // Seed with the parameter's live value: a lock almost always starts as
        // "what it sounds like now, but only on this step".
        valueSlider.value = Synth.paramValue(pid)
    }

    title: t.t("Lock a parameter on step") + " " + (step + 1)
    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(parent ? parent.width - 32 : 400, 480)
    standardButtons: Dialog.Cancel

    onAccepted: {}

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        ComboBox {
            id: picker
            Layout.fillWidth: true
            model: root.choices
            textRole: "name"
            valueRole: "id"
            editable: true
            // The list runs to a few hundred entries on a loaded patch, so let
            // the user type to narrow it rather than scroll.
            onActivated: (index) => root.choose(index)
            // Enter in the editable field emits accepted(), NOT activated() —
            // so handling only the latter meant the advertised "type the name"
            // route selected nothing: `pid` stayed -1, which left the value
            // slider hidden and the Add button dead with the name sitting
            // right there in the box. find() resolves the typed text against
            // the list; anything that matches nothing is left alone.
            onAccepted: {
                const i = find(editText, Qt.MatchFixedString)
                if (i >= 0) {
                    currentIndex = i
                    root.choose(i)
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            // >= 0, not > 0: master.volume is PID 0x0000, so a `> 0` gate left
            // it selectable in the picker but unlockable — no slider, and the
            // Add button dead. -1 is the "nothing picked yet" sentinel.
            visible: root.pid >= 0 && root.meta.exists === true
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: root.meta.name !== undefined ? root.meta.name : ""
                    color: Material.foreground
                    Layout.fillWidth: true
                }
                Label {
                    text: valueSlider.value.toFixed(3)
                    color: Material.foreground
                    font.bold: true
                }
            }

            Slider {
                id: valueSlider
                Layout.fillWidth: true
                from: root.meta.min !== undefined ? root.meta.min : 0
                to: root.meta.max !== undefined ? root.meta.max : 1
                // Int/Enum/Bool params snap; floats stay continuous.
                stepSize: (root.meta.type !== undefined && root.meta.type !== 0) ? 1 : 0
                snapMode: stepSize > 0 ? Slider.SnapAlways : Slider.NoSnap
            }

            Label {
                visible: root.meta.enumNames !== undefined && root.meta.enumNames.length > 0
                text: {
                    if (!root.meta.enumNames) return ""
                    var i = Math.round(valueSlider.value)
                    return (i >= 0 && i < root.meta.enumNames.length) ? root.meta.enumNames[i] : ""
                }
                opacity: 0.7
                color: Material.foreground
            }
        }

        Button {
            Layout.fillWidth: true
            text: t.t("Add lock")
            enabled: root.pid >= 0  // see the note above: PID 0 is master.volume
            highlighted: true
            onClicked: {
                Synth.setPlock(root.step, root.pid, valueSlider.value)
                root.close()
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.6
            font.pointSize: UI.fontSize * 0.75
            color: Material.foreground
            text: t.t("A locked parameter is forced while this step plays and "
                      + "restored when the track next plays a step without that "
                      + "lock.")
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: Synth.seqPlockUsed >= Synth.seqPlockCapacity
            color: Material.accent
            font.pointSize: UI.fontSize * 0.75
            text: t.t("The lock pool is full — remove a lock before adding another.")
        }
    }
}
