#ifndef IDATABASE_H
#define IDATABASE_H

#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QVariantList>

constexpr int MAX_LAST_CONNECTED_DEVICES = 1;

// Pure-virtual interface for the SQLite persistence layer.
//
// Lets consumers (App, SynthController) depend on an abstraction so tests can
// inject an in-memory Database or a fake instead of the on-disk singleton. See
// Database for the production implementation.
//
// osynth scope: app settings, the last-connected Bluetooth device (for
// reconnect preference), and a local patch library — named snapshots of the
// synth's live parameters the user can re-push. The synth itself owns its
// factory/user preset slots; those are not mirrored here.
class IDatabase {
 public:
  virtual ~IDatabase() = default;

  /* settings */
  virtual bool saveSetting(const QString& name, const QString& value) = 0;
  virtual bool getSetting(const QString& name, QString& outValue) = 0;
  virtual QHash<QString, QString> getSettingsCurrentValues() = 0;

  /* last bluetooth hosts */
  virtual QList<QString> getAllLastBluetoothHosts() = 0;
  virtual bool insertLastBluetoothHost(const QString& address) = 0;

  /* bluetooth devices */
  virtual bool updateConnectedBluetoothDevice(const QString& name, const QString& address) = 0;
  virtual QList<QString> getLastConnectedDevices(int maxDevices = MAX_LAST_CONNECTED_DEVICES) = 0;

  /* patch library
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

  /* file paths / backup / lifecycle */
  virtual QString getDatabaseFileFolder() = 0;
  virtual QString getDatabaseFileFullPath() = 0;
  virtual void saveDatabaseBackupTo(const QString& whereToFile) = 0;
  virtual void restoreDatabaseFrom(const QString& whereFromFile) = 0;
  virtual void forceCloseDatabase() = 0;
  virtual void forceOpenOrCreateDatabase() = 0;
};

#endif  // IDATABASE_H
