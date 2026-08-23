#!/usr/bin/env python3
"""Follow-up to tools/ui_preset_marks.py:

  * qualify the two new `highlighted:` bindings (the repo is mid-way through
    removing unqualified lookups for the QML AOT pass -- tools/qml_unqualified.py)
  * retire the now-dead pt["Patch library"] entry and its neighbouring note
  * carry the rename into the one user-facing sentence that still said
    "the patch library" (the reset dialog on Home), pt_BR included

Same rules as the first script: exact unique anchors, all-or-nothing write.
"""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

changes = []


def sub(path, old, new):
    changes.append((os.path.join(ROOT, path), old, new))


sub("app_osyntho/qml/PresetsScreen.qml",
    "                    readonly property bool current: tile.modelData.slot === Synth.presetSlot\n"
    "                    highlighted: current\n",
    "                    readonly property bool current: tile.modelData.slot === Synth.presetSlot\n"
    "                    highlighted: tileButton.current\n")

sub("app_osyntho/qml/PatchLibraryScreen.qml",
    "                readonly property bool current: modelData.id === Synth.libraryPatchId\n"
    "                highlighted: current\n",
    "                readonly property bool current: modelData.id === Synth.libraryPatchId\n"
    "                highlighted: patchRow.current\n")

sub("app_osyntho/qml/HomeScreen.qml",
    'text: Tr.t("Every sound setting goes back to its default, and the sequencer patterns and the modular patch are cleared. Your saved presets, the patch library, the looper and the volume and input settings are left alone.")',
    'text: Tr.t("Every sound setting goes back to its default, and the sequencer patterns and the modular patch are cleared. Your saved presets, your local presets, the looper and the volume and input settings are left alone.")')

sub("app_osyntho/src/translator.cpp",
    '  pt["Every sound setting goes back to its default, and the sequencer patterns and the modular patch are cleared. Your saved presets, the patch library, the looper and the volume and input settings are left alone."] =\n'
    '      "Todos os ajustes de som voltam ao padrão, e os padrões do sequenciador e o patch "\n'
    '      "modular são apagados. Seus presets salvos, a biblioteca de patches, o looper e "\n'
    '      "os ajustes de volume e de entrada não são alterados.";',

    '  pt["Every sound setting goes back to its default, and the sequencer patterns and the modular patch are cleared. Your saved presets, your local presets, the looper and the volume and input settings are left alone."] =\n'
    '      "Todos os ajustes de som voltam ao padrão, e os padrões do sequenciador e o patch "\n'
    '      "modular são apagados. Seus presets salvos, seus presets locais, o looper e "\n'
    '      "os ajustes de volume e de entrada não são alterados.";')

sub("app_osyntho/src/translator.cpp",
    '  pt["Patch library"] = "Biblioteca de patches";\n'
    '  // The page title and its full name in the startup-screen picker. The nav\n'
    "  // dock's short \"Loc. Pre\" reads the same in pt_BR and is left to fall back.\n"
    '  pt["Local presets"] = "Presets locais";',

    '  // The page title and its full name in the startup-screen picker (S?: the\n'
    '  // page used to be "Patch library"). The nav dock\'s short "Loc. Pre" reads\n'
    '  // the same in pt_BR and is left to fall back.\n'
    '  pt["Local presets"] = "Presets locais";')

sub("app_osyntho/src/translator.cpp",
    '  // Full page names for the picker above (UI.screens). "Home", "Sequencer",\n'
    '  // "Arpeggiator" and "Patch library" are already listed elsewhere; "Looper"\n'
    '  // and "Presets" read the same in pt_BR and are left to fall back.',

    '  // Full page names for the picker above (UI.screens). "Home", "Sequencer",\n'
    '  // "Arpeggiator" and "Local presets" are already listed elsewhere; "Looper"\n'
    '  // and "Presets" read the same in pt_BR and are left to fall back.')


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
