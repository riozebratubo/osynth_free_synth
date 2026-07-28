import QtQuick
import QtQuick.Controls.Material

// A Switch whose label wraps to multiple lines instead of being clipped/elided
// when the text is wider than the available width.
Switch {
    id: control
    width: parent ? parent.width : implicitWidth

    contentItem: Text {
        text: control.text
        font: control.font
        color: control.enabled ? Material.foreground : Material.hintTextColor
        leftPadding: control.indicator && !control.mirrored ? control.indicator.width + control.spacing : 0
        rightPadding: control.indicator && control.mirrored ? control.indicator.width + control.spacing : 0
        wrapMode: Text.WordWrap
        verticalAlignment: Text.AlignVCenter
    }
}
