#!/usr/bin/env python3
"""Keyboard.qml control strip: latch + chord-mode buttons and a ribbon hide/show.

Three additions to the on-screen keyboard's control strip, all requested
together:

  * a latch (hold) button after the computer-keys / drum-pad pair, writing the
    same keyboard_hold setting as Settings > Keyboard;
  * a chord-mode button after it, toggling the firmware's chord.enable param;
  * a right-aligned eye button that hides every control in the strip, for touch
    devices where the ribbon sits close enough to the keys to be hit while
    playing. Deliberately per-run state, not a setting.

Idempotent: every replacement is anchored on text the edit removes, so a second
run is a no-op that reports "already applied".

Kept per the project's artifact policy; not part of the build.
"""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
QML = os.path.join(ROOT, "app_osyntho", "qml", "Keyboard.qml")
TRANSLATOR = os.path.join(ROOT, "app_osyntho", "src", "translator.cpp")


# Both targets are stored CRLF. Matching is done on LF text so the anchors in
# this file can be written normally, and the original ending is put back on the
# way out -- rewriting the whole file with the other ending would turn a
# six-line edit into a whole-file diff.
def read(path):
    with io.open(path, encoding="utf-8", newline="") as f:
        text = f.read()
    eol = "\r\n" if "\r\n" in text else "\n"
    return text.replace("\r\n", "\n"), eol


def write(path, text, eol):
    with io.open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text.replace("\n", eol) if eol != "\n" else text)


def sub(text, old, new, label):
    if new in text:
        print("  = %s (already applied)" % label)
        return text
    if text.count(old) != 1:
        sys.exit("!! %s: anchor found %d times, expected 1" % (label, text.count(old)))
    print("  + %s" % label)
    return text.replace(old, new)


# ---------------------------------------------------------------- Keyboard.qml
q, q_eol = read(QML)

# 1. Per-run ribbon visibility, next to the other control-strip properties.
q = sub(q, '''    property bool showNoteNames: App.settingIsTrue("keyboard_show_note_names")
''', '''    property bool showNoteNames: App.settingIsTrue("keyboard_show_note_names")
    // Whether the control strip shows its buttons. Deliberately NOT a setting:
    // it is a per-take convenience for touch devices where the strip is close
    // enough to the keys to be hit while playing, so every run starts with the
    // buttons there - a player who hid them last week should not have to
    // remember where the octave controls went.
    property bool ribbonVisible: true
''', "ribbonVisible property")

# 2. Chord state is refreshed from Synth signals that are gated on `visible`,
#    so a strip hidden while the params arrived came back stale.
q = sub(q, '''    onVisibleChanged: {
        if (visible) {
            reloadSettings()
        } else {''', '''    onVisibleChanged: {
        if (visible) {
            reloadSettings()
            // The Synth connections below are gated on `visible` and nothing
            // replays what they missed, so a strip hidden while the params
            // were discovered came back with no chord state: stale key labels,
            // and now a chord button whose visibility depends on the param id.
            refreshChordState()
        } else {''', "refresh chord state on show")

# 3. The strip's own header comment, which named only two of its controls.
q = sub(q, '''    // Control strip: octave shift (left) and key-area size slider (right, only in
    // "slider" resize mode).''', '''    // Control strip: octave shift, the computer-key wiring and the latch /
    // chord-mode toggles (left), the key-area size slider (right, only in
    // "slider" resize mode), and the button that hides all of them (far
    // right, always shown).''', "control strip comment")

# 4. Everything except the hide/show button lives in the left Row.
q = sub(q, '''        Row {
            anchors.left: parent.left
            anchors.leftMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6

            ToolButton {
                text: "−"  // minus''', '''        Row {
            visible: root.ribbonVisible
            anchors.left: parent.left
            anchors.leftMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6

            ToolButton {
                text: "−"  // minus''', "gate left row on ribbonVisible")

