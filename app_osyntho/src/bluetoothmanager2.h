#ifndef BLUETOOTHMANAGER2H_H
#define BLUETOOTHMANAGER2H_H

#include <QByteArray>
#include <QFuture>
#include <QFutureWatcher>
#include <QMutex>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <atomic>

#include "ibluetoothmanager.h"

//
#include <simpleble/Adapter.h>
#include <simpleble/Peripheral.h>

// Windows/SimpleBLE backend. Scans for an osynth peripheral, connects, reads
// INFO, subscribes to EVT (notify) and writes command frames to CTRL. See
// SynthProto for the GATT UUIDs.
class BluetoothManager : public IBluetoothManager {
  Q_OBJECT

 public:
  static BluetoothManager& instance();

  bool getScanning() const override;
  void finish() override;
  bool askForBluetoothPermissionIfNotAvailable() override;

  void setScanning(bool newScanning) override;
  bool getConnected() override;
  QString getDeviceName() override;
  QString getDeviceAddress() override;
  QVariantList getDiscoveredDevices() const override;

  Q_INVOKABLE void connectToSelectedDevice();
  // Device-selector screen entry/exit (shared QML). startDeviceScan()
  // disconnects any connected synth and enters scan-to-list mode: while the
  // selector is open the manager only scans and lists nearby devices (never
  // auto-connects). stopDeviceScan() leaves that mode.
  Q_INVOKABLE void startDeviceScan();
  Q_INVOKABLE void stopDeviceScan();

 public slots:
  void write(const QByteArray& data, bool withResponse) override;
  void onBluetoothAvailable() override;
  void setBluetoothEnabled(bool enabled) override;

 private:
  explicit BluetoothManager(QObject* parent = nullptr);

  void scanAndConnect();
  bool connectAndSubscribe(SimpleBLE::Peripheral& peripheral);

  // m_adapter/m_foundPeripheral are SimpleBLE handles (shared_ptr wrappers)
  // shared between the GUI thread and the BLE worker. Always go through these
  // mutexed accessors: take a copy, then call methods on the copy.
  SimpleBLE::Adapter adapterHandle() const;
  void setAdapterHandle(SimpleBLE::Adapter adapter);
  SimpleBLE::Peripheral peripheralHandle() const;
  void setPeripheralHandle(SimpleBLE::Peripheral peripheral);

  QTimer m_rescanTimer;

  std::atomic<bool> m_scanning;
  QFuture<void> m_bluetoothThread;
  // Watches the worker for the async (non-blocking) disconnect-then-scan the
  // device selector needs, so the GUI thread never blocks on waitForFinished().
  QFutureWatcher<void> m_disconnectWatcher;
  std::atomic<bool> m_shouldStopBluetoothThread;
  std::atomic<bool> m_skipScanGoConnect;
  std::atomic<bool> m_selectorOpen;
  std::atomic<bool> m_adapterIsConnecting;

  mutable QMutex m_handleMutex;
  SimpleBLE::Adapter m_adapter;
  SimpleBLE::Peripheral m_foundPeripheral;

  QStringList m_lastConnectedDevices;
  QVariantList m_discoveredDevices;
};

#endif  // BLUETOOTHMANAGER2H_H
