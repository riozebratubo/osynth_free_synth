import QtQuick // 2.0

/**
 * @brief Manager that creates Toasts dynamically
 */
Column{

    /**
     * Public
     */

    /**
     * @brief Shows a Toast
     *
     * @param {string} text Text to show
     * @param {real} duration Duration to show in milliseconds, defaults to 3000
     */
    function show(text, duration, backgroundColor, foregroundColor, icon){
        // if the component is not available yet during initialization, the toast is not
        // showh. this is intended.
        if (!toastComponent) return
        var toast = toastComponent.createObject(root);
        toast.selfDestroying = true;
        toast.show(text, duration, backgroundColor, foregroundColor, icon);
    }

    /**
     * Private
     */

    id: root

    z: Infinity
    spacing: 5
    anchors.centerIn: parent
    // span the app width so each Toast can bound itself to it
    width: parent ? parent.width : 0

    property var toastComponent

    Component.onCompleted: toastComponent = Qt.createComponent("Toast.qml")
}
