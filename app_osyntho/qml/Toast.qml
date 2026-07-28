import QtQuick // 2.0

/**
* @brief An Android-like timed message text in a box that selfdestroys when finished if desired
*/
Rectangle{

    /**
    * Public
    */

    /**
    * @brief Shows this Toast
    *
    * @param {string} text Text to show
    * @param {real} duration Duration to show in milliseconds, defaults to 3000
    */
    function show(text, duration, backgroundColor, foregroundColor, icon){
        theText.text = text;
        theIcon.text = (typeof icon !== "undefined") ? icon : "";
        if(typeof duration !== "undefined"){
            if(duration >= 2*fadeTime)
                time = duration;
            else
                time = 2*fadeTime;
            }
        else
            time = defaultTime;
        if (typeof backgroundColor !== "undefined") {
            desiredBackgroundColor = backgroundColor;
        }
        if (typeof foregroundColor !== "undefined") {
            desiredForegroundColor = foregroundColor;
        }
        anim.start();
    }

    property bool selfDestroying: false ///< Whether this Toast will selfdestroy when it is finished

    /**
    * Private
    */

    id: root

    property real time: defaultTime
    readonly property real defaultTime: 3000
    readonly property real fadeTime: 300
    property color desiredForegroundColor: "black"
    property color desiredBackgroundColor: "white"

    property real margin: 10

    // parent is the ToastManager column, which spans the app width
    readonly property real maxWidth: parent ? parent.width - 2*margin : Infinity

    width: Math.min(content.width + 2*margin, maxWidth)
    height: content.height + 2*margin
    radius: margin

    anchors.horizontalCenter: parent.horizontalCenter

    // QMLCOMPILERCHANGE // opacity: 0
    opacity: 0
    color: desiredBackgroundColor

    Row {
        id: content
        x: margin
        y: margin
        spacing: 8

        Text {
            id: theIcon
            color: desiredForegroundColor
            text: ""
            visible: text.length > 0
            font.family: App.fontAwesomeName
            font.weight: Font.Black  // solid face
            anchors.verticalCenter: parent.verticalCenter
        }

        Text{
            id: theText
            color: desiredForegroundColor
            text: ""

            wrapMode: Text.Wrap
            width: Math.min(implicitWidth,
                            root.maxWidth - 2*root.margin
                            - (theIcon.visible ? theIcon.width + content.spacing : 0))
            horizontalAlignment: Text.AlignHCenter
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    SequentialAnimation on opacity{
        id: anim

        running: false

        NumberAnimation{
            to: 0.9
            duration: fadeTime
        }
        PauseAnimation{
            duration: time - 2*fadeTime
        }
        NumberAnimation{
            to: 0
            duration: fadeTime
        }

        onRunningChanged:{
            if(!running && selfDestroying)
                root.destroy();
        }
    }
}
