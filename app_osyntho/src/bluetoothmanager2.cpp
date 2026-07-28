#include "bluetoothmanager2.h"

#include <QDebug>
#include <QMutexLocker>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <utility>
#include <vector>

#include "src/ble/synthprotocol.h"
#include "src/business/database.h"
#include "src/business/settings.h"

using namespace std::chrono_literals;

constexpr int rescanRetryInterval = 2000;

// osynth GATT service + characteristics (see docs/BLE_PROTOCOL.md).
static const std::string serviceUuidStr = SynthProto::kServiceUuid;
static const std::string ctrlUuidStr = SynthProto::kCtrlUuid;   // Write / Write-no-response
static const std::string evtUuidStr = SynthProto::kEvtUuid;     // Notify
static const std::string infoUuidStr = SynthProto::kInfoUuid;   // Read

static QByteArray toQByteArray(const SimpleBLE::ByteArray& b) {
  return QByteArray(reinterpret_cast<const char*>(b.data()), int(b.size()));
}

BluetoothManager& BluetoothManager::instance() {
  static BluetoothManager myInstance;
  return myInstance;
}

BluetoothManager::BluetoothManager(QObject* parent)
  : IBluetoothManager{parent},
    m_rescanTimer{},
    m_scanning{false},
    m_bluetoothThread{},
    m_shouldStopBluetoothThread{false},
    m_skipScanGoConnect{false},
    m_selectorOpen{false},
    m_adapterIsConnecting{false},
    m_adapter{},
    m_foundPeripheral{} {
  /// @note this timer scans and tries to connect from time to time if not
  /// connected; it will scan and connect again if the peripheral disconnects.
  connect(&m_rescanTimer, &QTimer::timeout, this, [this]() {
    if (Settings::instance().setting("bluetooth_enabled") == "false") return;
    scanAndConnect();
  });
  m_rescanTimer.setInterval(rescanRetryInterval);

  for (auto& address : Database::instance().getLastConnectedDevices()) {
    m_lastConnectedDevices.append(address);
  }

  // Dedicated write thread (see writeBlocking). Started before any connection
  // can exist, so write() never has to check whether it is up.
  m_writeCtx = new QObject();
  m_writeCtx->moveToThread(&m_writeThread);
  m_writeThread.setObjectName("ble-write");
  m_writeThread.start();

  if (Settings::instance().setting("bluetooth_enabled") != "false") {
    m_rescanTimer.start();
    scanAndConnect();
  }
}

