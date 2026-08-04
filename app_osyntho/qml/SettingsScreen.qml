import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

Pane {
    id: settingsScreen

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.bottomMargin: 4

            RoundButton {
                text: "\uf104"  // angle-left
                font.family: App.fontAwesomeName
                font.weight: Font.Black  // solid face
                font.pointSize: UI.fontSize * 1.2
                highlighted: true
                onClicked: mainStackView.pop()
            }

            Column {
                Layout.leftMargin: 8
                Layout.alignment: Qt.AlignVCenter
                Text {
                    text: t.t("Osyntho — Settings")
                    font.bold: true
                    color: Material.foreground
                }
                Text {
                    text: t.t("Version:") + " " + App.getVersionFull()
                    color: Material.foreground
                    opacity: 0.7
                }
            }
        }

        TabBar {
            id: settingsTabBar
            Layout.fillWidth: true
            TabButton { text: t.t("General") }
            TabButton { text: t.t("Keyboard") }
            TabButton { text: t.t("Bluetooth") }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: settingsTabBar.currentIndex

            // ── General ──────────────────────────────────────────────────────
            Item {
                Flickable {
                    anchors.fill: parent
                    contentWidth: parent.width
                    contentHeight: generalCol.height
                    clip: true

                    Column {
                        id: generalCol
                        width: parent.width
                        spacing: 10
                        padding: 10

                        Text { text: t.t("App theme"); font.bold: true; color: Material.foreground }

                        SettingsSwitch {
                            text: t.t("Use dark theme")
                            checked: App.theme.type === "dark"
                            onToggled: App.setThemeType(checked ? "dark" : "light")
                        }

                        Row {
                            spacing: 8
                            Repeater {
                                model: 8
                                delegate: AbstractButton {
                                    id: swatchButton
                                    required property int index
                                    readonly property var swatchColors: [
                                        "#FF607D8B", "#FF673AB7", "#FF009688", "#FFFF5722",
                                        "#FFE91E63", "#FFFF8F00", "#FF795548", "#FF388E3C"
                                    ]
                                    readonly property bool isSelected: App.themePresetIndex === index
                                    width: 36
                                    height: 36
                                    onClicked: App.applyThemePreset(index)
                                    contentItem: Rectangle {
                                        anchors.fill: parent
                                        radius: width / 2
                                        color: swatchButton.swatchColors[swatchButton.index]
                                        border.color: swatchButton.isSelected ? "white" : "transparent"
                                        border.width: 2
                                    }
                                    background: Item {}
                                }
                            }
                        }

                        Text { text: t.t("App font size"); font.bold: true; color: Material.foreground }
                        Row {
                            spacing: 12
                            SpinBox {
                                id: fontSizeSpinBox
                                from: 6
                                to: 22
                                value: App.setting("app_font_size")
                                onValueModified: {
                                    App.saveSetting("app_font_size", value)
                                    UI.fontSize = value
                                }
                            }
                            Text {
                                anchors.verticalCenter: fontSizeSpinBox.verticalCenter
                                text: t.t("Sample text")
                                font.pointSize: fontSizeSpinBox.value
                                color: Material.foreground
                            }
                        }

                        Text { text: t.t("Panel layout"); font.bold: true; color: Material.foreground }
                        ComboBox {
                            width: Math.min(parent.width - 20, 300)
                            model: [
                                { text: t.t("Tiled"), value: "tiled" },
                                { text: t.t("One per line"), value: "rows" }
                            ]
                            textRole: "text"
                            valueRole: "value"
                            Component.onCompleted: currentIndex = Math.max(0, indexOfValue(App.setting("panel_layout")))
                            onActivated: {
                                App.saveSetting("panel_layout", currentValue)
                                UI.tiledPanels = (currentValue !== "rows")
                            }
                        }
                        Text {
                            width: parent.width - 20
                            wrapMode: Text.WordWrap
                            text: t.t("How the parameter panels (oscillator, filter, FX…) are arranged: packed left to right at the width each one needs, or each one alone on a full-width line.")
                            color: Material.foreground
                            opacity: 0.6
                            font.pointSize: Math.max(8, UI.fontSize * 0.7)
                        }

                        Text { text: t.t("Startup screen"); font.bold: true; color: Material.foreground }
                        ComboBox {
                            width: Math.min(parent.width - 20, 300)
                            // "last" first, then one entry per page in SwipeView
                            // order — the value is the page index as a string.
                            model: {
                                var entries = [{ text: t.t("Last used"), value: "last" }]
                                for (var i = 0; i < UI.screens.length; i++)
                                    entries.push({ text: t.t(UI.screens[i].name), value: String(i) })
                                return entries
                            }
                            textRole: "text"
                            valueRole: "value"
                            Component.onCompleted: currentIndex = Math.max(0, indexOfValue(App.setting("startup_screen")))
                            onActivated: App.saveSetting("startup_screen", currentValue)
                        }
                        Text {
                            width: parent.width - 20
                            wrapMode: Text.WordWrap
                            text: t.t("Which page the app opens on. \"Last used\" reopens whichever page you were on when you closed it.")
                            color: Material.foreground
                            opacity: 0.6
                            font.pointSize: Math.max(8, UI.fontSize * 0.7)
                        }

                        Text { text: t.t("App language (restart to apply)"); font.bold: true; color: Material.foreground }
                        ComboBox {
                            width: Math.min(parent.width - 20, 300)
                            model: [
                                { text: t.t("English"), value: "en" },
                                { text: t.t("Portuguese"), value: "pt_BR" }
                            ]
                            textRole: "text"
                            valueRole: "value"
                            Component.onCompleted: currentIndex = Math.max(0, indexOfValue(App.setting("force_app_language")))
                            onActivated: {
                                App.saveSetting("force_app_language", currentValue)
                                t.setActiveLanguage(currentValue)
                            }
                        }

                        // Android-only: immersive fullscreen. Also tends to stop
                        // MIUI/HyperOS intercepting multi-finger system gestures
                        // over the app (see the keyboard chord note).
                        Text {
                            visible: App.isAndroid()
                            text: t.t("Display")
                            font.bold: true
                            color: Material.foreground
                        }
                        SettingsSwitch {
                            visible: App.isAndroid()
                            text: t.t("Fullscreen (immersive)")
                            checked: App.settingIsTrue("android_immersive")
                            onToggled: {
                                // Saves the setting and moves the window; a
                                // live toggle has to do both. See setImmersive()
                                // in Main.qml.
                                UI.window.setImmersive(checked)
                            }
                        }
                        Text {
                            visible: App.isAndroid()
                            width: parent.width - 20
                            wrapMode: Text.WordWrap
                            text: t.t("Hides the status and navigation bars. Recommended: it also stops some phones (e.g. Xiaomi) from stealing multi-finger touches for system gestures.")
                            color: Material.foreground
                            opacity: 0.6
                            font.pointSize: Math.max(8, UI.fontSize * 0.7)
                        }
                    }
                    ScrollBar.vertical: ScrollBar {}
                }
            }

            // ── Keyboard ─────────────────────────────────────────────────────
            Item {
                Flickable {
                    anchors.fill: parent
                    contentWidth: parent.width
                    contentHeight: keyboardCol.height
                    clip: true

                    Column {
                        id: keyboardCol
                        width: parent.width
                        spacing: 10
                        padding: 10

                        Text { text: t.t("On-screen keyboard"); font.bold: true; color: Material.foreground }

                        Text { text: t.t("Base octave"); color: Material.foreground }
                        SpinBox {
                            from: 0
                            // The keyboard draws two octaves from this, so 7 is
                            // the highest that stays inside MIDI 0..127; above
                            // it the upper keys are silently dropped.
                            to: 7
                            value: App.setting("keyboard_octave")
                            onValueModified: App.saveSetting("keyboard_octave", value)
                        }

                        Text { text: t.t("Velocity"); color: Material.foreground }
                        Row {
                            spacing: 12
                            width: parent.width - 20
                            Slider {
                                id: velSlider
                                width: Math.min(parent.width - 60, 260)
                                from: 1
                                to: 127
                                stepSize: 1
                                value: App.setting("keyboard_velocity")
                                onMoved: App.saveSetting("keyboard_velocity", Math.round(value))
                            }
                            Text {
                                anchors.verticalCenter: velSlider.verticalCenter
                                text: Math.round(velSlider.value)
                                color: Material.foreground
                            }
                        }

                        SettingsSwitch {
                            text: t.t("Latch notes (hold)")
                            checked: App.settingIsTrue("keyboard_hold")
                            onToggled: App.saveSetting("keyboard_hold", checked ? "true" : "false")
                        }

                        SettingsSwitch {
                            text: t.t("Show note names on keys")
                            checked: App.settingIsTrue("keyboard_show_note_names")
                            onToggled: App.saveSetting("keyboard_show_note_names", checked ? "true" : "false")
                        }

                        // Both also live in the on-screen keyboard's control
                        // strip, next to the octave buttons, since they are
                        // rewired mid-take.
                        SettingsSwitch {
                            id: computerKeysSwitch
                            text: t.t("Computer keys play the synth")
                            checked: App.settingIsTrue("keyboard_computer_keys")
                            onToggled: App.saveSetting("keyboard_computer_keys", checked ? "true" : "false")
                        }

                        SettingsSwitch {
                            // Nothing reaches the drum mapping with the keys
                            // released to the app; follows the switch above
                            // rather than the setting, which is not reactive.
                            text: t.t("Top rows play drum pads")
                            enabled: computerKeysSwitch.checked
                            checked: App.settingIsTrue("keyboard_top_row_drums")
                            onToggled: App.saveSetting("keyboard_top_row_drums", checked ? "true" : "false")
                        }
                        Text {
                            width: parent.width - 20
                            wrapMode: Text.WordWrap
                            font.pointSize: UI.fontSize * 0.8
                            opacity: 0.65
                            color: Material.foreground
                            text: t.t("Computer keyboard: Q…I fire the lower eight pads and "
                                      + "1…8 the upper eight. Off, those keys play a second "
                                      + "octave instead. The Z…M row plays notes either way. "
                                      + "Both switches are also in the keyboard's own toolbar.")
                        }

                        Text { text: t.t("Resize control"); font.bold: true; color: Material.foreground }
                        ComboBox {
                            id: resizeModeCombo
                            width: Math.min(parent.width - 20, 300)
                            model: [
                                { text: t.t("Divider (drag handle)"), value: "divider" },
                                { text: t.t("Slider"), value: "slider" }
                            ]
                            textRole: "text"
                            valueRole: "value"
                            Component.onCompleted: currentIndex = Math.max(0, indexOfValue(App.setting("keyboard_resize_mode")))
                            onActivated: App.saveSetting("keyboard_resize_mode", currentValue)
                        }
                        Text {
                            width: parent.width - 20
                            wrapMode: Text.WordWrap
                            text: t.t("How to resize the on-screen keyboard: drag its top edge, or use a slider in its toolbar.")
                            color: Material.foreground
                            opacity: 0.6
                            font.pointSize: Math.max(8, UI.fontSize * 0.7)
                        }

                        Text {
                            text: t.t("Divider thickness (px)")
                            visible: resizeModeCombo.currentValue === "divider"
                            color: Material.foreground
                        }
                        SpinBox {
                            visible: resizeModeCombo.currentValue === "divider"
                            from: 2
                            to: 24
                            value: App.setting("keyboard_divider_thickness")
                            onValueModified: App.saveSetting("keyboard_divider_thickness", value)
                        }
                    }
                    ScrollBar.vertical: ScrollBar {}
                }
            }

            // ── Bluetooth ────────────────────────────────────────────────────
            Item {
                Flickable {
                    anchors.fill: parent
                    contentWidth: parent.width
                    contentHeight: bluetoothCol.height
                    clip: true

                    Column {
                        id: bluetoothCol
                        width: parent.width
                        spacing: 10
                        padding: 10

                        Text {
                            width: parent.width - 20
                            wrapMode: Text.WordWrap
                            text: t.t("Bluetooth changes take effect on the next scan; a restart is safest.")
                            color: Material.foreground
                            opacity: 0.7
                        }

                        Text { text: t.t("Bluetooth"); font.bold: true; color: Material.foreground }
                        SettingsSwitch {
                            text: t.t("Enable Bluetooth")
                            checked: App.bluetoothEnabled
                            onToggled: App.bluetoothEnabled = checked
                        }

                        Text { text: t.t("Specific device"); font.bold: true; color: Material.foreground }
                        SettingsSwitch {
                            id: useSelectedSwitch
                            text: t.t("Lock to a specific device")
                            Component.onCompleted: checked = App.settingIsTrue("bluetooth_use_selected")
                            onToggled: {
                                App.saveSetting("bluetooth_use_selected", checked ? "true" : "false")
                                if (!checked) App.setBluetoothSelectedDevice("", "")
                            }
                            // The device selector turns this setting on when a
                            // device is picked, and App.setting() is a plain
                            // invokable that no binding tracks — so without
                            // this, coming back from that screen left the
                            // switch reading "off" over a setting that was on.
                            Connections {
                                target: App
                                function onSettingChanged(name) {
                                    if (name === "bluetooth_use_selected")
                                        useSelectedSwitch.checked =
                                            App.settingIsTrue("bluetooth_use_selected")
                                }
                            }
                        }
                        Text {
                            width: parent.width - 20
                            wrapMode: Text.WordWrap
                            color: Material.foreground
                            text: App.bluetoothSelectedDeviceName !== ""
                                ? (t.t("Saved device") + ": " + App.bluetoothSelectedDeviceName + " (" + App.bluetoothSelectedDeviceAddress + ")")
                                : t.t("No device saved")
                        }
                        Row {
                            spacing: 8
                            Button {
                                visible: useSelectedSwitch.checked
                                text: t.t("Select device...")
                                // The selector scans; with Bluetooth off there
                                // is nothing for it to find, and letting it
                                // start one would restart the radio behind the
                                // switch above.
                                enabled: App.bluetoothEnabled
                                onClicked: mainStackView.push("BluetoothDeviceSelectorScreen.qml", {})
                            }
                            Button {
                                visible: App.bluetoothSelectedDeviceAddress !== ""
                                text: t.t("Clear")
                                onClicked: App.setBluetoothSelectedDevice("", "")
                            }
                        }

                        Text { text: t.t("Scan time (s)"); font.bold: true; color: Material.foreground }
                        SpinBox {
                            from: 4
                            to: 120
                            value: App.setting("bluetooth_scan_time")
                            onValueModified: App.saveSetting("bluetooth_scan_time", value)
                        }

                        Text { text: t.t("Device name prefix"); font.bold: true; color: Material.foreground }
                        TextField {
                            width: Math.min(parent.width - 20, 300)
                            text: App.setting("bluetooth_prefix")
                            onTextEdited: App.saveSetting("bluetooth_prefix", text)
                        }
                    }
                    ScrollBar.vertical: ScrollBar {}
                }
            }
        }
    }
}
