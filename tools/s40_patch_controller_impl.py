p = 'app_osyntho/src/synthcontroller.cpp'
s = open(p, encoding='utf-8').read()

# ---- saveCurrentAsPatch: capture the graph ------------------------------
old = """  if (params.isEmpty()) {
    emit showError(Translator::instance().t("Nothing to save yet — no parameters have been read."));
    return 0;
  }
  const int id = db().insertPatch(name, m_engine, params);
  if (id <= 0) emit showError(Translator::instance().t("Could not save the patch."));
  return id;
}"""
new = """  if (params.isEmpty()) {
    emit showError(Translator::instance().t("Nothing to save yet — no parameters have been read."));
    return 0;
  }
  const int id = db().insertPatch(name, m_engine, params, graphJson());
  if (id <= 0) emit showError(Translator::instance().t("Could not save the patch."));
  return id;
}

/* ------------------------------------------ the graph a patch is built on */

// The live model as a library patch stores it. Empty for every engine but
// Modular: the other four have no graph, and writing "[]" for them would be a
// stored claim that their graph is empty rather than absent.
QString SynthController::graphJson() const {
  if (!m_graphAvailable || m_engine != m_graphEngineIndex) return QString();
  if (m_graphNodes.isEmpty()) return QString();
  QJsonArray arr;
  for (const QVariant& v : m_graphNodes) {
    const QVariantMap n = v.toMap();
    QJsonObject o;
    o["kind"] = n.value("kind").toInt();
    QJsonArray ins;
    for (const QVariant& src : n.value("in").toList()) ins.append(src.toInt());
    o["in"] = ins;
    o["x"] = n.value("x").toInt();
    o["y"] = n.value("y").toInt();
    arr.append(o);
  }
  return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QList<GraphNode> SynthController::graphFromJson(const QString& text) {
  QList<GraphNode> out;
  if (text.isEmpty()) return out;
  const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
  if (!doc.isArray()) return out;
  for (const QJsonValue& v : doc.array()) {
    const QJsonObject o = v.toObject();
    GraphNode n;
    n.kind = o.value("kind").toInt();
    for (const QJsonValue& src : o.value("in").toArray()) n.in.append(src.toInt(-1));
    n.x = o.value("x").toInt();
    n.y = o.value("y").toInt();
    out.append(n);
  }
  return out;
}

// One edit for the whole model, so the synth compiles and cost-checks it once
// and never renders the half-built graphs in between. The per-node replay
// below is the fallback for firmware that has no such sub-op.
void SynthController::pushGraph(const QList<GraphNode>& nodes) {
  if (!m_graphAvailable || nodes.isEmpty()) return;
  if (!m_graphLoadModelSupported) {
    pushGraphNodeByNode(nodes);
    return;
  }
  m_pendingGraphPush = nodes;
  m_graphLoadModelInFlight = true;
  send(OP_GRAPH_EDIT, payloadGraphLoadModel(nodes, m_graphMaxNodes), true);
}

// Kinds first, then cables, then positions — the order the edit API demands:
// connect() refuses a port on a slot whose kind does not have it, and set_kind
// drops every cable touching the slot it changes, so a cable laid before both
// of its ends exist would be thrown away by the node that arrived later.
//
// Audible, and that is why it is not the first choice: each set_kind is its
// own recompile with its own duck, and the patch passes through every
// intermediate shape on the way.
void SynthController::pushGraphNodeByNode(const QList<GraphNode>& nodes) {
  // Not `slots`: Qt defines that as a keyword macro, so a local by that name
  // vanishes at preprocessing and the loop below stops parsing.
  const int slotCount = qMin(nodes.size(), m_graphMaxNodes);
  for (int i = 0; i < slotCount; ++i) {
    // The Out slot is structural and the firmware refuses to change it; asking
    // would earn a BAD_ARG and a red banner for a slot that is already right.
    if (i == m_graphOutSlot) continue;
    send(OP_GRAPH_EDIT, payloadGraphSetKind(i, nodes.at(i).kind), true);
  }
  for (int i = 0; i < slotCount; ++i) {
    const GraphNode& n = nodes.at(i);
    for (int port = 0; port < n.in.size() && port < m_graphMaxInputs; ++port) {
      if (n.in.at(port) < 0) continue;  // unpatched is the state after set_kind
      send(OP_GRAPH_EDIT, payloadGraphConnect(i, port, n.in.at(port)), true);
    }
  }
  for (int i = 0; i < slotCount; ++i) {
    send(OP_GRAPH_EDIT, payloadGraphSetPos(i, nodes.at(i).x, nodes.at(i).y), false);
  }
  refreshGraphModel();
}"""
assert old in s
s = s.replace(old, new, 1)

