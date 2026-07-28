import QtQuick
import QtQuick.Controls.Material

// A ComboBox whose selection keeps following the model after the user has
// touched it.
//
// ComboBox assigns its own `currentIndex` when an item is activated, and that
// assignment replaces any binding written onto it — so a plain
// `currentIndex: someModelValue` silently stops tracking after the first pick.
// The box then goes on showing a stale value: the previous track's target, the
// previously selected step's condition, a kit the synth is no longer on.
//
// Bind `modelIndex` instead of `currentIndex` and the value is pushed back in
// by hand whenever it moves. Same trap and same fix as StepField.qml, which
// does this for SpinBox.
//
// `activated(int index)` is untouched and remains the user-edit signal — it
// never fires for a value pushed in here, so echoing a write back to the synth
// is impossible.
ComboBox {
    id: control

    // The index the model says should be shown.
    property int modelIndex: 0

    function sync() {
        if (currentIndex !== modelIndex)
            currentIndex = modelIndex
    }

    currentIndex: modelIndex
    onModelIndexChanged: sync()
    // Swapping the model (a kit change, an engine switch) makes ComboBox reset
    // its own currentIndex. `count` settles after the new list is in place, so
    // it is the reliable moment to re-assert.
    onCountChanged: sync()
}
