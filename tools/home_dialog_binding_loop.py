#!/usr/bin/env python3
"""Both dialogs on HomeScreen warn "Binding loop detected for property
implicitHeight" (Material/Dialog.qml:18).

A wrapping Label used *directly* as a Dialog's contentItem closes a ring: the
dialog takes its implicitHeight from implicitContentHeight, which is the
label's implicitHeight, which a wrapping Text decides from its width -- and the
width is the one the dialog hands down from availableWidth, assigned by
Control::resizeContent() from inside that same evaluation.

The two dialogs in this app that never warned (PlockDialog, SeqSetDialog) both
put their wrapping labels inside a ColumnLayout with Layout.fillWidth. A Layout
re-arranges on its own polish pass rather than re-entering the binding, which
breaks the ring. Same shape here.

resetDialog had this from the start; restartDialog inherited it when the USB
card moved over from the osynth page.
"""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

NOTE = """        // The ColumnLayout is not here for the layout — it is one label. A
        // wrapping Label used directly as a Dialog's contentItem closes a
        // ring: the dialog measures its implicitHeight from the label, and the
        // label decides its height from the width the dialog hands down inside
        // that same evaluation. Qt reported a binding loop on implicitHeight
        // for both dialogs on this page. A Layout re-arranges on its own
        // polish pass instead, which is why PlockDialog and SeqSetDialog —
        // wrapping labels in a ColumnLayout, same style, same build — never
        // warned.
"""

changes = [
    ("app_osyntho/qml/HomeScreen.qml",
     """        contentItem: Label {
            wrapMode: Text.WordWrap
            color: Material.foreground
            // Says exactly what goes and what stays. The firmware draws the
            // same line: the working state is the patch, the graph and the
            // sequencer; the NVS settings and the looper are not in it.
            text: Tr.t("Every sound setting goes back to its default, and the sequencer patterns and the modular patch are cleared. Your saved presets, your local presets, the looper and the volume and input settings are left alone.")
        }""",

     NOTE +
     """        contentItem: ColumnLayout {
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Material.foreground
                // Says exactly what goes and what stays. The firmware draws
                // the same line: the working state is the patch, the graph and
                // the sequencer; the NVS settings and the looper are not in it.
                text: Tr.t("Every sound setting goes back to its default, and the sequencer patterns and the modular patch are cleared. Your saved presets, your local presets, the looper and the volume and input settings are left alone.")
            }
        }"""),

    ("app_osyntho/qml/HomeScreen.qml",
     """        contentItem: Label {
            wrapMode: Text.WordWrap
            color: Material.foreground
            // Presets, sequencer patterns and the looper all persist; live
            // notes and an unsaved take do not, and that is the part worth
            // saying out loud before the box goes away.
            text: Tr.t("The synth will restart to change its USB role. Audio stops and the app reconnects on its own. Saved presets, patterns and loops are kept.")
        }""",

     """        // Same reason as resetDialog above.
        contentItem: ColumnLayout {
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Material.foreground
                // Presets, sequencer patterns and the looper all persist; live
                // notes and an unsaved take do not, and that is the part worth
                // saying out loud before the box goes away.
                text: Tr.t("The synth will restart to change its USB role. Audio stops and the app reconnects on its own. Saved presets, patterns and loops are kept.")
            }
        }"""),
]

files = {}
for rel, old, new in changes:
    path = os.path.join(ROOT, rel)
    if path not in files:
        with io.open(path, encoding="utf-8") as fh:
            files[path] = fh.read()

failed = []
for rel, old, new in changes:
    path = os.path.join(ROOT, rel)
    if files[path].count(old) != 1:
        failed.append((rel, files[path].count(old), old.splitlines()[0][:72]))
        continue
    files[path] = files[path].replace(old, new, 1)

if failed:
    for rel, n, head in failed:
        print("ANCHOR x%d in %s: %s" % (n, rel, head))
    sys.exit(1)

for path, text in sorted(files.items()):
    with io.open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("patched", os.path.relpath(path, ROOT))
