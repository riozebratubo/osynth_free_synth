import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

// Local patch library — named snapshots of the whole synth, stored in the app's
// SQLite DB (independent of the synth's own preset slots): every parameter that
// is not an action, plus the modular graph the parameters hang off when the
// engine has one.
Item {
    id: screen

    property var patchList: []
    // Whether a Load also replays the two levels the snapshot carries. Off by
    // default and remembered per install: both describe the room and the
    // wiring rather than the sound — master volume is what you are monitoring
    // at, out.level is set once by ear for whatever is in the jack — and a
    // patch that moved either behind you is the surprise the synth's own
    // presets exclude them to avoid. They stay in the stored snapshot either
    // way, so turning a switch on later finds the value still there.
    property bool loadMasterVolume: App.settingIsTrue("patch_load_master_volume")
    property bool loadOutLevel: App.settingIsTrue("patch_load_out_level")
    // Indexed by the engine id stored on a saved patch, so this list is not
    // the picker's: it has to name every engine that could have written a row
    // in the DB, including ones the connected synth does not have. Positional
    // and append-only for the same reason the firmware's enum is.
    readonly property list<string> engineNames: ["Subtractive", "Additive", "FM", "Wavetable",
                                        "Modular", "Granular"]

    function refresh() { patchList = Synth.patches(-1) }
    Component.onCompleted: refresh()

    Connections {
        target: UI
        // Answers this page's Import button only; the Presets page has one of
        // its own, and both pages are alive in the SwipeView at the same time.
        function onJsonImported(page: string, text: string): void {
            if (page !== "library") return
            if (Synth.importPatchesToLibrary(text).ok) screen.refresh()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: Tr.t("Local presets")
                font.bold: true
                font.pointSize: UI.fontSize * 1.1
                color: Material.foreground
                elide: Label.ElideRight
                Layout.fillWidth: true
            }
            Button {
                text: Tr.t("Import…")
                // Stores the file's patches here and leaves the live sound
                // alone — applying one is the Presets page's Import. Needs no
                // connection either: the library is the app's own database.
                onClicked: UI.importJsonRequested("library")
                ToolTip.visible: hovered
                ToolTip.text: Tr.t("Import patches from a JSON file into this library, without playing them")
            }
            Button {
                text: Tr.t("Export all…")
                // Exports from the stored rows, so it works disconnected; the
                // parameter names are filled in only when the synth is there to
                // supply them (see patchToJson).
                enabled: screen.patchList.length > 0
                onClicked: UI.exportJsonRequested(Synth.libraryToJson(), "osyntho-patches")
            }
            Button {
                text: Tr.t("Save current…")
                enabled: Synth.connected && Synth.ready
                onClicked: { nameDialog.mode = "save"; nameDialog.patchId = -1; nameDialog.field = ""; nameDialog.open() }
            }
        }

        Flow {
            Layout.fillWidth: true
            spacing: 16
            Label {
                // No vertical anchor: a Flow positions its children, and
                // anchoring inside a positioner is refused at runtime.
                text: Tr.t("Load with the patch:")
                color: Material.foreground
                opacity: 0.7
            }
            Switch {
                text: Tr.t("Synth volume")
                checked: screen.loadMasterVolume
                onToggled: {
                    screen.loadMasterVolume = checked
                    App.saveSetting("patch_load_master_volume", checked ? "true" : "false")
                }
                ToolTip.visible: hovered
                ToolTip.text: Tr.t("Also set the master volume a patch was saved with. Off by default: that is the level you are monitoring at, not part of the sound.")
            }
            Switch {
                id: outLevelSwitch
                // out.level exists only on firmware with a codec that has an
                // output driver register (an ES8388 build). Hidden rather than
                // disabled elsewhere — a switch for a control the connected
                // synth does not have is just a puzzle. Resolved by name, like
                // the toolbar's own out.level strip: paramIdForName returns -1
                // until discovery finds it, and is not a tracked read, so the
                // signal is what re-runs it.
                property int outLevelId: -1
                visible: Synth.ready && outLevelId >= 0
                text: Tr.t("Headphone level")
                checked: screen.loadOutLevel
                onToggled: {
                    screen.loadOutLevel = checked
                    App.saveSetting("patch_load_out_level", checked ? "true" : "false")
                }
                Component.onCompleted: outLevelId = Synth.paramIdForName("out.level")
                Connections {
                    target: Synth
                    function onParamsDiscovered() {
                        outLevelSwitch.outLevelId = Synth.paramIdForName("out.level")
                    }
                }
                ToolTip.visible: hovered
                ToolTip.text: Tr.t("Also set the analogue output level a patch was saved with. Off by default: it is set once by ear for what is plugged into the jack.")
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
                id: patchRow
                required property var modelData
                width: ListView.view.width
                // The stored patch the live sound came from, marked the way the
                // Presets page marks the synth's own slot. Nothing on the synth
                // records this — see SynthController::libraryPatchId — so it is
                // dropped as soon as anything else claims the sound.
                readonly property bool current: modelData.id === Synth.libraryPatchId
                highlighted: patchRow.current

                // Before contentItem for the same reason as the marker on the
                // Presets page tiles.
                Rectangle {
                    anchors.fill: parent
                    radius: 4
                    color: Qt.rgba(Material.accent.r, Material.accent.g,
                                   Material.accent.b, 0.16)
                    border.width: 2
                    border.color: Material.accent
                    visible: patchRow.current
                }

                contentItem: RowLayout {
                    spacing: 8
                    Column {
                        Layout.fillWidth: true
                        Label {
                            // Display only — Rename seeds from modelData.name
                            // and Export names the file from it, both raw.
                            text: modelData.name && modelData.name.length
                                  ? UI.capitalized(modelData.name) : Tr.t("(unnamed)")
                            color: patchRow.current ? Material.accent : Material.foreground
                            font.bold: true
                        }
                        Label {
                            text: Tr.t(screen.engineNames[modelData.engine] || "?") + " · " + modelData.created
                            color: Material.foreground
                            opacity: 0.6
                            font.pointSize: UI.fontSize * 0.75
                        }
                    }
                    Button {
                        text: Tr.t("Load")
                        // Same gate as "Save current…": pushing a patch needs
                        // the parameter table, which is what `ready` announces.
                        enabled: Synth.connected && Synth.ready
                        onClicked: Synth.loadPatch(modelData.id, screen.loadMasterVolume,
                                                   screen.loadOutLevel)
                    }
                    // Icon-only: a fourth worded button does not fit a phone row.
                    ToolButton {
                        text: ""  // file-export
                        font.family: App.fontAwesomeName
                        font.weight: Font.Black  // solid face
                        font.pointSize: UI.fontSize
                        onClicked: UI.exportJsonRequested(Synth.patchToJson(modelData.id),
                                                          modelData.name)
                        ToolTip.visible: hovered
                        ToolTip.text: Tr.t("Export this patch to a JSON file")
                    }
                    Button {
                        text: Tr.t("Rename")
                        onClicked: {
                            nameDialog.mode = "rename"
                            nameDialog.patchId = modelData.id
                            nameDialog.field = modelData.name
                            nameDialog.open()
                        }
                    }
                    Button {
                        text: Tr.t("Delete")
                        onClicked: { Synth.deletePatch(modelData.id); screen.refresh() }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: patchListView.count === 0
                text: Tr.t("No saved patches yet")
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
        title: mode === "save" ? Tr.t("Save patch") : Tr.t("Rename patch")
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
            placeholderText: Tr.t("Patch name")
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