# ---- pushParams: the two levels are opt-in ------------------------------
old = """void SynthController::loadPatch(int patchId) {
  const QList<QPair<int, double>> params = db().getPatchParams(patchId);
  if (params.isEmpty()) return;

  // Find the patch's engine so we switch first if it differs from the live one.
  int patchEngine = m_engine;
  const QVariantList rows = db().getPatches(-1);
  for (const QVariant& r : rows) {
    const QVariantMap m = r.toMap();
    if (m.value("id").toInt() == patchId) {
      patchEngine = m.value("engine").toInt();
      break;
    }
  }

  if (patchEngine != m_engine) {
    selectEngine(patchEngine);
    m_pendingPatchParams = params;
    QTimer::singleShot(kEngineSwitchSettleMs, this, [this]() {
      pushParams(m_pendingPatchParams);
      m_pendingPatchParams.clear();
    });
  } else {
    pushParams(params);
  }
}"""
new = """void SynthController::loadPatch(int patchId, bool withMasterVolume, bool withOutLevel) {
  QList<QPair<int, double>> params = db().getPatchParams(patchId);
  if (params.isEmpty()) return;

  // The two levels the Lib page gates. Dropped from the push rather than from
  // the stored row: the row is the whole synth and stays that way, and a
  // switch turned on later has to find the value still there.
  if (!withMasterVolume || !withOutLevel) {
    const int volId = paramIdForName(QStringLiteral("master.volume"));
    const int outId = paramIdForName(QStringLiteral("out.level"));
    for (int i = params.size() - 1; i >= 0; --i) {
      const int id = params.at(i).first;
      if (!withMasterVolume && volId >= 0 && id == volId) params.removeAt(i);
      else if (!withOutLevel && outId >= 0 && id == outId) params.removeAt(i);
    }
  }

  // Find the patch's engine so we switch first if it differs from the live one.
  int patchEngine = m_engine;
  const QVariantList rows = db().getPatches(-1);
  for (const QVariant& r : rows) {
    const QVariantMap m = r.toMap();
    if (m.value("id").toInt() == patchId) {
      patchEngine = m.value("engine").toInt();
      break;
    }
  }

  const QList<GraphNode> graph = graphFromJson(db().getPatchGraph(patchId));

  // The graph goes in before the values, and it is the whole subtlety of
  // loading a modular patch: a node parameter id only exists while its slot
  // holds a kind that defines it, so values pushed onto the old graph land on
  // the wrong controls — or on none. The firmware's own preset loader has the
  // same rule for the same reason (presets.cpp, do_load).
  if (patchEngine != m_engine) {
    selectEngine(patchEngine);
    m_pendingPatchParams = params;
    QTimer::singleShot(kEngineSwitchSettleMs, this, [this, graph]() {
      pushGraph(graph);
      // A second settle after the graph, for the same reason as the first:
      // load_model re-registers every occupied slot's parameters, and a value
      // written before that lands nowhere.
      if (!graph.isEmpty()) {
        QTimer::singleShot(kGraphSettleMs, this, [this]() {
          pushParams(m_pendingPatchParams);
          m_pendingPatchParams.clear();
        });
      } else {
        pushParams(m_pendingPatchParams);
        m_pendingPatchParams.clear();
      }
    });
  } else if (!graph.isEmpty()) {
    pushGraph(graph);
    m_pendingPatchParams = params;
    QTimer::singleShot(kGraphSettleMs, this, [this]() {
      pushParams(m_pendingPatchParams);
      m_pendingPatchParams.clear();
    });
  } else {
    pushParams(params);
  }
}"""
assert old in s
s = s.replace(old, new, 1)

# ---- settle constant ----------------------------------------------------
old = """constexpr int kEngineSwitchSettleMs = 400; // let 0x02xx re-register before a patch push"""
new = """constexpr int kEngineSwitchSettleMs = 400; // let 0x02xx re-register before a patch push
constexpr int kGraphSettleMs = 300;        // load_model re-registers node params too"""
assert old in s
s = s.replace(old, new, 1)

# ---- handleGraphEdit: fall back when sub-op 3 is refused ----------------
old = """void SynthController::handleGraphEdit(const QByteArray& payload, quint8 status) {
  const GraphEditReply e = parseGraphEditReply(payload);
  if (m_graphCost != e.cost) {
    m_graphCost = e.cost;
    emit graphCostChanged();
  }
  if (status != 0) {"""
new = """void SynthController::handleGraphEdit(const QByteArray& payload, quint8 status) {
  const GraphEditReply e = parseGraphEditReply(payload);
  if (m_graphCost != e.cost) {
    m_graphCost = e.cost;
    emit graphCostChanged();
  }
  if (m_graphLoadModelInFlight) {
    m_graphLoadModelInFlight = false;
    const QList<GraphNode> pending = m_pendingGraphPush;
    m_pendingGraphPush.clear();
    // BAD_ARG here is firmware that has no whole-model sub-op — every other
    // refusal (too expensive, a cycle) is about the patch and is reported
    // below like any other. Remembered for the session so the rest of it goes
    // straight to the slow path instead of probing each time.
    if ((status & 0x7F) == 3) {
      m_graphLoadModelSupported = false;
      pushGraphNodeByNode(pending);
      return;
    }
  }
  if (status != 0) {"""
assert old in s
s = s.replace(old, new, 1)

