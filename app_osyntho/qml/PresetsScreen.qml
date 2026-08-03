import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// Browse / load / save the synth's own preset slots for the active engine
// (0-47 factory, 48-111 user — S33 widened the factory bank from 16).
Item {
    id: screen

    property var presetList: []
    // Deliberately not a binding to Synth.engine: the engine-change handler
    // below has to run in a defined order relative to it (request the new
    // engine's list, then repaint), and an imperative assignment inside that
    // handler would have silently broken a binding anyway. One writer, here.
    property int engine: 0
    // The save panel is raised from the header button rather than sitting on
    // the page: storing a slot is something you do now and then, and the line
    // it used to occupy is worth more to the preset tiles below.
    property bool saveOpen: false

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
        // Only this page's own Import button — the Lib page has one that stores
        // to the app's library instead, and both pages are alive at once. The
        // name seeds the save panel, ready to store it — and raises it, since
        // the imported patch is playing but is in no slot yet.
        function onJsonImported(page, text) {
            if (page !== "preset") return
            const result = Synth.importPatchJson(text)
            if (result.ok && result.name.length > 0) {
                nameField.text = result.name
                screen.saveOpen = true
            }
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
                text: t.t("Save…")
                // Raises the save panel at the foot of the page. Pressing it
                // again is the way back out of it, so the state is shown.
                highlighted: screen.saveOpen
                onClicked: screen.saveOpen = !screen.saveOpen
                ToolTip.visible: hovered
                ToolTip.text: t.t("Store the live sound in one of the synth's user slots")
            }
            ToolButton {
                text: t.t("Import…")
                // Applies the file to the live sound; the save panel then
                // writes it to a slot, which is the synth's own step.
                enabled: Synth.connected && Synth.ready
                onClicked: UI.importJsonRequested("preset")
            }
            ToolButton {
                text: t.t("Refresh")
                enabled: Synth.connected
                onClicked: screen.requestList()
            }
        }

        // Tiles rather than one slot per line: 112 slots read far better packed
        // left to right and wrapping downwards, the way PanelFlow lays out a
        // screen's panels. The whole tile is the Load button — a worded one
        // beside it would set the tile's width for no gain.
        GridView {
            id: presetGrid
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: screen.presetList
            ScrollBar.vertical: ScrollBar {}

            // Measured, not a breakpoint: a phone gets one or two columns and a
            // desktop window five, at whatever UI.fontSize is set to. Every cell
            // takes an equal share of the width so the columns line up; the
            // rounding remainder is left at the right edge.
            readonly property real minTileWidth: Math.max(150, UI.fontSize * 12)
            readonly property int columns: Math.max(1, Math.floor(width / minTileWidth))
            cellWidth: Math.floor(width / columns)
            // The slot line, the name under it, and the tile's own padding.
            // GridView wants the number up front, so it comes from the font size
            // rather than from a measured delegate.
            cellHeight: Math.max(78, Math.round(UI.fontSize * 6))

            delegate: Item {
                id: tile
                required property var modelData
                width: presetGrid.cellWidth
                height: presetGrid.cellHeight

                ItemDelegate {
                    anchors.fill: parent
                    anchors.margins: 3
                    padding: 8
                    highlighted: tile.modelData.slot === Synth.presetSlot
                    onClicked: Synth.loadPreset(screen.engine, tile.modelData.slot)
                    ToolTip.visible: hovered
                    ToolTip.text: (tile.modelData.factory ? t.t("Factory preset")
                                                          : t.t("User preset"))
                                  + " — " + t.t("tap to load")

                    contentItem: ColumnLayout {
                        spacing: 2

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            // Says what the word "factory" used to, in the room
                            // a tile has: industry for the read-only slots, user
                            // for your own.
                            Label {
                                text: tile.modelData.factory ? "\uf275"   // industry
                                                             : "\uf007"   // user
                                font.family: App.fontAwesomeName
                                font.weight: Font.Black  // solid face
                                font.pointSize: UI.fontSize * 0.8
                                color: Material.foreground
                                opacity: 0.7
                            }
                            Label {
                                Layout.fillWidth: true
                                text: tile.modelData.slot
                                font.pointSize: UI.fontSize * 0.8
                                color: Material.foreground
                                opacity: 0.7
                            }
                            // Icon-only, as it always was, and sized down from
                            // the 48 px touch target — at that size it would set
                            // the tile's height by itself.
                            ToolButton {
                                text: "\uf56e"  // file-export
                                font.family: App.fontAwesomeName
                                font.weight: Font.Black  // solid face
                                font.pointSize: UI.fontSize * 0.8
                                padding: 0
                                implicitWidth: 26
                                implicitHeight: 26
                                enabled: Synth.connected && Synth.ready
                                onClicked: Synth.exportPresetJson(screen.engine, tile.modelData.slot)
                                ToolTip.visible: hovered
                                ToolTip.text: t.t("Export this preset to a JSON file. It is loaded first — the only way to read a slot.")
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: (tile.modelData.name && tile.modelData.name.length)
                                  ? tile.modelData.name : t.t("(unnamed)")
                            color: Material.foreground
                            opacity: (tile.modelData.name && tile.modelData.name.length) ? 1.0 : 0.5
                            elide: Label.ElideRight
                        }

                        // Keeps the two lines at the top of a tile that is
                        // taller than they are, rather than centred in it.
                        Item { Layout.fillHeight: true }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: presetGrid.count === 0
                text: Synth.connected ? t.t("No presets") : t.t("Not connected")
                opacity: 0.5
                color: Material.foreground
            }
        }

        // Only up while you are actually storing something — see saveOpen.
        Rectangle {
            Layout.fillWidth: true
            visible: screen.saveOpen
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
                    from: 48
                    to: 111
                    value: 48
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
                        screen.saveOpen = false
                    }
                }
            }
        }
    }
}
