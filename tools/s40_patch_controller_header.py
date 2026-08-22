p = 'app_osyntho/src/synthcontroller.h'
s = open(p, encoding='utf-8').read()

old = """  // --- local patch library ----------------------------------------------
  Q_INVOKABLE int saveCurrentAsPatch(const QString& name);  // snapshot live params
  Q_INVOKABLE void loadPatch(int patchId);                  // push snapshot to synth
  Q_INVOKABLE bool renamePatch(int patchId, const QString& name);
  Q_INVOKABLE bool deletePatch(int patchId);
  Q_INVOKABLE QVariantList patches(int engine = -1);  // -1 = all engines
"""
new = """  // --- local patch library ----------------------------------------------
  // A library patch is the whole synth, not only its knobs: every parameter
  // that is not an action, plus — on the modular engine — the graph the
  // parameters hang off. Without the graph a modular patch comes back applied
  // to whatever happened to be cabled up, because a node's parameter ids only
  // mean what the kind in that slot says they mean.
  Q_INVOKABLE int saveCurrentAsPatch(const QString& name);  // snapshot live params
  // The two levels are opt-in, and default to off. They are in the snapshot —
  // there is no reason to lose them — but they describe the room and the
  // wiring rather than the sound: master volume is the level you are
  // monitoring at, out.level is set once by ear for what is plugged into the
  // jack. A patch that moved either behind the player is the surprise the
  // firmware's own presets exclude them to avoid, so replaying them is the
  // Lib page's switches, not the default.
  Q_INVOKABLE void loadPatch(int patchId, bool withMasterVolume = false,
                             bool withOutLevel = false);
  Q_INVOKABLE bool renamePatch(int patchId, const QString& name);
  Q_INVOKABLE bool deletePatch(int patchId);
  Q_INVOKABLE QVariantList patches(int engine = -1);  // -1 = all engines
"""
assert old in s
s = s.replace(old, new, 1)

old = """  // --- patch interchange (JSON, format version 1) -------------------------
  // Parameters travel by NAME ("flt.cutoff"), because ids are registration
  // order and shift between engines and firmware builds. The id is written
  // alongside as a fallback for files exported while disconnected, when the
  // stored patch's names cannot be resolved.
  static constexpr int kPatchJsonVersion = 1;"""
new = """  // --- patch interchange (JSON, format version 1) -------------------------
  // Parameters travel by NAME ("flt.cutoff"), because ids are registration
  // order and shift between engines and firmware builds. The id is written
  // alongside as a fallback for files exported while disconnected, when the
  // stored patch's names cannot be resolved.
  //
  // A modular patch also carries a "graph" array. Deliberately not a version
  // bump: it is an added key, files without one still read exactly as before,
  // and a build that does not know about it ignores it — which is the whole
  // reason to add a key rather than change a shape. Bumping the version would
  // instead make every older app refuse the file outright (see the version
  // check in importPatchJson).
  static constexpr int kPatchJsonVersion = 1;"""
assert old in s
s = s.replace(old, new, 1)

old = """  // A patch load may need to switch engine first; the snapshot waits here.
  QList<QPair<int, double>> m_pendingPatchParams;
  void pushParams(const QList<QPair<int, double>>& params);
"""
new = """  // A patch load may need to switch engine first; the snapshot waits here.
  QList<QPair<int, double>> m_pendingPatchParams;
  void pushParams(const QList<QPair<int, double>>& params);

  // --- modular graph, as a library patch stores it ------------------------
  // JSON array of { kind, in: [...], x, y }, index = slot. Empty when the
  // graph is unavailable (no modular engine on this firmware) or the live
  // engine is not Modular.
  QString graphJson() const;
  static QList<SynthProto::GraphNode> graphFromJson(const QString& text);
  // Pushes a whole graph. One GRAPH_EDIT with the model blob where the
  // firmware supports it; a replay of per-node edits where it does not, which
  // is slower and audibly steps through the intermediate patches — hence the
  // preference. Empty list = nothing to do.
  void pushGraph(const QList<SynthProto::GraphNode>& nodes);
  void pushGraphNodeByNode(const QList<SynthProto::GraphNode>& nodes);
  // Set while a whole-model push is in flight, so the ST_BAD_ARG that older
  // firmware answers sub-op 3 with can be told from a rejected ordinary edit
  // and turned into the fallback.
  QList<SynthProto::GraphNode> m_pendingGraphPush;
  bool m_graphLoadModelInFlight = false;
  // False once firmware has refused sub-op 3, so the rest of the session goes
  // straight to the per-node path instead of paying for the probe every time.
  bool m_graphLoadModelSupported = true;
"""
assert old in s
s = s.replace(old, new, 1)

open(p, 'w', encoding='utf-8', newline='').write(s)
print("ok")