void BluetoothManager::scanAndConnect() {
  if (not SimpleBLE::Adapter::bluetooth_enabled()) {
    qDebug() << "Bt | Scan | Will not scan and connect, bluetooth is not enabled!";
    return;
  }

  // A single worker is alive for its whole lifetime — scanning, connecting AND
  // while connected (the connectAndSubscribe keep-alive loop). scanAndConnect()
  // is only ever called on the main thread, so this makes spawning a second
  // concurrent worker structurally impossible.
  if (m_bluetoothThread.isRunning()) return;

  auto adapter = adapterHandle();
  if (adapter.initialized() and adapter.scan_is_active()) return;
  if (m_adapterIsConnecting) return;

  auto connectedPeripheral = peripheralHandle();
  if (connectedPeripheral.initialized() and connectedPeripheral.is_connected()) return;

  // While the selector is open we accumulate devices across rescans so newer
  // ones appear without the list emptying between cycles (deduped below).
  if (not m_selectorOpen) {
    m_discoveredDevices.clear();
    emit discoveredDevicesChanged();
  }

  // Settings is an unsynchronized cache over the GUI-thread DB connection, so it
  // must not be read from the worker; read here and hand copies to the lambda.
  const QString useSelected = Settings::instance().setting("bluetooth_use_selected");
  const QString selectedAddress = Settings::instance().setting("bluetooth_selected_device_address");
  const QString bluetoothPrefix = Settings::instance().setting("bluetooth_prefix");
  const int scanTimeMs = Settings::instance().setting("bluetooth_scan_time").toInt() * 1000;

  m_bluetoothThread = QtConcurrent::run([this, useSelected, selectedAddress, bluetoothPrefix,
                                         scanTimeMs] {
    // Nothing may escape the worker: an exception left m_adapterIsConnecting set
    // (blocking every future scanAndConnect) and would be rethrown into whoever
    // calls waitForFinished().
    try {
      // Captured at scan start: a scan begun in list mode never auto-connects,
      // even if the selector closes (e.g. a Select tap) before the scan ends.
      const bool listOnly = m_selectorOpen;

#ifdef BLE_DIRECT_CONNECT
      if (useSelected == "true" and not selectedAddress.isEmpty() and not m_selectorOpen) {
        qDebug() << "Bt | Direct connect | Scanning for " << selectedAddress;
        auto dcAdapters = SimpleBLE::Adapter::get_adapters();
        if (dcAdapters.empty()) {
          qDebug() << "Bt | No Bluetooth adapters found!";
          return;
        }
        SimpleBLE::Adapter dcAdapter = dcAdapters[0];
        setAdapterHandle(dcAdapter);

        SimpleBLE::Peripheral targetPeripheral;
        bool targetFound = false;

        dcAdapter.set_callback_on_scan_found([&](SimpleBLE::Peripheral peripheral) {
          if (m_shouldStopBluetoothThread or m_selectorOpen) {
            dcAdapter.scan_stop();
            return;
          }
          if (peripheral.is_connectable() and
              QString::fromStdString(peripheral.address()) == selectedAddress) {
            targetPeripheral = peripheral;
            targetFound = true;
            dcAdapter.scan_stop();
          }
        });

        setScanning(true);
        dcAdapter.scan_for(2000);
        setScanning(false);

        if (not targetFound or m_shouldStopBluetoothThread or m_selectorOpen) {
          qDebug() << "Bt | Direct connect | Device not found during pre-scan.";
          setAdapterHandle(SimpleBLE::Adapter{});
          return;
        }

        connectAndSubscribe(targetPeripheral);
        return;
      }
#endif

      auto adapters = SimpleBLE::Adapter::get_adapters();
      if (adapters.empty()) {
        qDebug() << "Bt | No Bluetooth adapters found!";
        return;
      }

      SimpleBLE::Adapter adapter = adapters[0];
      setAdapterHandle(adapter);

      std::vector<SimpleBLE::Peripheral> peripherals;

      adapter.set_callback_on_scan_found([&](SimpleBLE::Peripheral peripheral) {
        if (m_shouldStopBluetoothThread or m_skipScanGoConnect) {
          adapter.scan_stop();
          return;
        }
        if (peripheral.is_connectable()) {
          qDebug() << "Bt | Scan | Found: " << peripheral.identifier() << peripheral.address();
          peripherals.push_back(peripheral);
          QString name = QString::fromStdString(peripheral.identifier());
          QString addr = QString::fromStdString(peripheral.address());
          QMetaObject::invokeMethod(this, [this, name, addr]() {
            for (const auto& d : m_discoveredDevices) {
              if (d.toMap().value("address").toString() == addr) return;  // dedupe by address
            }
            m_discoveredDevices.append(QVariantMap{{"name", name}, {"address", addr}});
            emit discoveredDevicesChanged();
          }, Qt::QueuedConnection);
#ifndef BLE_DIRECT_CONNECT
          if (useSelected == "true" and addr == selectedAddress and not m_selectorOpen) {
            adapter.scan_stop();
          }
#endif
        }
      });
      adapter.set_callback_on_scan_updated([&](SimpleBLE::Peripheral) {
        if (m_shouldStopBluetoothThread or m_skipScanGoConnect) adapter.scan_stop();
      });

      setScanning(true);
      adapter.scan_for(scanTimeMs);
      setScanning(false);

      if (m_shouldStopBluetoothThread) return;
      m_skipScanGoConnect = false;
      if (peripherals.empty()) return;

      // Selector is/was open: we only list devices for the user; never connect.
      if (listOnly or m_selectorOpen) return;

      // Ranked ordering — prefer the last-connected device, then anything named
      // like an osynth. Ranks are precomputed (identifier()/address() are
      // non-const, and MSVC's stable_sort passes rvalue temporaries).
      auto rank = [this](SimpleBLE::Peripheral& p) {
        if (not m_lastConnectedDevices.isEmpty() and p.address() == m_lastConnectedDevices[0]) {
          return 0;
        }
        if (QString::fromStdString(p.identifier())
                .startsWith(SynthProto::kDeviceName, Qt::CaseInsensitive)) {
          return 1;
        }
        return 2;
      };
      std::vector<std::pair<int, SimpleBLE::Peripheral>> ranked;
      ranked.reserve(peripherals.size());
      for (auto& peripheral : peripherals) ranked.emplace_back(rank(peripheral), peripheral);
      std::stable_sort(ranked.begin(), ranked.end(),
                       [](const auto& a, const auto& b) { return a.first < b.first; });
      peripherals.clear();
      for (auto& rankedPeripheral : ranked) peripherals.push_back(rankedPeripheral.second);

      for (auto& peripheral : peripherals) {
#ifdef BLE_DIRECT_CONNECT
        if (not QString::fromStdString(peripheral.identifier())
                    .startsWith(bluetoothPrefix, Qt::CaseInsensitive)) {
          continue;
        }
#else
        if (useSelected == "true") {
          if (QString::fromStdString(peripheral.address()) != selectedAddress) continue;
        } else if (not QString::fromStdString(peripheral.identifier())
                       .startsWith(bluetoothPrefix, Qt::CaseInsensitive)) {
          continue;
        }
#endif
        if (connectAndSubscribe(peripheral)) return;
      }
    } catch (const std::exception& e) {
      qDebug() << "Bt | Exception: " << e.what();
      m_adapterIsConnecting = false;
      setScanning(false);
    } catch (...) {
      qDebug() << "Bt | Exception (unknown).";
      m_adapterIsConnecting = false;
      setScanning(false);
    }
  });
}

