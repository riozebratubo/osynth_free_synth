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

    // ---- File pickers ----
    // Three routes, in the order the functions below test them: Android has no
    // picker of its own and goes through the SAF flows in App; Windows and Linux
    // raise the platform's real dialog through nfd (App.openFileDialog /
    // App.saveFileDialog, which block and answer "" when nothing was chosen);
    // everything else — macOS, and a Linux box without zenity — keeps the Qt
    // FileDialogs declared further down. Sampled once: nothing about it can
    // change while the app runs.
    readonly property bool nativePickers: App.hasNativeFileDialogs()
    readonly property url documentsFolder: StandardPaths.standardLocations(StandardPaths.DocumentsLocation)[0]
    readonly property url downloadsFolder: StandardPaths.standardLocations(StandardPaths.DownloadLocation)[0]

    // ---- Backup / restore ----
    function shareBackup() {
        if (App.isAndroid()) {
            App.shareFile(App.getDatabaseFileLocation())
        } else if (nativePickers) {
            const path = App.saveFileDialog("db", downloadsFolder, "db")
            if (path.length > 0) writeBackupTo(path)
        } else {
            backupSaveDialog.open()
        }
    }

    function writeBackupTo(path) {
        if (App.saveBackupTo(path))
            toast.show(t.t("Backup saved."), 3000)
        else
            toast.show(t.t("Could not write the backup file."), 4000)
    }

    // Where an Android restore is staged; the picker copies the chosen file
    // here. Deliberately NOT the database's own path: the picker would then be
    // writing straight over the live database, which meant the file had to be
    // taken on trust — anything the user picked became the database, valid or
    // not. Staged first, restoreBackupFrom() checks it, and the app is left
    // alone if it isn't a backup.
    readonly property string backupImportPath: App.exportFileLocation("restore.db")

    function restoreBackup() {
        if (App.isAndroid()) {
            pendingRestore = true
            App.selectFile(backupImportPath, "db")
        } else if (nativePickers) {
            const path = App.openFileDialog("db", downloadsFolder)
            if (path.length > 0) App.restoreBackupFrom(path)
        } else {
            backupRestoreDialog.open()
        }
    }

    // ---- Patch interchange (JSON): pickers and per-platform delivery ----
    // Held between opening the save picker and the user choosing a path.
    property string pendingExportText: ""
    // True while a native "import patch" file pick is in flight (Android).
    property bool pendingJsonImport: false
    // Which page asked for the import now in flight ("preset" / "library"), so
    // the file goes back to that one alone. Set by importJson() on every route
    // and read by loadImportFrom(); only one pick can be open at a time.
    property string jsonImportTarget: ""
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
        } else if (nativePickers) {
            // The dialog blocks, so the text needs no holding onto — the answer
            // is already in hand by the time it returns.
            const path = App.saveFileDialog("json", documentsFolder + "/" + fileName, "json")
            if (path.length > 0) writeExportTo(path, text)
        } else {
            pendingExportText = text
            jsonSaveDialog.selectedFile = jsonSaveDialog.currentFolder + "/" + fileName
            jsonSaveDialog.open()
        }
    }

    function writeExportTo(path, text) {
        if (App.writeTextFile(path, text))
            toast.show(t.t("Exported."), 3000)
        else
            toast.show(t.t("Could not write the file."), 4000)
    }

    function importJson(page) {
        jsonImportTarget = page
        if (App.isAndroid()) {
            pendingJsonImport = true
            App.selectFile(jsonImportPath, "json")
        } else if (nativePickers) {
            const path = App.openFileDialog("json", documentsFolder)
            if (path.length > 0) loadImportFrom(path)
        } else {
            jsonOpenDialog.open()
        }
    }

    // readTextFile() answers "" for both an unreadable file and an empty one, so
    // the message names both rather than asserting the wrong one.
    function loadImportFrom(path) {
        const text = App.readTextFile(path)
        if (text.length > 0) UI.jsonImported(jsonImportTarget, text)
        else toast.show(t.t("That file is empty, or could not be read."), 4000)
    }

    // Fallback pickers, used where nativePickers is false. Left non-native
    // (DontUseNativeDialog) on purpose: the platforms that have a native dialog
    // worth showing now reach it through nfd instead of through Qt.
    FileDialog {
        id: jsonSaveDialog
        fileMode: FileDialog.SaveFile
        options: FileDialog.DontUseNativeDialog
        defaultSuffix: "json"
        currentFolder: mainWindow.documentsFolder
        nameFilters: [t.t("Patch files (*.json)"), t.t("All files (*)")]
        onAccepted: {
            writeExportTo(selectedFile, mainWindow.pendingExportText)
            mainWindow.pendingExportText = ""
        }
        onRejected: mainWindow.pendingExportText = ""
    }

    FileDialog {
        id: jsonOpenDialog
        fileMode: FileDialog.OpenFile
        options: FileDialog.DontUseNativeDialog
        currentFolder: mainWindow.documentsFolder
        nameFilters: [t.t("Patch files (*.json)"), t.t("All files (*)")]
        onAccepted: loadImportFrom(selectedFile)
    }

    FileDialog {
        id: backupSaveDialog
        fileMode: FileDialog.SaveFile
        options: FileDialog.DontUseNativeDialog
        // As on jsonSaveDialog, and for the same reason the native save route
        // passes a suffix to App.saveFileDialog: a name typed without ".db"
        // lands on disk extension-less, where the restore picker's own "*.db"
        // filter then hides it.
        defaultSuffix: "db"
        currentFolder: mainWindow.downloadsFolder
        nameFilters: [t.t("Database (*.db)"), t.t("All files (*)")]
        onAccepted: writeBackupTo(selectedFile)
    }

    FileDialog {
        id: backupRestoreDialog
        fileMode: FileDialog.OpenFile
        options: FileDialog.DontUseNativeDialog
        currentFolder: mainWindow.downloadsFolder
        nameFilters: [t.t("Database (*.db)"), t.t("All files (*)")]
        onAccepted: App.restoreBackupFrom(selectedFile)
    }

    Connections {
        target: UI
        function onShareBackupRequested() { shareBackup() }
        function onRestoreBackupRequested() { restoreBackup() }
        function onExportJsonRequested(text, suggestedName) { exportJson(text, suggestedName) }
        function onImportJsonRequested(page) { importJson(page) }
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
                loadImportFrom(mainWindow.jsonImportPath)
                return
            }
            if (pendingRestore) {
                pendingRestore = false
                // Reports its own failure through onRestoreFailed below, and
                // leaves the database untouched unless the staged file checks
                // out — so there is nothing to roll back here any more.
                App.restoreBackupFrom(mainWindow.backupImportPath)
            }
        }
        function onSelectFileCanceled() {
            if (pendingJsonImport) {
                pendingJsonImport = false
                return
            }
            if (pendingRestore) pendingRestore = false
        }
        function onDatabaseRestored() {
            App.onDatabaseRestoredAction()
            reApplySettings()
            toast.show(t.t("Backup restored."), 3000)
        }
        // A restore that did not happen. Without this the success toast above
        // was shown either way, so a rejected file — or one that failed to copy
        // and was rolled back — looked exactly like a restore that worked.
        function onRestoreFailed(reason) { toast.show(reason, 5000, "#B00020", "white") }
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
                GraphScreen {}
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

            // Computer-keyboard capture, owned here because it feeds BOTH
            // performance surfaces and this is the only place that can see the
            // two. It used to be bound inside Keyboard.qml to that component's
            // own `visible`, so turning the keyboard off from the toolbar menu
            // also stopped Q…I / 1…8 firing drum pads that were still on
            // screen. Each surface still gates what it does with the keys:
            // Keyboard's note handlers are enabled only while it is visible,
            // and DrumPads answers whether or not it is (playing pads from the
            // keys while they are hidden is the point of having them there).
            //
            // While only the pads are up the piano rows are consumed without
            // sounding anything. That costs nothing: the app binds no other
            // keys, and App's filter already steps aside for focused text
            // fields and for anything held with Ctrl/Alt/Meta.
            Component.onCompleted: App.keyboardCaptureEnabled = Qt.binding(function() {
                return App.isDesktop() && keyboard.computerKeys
                       && (keyboard.visible || drumPads.visible)
            })
            Component.onDestruction: App.keyboardCaptureEnabled = false
        }
    }

    onClosing: (close) => {
        if ((Qt.platform.os === "android" || Qt.platform.os === "ios") && mainStackView.depth > 1) {
            close.accepted = false
            mainStackView.pop()
        }
    }
}
