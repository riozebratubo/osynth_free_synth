// Source - https://stackoverflow.com/questions/31426528/saving-window-state-with-qml
// Posted by Alexander Dyagilev, modified by community. See post 'Timeline' for change history
// Retrieved 2026-01-11, License - CC BY-SA 4.0

import QtQuick
import QtQuick.Window
import QtQuick.Controls.Material
import QtCore

Item {
    id: root

    property Window window
    property string windowName: ""
    property bool isEnabled: false

    Settings {
        id: s
        category: root.windowName
        property int x
        property int y
        property int width
        property int height
        property int visibility
    }

    Component.onCompleted: {
        if (root.isEnabled && s.width && s.height) {
            root.window.x = s.x;
            root.window.y = s.y;
            root.window.width = s.width;
            root.window.height = s.height;
            root.window.visibility = s.visibility;
        }
    }

    Connections {
        target: root.window
        function onXChanged() { if (root.isEnabled) saveSettingsTimer.restart() }
        function onYChanged() { if (root.isEnabled) saveSettingsTimer.restart() }
        function onWidthChanged() { if (root.isEnabled) saveSettingsTimer.restart() }
        function onHeightChanged() { if (root.isEnabled) saveSettingsTimer.restart() }
        function onVisibilityChanged() { if (root.isEnabled) saveSettingsTimer.restart() }
    }

    Timer {
        id: saveSettingsTimer
        interval: 500
        repeat: false
        onTriggered: root.saveSettings()
    }

    function saveSettings() {
        if (!root.isEnabled) return
        switch(root.window.visibility) {
            case ApplicationWindow.Windowed:
                s.x = root.window.x;
                s.y = root.window.y;
                s.width = root.window.width;
                s.height = root.window.height;
                s.visibility = root.window.visibility;
                break;
            case ApplicationWindow.FullScreen:
                s.visibility = root.window.visibility;
                break;
            case ApplicationWindow.Maximized:
                s.visibility = root.window.visibility;
                break;
        }
    }
}
