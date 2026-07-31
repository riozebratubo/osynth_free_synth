#include "bluetoothmanager.h"

#include <QBluetoothPermission>
#include <QDebug>
#include <QGuiApplication>
#include <QLocationPermission>
#include <QLowEnergyConnectionParameters>
#include <QTimer>

#include "src/ble/synthprotocol.h"
#include "src/business/database.h"
#include "src/business/settings.h"

/*
 * On Windows this backend is not built (SimpleBLE is used instead). On Windows
 * with the WinRT Qt backend, subscribing can be broken — set
 * BLUETOOTH_FORCE_DBUS_LE_VERSION=1. See QTBUG-89723.
 */

// Releases the write queue when a completion never arrives. The short one is
// for writes issued without an acknowledgement, which settle in the stack
// rather than over the air.
constexpr int kWriteWatchdogMs = 1500;
constexpr int kFastWriteWatchdogMs = 250;

// osynth GATT service + characteristics (see docs/BLE_PROTOCOL.md).
static const QBluetoothUuid serviceUuid{QString(SynthProto::kServiceUuid)};
static const QBluetoothUuid ctrlUuid{QString(SynthProto::kCtrlUuid)};   // Write / Write-no-response
static const QBluetoothUuid evtUuid{QString(SynthProto::kEvtUuid)};     // Notify
static const QBluetoothUuid infoUuid{QString(SynthProto::kInfoUuid)};   // Read

BluetoothManager& BluetoothManager::instance() {
  static BluetoothManager myInstance;
  return myInstance;
}

BluetoothManager::BluetoothManager(QObject* parent)
  : IBluetoothManager{parent},
    localDevice{new QBluetoothLocalDevice{}},
    discoveryAgent{nullptr},
    bleController{nullptr},
    service{nullptr},
    t_deviceAddress{},
    t_remoteDeviceInfo{},
    m_isConnected{false},
    deviceQueue{},
    m_scanning{false},
    addressToDeviceMap{} {
  // Generous: it only has to beat "never", and firing it early would issue a
  // second write while the first is genuinely still in flight — the exact
  // thing the queue exists to prevent. pumpWriteQueue() picks the interval per
  // frame (an unacknowledged write settles far sooner than a round trip).
  m_writeWatchdog.setSingleShot(true);
  m_writeWatchdog.setInterval(kWriteWatchdogMs);
  connect(&m_writeWatchdog, &QTimer::timeout, this, [this]() {
    if (!m_writeInFlight) return;
    qWarning() << "bt | CTRL write never completed; releasing the queue";
    onWriteSettled();
  });

  if (Settings::instance().setting("bluetooth_enabled") != "false") {
    initializeBt();
  }
}

void BluetoothManager::initializeBt() {
  // The user's switch is authoritative at every entry point that brings the
  // stack up, not just in the constructor. startDeviceScan() calls in here
  // whenever the manager is not running, so without this, opening the device
  // selector with Bluetooth turned off silently restarted the scan and the
  // auto-connect loop — and left m_enabled true for the rest of the session,
  // behind a toggle still reading "off".
  //
  // Safe on the enable path: App::setBluetoothEnabled() writes the setting
  // before it calls into the manager, and requestPermissions() only emits
  // bluetoothAvailable() when the app-side flag is on.
  if (Settings::instance().setting("bluetooth_enabled") == "false") {
    qDebug() << "bt | Bt is disabled in settings; not initializing.";
    return;
  }

  bool btAllowed = hasAllBluetoothPermissions();
  if (btAllowed) {
    if (localDevice != nullptr) localDevice->deleteLater();
    localDevice = new QBluetoothLocalDevice{};
  }
  qDebug() << "bt | Bt available: " << localDevice->isValid()
           << " allowed: " << btAllowed;
  if (localDevice->isValid() and hasAllBluetoothPermissions()) {
    // Create the discovery agent once. initializeBt() is re-entrant (ctor,
    // onBluetoothAvailable(), setBluetoothEnabled(true)); recreating the agent
    // here would leak the previous one and could leave two scanning at once.
    if (discoveryAgent == nullptr) {
      discoveryAgent = new QBluetoothDeviceDiscoveryAgent();

      const int scanTime = Settings::instance().setting("bluetooth_scan_time").toInt();
      discoveryAgent->setLowEnergyDiscoveryTimeout(scanTime * 1000);

      connect(discoveryAgent, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered, this,
              &BluetoothManager::onDeviceDiscovered);
      connect(discoveryAgent, &QBluetoothDeviceDiscoveryAgent::errorOccurred, this,
              &BluetoothManager::onErrorOccurred);
      connect(discoveryAgent, &QBluetoothDeviceDiscoveryAgent::finished, this,
              &BluetoothManager::onScanFinished);
      connect(discoveryAgent, &QBluetoothDeviceDiscoveryAgent::canceled, this,
              &BluetoothManager::onScanCanceled);
    }

    // Seed the queue with previously-connected devices (deduped).
    for (auto& address : Database::instance().getLastConnectedDevices()) {
      if (not deviceQueue.contains(address)) deviceQueue.append(address);
    }

    m_enabled = true;
    idleAction();
  } else {
    qDebug() << "bt | Bt will not initialize now";
  }
}

