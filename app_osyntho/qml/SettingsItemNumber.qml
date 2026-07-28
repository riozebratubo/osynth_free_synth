import QtQuick
import QtQuick.Controls.Material

Item {
    property alias title: toggableSettingTitleText.text
    property alias from: toggableSettingSpin.from
    property alias to: toggableSettingSpin.to
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
            text: title
            font.bold: true
            color: Material.foreground
        }
        SpinBox {
            id: toggableSettingSpin
            value: {
                return parseInt(initialize())
            }
            onValueModified: {
                change(value)
            }
        }
    }
}
