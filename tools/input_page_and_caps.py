#!/usr/bin/env python3
"""Three changes, one pass:

  A. The osynth (Dev) page goes away. Its "Analogue output" group and its USB
     port card move to Home; its `in.` group becomes a new Input page, placed
     before FX -- and FX's duplicate copy of the same group is removed. Stored
     page indices are remapped once (settings.cpp) so nobody's startup screen
     silently moves.
  B. Parameter labels are capitalised at the point of display (UI.paramLabel),
     as are preset and library-patch names (UI.capitalized). The registered
     spelling is never touched.
  C. The toolbar's prev/next arrows stay on the bar at the ends of the pager,
     disabled instead of hidden.

Exact unique anchors, all-or-nothing write.
"""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

changes = []


def sub(path, old, new):
    changes.append((os.path.join(ROOT, path), old, new))


# ======================================================================= A
# ---------------------------------------------------------------- UI.qml
sub("app_osyntho/qml/UI.qml",
    """        { label: "Mod",  name: "Modulation",         icon: "\\uf4e2" },  // circle-nodes
        { label: "FX",   name: "Effects",            icon: "\\uf72b" },  // wand-magic-sparkles""",

    """        { label: "Mod",  name: "Modulation",         icon: "\\uf4e2" },  // circle-nodes
        // Before FX because that is the order the signal takes, and its own
        // page because the `in.` group used to be drawn on both the FX page
        // and the old osynth page — one set of controls under two titles.
        { label: "In",   name: "Input",              icon: "\\uf130" },  // microphone
        { label: "FX",   name: "Effects",            icon: "\\uf72b" },  // wand-magic-sparkles""")

sub("app_osyntho/qml/UI.qml",
    """        { label: "Loc. Pre", name: "Local presets",  icon: "\\uf02d" },  // book
        // The synth's own persisted settings (S35), as opposed to the app's
        // (SettingsScreen) or the patch's (everything above). Last, because it
        // is the page you visit least and its index must not shift the ones
        // people have set as their startup screen.
        { label: "Dev",  name: "osynth",             icon: "\\uf2db" }   // microchip
    ]""",

    """        { label: "Loc. Pre", name: "Local presets",  icon: "\\uf02d" }  // book
        // There was an osynth (Dev) page here holding the synth's own
        // persisted settings (S35). It held three things and repeated two of
        // them: master volume was already on the toolbar and its `in.` group
        // was already on FX. What was genuinely only there — the analogue
        // output level and the USB port card — is on Home now, and the input
        // has the page above. Removing it moved every index from FX up, so
        // Settings::migrateScreenOrder() remaps the two settings that store
        // one.
    ]""")

# ---------------------------------------------------------------- Main.qml
sub("app_osyntho/qml/Main.qml",
    """                ModScreen {}
                FxScreen {}""",
    """                ModScreen {}
                InputScreen {}
                FxScreen {}""")

sub("app_osyntho/qml/Main.qml",
    """                PatchLibraryScreen {}
                OsynthScreen {}
            }""",
    """                PatchLibraryScreen {}
            }""")

# ---------------------------------------------------------------- FxScreen
sub("app_osyntho/qml/FxScreen.qml",
    """// Master FX bus, plus the line input that feeds into it (S31).
// Engine-independent (registered at boot), so these persist across engine
// switches.""",

    """// Master FX bus. Engine-independent (registered at boot), so these persist
// across engine switches.
//
// The input that feeds the bus is the Input page next door, not a card here:
// the same `in.` group was drawn on this page and on the old osynth page at
// once, so one control appeared twice with nothing to say the two were one.""")

sub("app_osyntho/qml/FxScreen.qml",
    """            // Line input (S31). Self-hides on firmware without it, since
            // PARAM_INFO then reports no `in.` ids at all.
            ParamGroup { title: "Line in"; prefix: "in." }

            // Everything from here down self-hides on firmware that does not
            // register the prefix, same as Line in above — so one app build
            // still drives a pre-S33 or pre-S34 synth.""",

    """            // Every card here self-hides on firmware that does not register
            // its prefix — so one app build still drives a pre-S33 or pre-S34
            // synth.""")

# ---------------------------------------------------------------- CMakeLists
sub("app_osyntho/CMakeLists.txt",
    "    QML_FILES qml/ModScreen.qml\n    QML_FILES qml/FxScreen.qml\n",
    "    QML_FILES qml/ModScreen.qml\n    QML_FILES qml/InputScreen.qml\n    QML_FILES qml/FxScreen.qml\n")