bool BluetoothManager::connectAndSubscribe(SimpleBLE::Peripheral& peripheral) {
  if (not SimpleBLE::Adapter::bluetooth_enabled()) return false;
  if (m_shouldStopBluetoothThread) return false;

  qDebug() << "Bt | Connecting to " << peripheral.identifier() << " (" << peripheral.address() << ")";

  m_adapterIsConnecting = true;
  peripheral.connect();
  qDebug() << "Bt | Successfully connected. MTU:" << peripheral.mtu();

  // Verify the osynth service exposes CTRL + EVT (INFO is optional but expected).
  bool haveCtrl = false, haveEvt = false;
  for (auto service : peripheral.services()) {
    if (service.uuid() != serviceUuidStr) continue;
    for (auto characteristic : service.characteristics()) {
      if (characteristic.uuid() == ctrlUuidStr) haveCtrl = true;
      if (characteristic.uuid() == evtUuidStr) haveEvt = true;
    }
  }

  m_adapterIsConnecting = false;

  if (not(haveCtrl and haveEvt)) {
    qDebug() << "Bt | Service | osynth CTRL/EVT not found, disconnecting.";
    try {
      peripheral.disconnect();
    } catch (...) {
      qDebug() << "Bt | Error: cannot disconnect!";
    }
    return false;
  }

  setPeripheralHandle(peripheral);

  // Read INFO before subscribing so the controller has fw/target ready when it
  // sees the connection go live. infoRead is delivered to the controller with
  // an explicit queued connection (see App), so emitting it here is safe.
  try {
    auto info = peripheral.read(serviceUuidStr, infoUuidStr);
    emit infoRead(toQByteArray(info));
  } catch (const std::exception& e) {
    qDebug() << "Bt | INFO read failed: " << e.what();
  }

  // Subscribe to EVT. The firmware drops outgoing frames while unsubscribed, so
  // this must succeed before we announce the connection.
  try {
    peripheral.notify(serviceUuidStr, evtUuidStr, [this](SimpleBLE::ByteArray bytes) {
      emit receivedData(toQByteArray(bytes));
    });
  } catch (const std::exception& e) {
    qDebug() << "Bt | Service | Subscribe to EVT failed: " << e.what();
    setPeripheralHandle(SimpleBLE::Peripheral{});
    try {
      peripheral.disconnect();
    } catch (...) {
    }
    return false;
  }

  // EVT is live — now announce the connection. Every SimpleBLE read happens
  // here, on the worker; the GUI thread only ever sees the cached copies.
  const QString name = QString::fromStdString(peripheral.identifier());
  const QString address = QString::fromStdString(peripheral.address());
  int negotiatedMtu = 0;
  try {
    negotiatedMtu = int(peripheral.mtu());
  } catch (...) {
  }
  publishConnectionState(true, name, address, negotiatedMtu);

  emit updateConnectedBluetoothDevice(name, address);
  emitOnGuiThread([this]() {
    emit deviceNameChanged();
    emit deviceAddressChanged();
    emit connectedChanged(true);
  });

  peripheral.set_callback_on_disconnected(
      []() { qDebug() << "Bt | Peripheral disconnected (callback)"; });

  // Keep-alive: the worker parks here while connected, polling for a disconnect
  // or a stop request (startDeviceScan/finish block on this poll).
  while (true) {
    bool stillConnected = false;
    try {
      stillConnected = peripheral.is_connected();
    } catch (...) {
      stillConnected = false;
    }

    if (not stillConnected or m_shouldStopBluetoothThread) {
      qDebug() << "Bt | Thread will quit";
      setPeripheralHandle(SimpleBLE::Peripheral{});
      try {
        peripheral.unsubscribe(serviceUuidStr, evtUuidStr);
      } catch (...) {
      }
      try {
        peripheral.disconnect();
      } catch (...) {
      }
      setAdapterHandle(SimpleBLE::Adapter{});

      publishConnectionState(false, QString(), QString(), 0);
      emitOnGuiThread([this]() {
        emit connectedChanged(false);
        emit deviceNameChanged();
        emit deviceAddressChanged();
      });
      return true;
    }

    QThread::msleep(100);
  }
}