# 5. The latch and chord buttons, after the computer-key pair.
q = sub(q, '''                ToolTip.text: root.topRowDrums
                              ? Tr.t("Q…I and 1…8 fire the drum pads. Click to play a second octave instead.")
                              : Tr.t("Q…I and 1…8 play a second octave. Click to fire the drum pads instead.")
            }
''', '''                ToolTip.text: root.topRowDrums
                              ? Tr.t("Q…I and 1…8 fire the drum pads. Click to play a second octave instead.")
                              : Tr.t("Q…I and 1…8 play a second octave. Click to fire the drum pads instead.")
            }

            // What a key *does*, rather than which keys are wired up: latch and
            // chord mode. Their own group, and not desktop-gated like the pair
            // above - both are just as useful under a finger, which is also why
            // this separator stays when the desktop-only buttons are gone.
            Rectangle {
                width: 1
                height: 18
                anchors.verticalCenter: parent.verticalCenter
                color: Material.foreground
                opacity: 0.2
            }
            ToolButton {
                width: 34
                text: "\\uf08d"  // thumbtack: the note stays pinned down
                font.family: App.fontAwesomeName
                font.weight: Font.Black  // solid face
                font.pointSize: UI.fontSize * 0.95
                highlighted: root.hold
                opacity: root.hold ? 1.0 : 0.45
                // Written through the setting rather than the property, like
                // Settings ▸ Keyboard: reloadSettings() is the one writer of
                // `hold`, and onHoldChanged is what ends the latched notes when
                // it goes off. Assigning the property here would give it a
                // second writer for the Settings switch to disagree with.
                //
                // What is sounding goes first, in both directions, for the same
                // reason the wiring buttons drop it: with latch on, both release
                // paths bail out early (onComputerKeyReleased returns, and
                // onTouchUpdated returns before it reconciles), so a key held
                // across the click would never get its note-off.
                onClicked: {
                    root.releaseSounding()
                    App.saveSetting("keyboard_hold", root.hold ? "false" : "true")
                }
                ToolTip.visible: hovered
                ToolTip.text: root.hold
                              ? Tr.t("Notes latch on. Click to play them momentarily again.")
                              : Tr.t("Notes play momentarily. Click to latch them on.")
            }
            ToolButton {
                // Firmware without the chord engine registers no chord.enable,
                // and a button that cannot do anything is worse than an absent
                // one - the Chord page hides its controls the same way.
                visible: root.pidChordEnable >= 0
                width: 34
                text: "\\uf5fd"  // layer-group: one key, a stack of notes
                font.family: App.fontAwesomeName
                font.weight: Font.Black  // solid face
                font.pointSize: UI.fontSize * 0.95
                highlighted: root.chordOn
                opacity: root.chordOn ? 1.0 : 0.45
                // Only the param is written: chordOn follows it back through
                // refreshChordState(), which also re-labels the keys with the
                // chord names. Nothing sounding is dropped here, unlike the
                // buttons above - the firmware re-voices every held key when
                // chord.enable moves (chord.cpp revoice_entry), so a key down
                // across the click keeps playing and simply changes shape.
                onClicked: Synth.setParam(root.pidChordEnable, root.chordOn ? 0 : 1)
                ToolTip.visible: hovered
                ToolTip.text: root.chordOn
                              ? Tr.t("Chord mode is on: each key plays a chord. Click to play single notes.")
                              : Tr.t("Each key plays a single note. Click to play chords instead.")
            }
''', "latch + chord buttons")

# 6. The size slider hangs off the hide/show button and hides with the rest.
q = sub(q, '''        Row {
            visible: root.resizeMode === "slider"
            anchors.right: parent.right
            anchors.rightMargin: 8''', '''        Row {
            // A slider is exactly the sort of thing a stray thumb moves, so it
            // goes with the buttons; the toggle owns the right edge now, so
            // this hangs off the toggle rather than off the strip.
            visible: root.resizeMode === "slider" && root.ribbonVisible
            anchors.right: ribbonToggle.left
            anchors.rightMargin: 8''', "slider row anchored to the toggle")

# 7. The hide/show button itself, last child of the control strip.
q = sub(q, '''                onMoved: root.setKeyHeight(value)
            }
        }
    }
''', '''                onMoved: root.setKeyHeight(value)
            }
        }

        // Hides every control above. On a phone or a small touch panel the
        // strip sits a thumb's width from the keys, and a mistimed press lands
        // on the octave buttons mid-take; with the ribbon away the whole strip
        // is inert except for this one button, parked in the far corner.
        //
        // Its polarity is the opposite of the buttons it hides: they are dim
        // until switched on, this one is dim in the state the app starts in
        // (ribbon shown) and lights up while it is holding something back.
        ToolButton {
            id: ribbonToggle
            width: 34
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            text: root.ribbonVisible ? "\\uf06e" : "\\uf070"  // eye / eye-slash
            font.family: App.fontAwesomeName
            font.weight: Font.Black  // solid face
            font.pointSize: UI.fontSize * 0.95
            highlighted: !root.ribbonVisible
            opacity: root.ribbonVisible ? 0.45 : 1.0
            onClicked: root.ribbonVisible = !root.ribbonVisible
            ToolTip.visible: hovered
            ToolTip.text: root.ribbonVisible
                          ? Tr.t("Hide the keyboard's buttons, so playing cannot hit them.")
                          : Tr.t("The keyboard's buttons are hidden. Click to show them.")
        }
    }
''', "ribbon hide/show button")

write(QML, q, q_eol)

# --------------------------------------------------------------- translator.cpp
t, t_eol = read(TRANSLATOR)
t = sub(t, '''  pt["Q…I and 1…8 play a second octave. Click to fire the drum pads instead."] =
      "Q…I e 1…8 tocam uma segunda oitava. Clique para tocarem os pads de bateria.";
''', '''  pt["Q…I and 1…8 play a second octave. Click to fire the drum pads instead."] =
      "Q…I e 1…8 tocam uma segunda oitava. Clique para tocarem os pads de bateria.";
  pt["Notes latch on. Click to play them momentarily again."] =
      "As notas ficam sustentadas. Clique para voltarem a tocar momentaneamente.";
  pt["Notes play momentarily. Click to latch them on."] =
      "As notas tocam momentaneamente. Clique para sustentá-las.";
  pt["Chord mode is on: each key plays a chord. Click to play single notes."] =
      "O modo acorde está ligado: cada tecla toca um acorde. Clique para tocar notas "
      "simples.";
  pt["Each key plays a single note. Click to play chords instead."] =
      "Cada tecla toca uma nota simples. Clique para tocar acordes.";
  pt["Hide the keyboard's buttons, so playing cannot hit them."] =
      "Oculta os botões do teclado, para que tocar não os acione.";
  pt["The keyboard's buttons are hidden. Click to show them."] =
      "Os botões do teclado estão ocultos. Clique para mostrá-los.";
''', "pt_BR tooltip strings")
write(TRANSLATOR, t, t_eol)

print("done")
