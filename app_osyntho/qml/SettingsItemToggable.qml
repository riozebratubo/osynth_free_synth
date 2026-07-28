import QtQuick
import QtQuick.Controls.Material

Item {
    property alias title: toggableSettingTitleText.text
    property alias text: toggableSettingBodyText.text
    property var initialize
    property var change // parameter value

    width: parent.width
    height: childrenRect.height
    Column {
        width: parent.width - 20
        height: childrenRect.height
        x: 10
        Text {
            id: toggableSettingTitleText
            font.bold: true
            color: Material.foreground
        }
        Switch {
            id: toggableSettingBodyText
            Component.onCompleted: {
                checked = initialize()
            }
            onToggled: {
                change(checked)
            }
        }
    }
}
