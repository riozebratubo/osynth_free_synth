#ifndef DATABASE_H
#define DATABASE_H

#include <QHash>
#include <QList>
#include <QPair>
#include <QSqlDatabase>
#include <QSqlDriver>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariantList>

#include "src/business/idatabase.h"

class Database : public IDatabase {
 public:
  static Database& instance();

  // Test/DI construction: builds a Database on a private named connection so it
  // does not clash with the production singleton's default connection. The
  // caller owns the returned instance. Pass ":memory:" for an in-memory DB.
  // Tables are created automatically. No app-name hack or file cleanup needed.
  static Database* create(const QString& connectionName, const QString& filePath = ":memory:");

  ~Database() override;

  /* settings */
  bool saveSetting(const QString& name, const QString& value) override;
  bool getSetting(const QString& name, QString& outValue) override;
  QHash<QString, QString> getSettingsCurrentValues() override;

  /* last bluetooth hosts */
  QList<QString> getAllLastBluetoothHosts() override;
  bool insertLastBluetoothHost(const QString& address) override;

  /* bluetooth devices */
  bool updateConnectedBluetoothDevice(const QString& name, const QString& address) override;
  QList<QString> getLastConnectedDevices(int maxDevices = MAX_LAST_CONNECTED_DEVICES) override;

  /* patch library */
  int insertPatch(const QString& name,
                  int engine,
                  const QList<QPair<int, double>>& params,
                  const QString& graph = QString()) override;
  bool renamePatch(int id, const QString& name) override;
  bool deletePatch(int id) override;
  QVariantList getPatches(int engine = -1) override;
  QList<QPair<int, double>> getPatchParams(int patchId) override;
  QString getPatchGraph(int patchId) override;

  /* file paths / backup / lifecycle */
  QString getDatabaseFileFolder() override;
  QString getDatabaseFileFullPath() override;
  bool saveDatabaseBackupTo(const QString& whereToFile) override;
  bool restoreDatabaseFrom(const QString& whereFromFile, QString* errorOut = nullptr) override;
  void forceCloseDatabase() override;
  void forceOpenOrCreateDatabase() override;

  // True when `path` is a SQLite database this app could have written: it
  // opens, its schema reads, and it holds the `setting` table. The restore
  // picker always offers an "all files" entry (nfd appends one of its own), so
  // without this any file at all could be copied over the app database — after
  // which the app runs against something that is not a database and the tables
  // silently do not exist.
  static bool isSqliteDatabaseFile(const QString& path);

  // The schema version this build migrates databases to. Public (together with
  // getSchemaVersion) so tests can assert a DB ends up stamped at it after the
  // create/migrate pass.
  // 2 (S36): back-fills the per-effect enable switches into patches saved
  // before they existed. See the migration in Database::createTables().
  // 3 (S40): patch.graph, so a modular patch stores the cables it was built
  // from and not only the values hanging off them.
  static constexpr unsigned int currentSchemaVersion = 3;
  // Version recorded in the DB's schema_version table (0 = none/pre-versioning).
  // Stamped at the END of the create/migrate pass, so a migration interrupted
  // mid-way keeps the old version recorded and is retried on the next open.
  int getSchemaVersion();

 private:
  Database();
  Database(const QString& connectionName, const QString& filePath);
  void openDatabase(QString fileLocation);
  void closeDatabase();

  void runCreateTables();
  bool isDatabaseOpen();

  // Empty for the production singleton (Qt default connection). Non-empty for
  // test/DI instances created via create(), keeping their connection isolated.
  QString dbConnectionName;
  QString dbFileFolder;
  QString dbFileFullPath;
  QString dbFileTempFullPath;
  QSqlDatabase db;
  bool isDbOpen;
};

#endif  // DATABASE_H