sub("app_osyntho/CMakeLists.txt",
    "    QML_FILES qml/PatchLibraryScreen.qml\n    QML_FILES qml/OsynthScreen.qml\n",
    "    QML_FILES qml/PatchLibraryScreen.qml\n")

# ---------------------------------------------------------------- HomeScreen
sub("app_osyntho/qml/HomeScreen.qml",
    """// Overview: engine select, master + voice (glide/unison/bend) controls, the
// current preset, and the way back to a blank instrument.
Item {
    id: screen
    Flickable {""",

    """// Overview: engine select, master + voice (glide/unison/bend) controls, the
// analogue output level, the current preset, the USB port and the way back to
// a blank instrument.
//
// The last two came off the old osynth page when it was removed. They belong
// with the rest of "what is this synth set to", and that page was down to
// repeating what the toolbar and the FX page already showed.
Item {
    id: screen

    // Set when a restart never completed, cleared the moment one is asked for
    // again. Shown inline rather than as a toast: toasts live in Main.qml's
    // scope and a page cannot reach one, and this is a message you want still
    // on screen while you go and look at the synth.
    property bool restartFailed: false

    // Poll the port for what is attached to it. There is no event for "a
    // controller was plugged in", so the page has to ask — but only in host
    // mode, where the answer can change behind the app's back. In device mode
    // nothing does, and unlike the osynth page this one is where the app
    // opens, so a blanket 3 s poll would run for most of a session.
    Timer {
        interval: 3000
        running: screen.visible && Synth.connected && usbCard.visible
                 && Synth.usbActiveMode === 1
        repeat: true
        triggeredOnStart: true
        onTriggered: Synth.refreshUsbStatus()
    }

    Flickable {""")

sub("app_osyntho/qml/HomeScreen.qml",
    """            ParamGroup { title: "Master"; prefix: "master" }
            ParamGroup { title: "Voice"; prefix: "common" }
""",
    """            ParamGroup { title: "Master"; prefix: "master" }
            ParamGroup { title: "Voice"; prefix: "common" }
            // out.level — ES8388 builds only, and self-hiding elsewhere on the
            // same existence test the toolbar's copy uses. Distinct from
            // master volume: this one sets the operating point for whatever is
            // in the jack and is set once by ear, where master volume is the
            // digital level you ride.
            ParamGroup { title: "Analogue output"; prefix: "out." }
""")

