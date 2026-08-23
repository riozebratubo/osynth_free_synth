#!/usr/bin/env python3
"""One-shot source patch for the preset-identity UI pass:

  * UI.presetLabel - one "slot - name" form for the toolbar and the Home page.
  * Toolbar subtitle drops the BLE device name for engine + preset.
  * The Presets page outlines the slot the synth is on.
  * The patch library page becomes "Local presets" ("Loc. Pre" in the dock)
    and outlines the stored patch the live sound came from.
  * SynthController gains libraryPatchId, the app-side state the mark needs.

Every replacement is anchored on an exact, unique snippet; the script writes
nothing at all if any anchor is missing or already patched.
"""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

changes = []   # (path, old, new)


def sub(path, old, new):
    changes.append((os.path.join(ROOT, path), old, new))


# ------------------------------------------------------------------ UI.qml
sub("app_osyntho/qml/UI.qml",
    '        { label: "Lib",  name: "Patch library",      icon: "\\uf02d" },  // book',
    '        { label: "Loc. Pre", name: "Local presets",  icon: "\\uf02d" },  // book')

sub("app_osyntho/qml/UI.qml",
    "    property bool desktopLayout: !portrait\n",
    """    property bool desktopLayout: !portrait

    // The slot the synth is on, in the one form every surface names it in:
    // "5 - grain choir", carrying the same 0-47 / 48-111 number the Presets
    // page tiles are labelled with. Empty - not "-1", not a placeholder -
    // until the synth has reported a slot and that engine's listing has
    // supplied its name, so a caller can drop the whole clause rather than
    // print a stand-in.
    readonly property string presetLabel:
        Synth.presetSlot >= 0
            ? (Synth.presetSlot
               + (Synth.presetName.length > 0 ? (" - " + Synth.presetName) : ""))
            : ""
""")

# ---------------------------------------------------------------- Main.qml
sub("app_osyntho/qml/Main.qml",
    '                subtitle: Synth.connected\n'
    '                    ? (BluetoothManager.deviceName + (Synth.ready ? (" · " + Tr.t(Synth.engineName)) : (" · " + Tr.t("connecting…"))))\n'
    '                    : Tr.t("Not connected")',

    '                // Engine and preset, not the device name: which of your\n'
    '                // synths you are on is what the plug icon and the device\n'
    '                // picker are for, while *what the sound is* - engine, slot,\n'
    '                // name - is the thing you look up mid-session, and it was\n'
    '                // otherwise a page away on Home.\n'
    '                subtitle: Synth.connected\n'
    '                    ? (Synth.ready\n'
    '                       ? (Tr.t(Synth.engineName)\n'
    '                          + (UI.presetLabel.length > 0 ? (" · " + UI.presetLabel) : ""))\n'
    '                       : Tr.t("connecting…"))\n'
    '                    : Tr.t("Not connected")')

# ----------------------------------------------------------- HomeScreen.qml
sub("app_osyntho/qml/HomeScreen.qml",
    '                        text: Synth.presetSlot >= 0\n'
    '                            ? (Synth.presetSlot + (Synth.presetName ? (" · " + Synth.presetName) : "")\n'
    '                               + (Synth.presetIsFactory ? ("  (" + Tr.t("factory") + ")") : ""))\n'
    '                            : "—"',

    '                        // The same "slot - name" the toolbar shows and the\n'
    '                        // Presets page tiles are numbered with - one preset,\n'
    '                        // one way of naming it.\n'
    '                        text: UI.presetLabel.length > 0\n'
    '                            ? (UI.presetLabel\n'
    '                               + (Synth.presetIsFactory ? ("  (" + Tr.t("factory") + ")") : ""))\n'
    '                            : "—"')

# -------------------------------------------------------- PresetsScreen.qml
sub("app_osyntho/qml/PresetsScreen.qml",
    """                ItemDelegate {
                    anchors.fill: parent
                    anchors.margins: 3
                    padding: 8
                    highlighted: tile.modelData.slot === Synth.presetSlot
                    onClicked: Synth.loadPreset(screen.engine, tile.modelData.slot)""",

    """                ItemDelegate {
                    id: tileButton
                    anchors.fill: parent
                    anchors.margins: 3
                    padding: 8
                    // The slot the synth is actually sitting on. `highlighted`
                    // alone is a tint you have to hunt for across 112 tiles, so
                    // the same state also draws the outline below and colours
                    // the slot number.
                    readonly property bool current: tile.modelData.slot === Synth.presetSlot
                    highlighted: current
                    onClicked: Synth.loadPreset(screen.engine, tile.modelData.slot)""")

