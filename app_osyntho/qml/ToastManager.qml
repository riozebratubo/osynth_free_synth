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
        // A toast asked for before this component finished loading is dropped;
        // that is intended. But the status has to be checked too, not just the
        // handle: createObject() returns null for a component that is not Ready
        // (still loading, or failed to compile), and the next line would then
        // throw — taking the message with it and, since these carry the app's
        // error reporting, hiding whatever went wrong in the first place.
        if (!toastComponent || toastComponent.status !== Component.Ready) {
            console.warn("Toast dropped, component not ready:", text)
            return
        }
        var toast = toastComponent.createObject(root);
        if (!toast) {
            console.warn("Toast dropped, could not be created:", text)
            return
        }
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