# ---- patchJsonObject: carry the graph ------------------------------------
old = """QJsonObject SynthController::patchJsonObject(const QString& name,
                                             int engine,
                                             const QString& created,
                                             const QList<QPair<int, double>>& params) const {"""
new = """QJsonObject SynthController::patchJsonObject(const QString& name,
                                             int engine,
                                             const QString& created,
                                             const QList<QPair<int, double>>& params,
                                             const QString& graph) const {"""
assert old in s
s = s.replace(old, new, 1)

old = """  if (!created.isEmpty()) out["created"] = created;
  out["params"] = items;
  return out;
}"""
new = """  if (!created.isEmpty()) out["created"] = created;
  out["params"] = items;
  // Only when there is one, so a subtractive patch's file is byte-identical to
  // what earlier builds wrote.
  if (!graph.isEmpty()) {
    const QJsonDocument g = QJsonDocument::fromJson(graph.toUtf8());
    if (g.isArray()) out["graph"] = g.array();
  }
  return out;
}"""
assert old in s
s = s.replace(old, new, 1)

old = """    const QJsonObject obj = patchJsonObject(m.value("name").toString(),
                                            m.value("engine").toInt(),
                                            m.value("created").toString(),
                                            db().getPatchParams(patchId));"""
new = """    const QJsonObject obj = patchJsonObject(m.value("name").toString(),
                                            m.value("engine").toInt(),
                                            m.value("created").toString(),
                                            db().getPatchParams(patchId),
                                            db().getPatchGraph(patchId));"""
assert old in s
s = s.replace(old, new, 1)

old = """    patchArray.append(patchJsonObject(m.value("name").toString(),
                                      m.value("engine").toInt(),
                                      m.value("created").toString(),
                                      db().getPatchParams(m.value("id").toInt())));"""
new = """    patchArray.append(patchJsonObject(m.value("name").toString(),
                                      m.value("engine").toInt(),
                                      m.value("created").toString(),
                                      db().getPatchParams(m.value("id").toInt()),
                                      db().getPatchGraph(m.value("id").toInt())));"""
assert old in s
s = s.replace(old, new, 1)

# exportPresetJson snapshots the live synth, so its graph is the live one
old = """      const QJsonObject obj = patchJsonObject(name, engine, QString(), params);"""
new = """      const QJsonObject obj = patchJsonObject(name, engine, QString(), params,
                                             graphJson());"""
assert old in s
s = s.replace(old, new, 1)

# ---- import: keep the graph ---------------------------------------------
old = """    QString name = patch.value("name").toString().trimmed();
    if (name.isEmpty()) name = Translator::instance().t("Imported patch");
    if (db().insertPatch(name, engine, params) > 0) {"""
new = """    QString name = patch.value("name").toString().trimmed();
    if (name.isEmpty()) name = Translator::instance().t("Imported patch");
    // Stored verbatim. Unlike the parameters there is nothing to resolve — a
    // graph is slots, kinds and cables, and those are the same numbers on
    // every build that has the engine at all.
    QString graph;
    if (patch.value("graph").isArray()) {
      graph = QString::fromUtf8(
          QJsonDocument(patch.value("graph").toArray()).toJson(QJsonDocument::Compact));
    }
    if (db().insertPatch(name, engine, params, graph) > 0) {"""
assert old in s
s = s.replace(old, new, 1)

# importPatchJson pushes to the synth: push the graph too
old = """  if (engine >= 0 && engine != m_engine) {
    // The ids the names resolve to only exist after the new engine has
    // re-registered, so resolution waits with the push.
    selectEngine(engine);
    m_pendingImport = items;
    QTimer::singleShot(kEngineSwitchSettleMs, this, [this]() {
      resolveAndPushImport(m_pendingImport);
      m_pendingImport.clear();
    });
  } else {
    resolveAndPushImport(items);
  }"""
new = """  QList<GraphNode> graph;
  if (patch.value("graph").isArray()) {
    graph = graphFromJson(QString::fromUtf8(
        QJsonDocument(patch.value("graph").toArray()).toJson(QJsonDocument::Compact)));
  }

  if (engine >= 0 && engine != m_engine) {
    // The ids the names resolve to only exist after the new engine has
    // re-registered, so resolution waits with the push — and, on a modular
    // patch, waits again for the graph, which re-registers them a second time.
    selectEngine(engine);
    m_pendingImport = items;
    QTimer::singleShot(kEngineSwitchSettleMs, this, [this, graph]() {
      pushGraph(graph);
      QTimer::singleShot(graph.isEmpty() ? 0 : kGraphSettleMs, this, [this]() {
        resolveAndPushImport(m_pendingImport);
        m_pendingImport.clear();
      });
    });
  } else if (!graph.isEmpty()) {
    pushGraph(graph);
    m_pendingImport = items;
    QTimer::singleShot(kGraphSettleMs, this, [this]() {
      resolveAndPushImport(m_pendingImport);
      m_pendingImport.clear();
    });
  } else {
    resolveAndPushImport(items);
  }"""
assert old in s
s = s.replace(old, new, 1)

open(p, 'w', encoding='utf-8', newline='').write(s)
print("ok")
