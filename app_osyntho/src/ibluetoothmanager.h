#ifndef IBLUETOOTHMANAGER_H
#define IBLUETOOTHMANAGER_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVariantList>

// Abstraction over the platform BLE backends (Qt Bluetooth on Android/Linux/
// macOS, SimpleBLE on Windows). It owns scan/connect/reconnect and the osynth
// GATT service: it subscribes to EVT (notify -> receivedData), reads INFO once
// on connect (-> infoRead) and writes command frames to CTRL (-> write()).
//
// The data path is binary (QByteArray): SynthCtl v1 frames carry raw floats and
// would be corrupted by a UTF-8 round-trip through QString.
class IBluetoothManager : public QObject {
  Q_OBJECT

  Q_PROPERTY(bool scanning READ getScanning WRITE setScanning NOTIFY scanningChanged)
  Q_PROPERTY(bool connected READ getConnected NOTIFY connectedChanged)
  Q_PROPERTY(QString deviceName READ getDeviceName NOTIFY deviceNameChanged)
  Q_PROPERTY(QString deviceAddress READ getDeviceAddress NOTIFY deviceAddressChanged)
  Q_PROPERTY(QVariantList discoveredDevices READ getDiscoveredDevices NOTIFY discoveredDevicesChanged)

 public:
  virtual void finish() = 0;

  virtual bool askForBluetoothPermissionIfNotAvailable() = 0;

  virtual bool getScanning() const = 0;
  virtual void setScanning(bool newScanning) = 0;

  virtual bool getConnected() = 0;

  virtual QString getDeviceName() = 0;
  virtual QString getDeviceAddress() = 0;

  virtual QVariantList getDiscoveredDevices() const = 0;

 signals:
  // EVT notification frames, synth -> app.
  void receivedData(const QByteArray& data);
  // Result of the one-shot INFO characteristic read after connecting.
  void infoRead(const QByteArray& info);

  void scanningChanged();
  void connectedChanged(bool connected);
  void deviceNameChanged();
  void deviceAddressChanged();
  void discoveredDevicesChanged();

  // Persisted by App when a device connects. Declared here (rather than on each
  // concrete manager) so consumers can depend on IBluetoothManager alone.
  void updateConnectedBluetoothDevice(const QString& name, const QString& address);

 public slots:
  // Writes a command frame to CTRL. withResponse=false uses write-without-
  // response (low latency, for coalesced parameter sweeps); true requests an
  // ATT ack (for commands worth retrying, e.g. preset load/save).
  virtual void write(const QByteArray& data, bool withResponse) = 0;
  virtual void onBluetoothAvailable() = 0;
  virtual void setBluetoothEnabled(bool enabled) = 0;

 protected:
  explicit IBluetoothManager(QObject* parent = nullptr) : QObject(parent) {}
};

#endif  // IBLUETOOTHMANAGER_H
