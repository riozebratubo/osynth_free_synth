import QtQuick
import QtQuick.Controls.Material

// A checkable Button whose `checked` keeps following the model after the user
// has pressed it.
//
// AbstractButton assigns its own `checked` when the button is pressed, and that
// assignment replaces any binding written onto it — so a plain
// `checked: someModelValue` silently stops tracking after the first press. The
// button then goes on showing a stale state: the previously selected step's
// accent, a Song/Count-in toggle the synth has since moved (a loaded set, a
// preset), a looper switch a loaded set re-formatted. Worse for a per-step
// toggle, where the next press writes the inverse of the *stale* value to the
// newly selected step, so setting the flag takes two presses and the first one
// silently edited nothing.
//
// Bind `modelChecked` instead of `checked` and the value is pushed back in by
// hand whenever it moves. Same trap and same fix as SyncedComboBox.qml (for
// ComboBox) and StepField.qml (for SpinBox).
//
// `toggled()` is untouched and remains the user-edit signal: Qt emits it from
// the interaction path only, never for a `checked` written here, so echoing a
// pushed value back to the synth is impossible.
Button {
    id: control

    // What the model says the button should show.
    property bool modelChecked: false

    checkable: true
    checked: modelChecked
    onModelCheckedChanged: if (checked !== modelChecked) checked = modelChecked
}