void BluetoothManager::startScanning() {
  if (not m_enabled or discoveryAgent == nullptr) return;
  if (m_isConnected) return;
  if (bleController and bleController->state() != QLowEnergyController::UnconnectedState) {
    qDebug() << "bt | Connection attempt in progress, will not start scanning.";
    return;
  }
  qDebug() << "bt | Starting scan...";
  if (not m_selectorOpen) {
    m_discoveredDevices.clear();
    emit discoveredDevicesChanged();
  }
  const int scanTime = Settings::instance().setting("bluetooth_scan_time").toInt();
  discoveryAgent->setLowEnergyDiscoveryTimeout(scanTime * 1000);
  discoveryAgent->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
  setScanning(true);
}

void BluetoothManager::stopScanning() {
  if (discoveryAgent) discoveryAgent->stop();
  setScanning(false);
}

void BluetoothManager::stopScanningAndDiscover() {
  if (discoveryAgent) discoveryAgent->stop();
  setScanning(false);
  QTimer::singleShot(2000, this, &BluetoothManager::startDiscover);
}

void BluetoothManager::stopScanningAndRescan() {
  if (discoveryAgent) discoveryAgent->stop();
  setScanning(false);
  QTimer::singleShot(2000, this, &BluetoothManager::startScanning);
}

void BluetoothManager::idleAction() {
  if (not m_enabled or discoveryAgent == nullptr) return;
  if (m_isConnected) return;

  if (m_selectorOpen) {
    // Selector open: stay in scan-to-list mode.
    stopScanningAndRescan();
    return;
  }

  const QString useSelected = Settings::instance().setting("bluetooth_use_selected");
  if (useSelected == "true") {
    const QString selectedAddr = Settings::instance().setting("bluetooth_selected_device_address");
    if (not selectedAddr.isEmpty()) {
      deviceQueue.clear();
      deviceQueue.push_front(selectedAddr);
      stopScanningAndDiscover();
      return;
    }
  }

  if (not deviceQueue.empty()) {
    stopScanningAndDiscover();
  } else {
    stopScanningAndRescan();
  }
}

QString BluetoothManager::getDeviceName() { return m_deviceName; }

QString BluetoothManager::getDeviceAddress() { return m_deviceAddress; }

bool BluetoothManager::getConnected() { return m_isConnected; }

bool BluetoothManager::askForBluetoothPermissionIfNotAvailable() {
  return hasAllBluetoothPermissions();
}

void BluetoothManager::onBluetoothAvailable() { initializeBt(); }

void BluetoothManager::setBluetoothEnabled(bool enabled) {
  if (enabled) {
    onBluetoothAvailable();
  } else {
    finish();
  }
}

