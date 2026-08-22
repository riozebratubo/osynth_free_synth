p = 'app_osyntho/src/business/database.cpp'
s = open(p, encoding='utf-8').read()

# ---- migration: add the column to an existing patch table ----------------
old = """  // Stamp the schema version only AFTER any migrations above ran."""
new = """  // Schema 3 (S40): patch.graph. A database created by this build already has
  // the column (runCreateTable above), so this is only for one that predates
  // it — ALTER TABLE ADD COLUMN on SQLite rewrites no rows and fills existing
  // ones with NULL, which reads back as an empty string: exactly "this patch
  // stored no graph", which is true of every patch saved before now.
  if (dbSchemaVersion < 3 and tableExists("patch") and
      not columnExists("patch", "graph")) {
    runQueryNoError("patch", "ALTER TABLE patch ADD COLUMN graph TEXT");
  }

  // Stamp the schema version only AFTER any migrations above ran."""
assert old in s
s = s.replace(old, new, 1)

# ---- columnExists helper -------------------------------------------------
old = """  auto runCreateTable = [&tableExists, this](const QString& tableName, const QString& tableQuery) {"""
new = """  // For migrations that add a column: a database can be at an older *stamped*
  // version and still have the column (a restore from a newer backup, an
  // interrupted migrate), and ALTER TABLE ADD COLUMN is an error rather than a
  // no-op when it is already there.
  auto columnExists = [&thisDb](const QString& tableName, const QString& column) {
    const QSqlRecord rec = thisDb.record(tableName);
    return rec.indexOf(column) >= 0;
  };

  auto runCreateTable = [&tableExists, this](const QString& tableName, const QString& tableQuery) {"""
assert old in s
s = s.replace(old, new, 1)

# ---- insertPatch: store the graph ---------------------------------------
old = """int Database::insertPatch(const QString& name, int engine,
                          const QList<QPair<int, double>>& params) {
  if (not isDatabaseOpen()) return 0;

  if (not db.transaction()) {
    qWarning() << "Error: insertPatch could not start a transaction";
    return 0;
  }

  QSqlQuery query{db};
  query.prepare("INSERT INTO patch(name, engine) VALUES (:name, :engine)");
  query.bindValue(":name", name);
  query.bindValue(":engine", engine);"""
new = """int Database::insertPatch(const QString& name, int engine,
                          const QList<QPair<int, double>>& params,
                          const QString& graph) {
  if (not isDatabaseOpen()) return 0;

  if (not db.transaction()) {
    qWarning() << "Error: insertPatch could not start a transaction";
    return 0;
  }

  QSqlQuery query{db};
  query.prepare("INSERT INTO patch(name, engine, graph) VALUES (:name, :engine, :graph)");
  query.bindValue(":name", name);
  query.bindValue(":engine", engine);
  // NULL rather than "" when there is none, so "no graph" reads the same on a
  // row written now as on one migrated in from before the column existed.
  query.bindValue(":graph", graph.isEmpty() ? QVariant() : QVariant(graph));"""
assert old in s
s = s.replace(old, new, 1)

# ---- getPatchGraph -------------------------------------------------------
old = """/* ------------------------------------------------------ file paths / backup */"""
new = """QString Database::getPatchGraph(int patchId) {
  if (not isDatabaseOpen()) return {};

  QSqlQuery query{db};
  query.prepare("SELECT graph FROM patch WHERE id = :id");
  query.bindValue(":id", patchId);

  if (not query.exec()) {
    // Not a warning: a database still at schema 2 has no such column, and the
    // caller's answer for that is the same as for a patch that stored nothing.
    return {};
  }
  if (not query.next()) return {};
  return query.value(0).toString();
}

/* ------------------------------------------------------ file paths / backup */"""
assert old in s
s = s.replace(old, new, 1)

open(p, 'w', encoding='utf-8', newline='').write(s)
print("ok")
