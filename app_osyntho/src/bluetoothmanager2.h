#ifndef BLUETOOTHMANAGER2H_H
#define BLUETOOTHMANAGER2H_H

#include <QByteArray>
#include <QFuture>
#include <QFutureWatcher>
#include <QMutex>
#include <QObject>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <utility>

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
  // The first three are QML property READ accessors, so they run on every
  // binding re-evaluation. All four serve cached state rather than calling
  // into SimpleBLE: every SimpleBLE call is a synchronous hop onto the WinRT
  // MTA thread, which is not something a paint pass should be doing.
  bool getConnected() override;
  QString getDeviceName() override;
  QString getDeviceAddress() override;
  int mtu() const override;
  QVariantList getDiscoveredDevices() const override;

  Q_INVOKABLE void connectToSelectedDevice() override;
  // Device-selector screen entry/exit (shared QML). startDeviceScan()
  // disconnects any connected synth and enters scan-to-list mode: while the
  // selector is open the manager only scans and lists nearby devices (never
  // auto-connects). stopDeviceScan() leaves that mode.
  Q_INVOKABLE void startDeviceScan() override;
  Q_INVOKABLE void stopDeviceScan() override;

 public slots:
  void write(const QByteArray& data, bool withResponse) override;
  void onBluetoothAvailable() override;
  void setBluetoothEnabled(bool enabled) override;

 private:
  explicit BluetoothManager(QObject* parent = nullptr);

  void scanAndConnect();
  bool connectAndSubscribe(SimpleBLE::Peripheral& peripheral);

  // Every signal this class declares is either a QML NOTIFY (deviceName,
  // connected, scanning…) or is consumed by App. QML delivers a NOTIFY to its
  // bindings *synchronously, on the emitting thread* — so emitting one from
  // the BLE worker would evaluate JS bindings off the GUI thread. Everything
  // the worker announces therefore goes through this.
  template <typename Fn>
  void emitOnGuiThread(Fn&& fn) {
    QMetaObject::invokeMethod(this, std::forward<Fn>(fn), Qt::QueuedConnection);
  }

  // The actual SimpleBLE write, run on m_writeThread. write_request blocks
  // until the peer acknowledges, and even write_command is a synchronous hop
  // onto the WinRT MTA thread, so neither may run on the GUI thread.
  void writeBlocking(const QByteArray& data, bool withResponse);

  // Takes the next frame off the priority queue and writes it. Runs on
  // m_writeThread, once per frame write() enqueued.
  void drainWriteQueue();

  // Mirrors of the peripheral's state for the QML property accessors, written
  // by the worker as the connection comes up and goes down.
  void publishConnectionState(bool connected, const QString& name,
                              const QString& address, int mtu);

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

  // "The link is gone" — the keep-alive loop's authoritative signal, read
  // before its own peripheral.is_connected() poll (see the loop for why that
  // poll is neither reliable nor free). Cleared before each loop; set by
  // SimpleBLE's on_disconnected callback, which fires on a SimpleBLE thread and
  // so cannot do the teardown itself, and by writeBlocking() once consecutive
  // write failures say the peer has stopped answering. Desktop counterpart of
  // the Android link-loss fix in bluetoothmanager.cpp (handleLinkLost).
  std::atomic<bool> m_peripheralDropped{false};

  // Consecutive writeBlocking() failures on the current connection, reset by
  // any write that goes through. Windows keeps accepting writes into a link the
  // peer has already stopped answering and only fails them on async_get's
  // ten-second timeout, so this is the app's own evidence that the link is
  // dead — usually well before ConnectionStatusChanged says so.
  std::atomic<int> m_writeFailures{0};

  // Cached connection state, so the QML accessors never call into SimpleBLE.
  // The strings ride m_handleMutex with the handles they describe.
  std::atomic<bool> m_connectedCache{false};
  std::atomic<int> m_mtu{0};
  QString m_deviceNameCache;
  QString m_deviceAddressCache;

  // Serialises CTRL writes off the GUI thread. A plain QObject on its own
  // thread: invokeMethod posts a drain step per frame, so the GUI thread never
  // waits on the radio.
  QThread m_writeThread;
  QObject* m_writeCtx = nullptr;

  // Two-lane write queue, drained by drainWriteQueue() on m_writeThread.
  //
  // The frames themselves used to ride the invokeMethod, which made the write
  // path strictly FIFO — and since every SimpleBLE call blocks for a full hop
  // onto the single WinRT MTA thread (a write_request for an ATT round trip on
  // top), a keypress queued behind a discovery burst waited for all of it. An
  // engine switch is exactly that case: notes played right after one had their
  // note-off stranded behind ~80 listing frames and the synth droned until the
  // link dropped. Live gestures (SynthProto::isRealtimeOp) now go in their own
  // lane and are always taken first. Ordering *within* a lane is preserved,
  // which is what the paced SET_PARAM queue and the chunked listings rely on.
  struct PendingWrite {
    QByteArray data;
    bool withResponse = false;
  };
  QMutex m_writeQueueMutex;
  QList<PendingWrite> m_writeHigh;
  QList<PendingWrite> m_writeLow;

  QStringList m_lastConnectedDevices;
  QVariantList m_discoveredDevices;
};

#endif  // BLUETOOTHMANAGER2H_H
