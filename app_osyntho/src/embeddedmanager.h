#ifndef EMBEDDEDMANAGER_H
#define EMBEDDEDMANAGER_H

#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QThread>
#include <QVariantList>
#include <QWaitCondition>
#include <atomic>

#include "ibluetoothmanager.h"

// The synth in this process, behind the same interface as a BLE link.
//
// Built for OSYNTHO_EMBEDDED: the engine is compiled into the app
// (port/host/osynth_core) and reached through SynthCtl v1 rather than over the
// air. SynthController and every QML screen sit above IBluetoothManager and
// cannot tell the difference — which is the whole point, and the reason this
// is a transport rather than a second control path.
//
// What the interface asks for and how it is answered here:
//
//   write()          queues a command frame to the protocol thread
//   receivedData     the protocol's replies and events, marshalled to the GUI
//   infoRead         emitted once at startup from ctrl_proto_info()
//   connected        always true: an in-process engine cannot go away
//   mtu()            the frame ceiling; there is no negotiated limit here
//   scanning et al.  no-ops — there is nothing to scan for
//
// Threading. ctrl_proto requires that handle_frame() and flush_events() be
// called from one task and the same one (its handlers share a TX buffer and a
// chunker). So this owns a dedicated thread that does both, exactly as
// ble_ctrl's `ble_cmd` task does, and the GUI thread only ever posts to its
// queue.
class EmbeddedManager : public IBluetoothManager {
  Q_OBJECT

 public:
  static EmbeddedManager& instance();

  // Brings the engine up and starts the protocol thread. Safe to call twice.
  void start();
  void finish() override;

  bool askForBluetoothPermissionIfNotAvailable() override { return true; }

  bool getScanning() const override { return false; }
  void setScanning(bool) override {}

  bool getConnected() override { return true; }
  QString getDeviceName() override;
  QString getDeviceAddress() override { return QStringLiteral("embedded"); }
  QVariantList getDiscoveredDevices() const override { return {}; }

  // The frame ceiling the protocol itself imposes (CTRL_PROTO_MAX_FRAME).
  // SynthController sizes its batches from this and needs >= 247 to pack them
  // at full width; there is no ATT negotiation in the way here, so it always
  // gets the maximum.
  int mtu() const override;

  Q_INVOKABLE void connectToSelectedDevice() override {}
  Q_INVOKABLE void startDeviceScan() override {}
  Q_INVOKABLE void stopDeviceScan() override {}

 public slots:
  void write(const QByteArray& data, bool withResponse) override;
  void onBluetoothAvailable() override {}
  void setBluetoothEnabled(bool) override {}

 private:
  explicit EmbeddedManager(QObject* parent = nullptr);
  ~EmbeddedManager() override;

  // The protocol thread's body: drain the queue into ctrl_proto_handle_frame()
  // and call ctrl_proto_flush_events() on the same ~20 Hz cadence ble_ctrl
  // uses, so the app sees events arrive at the rate it was written against.
  void protocolLoop();

  // ctrl_transport_t is a C vtable of plain function pointers with no context
  // argument, so the four thunks below reach the instance through this. There
  // is one manager per process (it is a singleton and the engine is global),
  // which is what makes that sound rather than a shortcut.
  static EmbeddedManager* s_self;
  static bool tpSend(const uint8_t* frame, size_t len);
  static size_t tpAvailPayload();
  static bool tpLinkUp();

  QThread m_thread;
  QMutex m_mutex;
  QWaitCondition m_wake;
  QList<QByteArray> m_pending;  // guarded by m_mutex
  std::atomic<bool> m_quit{false};
  std::atomic<bool> m_started{false};
};

#endif  // EMBEDDEDMANAGER_H
