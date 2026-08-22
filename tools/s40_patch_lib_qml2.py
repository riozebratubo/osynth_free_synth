p = 'app_osyntho/qml/PatchLibraryScreen.qml'
s = open(p, encoding='utf-8').read()

old = """            Label {
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
            }"""
new = """            Label {
                // No vertical anchor: a Flow positions its children, and
                // anchoring inside a positioner is refused at runtime.
                text: t.t("Load with the patch:")
                color: Material.foreground
                opacity: 0.7
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
                id: outLevelSwitch
                // out.level exists only on firmware with a codec that has an
                // output driver register (an ES8388 build). Hidden rather than
                // disabled elsewhere — a switch for a control the connected
                // synth does not have is just a puzzle. Resolved by name, like
                // the toolbar's own out.level strip: paramIdForName returns -1
                // until discovery finds it, and is not a tracked read, so the
                // signal is what re-runs it.
                property int outLevelId: -1
                visible: Synth.ready && outLevelId >= 0
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
                ToolTip.visible: hovered
                ToolTip.text: t.t("Also set the analogue output level a patch was saved with. Off by default: it is set once by ear for what is plugged into the jack.")
            }"""
assert old in s
s = s.replace(old, new, 1)
open(p, 'w', encoding='utf-8', newline='').write(s)
print("ok")
