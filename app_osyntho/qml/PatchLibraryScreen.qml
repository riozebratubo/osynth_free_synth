import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// Local patch library — named snapshots of the live parameters, stored in the
// app's SQLite DB (independent of the synth's own preset slots).
Item {
    id: screen

    property var patchList: []
    readonly property var engineNames: ["Subtractive", "Additive", "FM", "Wavetable"]

    function refresh() { patchList = Synth.patches(-1) }
    Component.onCompleted: refresh()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: t.t("Patch library")
                font.bold: true
                font.pointSize: UI.fontSize * 1.1
                color: Material.foreground
                Layout.fillWidth: true
            }
            Button {
                text: t.t("Save current…")
                enabled: Synth.connected && Synth.ready
                onClicked: { nameDialog.mode = "save"; nameDialog.patchId = -1; nameDialog.field = ""; nameDialog.open() }
            }
        }

        ListView {
            id: patchListView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: screen.patchList
            spacing: 2
            ScrollBar.vertical: ScrollBar {}

            delegate: ItemDelegate {
                required property var modelData
                width: ListView.view.width

                contentItem: RowLayout {
                    spacing: 8
                    Column {
                        Layout.fillWidth: true
                        Label {
                            text: modelData.name && modelData.name.length ? modelData.name : t.t("(unnamed)")
                            color: Material.foreground
                            font.bold: true
                        }
                        Label {
                            text: t.t(screen.engineNames[modelData.engine] || "?") + " · " + modelData.created
                            color: Material.foreground
                            opacity: 0.6
                            font.pointSize: UI.fontSize * 0.75
                        }
                    }
                    Button {
                        text: t.t("Load")
                        // Same gate as "Save current…": pushing a patch needs
                        // the parameter table, which is what `ready` announces.
                        enabled: Synth.connected && Synth.ready
                        onClicked: Synth.loadPatch(modelData.id)
                    }
                    Button {
                        text: t.t("Rename")
                        onClicked: {
                            nameDialog.mode = "rename"
                            nameDialog.patchId = modelData.id
                            nameDialog.field = modelData.name
                            nameDialog.open()
                        }
                    }
                    Button {
                        text: t.t("Delete")
                        onClicked: { Synth.deletePatch(modelData.id); screen.refresh() }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: patchListView.count === 0
                text: t.t("No saved patches yet")
                opacity: 0.5
                color: Material.foreground
            }
        }
    }

    Dialog {
        id: nameDialog
        anchors.centerIn: Overlay.overlay
        modal: true
        // Without this the popup never takes the window's active focus, so the
        // field below could be given focus but not typed into.
        focus: true
        title: mode === "save" ? t.t("Save patch") : t.t("Rename patch")
        standardButtons: Dialog.Ok | Dialog.Cancel
        // The dialog sizes the field, not the other way round: an explicit width
        // on the content does not feed the dialog's implicit width, which is how
        // the old fixed-width field ended up wider than the frame around it.
        width: Math.min(parent ? parent.width - 32 : 360, 360)

        property string mode: "save"
        property int patchId: -1
        property alias field: dialogField.text

        // As *the* content item it is laid out to the dialog's content area, so
        // it follows the width above instead of overflowing it.
        contentItem: TextField {
            id: dialogField
            placeholderText: t.t("Patch name")
            onAccepted: nameDialog.accept()
        }

        onOpened: { dialogField.forceActiveFocus(); dialogField.selectAll() }
        onAccepted: {
            const name = dialogField.text.trim()
            if (name.length === 0) return
            if (mode === "save")
                Synth.saveCurrentAsPatch(name)
            else if (patchId >= 0)
                Synth.renamePatch(patchId, name)
            screen.refresh()
        }
    }
}
