#!/usr/bin/env python3
"""Phase 2b: give the array-valued properties real sequence types.

"Cannot generate efficient code for storing an array in a non-sequence type" is
qmlcachegen refusing to write a list into a `var`. The fix is to say what the
list holds -- `list<int>`, `list<string>`, `list<color>` -- so the store has a
known type and the element reads that follow are typed too.

Two kinds are handled:

  * literal arrays declared in QML, which only needed a better declaration
  * SynthController::paramIds() / paramIdsByPrefix(), which returned
    QVariantList and now return QList<int>; ParamGroup.ids becomes list<int>

Not touched:

  * Keyboard.blackDefs -- an array of pairs. QML has no `list<list<int>>`, and
    flattening it would make whiteMidi/noteForPos harder to read for one entry.
  * Repeater `model: [1, 2, 4]` literals -- Repeater.model is a QVariant by
    definition, so there is no better type to store into.
  * UI.screens -- an array of {label, name, icon} objects. That wants a real
    gadget, which is its own slice.

ParamGroup.refresh()'s element-wise comparison is deliberately left exactly as
it is: its comment explains that an unguarded assignment rebuilds every
ParamControl in the card up to ~7x/s during a discovery pass. Retyping the
property does not remove that need.

Idempotent. --check reports without writing.
"""
import argparse
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "app_osyntho")
QML = os.path.join(APP, "qml")

ARGS = None

QML_EDITS = {
    "Keyboard.qml": [
        ("readonly property var whiteSemis: [0, 2, 4, 5, 7, 9, 11]",
         "readonly property list<int> whiteSemis: [0, 2, 4, 5, 7, 9, 11]"),
        ('readonly property var semiNames: ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]',
         'readonly property list<string> semiNames: ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]'),
    ],
    "PatchLibraryScreen.qml": [
        ('readonly property var engineNames: ["Subtractive", "Additive", "FM", "Wavetable",',
         'readonly property list<string> engineNames: ["Subtractive", "Additive", "FM", "Wavetable",'),
    ],
    "SettingsScreen.qml": [
        ("readonly property var swatchColors: [",
         "readonly property list<color> swatchColors: ["),
    ],
    "EnumSelector.qml": [
        # enumNames is a typed QStringList on ParamMeta now, and a default
        # ParamMeta carries an empty one -- so both fallbacks are dead, and
        # mixing them back in is what forced the whole binding to var.
        ("model: root.meta.exists ? (root.meta.enumNames || []) : []",
         "model: root.meta.enumNames"),
    ],
    "ParamGroup.qml": [
        ("    property var ids: []",
         "    property list<int> ids"),
    ],
}

CPP_EDITS = {
    "src/synthcontroller.h": [
        ("  Q_INVOKABLE QVariantList paramIds() const;",
         "  Q_INVOKABLE QList<int> paramIds() const;"),
        ("  Q_INVOKABLE QVariantList paramIdsByPrefix(const QString& prefix) const;",
         "  Q_INVOKABLE QList<int> paramIdsByPrefix(const QString& prefix) const;"),
    ],
    "src/synthcontroller.cpp": [
        ("QVariantList SynthController::paramIds() const {\n"
         "  QVariantList out;\n"
         "  for (quint16 id : m_paramOrder) out.append(id);\n"
         "  return out;\n"
         "}",
         "QList<int> SynthController::paramIds() const {\n"
         "  QList<int> out;\n"
         "  out.reserve(m_paramOrder.size());\n"
         "  for (quint16 id : m_paramOrder) out.append(id);\n"
         "  return out;\n"
         "}"),
        ("QVariantList SynthController::paramIdsByPrefix(const QString& prefix) const {\n"
         "  QVariantList out;",
         "QList<int> SynthController::paramIdsByPrefix(const QString& prefix) const {\n"
         "  QList<int> out;"),
    ],
}


def apply(path, pairs, label):
    text = io.open(path, encoding="utf-8").read()
    before = text
    applied = 0
    for old, new in pairs:
        # Insertion pairs (old is a prefix of new) leave `old` in place, so
        # testing it first would re-apply on every run. Check the result first.
        if len(new) > len(old) and new in text:
            continue
        if text.count(old) == 1:
            text = text.replace(old, new, 1)
            applied += 1
        elif new in text:
            continue
        else:
            sys.exit("{}: could not match\n---\n{}\n---".format(label, old))
    if text != before:
        print("  {:<28} {} edits".format(label, applied))
        if not ARGS.check:
            io.open(path, "w", encoding="utf-8", newline="").write(text)
        return 1
    return 0


def main():
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    ARGS = parser.parse_args()

    changed = 0
    for name, pairs in sorted(QML_EDITS.items()):
        changed += apply(os.path.join(QML, name), pairs, name)
    for rel, pairs in sorted(CPP_EDITS.items()):
        changed += apply(os.path.join(APP, rel), pairs, rel)
    print("{} {} files".format("would change" if ARGS.check else "changed", changed))


if __name__ == "__main__":
    main()
