import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

import org.osynth.osyntho

Pane {
    id: bluetoothDeviceSelectorScreen

    property StackView openedInStack: null
    property bool shouldDestroyOnPop: false

    // Ask the BLE manager to scan-to-list (rather than auto-connect) while this
    // screen is open, and leave that mode when it closes.
    Component.onCompleted: BluetoothManager.startDeviceScan()

    StackView.onRemoved: {
        BluetoothManager.stopDeviceScan()
        if (shouldDestroyOnPop) destroy()
    }

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
                    text: t.t("Select Bluetooth Device")
                    font.bold: true
                    color: Material.foreground
                }
            }
        }

        Item { Layout.fillWidth: true; height: 10 }

        Item {
            Layout.fillWidth: true
            height: savedDeviceCol.height
            Column {
                id: savedDeviceCol
                width: parent.width - 20
                x: 10
                spacing: 4

                Text { font.bold: true; color: Material.foreground; text: t.t("Saved device") }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: Material.foreground
                    text: App.bluetoothSelectedDeviceName !== ""
                        ? (App.bluetoothSelectedDeviceName + " (" + App.bluetoothSelectedDeviceAddress + ")")
                        : t.t("None")
                }
                Button {
                    visible: App.bluetoothSelectedDeviceAddress !== ""
                    text: t.t("Clear selection")
                    onClicked: App.setBluetoothSelectedDevice("", "")
                }
            }
        }

        Item { Layout.fillWidth: true; height: 10 }

        Row {
            id: scanStatusRow
            Layout.leftMargin: 10
            spacing: 8

            BusyIndicator {
                visible: BluetoothManager.scanning
                running: BluetoothManager.scanning
                height: 28
                width: 28
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: Material.foreground
                text: BluetoothManager.scanning
                    ? t.t("Scanning for devices...")
                    : (App.bluetoothEnabled
                        ? t.t("Not scanning. Scan will start automatically.")
                        : t.t("Enable Bluetooth to discover devices."))
            }
        }

        Item { Layout.fillWidth: true; height: 6 }

        Text {
            Layout.leftMargin: 10
            font.bold: true
            color: Material.foreground
            text: t.t("Discovered devices")
        }

        Item { Layout.fillWidth: true; height: 4 }

        ListView {
            id: deviceList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: BluetoothManager.discoveredDevices

            ScrollBar.vertical: ScrollBar {}

            delegate: ItemDelegate {
                required property var modelData
                width: deviceList.width

                contentItem: RowLayout {
                    spacing: 8

                    Column {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            text: modelData.name !== "" ? modelData.name : t.t("(unknown)")
                            font.bold: true
                            color: Material.foreground
                        }
                        Text {
                            text: modelData.address
                            font.pixelSize: Math.max(10, UI.fontSize - 2)
                            color: Material.theme === Material.Dark ? "#FFAAAAAA" : "#FF666666"
                        }
                    }

                    Button {
                        text: t.t("Select")
                        onClicked: {
                            App.setBluetoothSelectedDevice(modelData.name, modelData.address)
                            App.saveSetting("bluetooth_use_selected", "true")
                            BluetoothManager.connectToSelectedDevice()
                            mainStackView.pop()
                        }
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: deviceList.count === 0 && !BluetoothManager.scanning
                color: Material.theme === Material.Dark ? "#FFAAAAAA" : "#FF888888"
                text: t.t("No devices discovered yet")
            }
        }
    }
}
