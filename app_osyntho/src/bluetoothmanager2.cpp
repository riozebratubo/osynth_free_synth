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

// Ceiling on the low-priority (listing/discovery) write lane. A full discovery
// pass is well under a hundred frames, so this only ever engages when the link
// has stopped draining at all — at which point the oldest frames are the least
// worth keeping.
constexpr int kMaxQueuedWrites = 256;

// Consecutive failed CTRL writes before the link is declared lost without
// waiting for Windows to agree. See writeBlocking() for why one is not enough
// and why waiting for a third is expensive.
constexpr int kWriteFailuresBeforeDrop = 2;

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
  // scanAndConnect() checks the bluetooth_enabled setting itself, so the timer
  // does not need its own copy of that gate.
  connect(&m_rescanTimer, &QTimer::timeout, this, [this]() { scanAndConnect(); });
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
  // The user's switch, checked here because this is the single entry point for
  // every scan — the rescan timer, connectToSelectedDevice() and, crucially,
  // startDeviceScan(): opening the device selector with Bluetooth turned off
  // must not put the radio back to work behind an "off" toggle.
  if (Settings::instance().setting("bluetooth_enabled") == "false") {
    qDebug() << "Bt | Scan | Bluetooth is disabled in settings.";
    return;
  }
  // Everything below this point that talks to SimpleBLE does so from the GUI
  // thread, and on Windows *every* SimpleBLE entry point — including the two
  // that look like plain getters, bluetooth_enabled() and is_connected() — is
  // a blocking hop onto one process-wide MTA thread (see MtaManager). That one
  // thread also carries the BLE worker's 2 s scan_for(), the whole connect +
  // service-discovery sequence, and every CTRL write, and it runs them
  // strictly one at a time. A GUI-thread call therefore does not cost a WinRT
  // round trip; it costs whatever the worker happens to be in the middle of —
  // which is seconds on a reconnect, and up to ten on a write into a link that
  // has stopped answering (async_get's timeout). The rescan timer fires this
  // function every 2 s for the whole session, so that was a guaranteed
  // multi-second UI freeze on every reconnect: the app "hangs after
  // discovery".
  //
  // So: the cheap cached tests first, and they are enough. A worker is alive
  // for its whole lifetime — scanning, connecting AND while connected (the
  // connectAndSubscribe keep-alive loop) — so while one is running there is
  // nothing here to do anyway, and the GUI thread makes no SimpleBLE call at
  // all. scanAndConnect() is only ever called on the main thread, so this also
  // makes spawning a second concurrent worker structurally impossible.
  if (m_bluetoothThread.isRunning()) return;
  if (m_adapterIsConnecting) return;
  // Replaces a peripheral.is_connected() probe that used to sit here. Same
  // answer from the cache the worker publishes, without the hop — and by this
  // line there is no worker running, so a live connection cannot exist.
  if (m_connectedCache.load()) return;

  if (not SimpleBLE::Adapter::bluetooth_enabled()) {
    qDebug() << "Bt | Scan | Will not scan and connect, bluetooth is not enabled!";
    return;
  }

  auto adapter = adapterHandle();
  if (adapter.initialized() and adapter.scan_is_active()) return;

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
  // SimpleBLE's mtu() is the usable ATT payload, not the ATT MTU (see below).
  qDebug() << "Bt | Successfully connected. ATT payload:" << peripheral.mtu();

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
  // IBluetoothManager::mtu() is contracted to report the *ATT MTU*, which is
  // what SynthProto::attPayloadFor() takes the 3-byte ATT header off. SimpleBLE
  // reports the usable payload instead — every backend subtracts the header
  // itself (PeripheralWindows::mtu() returns `mtu_ - 3`, and the Linux/BlueZ
  // one does the same) — so without adding it back the header came off twice
  // and every frame was packed 3 bytes short of what the link could carry.
  int negotiatedMtu = 0;
  try {
    const int attPayload = int(peripheral.mtu());
    if (attPayload > 0) negotiatedMtu = attPayload + 3;
  } catch (...) {
  }
  // Link-loss detection, armed before the connection is announced: both flags
  // still carry the *previous* session's verdict, and drainWriteQueue() reads
  // them against the cached state publishConnectionState() is about to set. The
  // callback goes in ahead of the announcement too, so a disconnect between the
  // two is not swallowed.
  m_peripheralDropped.store(false);
  m_writeFailures.store(0);
  peripheral.set_callback_on_disconnected([this]() {
    qDebug() << "Bt | Peripheral disconnected (callback)";
    m_peripheralDropped.store(true);
  });

  publishConnectionState(true, name, address, negotiatedMtu);

  emit updateConnectedBluetoothDevice(name, address);
  emitOnGuiThread([this]() {
    emit deviceNameChanged();
    emit deviceAddressChanged();
    emit connectedChanged(true);
  });

  // Keep-alive: the worker parks here while connected, polling for a disconnect
  // or a stop request (startDeviceScan/finish block on this poll).
  while (true) {
    bool stillConnected = false;

    // The flag first, and if it is set the poll is skipped entirely. Two
    // reasons, and the ordering matters for both.
    //
    // Correctness: peripheral.is_connected() is not reliable on every backend —
    // with the adapter switched off mid-session the Windows one can keep
    // reporting a cached `true`, so the loop would park forever, the app would
    // never publish connectedChanged(false), and SynthController::setConnected(true)
    // on the way back would early-return on the unchanged value, skipping every
    // reset and reusing stale discovery state for the session. This is the
    // desktop counterpart of the Android link-loss fix (bluetoothmanager.cpp:
    // handleLinkLost); the callback fires in that case, so it is authoritative.
    //
    // Cost: is_connected() is a blocking hop onto the one MTA thread, which is
    // also where writeBlocking() sits. A write into a link that has stopped
    // answering takes ten seconds to time out there (async_get), so a poll
    // queued behind one is a poll that learns nothing for ten seconds — on
    // exactly the link that has already been declared dead. Reading the flag
    // costs nothing and is not queued behind anything.
    if (m_peripheralDropped.load()) {
      stillConnected = false;
    } else {
      try {
        stillConnected = peripheral.is_connected();
      } catch (...) {
        stillConnected = false;
      }
    }

    if (not stillConnected or m_shouldStopBluetoothThread) {
      qDebug() << "Bt | Thread will quit";
      setPeripheralHandle(SimpleBLE::Peripheral{});

      // Published *before* the SimpleBLE teardown, not after. unsubscribe() and
      // disconnect() are two more MTA calls, and on a link that has stopped
      // answering they queue behind whatever writes are still draining — each
      // of which costs its own ten-second timeout. Announcing the disconnect
      // first makes drainWriteQueue() throw the queued frames away on its next
      // step (it bails on the cached state) and stops SynthController handing
      // it new ones, so the teardown below is not paid for one stale frame at a
      // time. The app's view of the link is already correct at this point: it
      // is gone, whether or not Windows has finished agreeing.
      publishConnectionState(false, QString(), QString(), 0);
      emitOnGuiThread([this]() {
        emit connectedChanged(false);
        emit deviceNameChanged();
        emit deviceAddressChanged();
      });

      try {
        peripheral.unsubscribe(serviceUuidStr, evtUuidStr);
      } catch (...) {
      }
      try {
        peripheral.disconnect();
      } catch (...) {
      }
      setAdapterHandle(SimpleBLE::Adapter{});
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

  // Cached state only, like scanAndConnect(): this runs on the GUI thread, and
  // a peripheral.is_connected() here would block the UI for however long the
  // MTA thread is busy — which, on the Connect button of a synth that has just
  // stopped answering, is the worst possible moment for it.
  auto adapter = adapterHandle();
  if (adapter.initialized() and adapter.scan_is_active()) {
    qDebug() << "Bt | connectToSelectedDevice | Stopping scan, will connect on the next one.";
    m_skipScanGoConnect = true;
  } else if (not m_adapterIsConnecting and not m_connectedCache.load()) {
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
  //
  // The test is the cached flag for the same reason: peripheral.is_connected()
  // is a blocking hop onto the shared MTA thread, so asking it here reintroduced
  // exactly the freeze this function was rewritten to remove — worst on a link
  // that has stopped answering, which is when the user reaches for the selector.
  if (m_connectedCache.load()) {
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
// froze the UI for the length of the burst. Queue it and return immediately;
// the write thread does the blocking part.
void BluetoothManager::write(const QByteArray& data, bool withResponse) {
  if (m_writeCtx == nullptr || data.isEmpty()) return;
  // Byte 0 is the opcode (SynthProto frame header).
  const bool live = SynthProto::isRealtimeOp(quint8(data.at(0)));
  {
    QMutexLocker lock(&m_writeQueueMutex);
    (live ? m_writeHigh : m_writeLow).append(PendingWrite{data, withResponse});
    // A link that has gone unresponsive must not grow this without bound: the
    // writes are already stale by the time it recovers. Live gestures are not
    // capped — that lane only ever holds a handful of frames, and dropping a
    // note-off is the one thing this whole path exists to prevent.
    while (m_writeLow.size() > kMaxQueuedWrites) m_writeLow.removeFirst();
  }
  QMetaObject::invokeMethod(
      m_writeCtx, [this]() { drainWriteQueue(); }, Qt::QueuedConnection);
}

// One posted step per queued frame, each taking the highest-priority frame
// pending at the moment it runs — so a note enqueued during a discovery burst
// goes out on the next step rather than after the rest of the burst.
void BluetoothManager::drainWriteQueue() {
  PendingWrite next;
  {
    QMutexLocker lock(&m_writeQueueMutex);
    // Nothing queued for a link that is down: the firmware runs
    // voice_manager_all_notes_off() on disconnect, and the listings would be
    // answered into a closed connection.
    //
    // m_peripheralDropped as well as the cached flag, because it is set first:
    // the keep-alive loop needs up to its 100 ms poll period to notice and
    // publish, and every frame written into that window costs its own
    // ten-second timeout on the MTA thread the teardown itself has to queue on.
    if (!m_connectedCache.load() || m_peripheralDropped.load()) {
      m_writeHigh.clear();
      m_writeLow.clear();
      return;
    }
    if (!m_writeHigh.isEmpty()) {
      next = m_writeHigh.takeFirst();
    } else if (!m_writeLow.isEmpty()) {
      next = m_writeLow.takeFirst();
    } else {
      return;
    }
  }
  writeBlocking(next.data, next.withResponse);
}

void BluetoothManager::writeBlocking(const QByteArray& data, bool withResponse) {
  try {
    auto peripheral = peripheralHandle();
    // Deliberately no is_connected() probe: that is itself a blocking hop onto
    // the WinRT MTA thread, so it doubled the serialised work per frame on the
    // one path that must stay quick. drainWriteQueue() has already checked the
    // cached state, and a peripheral that drops mid-write throws — which is
    // what the catch below is for.
    if (peripheral.initialized()) {
      SimpleBLE::ByteArray ba(reinterpret_cast<const uint8_t*>(data.constData()),
                              size_t(data.size()));
      if (withResponse) {
        peripheral.write_request(serviceUuidStr, ctrlUuidStr, ba);
      } else {
        peripheral.write_command(serviceUuidStr, ctrlUuidStr, ba);
      }
      m_writeFailures.store(0);
    }
  } catch (...) {
    // Windows does not fail a write into a peer that has gone quiet; it accepts
    // it and lets SimpleBLE's async_get give up ten seconds later, so each of
    // these cost ten seconds of the one MTA thread the keep-alive poll, the
    // teardown and every other write also have to queue on. ConnectionStatusChanged
    // has been seen half a minute behind that, which is a queue of stale frames
    // metered out at ten seconds each while the app still believes it is
    // connected — and the UI freezing on every GUI-thread SimpleBLE call in the
    // meantime.
    //
    // Two in a row is the app's own answer to a question Windows is still
    // thinking about. One is not enough to act on — a single frame can fail on
    // a link that is fine — but a second consecutive failure means nothing has
    // reached the synth for at least ten seconds, on a link whose supervision
    // timeout is four. Hand it to the keep-alive loop, which owns the teardown.
    const int failures = m_writeFailures.fetch_add(1) + 1;
    qDebug() << "Bt | Write error: peripheral disconnected during write;"
             << failures << "in a row";
    if (failures >= kWriteFailuresBeforeDrop && !m_peripheralDropped.exchange(true)) {
      qDebug() << "Bt | Link presumed lost after" << failures
               << "consecutive write failures; tearing down.";
    }
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
  // goes down with it. Empty the lanes first so a drain step still sitting in
  // its event queue finds nothing to write into a torn-down stack.
  {
    QMutexLocker lock(&m_writeQueueMutex);
    m_writeHigh.clear();
    m_writeLow.clear();
  }
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
