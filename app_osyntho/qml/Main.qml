import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtCore

import org.osynth.osyntho

ApplicationWindow {
    id: mainWindow
    objectName: "mainWindow"

    width: App.isDesktop() ? 1024 : 360
    height: 720
    visible: true
    title: "Osyntho"

    Material.theme: App.theme.type === "dark" ? Material.Dark : Material.Light
    Material.accent: App.theme.materialAccent
    Material.roundedScale: Material.NotRounded

    // True while a native "restore backup" file pick is in flight (Android).
    property bool pendingRestore: false
    // Bottom performance strip: on-screen keyboard and 4x4 drum pads, each
    // toggled from the toolbar menu. Both default on.
    property bool keyboardVisible: true
    property bool drumPadsVisible: true

    WindowStateSaver {
        window: mainWindow
        windowName: "mainWindow"
        isEnabled: App.isDesktop()
    }

    function applyImmersive() {
        if (App.isAndroid())
            App.setAndroidImmersiveMode(App.settingIsTrue("android_immersive"))
    }

    Component.onCompleted: {
        UI.window = mainWindow
        applyImmersive()
    }

    // Re-apply on return to the app (a transient system-bar show or a dialog can
    // clear the flags). SettingsScreen also calls applyImmersive() on toggle.
    Connections {
        target: Qt.application
        function onStateChanged() {
            if (Qt.application.state === Qt.ApplicationActive) applyImmersive()
        }
    }

    // The Android soft keyboard forces the system bars visible while it is up,
    // which clears the sticky-immersive flags. Android doesn't restore them when
    // the keyboard hides, so the status bar stays and the whole scene (toolbar
    // included) is left inset down by its height. Re-assert immersive once — and
    // only once the keyboard has finished animating out: re-applying while it is
    // still closing (or twice in a row) makes Qt relayout mid-animation, which
    // shows up as the toolbar jumping around instead of settling.
    Connections {
        target: Qt.inputMethod
        function onVisibleChanged() {
            if (!Qt.inputMethod.visible) immersiveReapplyTimer.restart()
        }
    }

    Timer {
        id: immersiveReapplyTimer
        interval: 350  // ~one IME close animation; single shot
        onTriggered: applyImmersive()
    }

    ToastManager { id: toast }

    // Which page the SwipeView opens on. The startup_screen setting holds either
    // a page index or "last", in which case the page left on the previous run is
    // used. Anything unparseable or out of range falls back to Home, so a stale
    // stored index can never open the app on nothing.
    function startupIndex() {
        const want = App.setting("startup_screen")
        const idx = parseInt(want === "last" ? App.setting("last_swipeview_index") : want)
        return (isNaN(idx) || idx < 0 || idx >= UI.screens.length) ? 0 : idx
    }

    function reApplySettings() {
        App.setThemeType(App.setting("theme_type"))
        App.applyThemePreset(parseInt(App.setting("theme_preset")))
        UI.fontSize = App.setting("app_font_size")
    }

    // ---- Backup / restore (Android SAF via App, Qt dialog elsewhere) ----
    function shareBackup() {
        if (App.isAndroid())
            App.shareFile(App.getDatabaseFileLocation())
        else
            backupSaveDialog.open()
    }

    function restoreBackup() {
        if (App.isAndroid()) {
            App.forceCloseDatabase()
            pendingRestore = true
            App.selectFile(App.getDatabaseFileLocation(), "db")
        } else {
            backupRestoreDialog.open()
        }
    }

    // ---- Patch interchange (JSON): pickers and per-platform delivery ----
    // Held between opening the save picker and the user choosing a path.
    property string pendingExportText: ""
    // True while a native "import patch" file pick is in flight (Android).
    property bool pendingJsonImport: false
    // Where an Android import is staged; the picker copies the chosen file here.
    readonly property string jsonImportPath: App.exportFileLocation("import.json")

    // Patch names are free text and file names are not.
    function safeFileName(name, fallback) {
        var base = String(name || "").replace(/[^A-Za-z0-9 ._-]/g, "_").trim()
        if (base.length === 0) base = fallback
        return base + ".json"
    }

    function exportJson(text, suggestedName) {
        if (!text || text.length === 0) {
            toast.show(t.t("Nothing to export."), 3000)
            return
        }
        const fileName = safeFileName(suggestedName, "osyntho-patch")
        if (App.isAndroid()) {
            // No save picker on Android: stage the file in app storage and hand
            // it to the share sheet, the same route the database backup takes.
            const path = App.exportFileLocation(fileName)
            if (App.writeTextFile(path, text)) App.shareFile(path)
            else toast.show(t.t("Could not write the file."), 4000)
        } else {
            pendingExportText = text
            jsonSaveDialog.selectedFile = jsonSaveDialog.currentFolder + "/" + fileName
            jsonSaveDialog.open()
        }
    }

    function importJson() {
        if (App.isAndroid()) {
            pendingJsonImport = true
            App.selectFile(jsonImportPath, "json")
        } else {
            jsonOpenDialog.open()
        }
    }

    FileDialog {
        id: jsonSaveDialog
        fileMode: FileDialog.SaveFile
        options: FileDialog.DontUseNativeDialog
        defaultSuffix: "json"
        currentFolder: StandardPaths.standardLocations(StandardPaths.DocumentsLocation)[0]
        nameFilters: [t.t("Patch files (*.json)"), t.t("All files (*)")]
        onAccepted: {
            if (App.writeTextFile(selectedFile, mainWindow.pendingExportText))
                toast.show(t.t("Exported."), 3000)
            else
                toast.show(t.t("Could not write the file."), 4000)
            mainWindow.pendingExportText = ""
        }
        onRejected: mainWindow.pendingExportText = ""
    }

    FileDialog {
        id: jsonOpenDialog
        fileMode: FileDialog.OpenFile
        options: FileDialog.DontUseNativeDialog
        currentFolder: StandardPaths.standardLocations(StandardPaths.DocumentsLocation)[0]
        nameFilters: [t.t("Patch files (*.json)"), t.t("All files (*)")]
        onAccepted: {
            const text = App.readTextFile(selectedFile)
            if (text.length > 0) UI.jsonImported(text)
            else toast.show(t.t("Could not read the file."), 4000)
        }
    }

    FileDialog {
        id: backupSaveDialog
        fileMode: FileDialog.SaveFile
        options: FileDialog.DontUseNativeDialog
        currentFolder: StandardPaths.standardLocations(StandardPaths.DownloadLocation)[0]
        nameFilters: [t.t("Database (*.db)"), t.t("All files (*)")]
        onAccepted: App.saveBackupTo(selectedFile)
    }

    FileDialog {
        id: backupRestoreDialog
        fileMode: FileDialog.OpenFile
        options: FileDialog.DontUseNativeDialog
        currentFolder: StandardPaths.standardLocations(StandardPaths.DownloadLocation)[0]
        nameFilters: [t.t("Database (*.db)"), t.t("All files (*)")]
        onAccepted: App.restoreBackupFrom(selectedFile)
    }

    Connections {
        target: UI
        function onShareBackupRequested() { shareBackup() }
        function onRestoreBackupRequested() { restoreBackup() }
        function onExportJsonRequested(text, suggestedName) { exportJson(text, suggestedName) }
        function onImportJsonRequested() { importJson() }
        function onSettingsRequested() { mainStackView.push("SettingsScreen.qml", {}) }
        function onSelectDeviceRequested() { mainStackView.push("BluetoothDeviceSelectorScreen.qml", {}) }
        function onUpdateFirmwareRequested(extension) {
            toast.show(t.t("Firmware update is not available yet for osynth."), 4000)
        }
    }

    Connections {
        target: App
        function onSelectFileSelected(filename) {
            // The signal carries a display name, not a path — the picker copied
            // the file to the destination we asked for, so read that.
            if (pendingJsonImport) {
                pendingJsonImport = false
                const text = App.readTextFile(mainWindow.jsonImportPath)
                if (text.length > 0) UI.jsonImported(text)
                else toast.show(t.t("Could not read the file."), 4000)
                return
            }
            if (pendingRestore) {
                pendingRestore = false
                App.forceOpenDatabase()
                App.emitDatabaseRestored()
            }
        }
        function onSelectFileCanceled() {
            if (pendingJsonImport) {
                pendingJsonImport = false
                return
            }
            if (pendingRestore) {
                pendingRestore = false
                App.forceOpenDatabase()
            }
        }
        function onDatabaseRestored() {
            App.onDatabaseRestoredAction()
            reApplySettings()
            toast.show(t.t("Backup restored."), 3000)
        }
    }

    Connections {
        target: Synth
        function onShowError(msg) { toast.show(msg, 5000, "#B00020", "white") }
        function onShowInfo(msg) { toast.show(msg, 3000, "#2E7D32", "white") }
    }

    StackView {
        id: mainStackView
        anchors.fill: parent

        initialItem: Page {
            header: Toolbar {
                subtitle: Synth.connected
                    ? (BluetoothManager.deviceName + (Synth.ready ? (" · " + t.t(Synth.engineName)) : (" · " + t.t("connecting…"))))
                    : t.t("Not connected")
            }

            // Navigator dock: jump straight to any screen. Centered under the
            // toolbar; the labels match the SwipeView page order below.
            Rectangle {
                id: navDock
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: navRow.implicitHeight + 6
                color: Material.theme === Material.Dark ? "#14FFFFFF" : "#0A000000"
                z: 2

                Row {
                    id: navRow
                    anchors.centerIn: parent
                    spacing: 1

                    Repeater {
                        // UI.screens is in SwipeView order, so the list index is
                        // the page index (see the pages declared below).
                        model: UI.screens
                        delegate: ToolButton {
                            id: navBtn
                            required property var modelData
                            required property int index
                            readonly property bool current: swipeView.currentIndex === index
                            highlighted: current
                            // Compact padding: the stacked icon+label content
                            // keeps the dock close to its old single-line height.
                            padding: 4
                            onClicked: swipeView.currentIndex = navBtn.index
                            contentItem: Column {
                                spacing: 1
                                Label {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: navBtn.modelData.icon
                                    font.family: App.fontAwesomeName
                                    font.weight: Font.Black  // solid face
                                    font.pointSize: UI.fontSize * 0.95
                                    color: navBtn.current ? Material.accent : Material.foreground
                                    opacity: navBtn.current ? 1.0 : 0.75
                                }
                                Label {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: t.t(navBtn.modelData.label)
                                    font.pointSize: UI.fontSize * 0.62
                                    font.bold: navBtn.current
                                    color: navBtn.current ? Material.accent : Material.foreground
                                    opacity: navBtn.current ? 1.0 : 0.75
                                }
                            }
                        }
                    }
                }
            }

            SwipeView {
                id: swipeView
                anchors.top: navDock.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Math.max(keyboard.visible ? keyboard.height : 0,
                                               drumPads.visible ? drumPads.height : 0)

                // Set once the pages exist, so an out-of-range stored index can
                // be clamped against the real page count.
                Component.onCompleted: currentIndex = mainWindow.startupIndex()
                // Remembered for the "Last used" startup option. Written on every
                // page change, which is what makes it survive a crash or a kill
                // as well as a clean exit.
                onCurrentIndexChanged: App.saveSetting("last_swipeview_index", currentIndex)

                HomeScreen {}
                ToneScreen {}
                FilterEnvScreen {}
                ModScreen {}
                FxScreen {}
                SequencerScreen {}
                DrumsScreen {}
                ArpSeqScreen {}
                LooperScreen {}
                PresetsScreen {}
                PatchLibraryScreen {}
            }

            // The bottom performance strip: drum pads on the left, keyboard
            // filling whatever is left. The pads take their height from the
            // keyboard so the two sit flush; with the keyboard hidden they
            // keep a playable height of their own.
            DrumPads {
                id: drumPads
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                height: keyboard.visible ? keyboard.height
                                         : Math.min(220, mainWindow.height * 0.3)
                // Square-ish, but never more than a third of a phone screen.
                width: Math.min(height, parent.width * 0.34)
                visible: mainWindow.drumPadsVisible && Synth.connected
            }

            Keyboard {
                id: keyboard
                anchors.left: drumPads.visible ? drumPads.right : parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                visible: mainWindow.keyboardVisible && Synth.connected
            }
        }
    }

    onClosing: (close) => {
        if ((Qt.platform.os === "android" || Qt.platform.os === "ios") && mainStackView.depth > 1) {
            close.accepted = false
            mainStackView.pop()
        }
    }
}
