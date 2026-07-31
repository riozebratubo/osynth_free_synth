#ifndef BLUETOOTHMANAGER_H
#define BLUETOOTHMANAGER_H

#include <QBluetoothAddress>
#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothDeviceInfo>
#include <QBluetoothLocalDevice>
#include <QByteArray>
#include <QHash>
#include <QLowEnergyController>
#include <QLowEnergyService>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>

#include "ibluetoothmanager.h"

// Qt Bluetooth backend (Android / Linux / macOS / iOS). Scans for an osynth
// peripheral, connects, reads INFO, subscribes to EVT (notify) and writes
// command frames to CTRL. Mirrors the SimpleBLE Windows backend
// (bluetoothmanager2) over the shared IBluetoothManager binary interface.
class BluetoothManager : public IBluetoothManager {
  Q_OBJECT

 public:
  static BluetoothManager& instance();

  bool getScanning() const override;
  void finish() override;
  bool askForBluetoothPermissionIfNotAvailable() override;
  QString getDeviceName() override;
  QString getDeviceAddress() override;
  bool getConnected() override;
  QVariantList getDiscoveredDevices() const override;
  int mtu() const override;

  Q_INVOKABLE void connectToSelectedDevice();
  // Device-selector screen entry/exit. startDeviceScan() disconnects any
  // connected synth and enters scan-to-list mode (list nearby devices, never
  // auto-connect). stopDeviceScan() leaves that mode.
  Q_INVOKABLE void startDeviceScan();
  Q_INVOKABLE void stopDeviceScan();

 public slots:
  void write(const QByteArray& data, bool withResponse) override;
  void onBluetoothAvailable() override;
  void setBluetoothEnabled(bool enabled) override;

  void onDeviceDiscovered(const QBluetoothDeviceInfo& device);
  void onErrorOccurred(QBluetoothDeviceDiscoveryAgent::Error error);
  void onScanFinished();
  void onScanCanceled();
  void initializeBt();

 private:
  explicit BluetoothManager(QObject* parent = nullptr);

  bool hasAllBluetoothPermissions();
  void setScanning(bool newScanning) override;

  void discover(QString deviceAddress);
  void stopScanning();
  void stopScanningAndDiscover();
  void stopScanningAndRescan();
  void startScanning();
  void startDiscover();
  void idleAction();

  void setIsConnected(bool is);
  void teardownConnection();
  // Called once the osynth service's details are discovered: reads INFO, enables
  // EVT notifications, then announces the connection.
  void onServiceDetailsDiscovered();

  // --- serialised CTRL writes -------------------------------------------
  //
  // Android's BluetoothGatt carries exactly ONE outstanding GATT operation at
  // a time. writeCharacteristic() while another is in flight is refused, and
  // the refusal surfaces as QLowEnergyService::errorOccurred — which nothing
  // used to listen to, so the frame simply disappeared. Anything that writes
  // in a burst therefore lost most of it: an engine switch (the app re-reads
  // the parameter table on EVT_ENGINE) reliably wedged the link, and any
  // note-off issued in that window went with it, which is why notes stuck on.
  //
  // So frames queue here and go out one at a time, each released by the
  // previous one's completion. Live gestures — notes, pads, knobs — get their
  // own lane so they are never stuck behind a listing burst.
  //
  // Every queued frame is written WithResponse regardless of what the caller
  // asked for: characteristicWritten() is the only portable "the stack is
  // ready for the next one" signal (Qt does not emit it for
  // WriteWithoutResponse on BlueZ or CoreBluetooth), and without a completion
  // signal there is nothing to pace against. One ATT round trip per frame is
  // the price of not losing them.
  struct PendingWrite {
    QByteArray data;
    bool withResponse = false;
  };
  void pumpWriteQueue();   // issue the next frame if the stack is idle
  void onWriteSettled();   // completion, refusal or watchdog: stack is idle
  void clearWriteQueue();

  QList<PendingWrite> m_writeHigh;
  QList<PendingWrite> m_writeLow;
  bool m_writeInFlight = false;
  // Releases the queue if a completion never arrives. Without it one lost
  // callback would stall every later frame for the rest of the session —
  // exactly the failure this queue exists to remove.
  QTimer m_writeWatchdog;

  QBluetoothLocalDevice* localDevice;
  QBluetoothDeviceDiscoveryAgent* discoveryAgent;

  QPointer<QLowEnergyController> bleController;
  QPointer<QLowEnergyService> service;
  QBluetoothAddress t_deviceAddress;
  QBluetoothDeviceInfo t_remoteDeviceInfo;
  bool m_isConnected;

  // False until initializeBt() succeeds; finish() resets it so pending
  // singleShot scan/discover steps go quiet.
  bool m_enabled = false;

  // Set once a connection attempt's error/disconnected handler has dispatched
  // the next state-machine step, so the pair some platforms emit for one failed
  // attempt can't double-dispatch. Reset in discover().
  bool m_attemptHandled = false;

  QList<QString> deviceQueue;

  QString m_deviceName;
  QString m_deviceAddress;
  bool m_scanning;

  // True while the device-selector screen is open: scan to list devices for the
  // user instead of auto-connecting.
  bool m_selectorOpen = false;

  QVariantList m_discoveredDevices;
  QHash<QString, QBluetoothDeviceInfo> addressToDeviceMap;
};

#endif  // BLUETOOTHMANAGER_H
