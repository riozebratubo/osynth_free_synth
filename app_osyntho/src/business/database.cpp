#include "database.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>
#include <QVariantMap>

#include "src/apputils.h"

constexpr const char* globalDatabaseFileName = "osyntho.db";
constexpr const char* globalDatabaseTempFileName = "osyntho_temp.db";
// The target schema version lives on the class (Database::currentSchemaVersion)
// so tests can assert against it.

Database::Database()
  : dbConnectionName{},  // empty => Qt default connection (production singleton)
    dbFileFolder{QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)},
    dbFileFullPath{
        dbFileFolder +
        (((dbFileFolder.right(1) == "\\") or (dbFileFolder.right(1) == "/")) ? "" : "/") +
        QString{globalDatabaseFileName}},
    dbFileTempFullPath{
        dbFileFolder +
        (((dbFileFolder.right(1) == "\\") or (dbFileFolder.right(1) == "/")) ? "" : "/") +
        QString{globalDatabaseTempFileName}},
    isDbOpen{false} {
  qDebug() << "Db | Location: " << dbFileFullPath;

  const QDir dbDir{dbFileFolder};
  if (not dbDir.exists()) {
    dbDir.mkpath(dbFileFolder);
  }

  openDatabase(dbFileFullPath);
  runCreateTables();
}

Database& Database::instance() {
  static Database myInstance;
  return myInstance;
}

// Test/DI constructor: opens `filePath` (e.g. ":memory:") on a private named
// connection so it never collides with the production singleton's default
// connection.
Database::Database(const QString& connectionName, const QString& filePath)
  : dbConnectionName{connectionName},
    dbFileFolder{},
    dbFileFullPath{filePath},
    dbFileTempFullPath{},
    isDbOpen{false} {
  openDatabase(filePath);
  runCreateTables();
}

Database* Database::create(const QString& connectionName, const QString& filePath) {
  return new Database(connectionName, filePath);
}

Database::~Database() {
  if (isDbOpen) {
    db.close();
  }
  // Release our handle before removing the connection, otherwise Qt warns the
  // connection is still in use. Only named (test/DI) connections are removed;
  // the production singleton uses the default connection and lives until exit.
  if (not dbConnectionName.isEmpty()) {
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase(dbConnectionName);
  }
}

void Database::openDatabase(QString fileLocation) {
  if (QSqlDatabase::isDriverAvailable("QSQLITE")) {
    // Reuse a connection that is already registered (the restore flow closes and
    // reopens the same connection): calling addDatabase again with the same name
    // logs Qt's duplicate-connection warning and tears down the old handle.
    const QString connectionName = dbConnectionName.isEmpty()
                                       ? QString::fromLatin1(QSqlDatabase::defaultConnection)
                                       : dbConnectionName;
    db = QSqlDatabase::contains(connectionName)
             ? QSqlDatabase::database(connectionName, /*open=*/false)
             : QSqlDatabase::addDatabase("QSQLITE", connectionName);
    db.setDatabaseName(fileLocation);
    if (db.open()) {
      isDbOpen = true;
    } else {
      isDbOpen = false;
      qWarning() << "Db | Error: Cannot open database: " << db.lastError().text();
    }
  } else {
    qWarning() << "Db | Error: Database: no sqlite driver available";
  }
}

void Database::closeDatabase() {
  if (isDbOpen) {
    db.close();
    // Without this, isDatabaseOpen() keeps reporting open between a
    // forceCloseDatabase() and the reopen (queries would run against a closed
    // connection), and a failed reopen would leave every later query passing
    // the guard and failing silently.
    isDbOpen = false;
  } else {
    qWarning() << "Db | Warning: Database: trying to close but database is already closed";
  }
}

bool Database::isDatabaseOpen() {
  if (not isDbOpen) {
    qWarning() << "Db is not open, cannot run query";
  }
  return isDbOpen;
}