sub("app_osyntho/qml/HomeScreen.qml",
    """            // Back to a blank instrument. Resolved by name rather than by a""",

    """            // ---- USB ------------------------------------------------------
            // Not a ParamGroup, though `usb.mode` is an ordinary enum
            // parameter and would render as one: writing it changes nothing
            // until the synth restarts, and a control that silently does
            // nothing is worse than no control. So the write is paired with
            // the state that explains it and the button that applies it.
            Rectangle {
                id: usbCard
                width: panels.contentWidth
                height: usbCol.implicitHeight + 20
                radius: 8
                // Hidden entirely on firmware that cannot host: no USB-OTG, or
                // a build where the USB sink is the audio clock and dropping
                // the device role would leave the synth silent. The firmware
                // decides that, not the app.
                visible: Synth.usbStatusKnown && Synth.usbHostSupported
                color: Material.theme === Material.Dark ? "#1AFFFFFF" : "#0D000000"

                ColumnLayout {
                    id: usbCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 10
                    spacing: 8

                    Label {
                        text: Tr.t("USB port")
                        font.bold: true
                        font.pointSize: UI.fontSize * 0.95
                        color: Material.foreground
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.7
                        color: Material.foreground
                        font.pointSize: UI.fontSize * 0.85
                        text: Tr.t("One socket, one role. As a device the synth is an audio interface and MIDI port on a computer; as a host it plays a USB MIDI controller you plug into it. Changing this restarts the synth.")
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        ParamControl {
                            paramId: Synth.paramIdForName("usb.mode")
                            enabled: !Synth.restarting
                        }

                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Material.foreground
                            font.pointSize: UI.fontSize * 0.85
                            // Three states worth telling apart: mid-restart, a
                            // pending change, and settled.
                            text: Synth.restarting
                                  ? Tr.t("Restarting…")
                                  : Synth.usbRestartRequired
                                    ? Tr.t("Restart to apply — the port is still in %1 mode.")
                                       .arg(Synth.usbActiveMode === 1 ? Tr.t("host") : Tr.t("device"))
                                    : (Synth.usbActiveMode === 1
                                       ? Tr.t("Hosting MIDI controllers.")
                                       : Tr.t("Connected to a computer as an audio + MIDI device."))
                        }
                    }

                    // Only offered when it would do something. A restart with
                    // nothing pending is just a way to interrupt yourself.
                    Button {
                        text: Synth.restarting ? Tr.t("Restarting…") : Tr.t("Restart synth")
                        enabled: Synth.usbRestartRequired && !Synth.restarting
                        visible: Synth.usbRestartRequired || Synth.restarting
                        highlighted: true
                        onClicked: restartDialog.open()
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        visible: screen.restartFailed
                        color: Material.color(Material.Red)
                        font.pointSize: UI.fontSize * 0.85
                        text: Tr.t("The synth did not come back. It may still be restarting — check its power and Bluetooth.")
                    }

                    // ---- what is attached ----
                    // Host mode only: in device mode there is nothing on the
                    // other end of the cable to enumerate. Without this the
                    // only symptom of a cable, power or descriptor problem is
                    // "no sound", which says nothing about which of the three
                    // it is.
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        visible: Synth.usbActiveMode === 1 && !Synth.restarting
                        color: Material.foreground
                        font.pointSize: UI.fontSize * 0.85
                        text: Synth.usbAttachedCount === 0
                              ? Tr.t("No controller detected. Check that it is powered and that its cable carries data.")
                              : Synth.usbAttachedCount === 1
                                ? Tr.t("Connected: %1").arg(Synth.usbAttachedName)
                                : Tr.t("Connected: %1 (+%2 more)")
                                   .arg(Synth.usbAttachedName).arg(Synth.usbAttachedCount - 1)
                    }
                }
            }

            // Back to a blank instrument. Resolved by name rather than by a""")

sub("app_osyntho/qml/HomeScreen.qml",
    """        onAccepted: {
            if (resetCard.resetId >= 0) Synth.setParamNow(resetCard.resetId, 1)
        }
    }
""",
    """        onAccepted: {
            if (resetCard.resetId >= 0) Synth.setParamNow(resetCard.resetId, 1)
        }
    }

    Dialog {
        id: restartDialog
        anchors.centerIn: Overlay.overlay
        modal: true
        focus: true
        title: Tr.t("Restart the synth?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: Math.min(parent ? parent.width - 32 : 420, 420)

        contentItem: Label {
            wrapMode: Text.WordWrap
            color: Material.foreground
            // Presets, sequencer patterns and the looper all persist; live
            // notes and an unsaved take do not, and that is the part worth
            // saying out loud before the box goes away.
            text: Tr.t("The synth will restart to change its USB role. Audio stops and the app reconnects on its own. Saved presets, patterns and loops are kept.")
        }

        onAccepted: {
            screen.restartFailed = false
            Synth.restartSynth()
        }
    }

    Connections {
        target: Synth
        function onRestartTimedOut() { screen.restartFailed = true }
    }
""")

# ======================================================================= B
# ---------------------------------------------------------------- UI.qml
sub("app_osyntho/qml/UI.qml",
    """    // The slot the synth is on, in the one form every surface names it in:""",

    """    // Display forms for names the firmware owns. The registered spelling is
    // never touched: a parameter's name is the firmware's identifier and every
    // lookup — paramIdForName, the p-lock picker, a stored patch's rows, an
    // exported JSON file — still spells it exactly as registered. Only what is
    // drawn passes through here.
    function capitalized(text: string): string {
        return text.length > 0 ? text.charAt(0).toUpperCase() + text.slice(1) : text
    }

    // The leaf of a parameter's dotted name, as a control's label:
    // "common.glide" -> "Glide", "fx.lfo1.rate" -> "Rate". Every control that
    // labels itself from a parameter goes through this, so the pages cannot
    // drift apart on capitalisation the way three separate `.split('.').pop()`
    // expressions were free to.
    function paramLabel(name: string): string {
        const leaf = name.split('.').pop()
        return leaf.length > 0 ? leaf.charAt(0).toUpperCase() + leaf.slice(1) : leaf
    }

    // The slot the synth is on, in the one form every surface names it in:""")

