#!/usr/bin/env python3
"""preset.load's value is a LINEAR slot number.

The firmware registers preset.load (0x0002) over 0 .. SYNTH_ENGINE_COUNT *
PRESETS_PER_ENGINE - 1 and rests it on `engine * 112 + slot` (presets.cpp,
do_load).  Everything else in the protocol -- OP_LOAD_PRESET, OP_SAVE_PRESET,
OP_LIST_PRESETS -- and the whole Presets page speak the per-engine 0-111 number
instead (0-47 factory, 48-111 user).

The app read the linear value as if it were the per-engine slot, so:

  * Home printed "565" for Granular slot 5;
  * updatePresetFromSlot() looked 565 up in a list numbered 0-111, found
    nothing, and left presetName empty for every engine but Subtractive;
  * the Presets page tile test (slot === Synth.presetSlot) never matched, so
    nothing was ever marked as current.

This splits the linear value into engine + slot at the one place it enters the
app, and makes presetSlot() report -1 while the slot belongs to another engine
-- a plain engine switch does not move preset.load, so the number would
otherwise name a preset the live engine does not have.

Exact unique anchors, all-or-nothing write, as with tools/ui_preset_marks.py.
"""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

changes = []


def sub(path, old, new):
    changes.append((os.path.join(ROOT, path), old, new))


# ----------------------------------------------------- synthcontroller.cpp
sub("app_osyntho/src/synthcontroller.cpp",
    "// Well-known ids the controller tracks directly.\nconstexpr quint16 ID_PRESET_LOAD = 0x0002;\n",

    "// Well-known ids the controller tracks directly.\n"
    "constexpr quint16 ID_PRESET_LOAD = 0x0002;\n"
    "// preset.load's *value* is a linear slot — engine * PRESETS_PER_ENGINE +\n"
    "// slot — while OP_LOAD_PRESET, OP_LIST_PRESETS and every screen speak the\n"
    "// per-engine 0-111 number (0-47 factory, 48-111 user). Splitting it on the\n"
    "// way in is what lets presetSlot() mean the same thing everywhere: read raw,\n"
    "// it printed 565 for Granular's slot 5, matched no entry in a list numbered\n"
    "// 0-111 — so presetName stayed empty on every engine but the first — and no\n"
    "// preset tile ever recognised itself as the current one.\n"
    "constexpr int kPresetsPerEngine = 112;  // PRESETS_PER_ENGINE (presets.h)\n")

sub("app_osyntho/src/synthcontroller.cpp",
    """  if (id == ID_PRESET_LOAD) {
    const int slot = int(std::lround(value));
    if (slot != m_presetSlot) {
      m_presetSlot = slot;
      updatePresetFromSlot();
      emit presetChanged();
      // A firmware slot is what is playing now, so a library patch is not.
      setLibraryPatch(-1, -1);
    }
  } else if (id == ID_GRAPH_REV) {""",

    """  if (id == ID_PRESET_LOAD) {
    const int linear = int(std::lround(value));
    const int eng = (linear >= 0) ? linear / kPresetsPerEngine : -1;
    const int slot = (linear >= 0) ? linear % kPresetsPerEngine : -1;
    if (slot != m_presetSlot || eng != m_presetEngine) {
      m_presetSlot = slot;
      m_presetEngine = eng;
      updatePresetFromSlot();
      emit presetChanged();
      // A firmware slot is what is playing now, so a library patch is not.
      // Guarded by the change test above on purpose: discovery's value sweep
      // re-reads this id, and an unconditional clear there would drop the mark
      // a library patch had just set.
      setLibraryPatch(-1, -1);
    }
  } else if (id == ID_GRAPH_REV) {""")

sub("app_osyntho/src/synthcontroller.cpp",
    """void SynthController::updatePresetFromSlot() {
  m_presetName.clear();
  m_presetIsFactory = false;
  const auto it = m_presets.constFind(m_engine);
  if (it == m_presets.constEnd()) return;
  for (const PresetEntry& e : it.value()) {
    if (int(e.slot) == m_presetSlot) {""",

    """void SynthController::updatePresetFromSlot() {
  m_presetName.clear();
  m_presetIsFactory = false;
  // presetSlot(), not m_presetSlot: a slot belonging to another engine is not
  // this engine's current preset and must not borrow a name from its list.
  const int slot = presetSlot();
  if (slot < 0) return;
  const auto it = m_presets.constFind(m_engine);
  if (it == m_presets.constEnd()) return;
  for (const PresetEntry& e : it.value()) {
    if (int(e.slot) == slot) {""")

sub("app_osyntho/src/synthcontroller.cpp",
    """  m_presetSlot = -1;
  m_presetName.clear();
  m_presetIsFactory = false;
  setLibraryPatch(-1, -1);""",

    """  m_presetSlot = -1;
  m_presetEngine = -1;
  m_presetName.clear();
  m_presetIsFactory = false;
  setLibraryPatch(-1, -1);""")

# ------------------------------------------------------- synthcontroller.h
sub("app_osyntho/src/synthcontroller.h",
    "  int presetSlot() const { return m_presetSlot; }\n",

    "  // The per-engine 0-111 slot the synth is on, or -1 — including whenever\n"
    "  // the slot preset.load rests on belongs to a different engine. It rests on\n"
    "  // the last preset *loaded*, and a plain engine switch does not move it, so\n"
    "  // the raw number would otherwise name a preset the live engine has not got.\n"
    "  int presetSlot() const {\n"
    "    return (m_presetEngine == m_engine) ? m_presetSlot : -1;\n"
    "  }\n")

sub("app_osyntho/src/synthcontroller.h",
    """  int m_presetSlot = -1;
  QString m_presetName;
  bool m_presetIsFactory = false;
""",
    """  int m_presetSlot = -1;
  // The engine half of preset.load's linear value; -1 until one is reported.
  // Kept apart from m_engine so presetSlot() can tell "no preset on this
  // engine" from "slot 0".
  int m_presetEngine = -1;
  QString m_presetName;
  bool m_presetIsFactory = false;
""")


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