sub("app_osyntho/qml/PresetsScreen.qml",
    """                    contentItem: ColumnLayout {
                        spacing: 2
""",
    """                    // Declared *before* contentItem so it lands between the
                    // delegate's background and its labels: the fill would wash
                    // out text drawn under it, and replacing the background
                    // outright would take the press ripple with it. No input
                    // handlers, so the tap still reaches the delegate.
                    Rectangle {
                        anchors.fill: parent
                        radius: 4
                        color: Qt.rgba(Material.accent.r, Material.accent.g,
                                       Material.accent.b, 0.16)
                        border.width: 2
                        border.color: Material.accent
                        visible: tileButton.current
                    }

                    contentItem: ColumnLayout {
                        spacing: 2
""")

sub("app_osyntho/qml/PresetsScreen.qml",
    """                            Label {
                                Layout.fillWidth: true
                                text: tile.modelData.slot
                                font.pointSize: UI.fontSize * 0.8
                                color: Material.foreground
                                opacity: 0.7
                            }""",

    """                            Label {
                                Layout.fillWidth: true
                                text: tile.modelData.slot
                                font.pointSize: UI.fontSize * 0.8
                                font.bold: tileButton.current
                                color: tileButton.current ? Material.accent
                                                          : Material.foreground
                                opacity: tileButton.current ? 1.0 : 0.7
                            }""")

# --------------------------------------------------- PatchLibraryScreen.qml
sub("app_osyntho/qml/PatchLibraryScreen.qml",
    '                text: Tr.t("Patch library")',
    '                text: Tr.t("Local presets")')

sub("app_osyntho/qml/PatchLibraryScreen.qml",
    """            delegate: ItemDelegate {
                required property var modelData
                width: ListView.view.width

                contentItem: RowLayout {""",

    """            delegate: ItemDelegate {
                id: patchRow
                required property var modelData
                width: ListView.view.width
                // The stored patch the live sound came from, marked the way the
                // Presets page marks the synth's own slot. Nothing on the synth
                // records this - see SynthController::libraryPatchId - so it is
                // dropped as soon as anything else claims the sound.
                readonly property bool current: modelData.id === Synth.libraryPatchId
                highlighted: current

                // Before contentItem for the same reason as the marker on the
                // Presets page tiles.
                Rectangle {
                    anchors.fill: parent
                    radius: 4
                    color: Qt.rgba(Material.accent.r, Material.accent.g,
                                   Material.accent.b, 0.16)
                    border.width: 2
                    border.color: Material.accent
                    visible: patchRow.current
                }

                contentItem: RowLayout {""")

sub("app_osyntho/qml/PatchLibraryScreen.qml",
    """                        Label {
                            text: modelData.name && modelData.name.length ? modelData.name : Tr.t("(unnamed)")
                            color: Material.foreground
                            font.bold: true
                        }""",

    """                        Label {
                            text: modelData.name && modelData.name.length ? modelData.name : Tr.t("(unnamed)")
                            color: patchRow.current ? Material.accent : Material.foreground
                            font.bold: true
                        }""")

# ------------------------------------------------------- synthcontroller.h
sub("app_osyntho/src/synthcontroller.h",
    "  Q_PROPERTY(bool presetIsFactory READ presetIsFactory NOTIFY presetChanged FINAL)\n",

    """  Q_PROPERTY(bool presetIsFactory READ presetIsFactory NOTIFY presetChanged FINAL)

  // Which row of the app's own patch library the live sound came from, or -1.
  // App-side state by necessity: the firmware knows nothing about the library,
  // so nothing but this can say a stored patch is what is playing - and that is
  // exactly why it has to be dropped the moment something else claims the sound
  // (a preset load, an engine change that is not this patch's own, a fresh
  // connection, or the row being deleted).
  Q_PROPERTY(int libraryPatchId READ libraryPatchId NOTIFY libraryPatchChanged FINAL)
""")

sub("app_osyntho/src/synthcontroller.h",
    "  bool presetIsFactory() const { return m_presetIsFactory; }\n",
    "  bool presetIsFactory() const { return m_presetIsFactory; }\n"
    "  int libraryPatchId() const { return m_libraryPatchId; }\n")

sub("app_osyntho/src/synthcontroller.h",
    "  void presetChanged();\n",
    "  void presetChanged();\n  void libraryPatchChanged();\n")

