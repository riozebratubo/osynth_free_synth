p = 'app_osyntho/qml/PatchLibraryScreen.qml'
s = open(p, encoding='utf-8').read()

old = """// Local patch library — named snapshots of the live parameters, stored in the
// app's SQLite DB (independent of the synth's own preset slots).
Item {
    id: screen

    property var patchList: []"""
new = """// Local patch library — named snapshots of the whole synth, stored in the app's
// SQLite DB (independent of the synth's own preset slots): every parameter that
// is not an action, plus the modular graph the parameters hang off when the
// engine has one.
Item {
    id: screen

    property var patchList: []
    // Whether a Load also replays the two levels the snapshot carries. Off by
    // default and remembered per install: both describe the room and the
    // wiring rather than the sound — master volume is what you are monitoring
    // at, out.level is set once by ear for whatever is in the jack — and a
    // patch that moved either behind you is the surprise the synth's own
    // presets exclude them to avoid. They stay in the stored snapshot either
    // way, so turning a switch on later finds the value still there.
    property bool loadMasterVolume: App.settingIsTrue("patch_load_master_volume")
    property bool loadOutLevel: App.settingIsTrue("patch_load_out_level")"""
assert old in s
s = s.replace(old, new, 1)

old = """            Button {
                text: t.t("Save current…")
                enabled: Synth.connected && Synth.ready
                onClicked: { nameDialog.mode = "save"; nameDialog.patchId = -1; nameDialog.field = ""; nameDialog.open() }
            }
        }
"""
new = """            Button {
                text: t.t("Save current…")
                enabled: Synth.connected && Synth.ready
                onClicked: { nameDialog.mode = "save"; nameDialog.patchId = -1; nameDialog.field = ""; nameDialog.open() }
            }
        }

        Flow {
            Layout.fillWidth: true
            spacing: 16
            Label {
                text: t.t("Load with the patch:")
                color: Material.foreground
                opacity: 0.7
                anchors.verticalCenter: parent.verticalCenter
            }
            Switch {
                text: t.t("Synth volume")
                checked: screen.loadMasterVolume
                onToggled: {
                    screen.loadMasterVolume = checked
                    App.saveSetting("patch_load_master_volume", checked ? "true" : "false")
                }
                ToolTip.visible: hovered
                ToolTip.text: t.t("Also set the master volume a patch was saved with. Off by default: that is the level you are monitoring at, not part of the sound.")
            }
            Switch {
                // out.level exists only on firmware with a codec that has an
                // output driver register (an ES8388 build). Hidden rather than
                // disabled elsewhere — a switch for a control the connected
                // synth does not have is just a puzzle.
                visible: Synth.ready && Synth.paramExists(outLevelId)
                property int outLevelId: -1
                text: t.t("Headphone level")
                checked: screen.loadOutLevel
                onToggled: {
                    screen.loadOutLevel = checked
                    App.saveSetting("patch_load_out_level", checked ? "true" : "false")
                }
                Component.onCompleted: outLevelId = Synth.paramIdForName("out.level")
                Connections {
                    target: Synth
                    function onParamsDiscovered() {
                        outLevelSwitch.outLevelId = Synth.paramIdForName("out.level")
                    }
                }
                id: outLevelSwitch
                ToolTip.visible: hovered
                ToolTip.text: t.t("Also set the analogue output level a patch was saved with. Off by default: it is set once by ear for what is plugged into the jack.")
            }
        }
"""
assert old in s
s = s.replace(old, new, 1)

old = """                        enabled: Synth.connected && Synth.ready
                        onClicked: Synth.loadPatch(modelData.id)"""
new = """                        enabled: Synth.connected && Synth.ready
                        onClicked: Synth.loadPatch(modelData.id, screen.loadMasterVolume,
                                                   screen.loadOutLevel)"""
assert old in s
s = s.replace(old, new, 1)

open(p, 'w', encoding='utf-8', newline='').write(s)
print("ok")