QVariantList BluetoothManager::getDiscoveredDevices() const { return m_discoveredDevices; }

void BluetoothManager::connectToSelectedDevice() {
  QString selectedAddr = Settings::instance().setting("bluetooth_selected_device_address");
  if (selectedAddr.isEmpty()) return;

  // Explicit connect intent: leave list-only (selector) mode.
  m_selectorOpen = false;

  auto adapter = adapterHandle();
  auto peripheral = peripheralHandle();
  if (adapter.initialized() and adapter.scan_is_active()) {
    qDebug() << "Bt | connectToSelectedDevice | Stopping scan, will connect on the next one.";
    m_skipScanGoConnect = true;
  } else if (not m_adapterIsConnecting and
             not(peripheral.initialized() and peripheral.is_connected())) {
    scanAndConnect();
  }
}

void BluetoothManager::startDeviceScan() {
  qDebug() << "Bt | Selector | Start device scan.";
  m_selectorOpen = true;
  m_discoveredDevices.clear();
  emit discoveredDevicesChanged();

  // Disconnect any connected synth so the user starts from a clean scan — but
  // WITHOUT blocking the GUI thread. The old waitForFinished() froze the UI for
  // the whole disconnect (~1-2s), which is the "app hangs then the screen comes"
  // when opening the selector while connected. Instead ask the worker to stop
  // and resume scanning once it has finished, via a QFutureWatcher.
  auto peripheral = peripheralHandle();
  if (peripheral.initialized() and peripheral.is_connected()) {
    m_shouldStopBluetoothThread = true;
    if (m_bluetoothThread.isRunning()) {
      // Re-point the watcher at the running worker; when it finishes, clear the
      // stop flag and start the list scan (only if the selector is still open).
      m_disconnectWatcher.disconnect();
      connect(&m_disconnectWatcher, &QFutureWatcher<void>::finished, this, [this]() {
        m_disconnectWatcher.disconnect();
        m_shouldStopBluetoothThread = false;
        if (m_selectorOpen) scanAndConnect();
      });
      m_disconnectWatcher.setFuture(m_bluetoothThread);
      return;
    }
    m_shouldStopBluetoothThread = false;
  }

  scanAndConnect();
}

void BluetoothManager::stopDeviceScan() {
  qDebug() << "Bt | Selector | Stop device scan.";
  m_selectorOpen = false;
}

void BluetoothManager::publishConnectionState(bool connected, const QString& name,
                                              const QString& address, int mtu) {
  {
    QMutexLocker lock(&m_handleMutex);
    m_deviceNameCache = name;
    m_deviceAddressCache = address;
  }
  m_mtu.store(mtu);
  m_connectedCache.store(connected);
}

QString BluetoothManager::getDeviceName() {
  QMutexLocker lock(&m_handleMutex);
  return m_deviceNameCache;
}

