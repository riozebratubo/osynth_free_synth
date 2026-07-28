import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// Browse / load / save the synth's own preset slots for the active engine
// (0-15 factory, 16-79 user).
Item {
    id: screen

    property var presetList: []
    // Deliberately not a binding to Synth.engine: the engine-change handler
    // below has to run in a defined order relative to it (request the new
    // engine's list, then repaint), and an imperative assignment inside that
    // handler would have silently broken a binding anyway. One writer, here.
    property int engine: 0

    function refresh() { presetList = Synth.presetsFor(engine) }
    function requestList() { if (Synth.connected) Synth.listPresets(engine) }

    Component.onCompleted: {
        engine = Synth.engine
        requestList()
        refresh()
    }

    Connections {
        target: Synth
        function onPresetsChanged(eng) { if (eng === screen.engine) screen.refresh() }
        function onEngineChanged() { screen.engine = Synth.engine; screen.requestList(); screen.refresh() }
        function onReadyChanged() { screen.requestList() }
        function onConnectedChanged() { if (Synth.connected) screen.requestList() }
        // exportPresetJson() had to load the slot to read it; the file text
        // comes back here once the values have landed.
        function onPresetJsonReady(json, name) { UI.exportJsonRequested(json, name) }
    }

    Connections {
        target: UI
        // This page owns the Import button, so it is the one that consumes the
        // picked file. The name seeds the save row below, ready to store it.
        function onJsonImported(text) {
            const result = Synth.importPatchJson(text)
            if (result.ok && result.name.length > 0) nameField.text = result.name
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: t.t("Presets") + " — " + t.t(Synth.engineName)
                font.bold: true
                font.pointSize: UI.fontSize * 1.1
                color: Material.foreground
                elide: Label.ElideRight
                Layout.fillWidth: true
            }
            ToolButton {
                text: t.t("Import…")
                // Applies the file to the live sound; the save row below then
                // writes it to a slot, which is the synth's own step.
                enabled: Synth.connected && Synth.ready
                onClicked: UI.importJsonRequested()
            }
            ToolButton {
                text: t.t("Refresh")
                enabled: Synth.connected
                onClicked: screen.requestList()
            }
        }

        ListView {
            id: presetListView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: screen.presetList
            spacing: 2
            ScrollBar.vertical: ScrollBar {}

            delegate: ItemDelegate {
                required property var modelData
                width: ListView.view.width
                highlighted: modelData.slot === Synth.presetSlot

                contentItem: RowLayout {
                    spacing: 8
                    Label {
                        text: modelData.slot
                        Layout.preferredWidth: 36
                        color: Material.foreground
                    }
                    Label {
                        Layout.fillWidth: true
                        text: (modelData.name && modelData.name.length) ? modelData.name : t.t("(unnamed)")
                        color: Material.foreground
                    }
                    Label {
                        visible: modelData.factory
                        text: t.t("factory")
                        opacity: 0.6
                        color: Material.foreground
                    }
                    // Icon-only, to keep the row usable on a phone.
                    ToolButton {
                        text: "\uf56e"  // file-export
                        font.family: App.fontAwesomeName
                        font.weight: Font.Black  // solid face
                        font.pointSize: UI.fontSize
                        enabled: Synth.connected && Synth.ready
                        onClicked: Synth.exportPresetJson(screen.engine, modelData.slot)
                        ToolTip.visible: hovered
                        ToolTip.text: t.t("Export this preset to a JSON file. It is loaded first — the only way to read a slot.")
                    }
                    Button {
                        text: t.t("Load")
                        onClicked: Synth.loadPreset(screen.engine, modelData.slot)
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: presetListView.count === 0
                text: Synth.connected ? t.t("No presets") : t.t("Not connected")
                opacity: 0.5
                color: Material.foreground
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: saveRow.implicitHeight + 16
            radius: 8
            color: Material.theme === Material.Dark ? "#1AFFFFFF" : "#0D000000"
            RowLayout {
                id: saveRow
                anchors.fill: parent
                anchors.margins: 8
                spacing: 8
                Label { text: t.t("Save to slot"); color: Material.foreground }
                SpinBox {
                    id: slotBox
                    from: 16
                    to: 79
                    value: 16
                }
                TextField {
                    id: nameField
                    Layout.fillWidth: true
                    placeholderText: t.t("Preset name")
                }
                Button {
                    text: t.t("Save")
                    enabled: Synth.connected
                    onClicked: {
                        Synth.savePreset(screen.engine, slotBox.value, nameField.text.trim())
                        nameField.clear()
                    }
                }
            }
        }
    }
}