void Database::runCreateTables() {
  if (not isDatabaseOpen()) return;

  auto& thisDb = this->db;

  auto tableExists = [&thisDb](const QString& tableName) {
    QSqlQuery query{thisDb};
    query.prepare(
        QString{"SELECT name FROM sqlite_master WHERE type='table' AND name='%1';"}.arg(tableName));
    if (not query.exec()) {
      qWarning() << "Db | Error: Cannot query schema: " << thisDb.lastError().text();
      return false;
    }
    return query.next();
  };

  auto runCreateTable = [&tableExists, this](const QString& tableName, const QString& tableQuery) {
    if (not tableExists(tableName)) {
      QSqlQuery query{db};
      query.prepare(tableQuery);
      if (not query.exec()) {
        qWarning() << "Db | Error: could not create sqlite table!";
        qDebug() << "Db |   Table name: " << tableName;
        qDebug() << "Db |   Error details: " << query.lastError().text();
      }
    }
  };

  auto runQueryNoError = [&tableExists, this](const QString& tableName,
                                              const QString& queryNoError) {
    if (tableExists(tableName)) {
      QSqlQuery query{db};
      query.prepare(queryNoError);
      query.exec();
    }
  };

  /* settings */
  runCreateTable("setting", R"EOF(
      CREATE TABLE setting(
          name TEXT PRIMARY KEY,
          value TEXT,
          timestamp DATETIME DEFAULT CURRENT_TIMESTAMP
      );
  )EOF");

  runCreateTable("last_bt_hosts", R"EOF(
      CREATE TABLE last_bt_hosts(
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          address TEXT,
          timestamp DATETIME DEFAULT CURRENT_TIMESTAMP
      );
  )EOF");

  /* bluetooth devices */
  runCreateTable("bluetooth_device", R"EOF(
      CREATE TABLE bluetooth_device(
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          name TEXT,
          address TEXT UNIQUE,
          last_connected_on DATETIME DEFAULT CURRENT_TIMESTAMP
      )
  )EOF");

  /* patch library — named snapshots of the synth's live parameters */
  runCreateTable("patch", R"EOF(
      CREATE TABLE patch(
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          name TEXT,
          engine INTEGER DEFAULT 0,
          created DATETIME DEFAULT CURRENT_TIMESTAMP
      )
  )EOF");

  runCreateTable("patch_param", R"EOF(
      CREATE TABLE patch_param(
          patch_id INTEGER,
          param_id INTEGER,
          value REAL
      )
  )EOF");

  // De-dupe last_bt_hosts and enforce address uniqueness so REPLACE INTO works.
  runQueryNoError("last_bt_hosts",
                  "DELETE FROM last_bt_hosts WHERE id NOT IN "
                  "(SELECT MAX(id) FROM last_bt_hosts GROUP BY address)");
  runQueryNoError("last_bt_hosts",
                  "CREATE UNIQUE INDEX IF NOT EXISTS idx_last_bt_hosts_address "
                  "ON last_bt_hosts(address)");
  runQueryNoError("patch_param",
                  "CREATE INDEX IF NOT EXISTS idx_patch_param_patch_id "
                  "ON patch_param(patch_id)");

  const unsigned int dbSchemaVersion = getSchemaVersion();

  // Stamp the schema version only AFTER any migrations above ran.
  if (dbSchemaVersion == 0) {
    runQueryNoError("setting",
                    "CREATE TABLE IF NOT EXISTS schema_version(version INTEGER PRIMARY KEY, "
                    "applied_on DATETIME DEFAULT CURRENT_TIMESTAMP)");
    runQueryNoError("schema_version",
                    QString("INSERT INTO schema_version(version) VALUES (%1)")
                        .arg(QString::number(currentSchemaVersion)));
  } else if (dbSchemaVersion < currentSchemaVersion) {
    runQueryNoError("schema_version",
                    QString("UPDATE schema_version SET version = %1")
                        .arg(QString::number(currentSchemaVersion)));
  }
}

int Database::getSchemaVersion() {
  if (not isDatabaseOpen()) return {};

  QSqlQuery query{db};
  query.prepare("SELECT version FROM schema_version LIMIT 1");
  if (query.exec()) {
    if (query.next()) return query.value("version").toUInt();
    return 0;
  }
  return 0;
}

/* ----------------------------------------------------------------- settings */

bool Database::saveSetting(const QString& name, const QString& value) {
  if (not isDatabaseOpen()) return false;

  QSqlQuery query{db};
  query.prepare(R"EOF(
      REPLACE INTO setting(name, value)
      VALUES (:name, :value)
  )EOF");
  query.bindValue(":name", name);
  query.bindValue(":value", value);

  if (query.exec()) return true;

  qWarning() << "Error: save setting: '" << name << "' with value: '" << value << "'";
  return false;
}

