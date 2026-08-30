#include "src/embeddedmanager.h"

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QStandardPaths>

extern "C" {
#include "ctrl_proto.h"
#include "osynth_host.h"
}

EmbeddedManager* EmbeddedManager::s_self = nullptr;

EmbeddedManager& EmbeddedManager::instance() {
  static EmbeddedManager manager;
  return manager;
}

EmbeddedManager::EmbeddedManager(QObject* parent) : IBluetoothManager(parent) {
  s_self = this;
}

EmbeddedManager::~EmbeddedManager() {
  finish();
  s_self = nullptr;
}

// ---------------------------------------------------------------------------
// The transport
// ---------------------------------------------------------------------------

bool EmbeddedManager::tpSend(const uint8_t* frame, size_t len) {
  EmbeddedManager* self = s_self;
  if (self == nullptr || frame == nullptr || len == 0) return false;

  // Called on the protocol thread. QueuedConnection is what carries it to the
  // GUI thread, where SynthController lives -- the same hop App already makes
  // for the BLE backends' receivedData, so the controller's slot runs on the
  // thread it has always run on.
  //
  // The QByteArray copies the frame here rather than referencing it, which it
  // must: the protocol's TX buffer is reused by the very next response.
  QMetaObject::invokeMethod(
      self,
      [self, data = QByteArray(reinterpret_cast<const char*>(frame),
                               static_cast<qsizetype>(len))]() {
        emit self->receivedData(data);
      },
      Qt::QueuedConnection);
  return true;
}

size_t EmbeddedManager::tpAvailPayload() {
  // What fits after the 4-byte header and the status byte. No negotiated
  // limit to track: the ceiling is the frame itself.
  return CTRL_PROTO_MAX_FRAME - 4 - 1;
}

bool EmbeddedManager::tpLinkUp() {
  // An in-process engine has no link to lose, and the app is by definition
  // listening -- it is the same process. Returning false here would make the
  // protocol drop every event it built.
  return s_self != nullptr && !s_self->m_quit.load();
}

int EmbeddedManager::mtu() const {
  // Reported the way a BLE link would: the ATT MTU, from which the controller
  // derives its usable payload. Frame ceiling plus the 3 bytes of ATT header
  // it subtracts again, so the arithmetic on the far side lands on
  // CTRL_PROTO_MAX_FRAME rather than 3 bytes short of it.
  return CTRL_PROTO_MAX_FRAME + 3;
}

