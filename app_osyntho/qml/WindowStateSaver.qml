// Source - https://stackoverflow.com/questions/31426528/saving-window-state-with-qml
// Posted by Alexander Dyagilev, modified by community. See post 'Timeline' for change history
// Retrieved 2026-01-11, License - CC BY-SA 4.0

import QtQuick
import QtQuick.Window
import QtQuick.Controls.Material
import QtCore

Item {
    property Window window
    property string windowName: ""
    property bool isEnabled: false

    Settings {
        id: s
        category: windowName
        property int x
        property int y
        property int width
        property int height
        property int visibility
    }

    Component.onCompleted: {
        if (isEnabled && s.width && s.height) {
            window.x = s.x;
            window.y = s.y;
            window.width = s.width;
            window.height = s.height;
            window.visibility = s.visibility;
        }
    }

    Connections {
        target: window
        function onXChanged() { if (isEnabled) saveSettingsTimer.restart() }
        function onYChanged() { if (isEnabled) saveSettingsTimer.restart() }
        function onWidthChanged() { if (isEnabled) saveSettingsTimer.restart() }
        function onHeightChanged() { if (isEnabled) saveSettingsTimer.restart() }
        function onVisibilityChanged() { if (isEnabled) saveSettingsTimer.restart() }
    }

    Timer {
        id: saveSettingsTimer
        interval: 500
        repeat: false
        onTriggered: saveSettings()
    }

    function saveSettings() {
        if (!isEnabled) return
        switch(window.visibility) {
            case ApplicationWindow.Windowed:
                s.x = window.x;
                s.y = window.y;
                s.width = window.width;
                s.height = window.height;
                s.visibility = window.visibility;
                break;
            case ApplicationWindow.FullScreen:
                s.visibility = window.visibility;
                break;
            case ApplicationWindow.Maximized:
                s.visibility = window.visibility;
                break;
        }
    }
}