bool Database::getSetting(const QString& name, QString& outValue) {
  static QString emptyString{};

  if (not isDatabaseOpen()) {
    outValue = emptyString;
    return false;
  }

  QSqlQuery query{db};
  query.prepare("SELECT value FROM setting WHERE name = :name LIMIT 1");
  query.bindValue(":name", name);

  if (query.exec()) {
    if (query.next()) {
      outValue = query.value("value").toString();
      return true;
    }
  }

  outValue = emptyString;
  return false;
}

QHash<QString, QString> Database::getSettingsCurrentValues() {
  if (not isDatabaseOpen()) return {};

  QSqlQuery query{db};
  query.prepare("SELECT name, value FROM setting");

  QHash<QString, QString> returnMap;
  if (query.exec()) {
    while (query.next()) {
      returnMap[query.value("name").toString()] = query.value("value").toString();
    }
  } else {
    qWarning() << "Error: filling settings cache current values";
  }
  return returnMap;
}

/* ------------------------------------------------------- last bluetooth hosts */

QList<QString> Database::getAllLastBluetoothHosts() {
  if (not isDatabaseOpen()) return {};

  QSqlQuery query{db};
  query.prepare(
      "SELECT address FROM last_bt_hosts ORDER BY timestamp DESC");

  if (query.exec()) {
    QList<QString> hostList;
    while (query.next()) hostList.push_back(query.value("address").toString());
    return hostList;
  }

  qWarning() << "Error: get all last bluetooth hosts query";
  return {};
}

bool Database::insertLastBluetoothHost(const QString& address) {
  if (not isDatabaseOpen()) return false;

  QSqlQuery query{db};
  query.prepare("REPLACE INTO last_bt_hosts(address) VALUES (:address)");
  query.bindValue(":address", address);

  if (query.exec()) return true;

  qWarning() << "Error: save last bluetooth host: '" << address;
  return false;
}

/* ----------------------------------------------------------- bluetooth devices */

bool Database::updateConnectedBluetoothDevice(const QString& name, const QString& address) {
  if (not isDatabaseOpen()) return false;

  QSqlQuery query{db};
  query.prepare("REPLACE INTO bluetooth_device(name, address) VALUES(:name, :address)");
  query.bindValue(":name", name);
  query.bindValue(":address", address);

  if (query.exec()) return true;

  qWarning() << "Error: replace bluetooth device, address and name: " << address << name;
  return false;
}

QList<QString> Database::getLastConnectedDevices(int maxDevices) {
  if (not isDatabaseOpen()) return {};

  QSqlQuery query{db};
  query.prepare(
      "SELECT address FROM bluetooth_device ORDER BY last_connected_on DESC LIMIT :max_devices");
  query.bindValue(":max_devices", maxDevices);

  if (query.exec()) {
    QList<QString> deviceList;
    while (query.next()) deviceList.append(query.value("address").toString());
    return deviceList;
  }

  qWarning() << "Error: get bluetooth devices query";
  return {};
}

/* --------------------------------------------------------------- patch library */

int Database::insertPatch(const QString& name, int engine,
                          const QList<QPair<int, double>>& params) {
  if (not isDatabaseOpen()) return 0;

  if (not db.transaction()) {
    qWarning() << "Error: insertPatch could not start a transaction";
    return 0;
  }

  QSqlQuery query{db};
  query.prepare("INSERT INTO patch(name, engine) VALUES (:name, :engine)");
  query.bindValue(":name", name);
  query.bindValue(":engine", engine);
  if (not query.exec()) {
    qWarning() << "Error: insertPatch failed:" << query.lastError().text();
    db.rollback();
    return 0;
  }

  const int patchId = query.lastInsertId().toInt();
  if (patchId <= 0) {
    db.rollback();
    return 0;
  }

  for (const auto& pv : params) {
    QSqlQuery q{db};
    q.prepare(
        "INSERT INTO patch_param(patch_id, param_id, value) VALUES (:pid, :param, :value)");
    q.bindValue(":pid", patchId);
    q.bindValue(":param", pv.first);
    q.bindValue(":value", pv.second);
    if (not q.exec()) {
      qWarning() << "Error: insertPatch param failed:" << q.lastError().text();
      db.rollback();
      return 0;
    }
  }

  if (not db.commit()) {
    qWarning() << "Error: insertPatch failed to commit" << db.lastError();
    db.rollback();
    return 0;
  }
  return patchId;
}

bool Database::renamePatch(int id, const QString& name) {
  if (not isDatabaseOpen()) return false;

  QSqlQuery query{db};
  query.prepare("UPDATE patch SET name = :name WHERE id = :id");
  query.bindValue(":name", name);
  query.bindValue(":id", id);
  if (query.exec()) return query.numRowsAffected() != 0;

  qWarning() << "Error: renamePatch id:" << id;
  return false;
}