QString EmbeddedManager::getDeviceName() {
  return QStringLiteral("osynth (built in)");
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void EmbeddedManager::start() {
  bool expected = false;
  if (!m_started.compare_exchange_strong(expected, true)) return;

  // Where the engine keeps presets, settings and loop takes.
  //
  // Asked of Qt rather than left to the port's own default, and that matters
  // most where it is least visible: on Android and iOS an app may only write
  // inside the sandbox the OS hands it, which nothing in port/host can guess.
  // AppDataLocation is that place on every platform Qt supports.
  const QString dataDir =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  if (!dataDir.isEmpty()) QDir().mkpath(dataDir);

  osynth_host_config_t cfg;
  osynth_host_config_default(&cfg);
  const QByteArray dataDirUtf8 = QDir::toNativeSeparators(dataDir).toUtf8();
  if (!dataDirUtf8.isEmpty()) cfg.data_dir = dataDirUtf8.constData();

  const esp_err_t err = osynth_host_start(&cfg);
  if (err != ESP_OK) {
    qWarning() << "embedded engine failed to start:" << err;
    m_started = false;
    return;
  }

  // Install this as the protocol's transport only once the engine is up, so a
  // frame can never arrive before there is something to answer it.
  static const ctrl_transport_t transport = {
      &EmbeddedManager::tpSend,
      nullptr,  // no back-pressure: see below
      &EmbeddedManager::tpAvailPayload,
      &EmbeddedManager::tpLinkUp,
  };
  // send_paced is null on purpose. It exists for the loop-track download,
  // whose thousands of frames can outrun a radio's buffers; here the "buffer"
  // is a queued signal to a thread in the same process, and there is nothing
  // to wait for. ctrl_proto falls back to the plain send, which is exactly
  // right.
  ctrl_proto_set_transport(&transport);

  m_thread.setObjectName(QStringLiteral("osynth_proto"));
  connect(&m_thread, &QThread::started, this, &EmbeddedManager::protocolLoop,
          Qt::DirectConnection);
  m_thread.start();

  // The app reads INFO once on connect to learn the protocol version, the
  // firmware version and the target. Built by the protocol, so it says the
  // same thing here as it does over a GATT read.
  uint8_t info[32];
  const size_t n = ctrl_proto_info(info, sizeof(info));
  if (n > 0) {
    // Queued, like connectedChanged below. start() is called from App's
    // constructor, so a direct emit here would run SynthController's slot
    // while App is still being built.
    QMetaObject::invokeMethod(
        this,
        [this, data = QByteArray(reinterpret_cast<const char*>(info),
                                 static_cast<qsizetype>(n))]() {
          emit infoRead(data);
        },
        Qt::QueuedConnection);
  }

  // Everything above is ready, so the link is "up". Emitted last, and
  // queued: App's handler pushes the MTU into the controller and starts
  // discovery, which must not run inside start().
  QMetaObject::invokeMethod(
      this, [this]() { emit connectedChanged(true); }, Qt::QueuedConnection);
}

void EmbeddedManager::finish() {
  if (!m_started.exchange(false)) return;

  m_quit = true;
  m_wake.wakeAll();
  m_thread.quit();
  // Bounded: the loop wakes on m_wake and re-checks m_quit every flush period,
  // so it cannot take longer than that plus the frame in hand.
  m_thread.wait(2000);

  // Detach before the engine goes, so a late event has nowhere to be sent
  // rather than reaching a half-torn-down object.
  ctrl_proto_set_transport(nullptr);
  osynth_host_stop();
}

// ---------------------------------------------------------------------------
// The protocol thread
// ---------------------------------------------------------------------------

void EmbeddedManager::write(const QByteArray& data, bool withResponse) {
  // withResponse is a BLE distinction -- whether the ATT layer acknowledges
  // the write. There is no such layer here and no frame can be lost between
  // two points in one process, so both kinds are queued identically.
  Q_UNUSED(withResponse);
  if (data.isEmpty() || !m_started.load()) return;

  {
    QMutexLocker lock(&m_mutex);
    m_pending.append(data);
  }
  m_wake.wakeOne();
}

void EmbeddedManager::protocolLoop() {
  // ~20 Hz, the cadence ble_ctrl flushes at. Kept the same so the app's event
  // rate does not change with the transport: its coalescing and its UI update
  // rate were both tuned against this.
  constexpr qint64 kFlushMs = 50;

  QElapsedTimer sinceFlush;
  sinceFlush.start();

  while (!m_quit.load()) {
    QByteArray frame;
    {
      QMutexLocker lock(&m_mutex);
      if (m_pending.isEmpty()) {
        // Waking on the flush period rather than only on a frame is what keeps
        // events flowing while the app is idle: a parameter changed by a
        // preset load or by the sequencer still has to reach it.
        m_wake.wait(&m_mutex, kFlushMs);
      }
      if (!m_pending.isEmpty()) frame = m_pending.takeFirst();
    }

    if (!frame.isEmpty()) {
      ctrl_proto_handle_frame(
          reinterpret_cast<const uint8_t*>(frame.constData()),
          static_cast<size_t>(frame.size()));
    }

    // Time-based, and deliberately not "flush only when the queue is empty".
    // A connect burst is dozens of frames back to back; gating the flush on an
    // idle queue would hold every event behind the whole burst, and a client
    // that keeps writing could starve it indefinitely. This is the same rule
    // ble_ctrl's cmd_task follows, for the same reason.
    if (sinceFlush.elapsed() >= kFlushMs) {
      ctrl_proto_flush_events();
      sinceFlush.restart();
    }
  }

  // The client is going away. Releases anything it was holding -- the same
  // call ble_ctrl makes on disconnect, and for the same reason: a note played
  // through OP_NOTE_ON has no other way to be released.
  ctrl_proto_link_down();
}