QString BluetoothManager::getDeviceAddress() {
  QMutexLocker lock(&m_handleMutex);
  return m_deviceAddressCache;
}

bool BluetoothManager::getConnected() { return m_connectedCache.load(); }

int BluetoothManager::mtu() const { return m_mtu.load(); }

bool BluetoothManager::askForBluetoothPermissionIfNotAvailable() { return true; }

bool BluetoothManager::getScanning() const { return m_scanning; }

void BluetoothManager::setScanning(bool newScanning) {
  if (m_scanning.exchange(newScanning) == newScanning) return;
  // Called from the worker around every scan_for(); `scanning` is bound by the
  // device selector and the toolbar, so the notify has to reach the GUI thread.
  emitOnGuiThread([this]() { emit scanningChanged(); });
}

// Called on the GUI thread (App forwards SynthController::writeToSynth here).
// It must not touch SimpleBLE itself: write_request blocks until the peer
// acknowledges — a full ATT round trip, tens of milliseconds at a typical
// connection interval — and the controller issues these in bursts
// (refreshSequencer sends five, writeSteps up to eleven). Doing that inline
// froze the UI for the length of the burst. Hand it to the write thread and
// return immediately; ordering is preserved because that thread drains its
// event queue in post order.
void BluetoothManager::write(const QByteArray& data, bool withResponse) {
  if (m_writeCtx == nullptr) return;
  QMetaObject::invokeMethod(
      m_writeCtx, [this, data, withResponse]() { writeBlocking(data, withResponse); },
      Qt::QueuedConnection);
}

void BluetoothManager::writeBlocking(const QByteArray& data, bool withResponse) {
  try {
    auto peripheral = peripheralHandle();
    if (peripheral.initialized() and peripheral.is_connected()) {
      SimpleBLE::ByteArray ba(reinterpret_cast<const uint8_t*>(data.constData()),
                              size_t(data.size()));
      if (withResponse) {
        peripheral.write_request(serviceUuidStr, ctrlUuidStr, ba);
      } else {
        peripheral.write_command(serviceUuidStr, ctrlUuidStr, ba);
      }
    }
  } catch (...) {
    qDebug() << "Bt | Write error: peripheral disconnected during write";
  }
}

void BluetoothManager::onBluetoothAvailable() {}

void BluetoothManager::setBluetoothEnabled(bool enabled) {
  if (enabled) {
    m_shouldStopBluetoothThread = false;
    m_rescanTimer.start();
    scanAndConnect();
  } else {
    m_rescanTimer.stop();
    m_shouldStopBluetoothThread = true;
    try {
      auto adapter = adapterHandle();
      if (adapter.initialized() and adapter.scan_is_active()) adapter.scan_stop();
    } catch (...) {
    }
  }
}

void BluetoothManager::finish() {
  m_rescanTimer.stop();
  m_shouldStopBluetoothThread = true;
  try {
    auto adapter = adapterHandle();
    if (adapter.initialized() and adapter.scan_is_active()) adapter.scan_stop();
  } catch (...) {
  }
  if (m_bluetoothThread.isRunning()) {
    m_bluetoothThread.waitForFinished();
  }
  // Terminal (called once from main after exec() returns), so the write thread
  // goes down with it. Drain first: a queued write still holding the last
  // handle would otherwise be destroyed mid-flight.
  if (m_writeThread.isRunning()) {
    m_writeThread.quit();
    m_writeThread.wait();
  }
  delete m_writeCtx;
  m_writeCtx = nullptr;
}

SimpleBLE::Adapter BluetoothManager::adapterHandle() const {
  QMutexLocker lock(&m_handleMutex);
  return m_adapter;
}

void BluetoothManager::setAdapterHandle(SimpleBLE::Adapter adapter) {
  QMutexLocker lock(&m_handleMutex);
  m_adapter = std::move(adapter);
}

SimpleBLE::Peripheral BluetoothManager::peripheralHandle() const {
  QMutexLocker lock(&m_handleMutex);
  return m_foundPeripheral;
}

void BluetoothManager::setPeripheralHandle(SimpleBLE::Peripheral peripheral) {
  QMutexLocker lock(&m_handleMutex);
  m_foundPeripheral = std::move(peripheral);
}