bool Database::deletePatch(int id) {
  if (not isDatabaseOpen()) return false;

  if (not db.transaction()) return false;
  {
    QSqlQuery q{db};
    q.prepare("DELETE FROM patch_param WHERE patch_id = :id");
    q.bindValue(":id", id);
    if (not q.exec()) {
      db.rollback();
      return false;
    }
  }
  {
    QSqlQuery q{db};
    q.prepare("DELETE FROM patch WHERE id = :id");
    q.bindValue(":id", id);
    if (not q.exec()) {
      db.rollback();
      return false;
    }
  }
  return db.commit();
}

QVariantList Database::getPatches(int engine) {
  if (not isDatabaseOpen()) return {};

  QSqlQuery query{db};
  if (engine < 0) {
    query.prepare(
        "SELECT id, name, engine, datetime(created, 'localtime') AS created "
        "FROM patch ORDER BY created DESC");
  } else {
    query.prepare(
        "SELECT id, name, engine, datetime(created, 'localtime') AS created "
        "FROM patch WHERE engine = :engine ORDER BY created DESC");
    query.bindValue(":engine", engine);
  }

  QVariantList out;
  if (query.exec()) {
    while (query.next()) {
      out.append(QVariantMap{
          {"id", query.value("id").toInt()},
          {"name", query.value("name").toString()},
          {"engine", query.value("engine").toInt()},
          {"created", query.value("created").toString()},
      });
    }
  } else {
    qWarning() << "Error: getPatches query";
  }
  return out;
}

QList<QPair<int, double>> Database::getPatchParams(int patchId) {
  QList<QPair<int, double>> out;
  if (not isDatabaseOpen()) return out;

  QSqlQuery query{db};
  query.prepare("SELECT param_id, value FROM patch_param WHERE patch_id = :id");
  query.bindValue(":id", patchId);

  if (query.exec()) {
    while (query.next()) {
      out.append({query.value("param_id").toInt(), query.value("value").toDouble()});
    }
  } else {
    qWarning() << "Error: getPatchParams query, id:" << patchId;
  }
  return out;
}

/* ------------------------------------------------------ file paths / backup */

QString Database::getDatabaseFileFolder() { return dbFileFolder; }

QString Database::getDatabaseFileFullPath() { return dbFileFullPath; }

void Database::saveDatabaseBackupTo(const QString& whereToFile) {
  QFile::copy(dbFileFullPath, AppUtils::getFilePathFromCanonical(whereToFile));
}

void Database::restoreDatabaseFrom(const QString& whereFromFile) {
  const QFileInfo dbFileInfo{dbFileFullPath};
  if (dbFileInfo.exists() and not dbFileInfo.isWritable()) {
    qDebug() << "Error restoring database: app database file is not writable!";
    return;
  }
  QString whereFrom = AppUtils::getFilePathFromCanonical(whereFromFile);
  if (not QFile::exists(whereFrom)) {
    qDebug() << "Error restoring database: file to restore from does not exist!";
    return;
  }
  // QFile::copy never overwrites: a stale temp backup left by an interrupted
  // previous restore would make this copy fail and abort the restore.
  QFile::remove(dbFileTempFullPath);
  bool copiedBackup = QFile::copy(dbFileFullPath, dbFileTempFullPath);
  if (not copiedBackup) {
    qDebug() << "Error restoring database: cannot save backup before restoring!";
    return;
  }
  closeDatabase();
  QFile::remove(dbFileFullPath);
  bool copiedNew = QFile::copy(whereFrom, dbFileFullPath);
  if (not copiedNew) {
    qDebug() << "Error restoring database, reverting to the old one.";
    QFile::copy(dbFileTempFullPath, dbFileFullPath);
  }
  openDatabase(dbFileFullPath);
  // The restored file may come from an older app version: run the create/migrate
  // pass so this session doesn't run against an outdated schema until restart.
  runCreateTables();
  QFile::remove(dbFileTempFullPath);
}

void Database::forceCloseDatabase() { closeDatabase(); }

void Database::forceOpenOrCreateDatabase() {
  const QDir dbDir{dbFileFolder};
  if (not dbDir.exists()) {
    dbDir.mkpath(dbFileFolder);
  }
  openDatabase(dbFileFullPath);
  runCreateTables();
}