bool BluetoothManager::hasAllBluetoothPermissions() {
#if QT_CONFIG(permissions)
  auto checkBt = []() {
    QBluetoothPermission p;
    // We are a BLE central: request central-role access only. The default
    // (Access | Advertise) would also demand BLUETOOTH_ADVERTISE, which the app
    // neither declares nor needs — making the check fail even when the user has
    // granted scan/connect.
    p.setCommunicationModes(QBluetoothPermission::Access);
    return qApp->checkPermission(p) == Qt::PermissionStatus::Granted;
  };
  auto checkLoc = []() {
    QLocationPermission p;
    return qApp->checkPermission(p) == Qt::PermissionStatus::Granted;
  };
#if defined(Q_OS_MACOS)
  return checkBt();
#else
  // Android: BLUETOOTH_SCAN is declared WITHOUT neverForLocation, so the OS
  // still needs location granted to return scan results — keep both gates.
  return checkBt() and checkLoc();
#endif
#else
  return true;
#endif
}

void BluetoothManager::onErrorOccurred(QBluetoothDeviceDiscoveryAgent::Error error) {
  qDebug() << "bt | Scan | Error: " << (int)error;
  setScanning(false);
  // A scan error emits neither finished nor canceled; retry the loop.
  QTimer::singleShot(5000, this, &BluetoothManager::idleAction);
}

QVariantList BluetoothManager::getDiscoveredDevices() const { return m_discoveredDevices; }

// Qt has no API to *request* an MTU — the platform negotiates it (Android's
// backend asks for 517, BlueZ exchanges on connect) — so this only reports
// what was agreed. 0 while unconnected, which the controller reads as
// "assume the documented 247".
int BluetoothManager::mtu() const {
  return bleController ? bleController->mtu() : 0;
}

void BluetoothManager::connectToSelectedDevice() {
  const QString selectedAddr = Settings::instance().setting("bluetooth_selected_device_address");
  if (selectedAddr.isEmpty()) return;

  m_selectorOpen = false;
  if (discoveryAgent and discoveryAgent->isActive()) {
    discoveryAgent->stop();
    setScanning(false);
  }
  deviceQueue.clear();
  deviceQueue.push_front(selectedAddr);
  QTimer::singleShot(500, this, &BluetoothManager::startDiscover);
}

void BluetoothManager::startDeviceScan() {
  qDebug() << "bt | Selector | Start device scan.";
  m_selectorOpen = true;
  m_discoveredDevices.clear();
  emit discoveredDevicesChanged();

  if (discoveryAgent == nullptr or not m_enabled) {
    initializeBt();
    return;
  }

  if (m_isConnected) setIsConnected(false);
  teardownConnection();
  deviceQueue.clear();
  startScanning();
}

void BluetoothManager::stopDeviceScan() {
  qDebug() << "bt | Selector | Stop device scan.";
  m_selectorOpen = false;
}

void BluetoothManager::onDeviceDiscovered(const QBluetoothDeviceInfo& device) {
  if (not(device.coreConfigurations() & QBluetoothDeviceInfo::LowEnergyCoreConfiguration)) return;

  const QString deviceAddress = device.address().toString();

  // Dedupe by address for the selector list.
  bool alreadyListed = false;
  for (const auto& listed : m_discoveredDevices) {
    if (listed.toMap().value("address").toString() == deviceAddress) {
      alreadyListed = true;
      break;
    }
  }
  if (not alreadyListed) {
    m_discoveredDevices.append(QVariantMap{{"name", device.name()}, {"address", deviceAddress}});
    emit discoveredDevicesChanged();
  }

  addressToDeviceMap[deviceAddress] = device;

  // While the selector is open we only list devices; never auto-queue.
  if (m_selectorOpen) return;

  const QString useSelected = Settings::instance().setting("bluetooth_use_selected");
  const QString selectedAddress = Settings::instance().setting("bluetooth_selected_device_address");
  if (useSelected == "true" and not selectedAddress.isEmpty()) {
    if (deviceAddress != selectedAddress) return;
    if (not deviceQueue.contains(deviceAddress)) deviceQueue.push_front(deviceAddress);
    return;
  }

  const QString prefix = Settings::instance().setting("bluetooth_prefix");
  if (device.name().startsWith(prefix, Qt::CaseInsensitive)) {
    if (not deviceQueue.contains(deviceAddress)) deviceQueue.push_front(deviceAddress);
  }
}

