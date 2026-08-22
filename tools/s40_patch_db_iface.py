# Adds the graph column to the patch library: interface, header, impl, schema.

# ---- idatabase.h ---------------------------------------------------------
p = 'app_osyntho/src/business/idatabase.h'
s = open(p, encoding='utf-8').read()
old = """  /* patch library
   *
   * A patch is a full snapshot of the live parameter values for one engine.
   * Params travel as (id, value) pairs — ids are the synth's 16-bit param ids,
   * values are the raw floats the wire uses. getPatches returns rows shaped for
   * QML: { id, name, engine, created }. */
  virtual int insertPatch(const QString& name,
                          int engine,
                          const QList<QPair<int, double>>& params) = 0;
  virtual bool renamePatch(int id, const QString& name) = 0;
  virtual bool deletePatch(int id) = 0;
  // engine < 0 lists every patch regardless of engine.
  virtual QVariantList getPatches(int engine = -1) = 0;
  virtual QList<QPair<int, double>> getPatchParams(int patchId) = 0;
"""
new = """  /* patch library
   *
   * A patch is a full snapshot of the live parameter values for one engine.
   * Params travel as (id, value) pairs — ids are the synth's 16-bit param ids,
   * values are the raw floats the wire uses. getPatches returns rows shaped for
   * QML: { id, name, engine, created }.
   *
   * `graph` is the modular patch's *structure* — node kinds, cables and canvas
   * positions — as the JSON array SynthController::graphJson() builds, and is
   * empty for every engine but Modular. It has to be stored separately because
   * it is not parameter space: a modular node's parameter ids only mean what
   * the kind in that slot says they mean, so a snapshot of the values alone
   * comes back applied to whatever graph happened to be patched. */
  virtual int insertPatch(const QString& name,
                          int engine,
                          const QList<QPair<int, double>>& params,
                          const QString& graph = QString()) = 0;
  virtual bool renamePatch(int id, const QString& name) = 0;
  virtual bool deletePatch(int id) = 0;
  // engine < 0 lists every patch regardless of engine.
  virtual QVariantList getPatches(int engine = -1) = 0;
  virtual QList<QPair<int, double>> getPatchParams(int patchId) = 0;
  // Empty when the patch stored none (any non-modular engine, or a row saved
  // before the column existed).
  virtual QString getPatchGraph(int patchId) = 0;
"""
assert old in s
s = s.replace(old, new, 1)
open(p, 'w', encoding='utf-8', newline='').write(s)

# ---- database.h ----------------------------------------------------------
p = 'app_osyntho/src/business/database.h'
s = open(p, encoding='utf-8').read()
old = """  int insertPatch(const QString& name,
                  int engine,
                  const QList<QPair<int, double>>& params) override;
  bool renamePatch(int id, const QString& name) override;
  bool deletePatch(int id) override;
  QVariantList getPatches(int engine = -1) override;
  QList<QPair<int, double>> getPatchParams(int patchId) override;"""
new = """  int insertPatch(const QString& name,
                  int engine,
                  const QList<QPair<int, double>>& params,
                  const QString& graph = QString()) override;
  bool renamePatch(int id, const QString& name) override;
  bool deletePatch(int id) override;
  QVariantList getPatches(int engine = -1) override;
  QList<QPair<int, double>> getPatchParams(int patchId) override;
  QString getPatchGraph(int patchId) override;"""
assert old in s
s = s.replace(old, new, 1)

old = """  // 2 (S36): back-fills the per-effect enable switches into patches saved
  // before they existed. See the migration in Database::createTables().
  static constexpr unsigned int currentSchemaVersion = 2;"""
new = """  // 2 (S36): back-fills the per-effect enable switches into patches saved
  // before they existed. See the migration in Database::createTables().
  // 3 (S40): patch.graph, so a modular patch stores the cables it was built
  // from and not only the values hanging off them.
  static constexpr unsigned int currentSchemaVersion = 3;"""
assert old in s
s = s.replace(old, new, 1)
open(p, 'w', encoding='utf-8', newline='').write(s)

# ---- database.cpp --------------------------------------------------------
p = 'app_osyntho/src/business/database.cpp'
s = open(p, encoding='utf-8').read()

old = """  /* patch library — named snapshots of the synth's live parameters */
  runCreateTable("patch", R"EOF(
      CREATE TABLE patch(
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          name TEXT,
          engine INTEGER DEFAULT 0,
          created DATETIME DEFAULT CURRENT_TIMESTAMP
      )
  )EOF");"""
new = """  /* patch library — named snapshots of the synth's live parameters */
  runCreateTable("patch", R"EOF(
      CREATE TABLE patch(
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          name TEXT,
          engine INTEGER DEFAULT 0,
          created DATETIME DEFAULT CURRENT_TIMESTAMP,
          graph TEXT
      )
  )EOF");"""
assert old in s
s = s.replace(old, new, 1)

open(p, 'w', encoding='utf-8', newline='').write(s)
print("ok")