sub("app_osyntho/src/synthcontroller.h",
    "  void updatePresetFromSlot();\n",
    """  void updatePresetFromSlot();
  // -1 clears the mark. `engine` is the engine the patch was stored for, and is
  // what lets setEngineCaps() tell loadPatch()'s own engine switch apart from
  // the player picking a different engine.
  void setLibraryPatch(int patchId, int engine);
""")

sub("app_osyntho/src/synthcontroller.h",
    """  int m_presetSlot = -1;
  QString m_presetName;
  bool m_presetIsFactory = false;
""",
    """  int m_presetSlot = -1;
  QString m_presetName;
  bool m_presetIsFactory = false;

  int m_libraryPatchId = -1;
  int m_libraryPatchEngine = -1;
""")

# ----------------------------------------------------- synthcontroller.cpp
sub("app_osyntho/src/synthcontroller.cpp",
    """  m_presetSlot = -1;
  m_presetName.clear();
  m_presetIsFactory = false;
  emit readyChanged();""",

    """  m_presetSlot = -1;
  m_presetName.clear();
  m_presetIsFactory = false;
  setLibraryPatch(-1, -1);
  emit readyChanged();""")

sub("app_osyntho/src/synthcontroller.cpp",
    """    const int slot = int(std::lround(value));
    if (slot != m_presetSlot) {
      m_presetSlot = slot;
      updatePresetFromSlot();
      emit presetChanged();
    }""",

    """    const int slot = int(std::lround(value));
    if (slot != m_presetSlot) {
      m_presetSlot = slot;
      updatePresetFromSlot();
      emit presetChanged();
      // A firmware slot is what is playing now, so a library patch is not.
      setLibraryPatch(-1, -1);
    }""")

sub("app_osyntho/src/synthcontroller.cpp",
    """  const bool engineChangedNow = (int(engine) != m_engine);
  if (engineChangedNow) {
""",
    """  const bool engineChangedNow = (int(engine) != m_engine);
  if (engineChangedNow) {
    // loadPatch() switches engines itself when the stored patch belongs to
    // another one, and that switch must not clear the mark it has just set -
    // which is the whole reason the patch's engine is remembered beside its id.
    if (m_libraryPatchEngine != int(engine)) setLibraryPatch(-1, -1);
""")

sub("app_osyntho/src/synthcontroller.cpp",
    "QVariantList SynthController::presetsFor(int engine) const {",

    """void SynthController::setLibraryPatch(int patchId, int engine) {
  m_libraryPatchEngine = engine;
  if (patchId == m_libraryPatchId) return;
  m_libraryPatchId = patchId;
  emit libraryPatchChanged();
}

QVariantList SynthController::presetsFor(int engine) const {""")

sub("app_osyntho/src/synthcontroller.cpp",
    """  const int id = db().insertPatch(name, m_engine, params, graphJson());
  if (id <= 0) emit showError(Translator::instance().t("Could not save the patch."));
  return id;""",

    """  const int id = db().insertPatch(name, m_engine, params, graphJson());
  if (id <= 0) {
    emit showError(Translator::instance().t("Could not save the patch."));
    return id;
  }
  // The row *is* the live sound - it was just taken from it - so the library
  // page marks it straight away, without a round trip through loadPatch().
  setLibraryPatch(id, m_engine);
  return id;""")

sub("app_osyntho/src/synthcontroller.cpp",
    "  const QList<GraphNode> graph = graphFromJson(db().getPatchGraph(patchId));\n",

    """  const QList<GraphNode> graph = graphFromJson(db().getPatchGraph(patchId));

  // Set before the engine switch below, so the EVT_ENGINE that switch provokes
  // finds the patch's own engine here and leaves the mark alone.
  setLibraryPatch(patchId, patchEngine);
""")

sub("app_osyntho/src/synthcontroller.cpp",
    "bool SynthController::deletePatch(int patchId) { return db().deletePatch(patchId); }",

    """bool SynthController::deletePatch(int patchId) {
  // Nothing left to mark once the row is gone.
  if (patchId == m_libraryPatchId) setLibraryPatch(-1, -1);
  return db().deletePatch(patchId);
}""")

# ---------------------------------------------------------- translator.cpp
sub("app_osyntho/src/translator.cpp",
    '  pt["Patch library"] = "Biblioteca de patches";',

    '  pt["Patch library"] = "Biblioteca de patches";\n'
    '  // The page title and its full name in the startup-screen picker. The nav\n'
    '  // dock\'s short "Loc. Pre" reads the same in pt_BR and is left to fall back.\n'
    '  pt["Local presets"] = "Presets locais";')


# ------------------------------------------------------------------- apply
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