void BluetoothManager::onScanFinished() {
  setScanning(false);
  idleAction();
}

void BluetoothManager::onScanCanceled() {
  // A deliberate stop()/cancel, NOT a natural timeout. Every caller of stop()
  // already schedules its own next step, so do NOT re-enter idleAction() here.
  setScanning(false);
}

void BluetoothManager::startDiscover() {
  if (not m_enabled) return;
  if (not deviceQueue.isEmpty()) {
    stopScanning();
    QString address = deviceQueue.front();
    deviceQueue.pop_front();
    discover(address);
  } else {
    QTimer::singleShot(2000, this, &BluetoothManager::idleAction);
  }
}

void BluetoothManager::setIsConnected(bool is) {
  m_isConnected = is;
  if (not is) {
    if (not m_deviceName.isEmpty()) {
      m_deviceName.clear();
      emit deviceNameChanged();
    }
    if (not m_deviceAddress.isEmpty()) {
      m_deviceAddress.clear();
      emit deviceAddressChanged();
    }
  }
  emit connectedChanged(is);
}

void BluetoothManager::teardownConnection() {
  // Before the service goes: anything still queued belongs to a link that is
  // ending, and the firmware runs voice_manager_all_notes_off() on disconnect.
  clearWriteQueue();
  if (service) {
    service->disconnect(this);
    service->deleteLater();
    service = nullptr;
  }
  if (bleController) {
    bleController->disconnect(this);
    if (bleController->state() != QLowEnergyController::UnconnectedState) {
      bleController->disconnectFromDevice();
    }
    bleController->deleteLater();
    bleController = nullptr;
  }
}

void BluetoothManager::discover(QString deviceAddress) {
  qDebug() << "bt | Discover: " << deviceAddress;

  teardownConnection();

  t_deviceAddress = QBluetoothAddress(deviceAddress);
  if (addressToDeviceMap.contains(deviceAddress)) {
    t_remoteDeviceInfo = addressToDeviceMap[deviceAddress];
  } else {
    t_remoteDeviceInfo = QBluetoothDeviceInfo(t_deviceAddress, QString{}, 0);
  }

  m_attemptHandled = false;
  bleController = QLowEnergyController::createCentral(t_remoteDeviceInfo, this);

  connect(bleController, &QLowEnergyController::connected, this, [this]() {
    qDebug() << "bt | Connected, discovering services. MTU:"
             << (bleController ? bleController->mtu() : 0);
    if (bleController) bleController->discoverServices();
  });

  connect(bleController, &QLowEnergyController::disconnected, this, [this]() {
    const bool wasConnected = m_isConnected;
    setIsConnected(false);
    if (not wasConnected and m_attemptHandled) return;
    m_attemptHandled = true;
    idleAction();
  });

  connect(bleController, &QLowEnergyController::errorOccurred, this,
          [this](QLowEnergyController::Error error) {
            qDebug() << "bt | Controller error:" << error;
            if (m_isConnected) return;
            if (m_attemptHandled) return;
            m_attemptHandled = true;
            if (deviceQueue.isEmpty()) {
              idleAction();
            } else {
              QTimer::singleShot(1000, this, &BluetoothManager::startDiscover);
            }
          });

  connect(bleController, &QLowEnergyController::serviceDiscovered, this,
          [this](const QBluetoothUuid& discoveredServiceUuid) {
            if (discoveredServiceUuid != serviceUuid) return;
            if (not bleController) return;

            if (service) {
              service->disconnect(this);
              service->deleteLater();
              service = nullptr;
            }
            service = bleController->createServiceObject(discoveredServiceUuid, this);
            if (not service) return;

            connect(service, &QLowEnergyService::stateChanged, this,
                    [this](QLowEnergyService::ServiceState st) {
                      if (st == QLowEnergyService::RemoteServiceDiscovered) {
                        onServiceDetailsDiscovered();
                      }
                    });
            connect(service, &QLowEnergyService::characteristicChanged, this,
                    [this](const QLowEnergyCharacteristic& c, const QByteArray& value) {
                      if (c.uuid() == evtUuid) emit receivedData(value);
                    });
            connect(service, &QLowEnergyService::characteristicRead, this,
                    [this](const QLowEnergyCharacteristic& c, const QByteArray& value) {
                      if (c.uuid() == infoUuid) emit infoRead(value);
                    });

            // The write queue's clock. Without this the queue would only ever
            // advance on its watchdog.
            connect(service, &QLowEnergyService::characteristicWritten, this,
                    [this](const QLowEnergyCharacteristic& c, const QByteArray&) {
                      if (c.uuid() == ctrlUuid) onWriteSettled();
                    });

            // Nothing used to listen here, which is why a refused write was
            // invisible: the frame was dropped, the firmware never saw it, and
            // the app went on waiting for a response that could not come.
            connect(service, &QLowEnergyService::errorOccurred, this,
                    [this](QLowEnergyService::ServiceError e) {
                      qWarning() << "bt | Service error:" << e;
                      if (e == QLowEnergyService::CharacteristicWriteError) {
                        // The frame is gone. Release the queue so the rest of
                        // it still goes out — the controller retries what
                        // matters off the BUSY/timeout paths it already has.
                        onWriteSettled();
                      }
                    });

            service->discoverDetails();
          });

  connect(bleController, &QLowEnergyController::discoveryFinished, this, [this]() {
    if (service != nullptr) return;  // service found; awaiting characteristic discovery
    // No osynth service on this device: move to the next candidate, or idle.
    if (not deviceQueue.isEmpty()) {
      auto next = deviceQueue.front();
      deviceQueue.pop_front();
      discover(next);
    } else {
      QTimer::singleShot(5000, this, &BluetoothManager::idleAction);
    }
  });

  bleController->connectToDevice();
}