sub("app_osyntho/qml/UI.qml",
    """            ? (Synth.presetSlot
               + (Synth.presetName.length > 0 ? (" - " + Synth.presetName) : ""))
            : \"\"""",
    """            ? (Synth.presetSlot
               + (Synth.presetName.length > 0
                  ? (" - " + capitalized(Synth.presetName)) : ""))
            : \"\"""")

# -------------------------------------------------- the three label sites
sub("app_osyntho/qml/Knob.qml",
    "            text: root.meta.exists ? root.meta.name.split('.').pop() : \"\"",
    "            text: root.meta.exists ? UI.paramLabel(root.meta.name) : \"\"")

sub("app_osyntho/qml/EnumSelector.qml",
    "            text: root.meta.exists ? root.meta.name.split('.').pop() : \"\"",
    "            text: root.meta.exists ? UI.paramLabel(root.meta.name) : \"\"")

sub("app_osyntho/qml/ParamControl.qml",
    "                    text: pc.meta.exists ? pc.meta.name.split('.').pop() : \"\"",
    "                    text: pc.meta.exists ? UI.paramLabel(pc.meta.name) : \"\"")

# ------------------------------------------------- preset / patch names
sub("app_osyntho/qml/PresetsScreen.qml",
    """                            text: (tile.modelData.name && tile.modelData.name.length)
                                  ? tile.modelData.name : Tr.t("(unnamed)")""",
    """                            text: (tile.modelData.name && tile.modelData.name.length)
                                  ? UI.capitalized(tile.modelData.name)
                                  : Tr.t("(unnamed)")""")

sub("app_osyntho/qml/PatchLibraryScreen.qml",
    """                            text: modelData.name && modelData.name.length ? modelData.name : Tr.t("(unnamed)")""",
    """                            // Display only — Rename seeds from modelData.name
                            // and Export names the file from it, both raw.
                            text: modelData.name && modelData.name.length
                                  ? UI.capitalized(modelData.name) : Tr.t("(unnamed)")""")

# ======================================================================= C
sub("app_osyntho/qml/Toolbar.qml",
    """    readonly property real fixedChromeWidth:
        (prevButton.visible ? prevButton.width : 8)
        + (masterVol.visible ? 76 : 0)
        + (outLevel.visible ? 76 : 0)
        + plugLabel.width + 4
        + menuButton.width
        + (nextButton.visible ? nextButton.width : 0)
        + 24  // RowLayout spacing and margins""",

    """    readonly property real fixedChromeWidth:
        prevButton.width
        + (masterVol.visible ? 76 : 0)
        + (outLevel.visible ? 76 : 0)
        + plugLabel.width + 4
        + menuButton.width
        + nextButton.width
        + 24  // RowLayout spacing and margins""")

sub("app_osyntho/qml/Toolbar.qml",
    """            visible: t1.pager && t1.pager.currentIndex > 0
            onClicked: if (t1.pager.currentIndex > 0) t1.pager.currentIndex--
        }
        // Stands in for the hidden prev button so the title does not jump to the
        // edge on the first page. Layout.preferredWidth, not width: a RowLayout
        // sets its children's width itself, from their preferred/implicit size,
        // so a plain `width: 8` was overwritten with the Item's implicitWidth of
        // 0 — the spacer was not there at all, while fixedChromeWidth below has
        // always charged 8 px for it.
        Item { visible: !prevButton.visible; Layout.preferredWidth: 8 }
""",
    """            // Greyed at the ends of the pager rather than hidden. A button
            // that comes and goes moves everything beside it: the title used
            // to jump a whole button's width on the first and last pages, and
            // the spacer that used to stand in for this one existed only to
            // soften that. It also kept the arrow from being somewhere your
            // eye could learn.
            enabled: t1.pager && t1.pager.currentIndex > 0
            onClicked: if (t1.pager.currentIndex > 0) t1.pager.currentIndex--
        }
""")

sub("app_osyntho/qml/Toolbar.qml",
    """            visible: t1.pager && t1.pager.currentIndex < t1.pager.count - 1
            onClicked: if (t1.pager.currentIndex < t1.pager.count - 1) t1.pager.currentIndex++""",
    """            enabled: t1.pager && t1.pager.currentIndex < t1.pager.count - 1
            onClicked: if (t1.pager.currentIndex < t1.pager.count - 1) t1.pager.currentIndex++""")

