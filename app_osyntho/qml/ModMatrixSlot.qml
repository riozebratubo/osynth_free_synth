import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// One mod-matrix slot (1..8): source enum, destination param picker, amount.
// Destination is a param id, so it is chosen from the discovered param list.
RowLayout {
    id: root

    property int slot: 1
    // Ids are resolved imperatively (paramIdForName has no change signal), so
    // they update when discovery completes rather than staying at their initial
    // pre-discovery value of -1.
    property int srcId: -1
    property int destId: -1
    property int amtId: -1

    visible: srcId >= 0
    spacing: 8

    property var picker: []

    function refreshIds() {
        srcId = Synth.paramIdForName("mod" + slot + ".src")
        destId = Synth.paramIdForName("mod" + slot + ".dest")
        amtId = Synth.paramIdForName("mod" + slot + ".amount")
    }
    function refreshPicker() {
        var list = [{ id: 0, name: t.t("— none —") }]
        var pl = Synth.paramPickerList()
        for (var i = 0; i < pl.length; i++) {
            // Skip PID 0 (master.volume). The firmware reads dest 0 as "no
            // destination" (synth_mod.cpp), so it cannot be modulated — and
            // leaving it in gave the list two entries sharing id 0: syncDest()
            // resolved 0 to "— none —" every time, so picking master.volume
            // silently did nothing and snapped straight back.
            if (pl[i].id === 0) continue
            list.push(pl[i])
        }
        picker = list
        syncDest()
    }
    function syncDest() {
        if (destId < 0)
            return
        var cur = Math.round(Synth.paramValue(destId))
        for (var i = 0; i < picker.length; i++) {
            if (picker[i].id === cur) {
                destCombo.currentIndex = i
                return
            }
        }
        destCombo.currentIndex = 0
    }

    Component.onCompleted: { refreshIds(); refreshPicker() }
    Connections {
        target: Synth
        function onParamsDiscovered() { root.refreshIds(); root.refreshPicker() }
        function onParamChanged(id, value) { if (id === root.destId) root.syncDest() }
    }

    Label {
        text: "#" + root.slot
        color: Material.foreground
        Layout.preferredWidth: 22
    }

    EnumSelector {
        paramId: root.srcId
        Layout.preferredWidth: 118
    }

    ColumnLayout {
        spacing: 1
        Label {
            text: t.t("dest")
            font.pointSize: UI.fontSize * 0.68
            opacity: 0.75
            color: Material.foreground
        }
        ComboBox {
            id: destCombo
            Layout.preferredWidth: 150
            model: root.picker
            textRole: "name"
            onActivated: if (root.destId >= 0 && root.picker[currentIndex])
                             Synth.setParam(root.destId, root.picker[currentIndex].id)
        }
    }

    Knob {
        paramId: root.amtId
        Layout.preferredWidth: 84
    }
}