void BluetoothManager::onServiceDetailsDiscovered() {
  if (not service) return;

  const QLowEnergyCharacteristic ctrlChar = service->characteristic(ctrlUuid);
  const QLowEnergyCharacteristic evtChar = service->characteristic(evtUuid);
  if (not ctrlChar.isValid() or not evtChar.isValid()) {
    qDebug() << "bt | osynth CTRL/EVT missing; abandoning this device.";
    teardownConnection();
    idleAction();
    return;
  }

  m_deviceName = t_remoteDeviceInfo.name();
  m_deviceAddress = t_remoteDeviceInfo.address().toString();

  // Read INFO (async -> characteristicRead -> infoRead signal).
  const QLowEnergyCharacteristic infoChar = service->characteristic(infoUuid);
  if (infoChar.isValid()) service->readCharacteristic(infoChar);

  // Enable EVT notifications (the firmware drops outgoing frames while
  // unsubscribed). The controller waits ~300ms before its first write, so the
  // CCCD write has landed by then.
  const QLowEnergyDescriptor cccd =
      evtChar.descriptor(QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);
  if (cccd.isValid()) service->writeDescriptor(cccd, QByteArray::fromHex("0100"));

#ifdef Q_OS_ANDROID
  // Ask for a connection interval a keyboard can be played on. Android drops
  // to 30-50 ms once discovery is done, and since a frame only leaves on a
  // connection event that interval is the floor under every note's latency.
  // Qt maps a sub-30 ms request onto BluetoothGatt.requestConnectionPriority
  // (CONNECTION_PRIORITY_HIGH, ~11-15 ms). The firmware asks for the same
  // thing from its end when it sees the EVT subscription — either side
  // getting through is enough, and centrals treat their own app's request as
  // the more authoritative one.
  if (bleController) {
    QLowEnergyConnectionParameters fast;
    fast.setIntervalRange(7.5, 15.0);
    fast.setLatency(0);
    fast.setSupervisionTimeout(4000);
    bleController->requestConnectionUpdate(fast);
  }
#endif

  emit deviceNameChanged();
  emit deviceAddressChanged();
  emit updateConnectedBluetoothDevice(m_deviceName, m_deviceAddress);
  setIsConnected(true);
}