# =============================================================== settings
sub("app_osyntho/src/business/settings.h",
    """  void fillSettingsCacheDefaultValues();
  void fillSettingsCacheCurrentValues();
""",
    """  void fillSettingsCacheDefaultValues();
  void fillSettingsCacheCurrentValues();
  void migrateScreenOrder();
""")

sub("app_osyntho/src/business/settings.cpp",
    """  settingsCache["startup_screen"] = "0";
  settingsCache["last_swipeview_index"] = "0";  // page left on the last run""",

    """  settingsCache["startup_screen"] = "0";
  settingsCache["last_swipeview_index"] = "0";  // page left on the last run
  // Which revision of UI.screens the two indices above were written against.
  // Bumped whenever a page is inserted or removed, so migrateScreenOrder() can
  // move them once and only once. "0" is the pre-Input-page order.
  settingsCache["screen_order_rev"] = "0";""")

sub("app_osyntho/src/business/settings.cpp",
    """void Settings::reloadCurrentSettings() {
  fillSettingsCacheDefaultValues();
  fillSettingsCacheCurrentValues();
}""",

    """void Settings::reloadCurrentSettings() {
  fillSettingsCacheDefaultValues();
  fillSettingsCacheCurrentValues();
  // After the cache is whole, and here rather than in the constructor: a
  // restored backup carries someone else's indices and comes back through this
  // same funnel (App::onDatabaseRestoredAction).
  migrateScreenOrder();
}

// UI.screens gained an Input page before FX and lost the osynth (Dev) page,
// which moved every index from FX up by one and left index 12 naming nothing.
// Two settings store one of those indices — the startup screen and the page
// the last run was left on — and both would otherwise quietly point at the
// wrong page. Remapped once, marked by screen_order_rev.
//
// The old order was: 0 Home, 1 Osc, 2 Flt, 3 Mod, 4 FX, 5 Seq, 6 Drum, 7 Arp,
// 8 Loop, 9 Patch, 10 Pre, 11 Lib, 12 Dev.
void Settings::migrateScreenOrder() {
  if (settingsCache.value("screen_order_rev") == "1") return;

  const auto remap = [](const QString& value) {
    bool ok = false;
    const int idx = value.toInt(&ok);
    if (!ok || idx < 4) return value;  // "last", Home..Mod: unmoved
    if (idx == 12) return QStringLiteral("0");  // the page that is gone
    if (idx > 12) return value;                 // not one of ours; leave it
    return QString::number(idx + 1);
  };

  for (const QString& key :
       {QStringLiteral("startup_screen"), QStringLiteral("last_swipeview_index")}) {
    const QString moved = remap(settingsCache.value(key));
    if (moved != settingsCache.value(key)) saveSetting(key, moved);
  }
  saveSetting(QStringLiteral("screen_order_rev"), QStringLiteral("1"));
}""")

# ============================================================== translator
sub("app_osyntho/src/translator.cpp",
    '  pt["Effects"] = "Efeitos";',
    '  pt["Input"] = "Entrada";\n'
    '  pt["Effects"] = "Efeitos";')

sub("app_osyntho/src/translator.cpp",
    '  // ── Patch library (PatchLibraryScreen.qml) ─────────────────────────────',

    '  // ── Input / output / USB (InputScreen.qml, HomeScreen.qml) ─────────────\n'
    '  pt["Audio input"] = "Entrada de áudio";\n'
    '  pt["Analogue output"] = "Saída analógica";\n'
    '  pt["This synth has no audio input."] = "Este sintetizador não tem entrada de áudio.";\n'
    '  pt["USB port"] = "Porta USB";\n'
    '\n'
    '  // ── Patch library (PatchLibraryScreen.qml) ─────────────────────────────')


files = {}
for path, old, new in changes:
    if path not in files:
        with io.open(path, encoding="utf-8") as fh:
            files[path] = fh.read()

failed = []
for path, old, new in changes:
    text = files[path]
    if text.count(old) != 1:
        failed.append((path, text.count(old), old.splitlines()[0][:72]))
        continue
    files[path] = text.replace(old, new, 1)

if failed:
    for path, n, head in failed:
        print("ANCHOR x%d in %s: %s" % (n, os.path.relpath(path, ROOT), head))
    sys.exit(1)

for path, text in sorted(files.items()):
    with io.open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("patched", os.path.relpath(path, ROOT))
