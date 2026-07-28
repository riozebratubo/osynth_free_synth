import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// Browse / load / save the synth's own preset slots for the active engine
// (0-15 factory, 16-79 user).
Item {
    id: screen

    property var presetList: []
    property int engine: Synth.engine

    function refresh() { presetList = Synth.presetsFor(engine) }
    function requestList() { if (Synth.connected) Synth.listPresets(engine) }

    Component.onCompleted: { requestList(); refresh() }

    Connections {
        target: Synth
        function onPresetsChanged(eng) { if (eng === screen.engine) screen.refresh() }
        function onEngineChanged() { screen.engine = Synth.engine; screen.requestList(); screen.refresh() }
        function onReadyChanged() { screen.requestList() }
        function onConnectedChanged() { if (Synth.connected) screen.requestList() }
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
                Layout.fillWidth: true
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