void BluetoothManager::write(const QByteArray& data, bool withResponse) {
  if (not service or data.isEmpty()) return;
  // Byte 0 is the opcode (SynthProto frame header). Live gestures skip the
  // listing backlog; see the note on the lanes in the header.
  const bool live = SynthProto::isRealtimeOp(quint8(data.at(0)));
  (live ? m_writeHigh : m_writeLow).append(PendingWrite{data, withResponse});
  // A link that has stopped draining must not accumulate stale listing frames.
  // The live lane is never capped — dropping a note-off is the one outcome
  // this whole path exists to prevent.
  while (m_writeLow.size() > 256) m_writeLow.removeFirst();
  pumpWriteQueue();
}

void BluetoothManager::pumpWriteQueue() {
  if (m_writeInFlight) return;
  if (not service) {
    clearWriteQueue();
    return;
  }
  const QLowEnergyCharacteristic ctrlChar = service->characteristic(ctrlUuid);
  if (not ctrlChar.isValid()) {
    clearWriteQueue();
    return;
  }

  PendingWrite next;
  if (not m_writeHigh.isEmpty()) {
    next = m_writeHigh.takeFirst();
  } else if (not m_writeLow.isEmpty()) {
    next = m_writeLow.takeFirst();
  } else {
    return;
  }

  m_writeInFlight = true;
#ifdef Q_OS_ANDROID
  // A frame the caller did not want acknowledged goes out unacknowledged. It
  // is still paced by this queue, but it settles as soon as the stack has
  // handed the frame to the controller instead of after a full ATT round trip
  // with the synth. That round trip is the whole of a played note's latency:
  // with it, notes leave at best one per connection interval and a chord
  // audibly arpeggiates — which is what made the on-screen keyboard feel
  // unresponsive once every frame became WriteWithResponse.
  //
  // Safe because of two things in Qt's Android backend (verified in 6.11):
  // handleOnCharacteristicWrite() does not branch on write type, so
  // characteristicWritten still arrives and this queue keeps its clock; and
  // QtBluetoothLE.java has a job queue of its own, so a frame issued while
  // another is genuinely outstanding is queued rather than refused. The
  // shorter watchdog is therefore a formality — and cheap to wait out.
  //
  // Android only. The other backends this file builds for (BlueZ,
  // CoreBluetooth) do not report no-response writes at all, so there would be
  // nothing to advance the queue: they stay on WriteWithResponse.
  const bool ack = next.withResponse;
  m_writeWatchdog.start(ack ? kWriteWatchdogMs : kFastWriteWatchdogMs);
  service->writeCharacteristic(ctrlChar, next.data,
                               ack ? QLowEnergyService::WriteWithResponse
                                   : QLowEnergyService::WriteWithoutResponse);
#else
  m_writeWatchdog.start(kWriteWatchdogMs);
  service->writeCharacteristic(ctrlChar, next.data,
                               QLowEnergyService::WriteWithResponse);
#endif
}

void BluetoothManager::onWriteSettled() {
  m_writeWatchdog.stop();
  m_writeInFlight = false;
  pumpWriteQueue();
}

void BluetoothManager::clearWriteQueue() {
  m_writeWatchdog.stop();
  m_writeInFlight = false;
  m_writeHigh.clear();
  m_writeLow.clear();
}

void BluetoothManager::finish() {
  qDebug() << "bt | Finish: stop scanning and disconnect.";
  m_enabled = false;
  if (discoveryAgent and discoveryAgent->isActive()) discoveryAgent->stop();
  setScanning(false);
  teardownConnection();
  if (m_isConnected) setIsConnected(false);
  deviceQueue.clear();
}

bool BluetoothManager::getScanning() const { return m_scanning; }

void BluetoothManager::setScanning(bool newScanning) {
  if (m_scanning == newScanning) return;
  m_scanning = newScanning;
  emit scanningChanged();
}
