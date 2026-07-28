#include "synthcontroller.h"

#include "translator.h"

#include <QDateTime>
#include <QDebug>
#include <QSet>

#include <cmath>
#include <limits>
#include <utility>

using namespace SynthProto;

namespace {
constexpr int kSetCoalesceMs = 40;         // ~25 Hz knob-write batches
constexpr int kEngineSwitchSettleMs = 400; // let 0x02xx re-register before a patch push
constexpr int kMaxSetPairsPerFrame = 40;   // 4B header + 40×6B = 244B < 252
constexpr int kMaxGetIdsPerFrame = 120;    // 4B header + 120×2B = 244B < 252

// --- Flow-controlled PARAM_INFO discovery -------------------------------
// The firmware has a SMALL command queue and returns BUSY (status 5) when
// flooded — the old "blast all requests" pump caused BUSY storms, jammed the
// write path (notes stopped working), and dropped the link. So we keep only a
// tiny window of requests outstanding and send the next as each response lands.
constexpr int kInfoWindow = 3;             // max PARAM_INFO requests in flight
constexpr int kInfoPumpIntervalMs = 60;    // pump/watchdog cadence
constexpr int kInfoTimeoutMs = 700;        // resend a request unanswered this long
constexpr int kInfoBusyBackoffMs = 300;    // pause new sends after a BUSY
constexpr int kDiscoveryBudgetMs = 40000;  // overall safety cap
// The list request is the linchpin (no list -> no ids). Resend if unanswered.
constexpr int kListWatchdogMs = 1200;
constexpr int kListRetries = 8;
// Coalesce the paramsDiscovered signal during progressive fill.
constexpr int kDiscoveredCoalesceMs = 150;
// Settle time before re-reading a pattern the firmware switched to itself.
constexpr int kFollowPatternMs = 500;

// Well-known ids the controller tracks directly.
constexpr quint16 ID_PRESET_LOAD = 0x0002;
// seq.pos: firmware-written playhead, -1 while stopped. It arrives in the
// ~20 Hz EVT_PARAMS batches like any other change, so the grid highlight
// costs no extra traffic.
constexpr quint16 ID_SEQ_POS = 0x040B;
constexpr quint16 ID_SEQ_CURPAT = 0x040C;

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
}  // namespace

SynthController::SynthController(QObject* parent) : QObject(parent) {
  m_setTimer.setSingleShot(true);
  m_setTimer.setInterval(kSetCoalesceMs);
  connect(&m_setTimer, &QTimer::timeout, this, &SynthController::flushPendingSets);

  m_infoRequestTimer.setSingleShot(false);
  m_infoRequestTimer.setInterval(kInfoPumpIntervalMs);
  connect(&m_infoRequestTimer, &QTimer::timeout, this, &SynthController::pumpInfoRequests);

  m_followPatternTimer.setSingleShot(true);
  m_followPatternTimer.setInterval(kFollowPatternMs);
  connect(&m_followPatternTimer, &QTimer::timeout, this,
          &SynthController::refreshSequencer);

  m_discoveredCoalesceTimer.setSingleShot(true);
  m_discoveredCoalesceTimer.setInterval(kDiscoveredCoalesceMs);
  connect(&m_discoveredCoalesceTimer, &QTimer::timeout, this,
          [this]() { emit paramsDiscovered(); });

  // List-response watchdog: only resends the PARAM_INFO 0xFFFF list request. The
  // per-id metadata is handled by the self-healing rotating pump, not here.
  m_discoveryTimer.setSingleShot(true);
  m_discoveryTimer.setInterval(kListWatchdogMs);
  connect(&m_discoveryTimer, &QTimer::timeout, this, [this]() {
    if (!m_discovering || !m_awaitingInfoList) return;
    if (m_listRetries <= 0) {
      qDebug() << "Synth | Discovery: list request unanswered; giving up.";
      m_discovering = false;
      return;
    }
    m_listRetries--;
    qDebug() << "Synth | Discovery: list response missing, resending (retries"
             << m_listRetries << ")";
    m_infoListAccum.clear();
    m_infoListSeq = nextSeq();
    sendWithSeq(OP_PARAM_INFO, m_infoListSeq, payloadParamInfo(PARAM_INFO_LIST_ALL), false);
    m_discoveryTimer.start();
  });
}

SynthController::~SynthController() = default;

/* ---------------------------------------------------------------- helpers */

quint8 SynthController::nextSeq() {
  m_seq++;
  if (m_seq == 0) m_seq = 1;  // 0 is reserved for events
  return m_seq;
}

void SynthController::sendWithSeq(quint8 op, quint8 seq, const QByteArray& payload,
                                  bool withResponse) {
  if (!m_connected) return;
  emit writeToSynth(buildFrame(op, seq, payload), withResponse);
}

void SynthController::send(quint8 op, const QByteArray& payload, bool withResponse) {
  sendWithSeq(op, nextSeq(), payload, withResponse);
}

QString SynthController::engineNameFor(int engine) {
  switch (engine) {
    case ENG_SUBTRACTIVE: return QStringLiteral("Subtractive");
    case ENG_ADDITIVE:    return QStringLiteral("Additive");
    case ENG_FM:          return QStringLiteral("FM");
    case ENG_WAVETABLE:   return QStringLiteral("Wavetable");
    default:              return QStringLiteral("?");
  }
}

QString SynthController::engineName() const { return engineNameFor(m_engine); }

/* ------------------------------------------------------------ connection */

void SynthController::setConnected(bool connected) {
  if (m_connected == connected) return;
  m_connected = connected;
  emit connectedChanged();

  resetState();
  if (connected) {
    // The manager emits connectedChanged AFTER subscribing to EVT and reading
    // INFO, so it is safe to talk now. The EVT_ENGINE greeting normally kicks
    // discovery; this is a fallback in case it was missed.
    QTimer::singleShot(300, this, [this]() {
      if (m_connected && m_paramOrder.isEmpty() && !m_discovering) beginDiscovery();
    });
  }
}

void SynthController::resetState() {
  m_ready = false;
  m_discovering = false;
  m_awaitingInfoList = false;
  m_discoveryTimer.stop();
  m_infoRequestTimer.stop();
  m_discoveredCoalesceTimer.stop();
  m_followPatternTimer.stop();
  m_setTimer.stop();
  m_pendingSets.clear();
  m_params.clear();
  m_paramOrder.clear();
  m_pendingInfoIds.clear();
  m_infoQueue.clear();
  m_infoInflight.clear();
  m_infoSeqToId.clear();
  m_infoBackoffUntilMs = 0;
  m_discoveryStartMs = 0;
  m_infoListAccum.clear();
  m_presetsAccum.clear();
  m_seqInfo = SynthProto::SeqInfo();
  m_steps.clear();
  m_stepsAccum.clear();
  m_stepsWindowNext = -1;
  m_trackCfg = SynthProto::SeqTrackCfg();
  m_plocks.clear();
  m_plocksAccum.clear();
  m_song.clear();
  m_songAccum.clear();
  m_kitSlots.clear();
  m_kitSlotsAccum.clear();
  m_kits.clear();
  m_kitsAccum.clear();
  m_playhead = -1;
  emit seqInfoChanged();
  emit stepsChanged();
  emit kitChanged();
  emit playheadChanged();
  m_presetSlot = -1;
  m_presetName.clear();
  m_presetIsFactory = false;
  emit readyChanged();
  emit presetChanged();
  emit paramsDiscovered();
}

void SynthController::onInfoRead(const QByteArray& info) {
  const DeviceInfo di = parseInfo(info);
  if (!di.valid) {
    qWarning() << "Synth | INFO read malformed";
    return;
  }
  m_protoVersion = di.protoVersion;
  m_fwVersion = QStringLiteral("%1.%2.%3").arg(di.fwMajor).arg(di.fwMinor).arg(di.fwPatch);
  m_target = di.target;
  emit infoChanged();
}

/* ------------------------------------------------------------- discovery */

void SynthController::beginDiscovery() {
  m_discovering = true;
  m_awaitingInfoList = true;
  m_listRetries = kListRetries;
  m_infoListAccum.clear();
  m_infoQueue.clear();
  m_infoInflight.clear();
  m_infoSeqToId.clear();
  m_infoBackoffUntilMs = 0;
  m_discoveryStartMs = nowMs();
  m_infoRequestTimer.stop();
  m_infoListSeq = nextSeq();
  // Write-without-response (non-blocking) even for this linchpin request: on an
  // unsettled link a with-response write can block the GUI until the ATT
  // timeout. If it (or its response) is dropped, the watchdog resends it.
  sendWithSeq(OP_PARAM_INFO, m_infoListSeq, payloadParamInfo(PARAM_INFO_LIST_ALL), false);
  m_discoveryTimer.start();  // list watchdog
}

// The id list is known: register the ids, mark the controller ready so the UI
// paints the shell immediately, then start the flow-controlled pump that fills
// in each id's metadata a few requests at a time.
void SynthController::onParamListComplete() {
  m_awaitingInfoList = false;
  m_discoveryTimer.stop();  // list arrived; the watchdog is done

  for (quint16 id : std::as_const(m_infoListAccum)) {
    if (!m_paramOrder.contains(id)) m_paramOrder.append(id);
    if (!m_params.contains(id)) m_params.insert(id, Param{});
    if (!m_params[id].infoKnown && !m_pendingInfoIds.contains(id)) {
      m_pendingInfoIds.insert(id);
      m_infoQueue.append(id);
    }
  }
  setEngineCaps(m_infoListEngine, m_infoListCaps);

  // Ready as soon as the ids are known: pages render their shells and fill in as
  // each PARAM_INFO arrives (every arrival coalesces a paramsDiscovered).
  if (!m_ready) {
    m_ready = true;
    emit readyChanged();
  }
  scheduleParamsDiscovered();

  // Values first, before the metadata pump starts. Until a value arrives,
  // paramValue() falls back to the parameter's *default*, so every control
  // shows a plausible-but-wrong number — the master volume sitting at 0.8
  // while the synth is actually somewhere else. Deferring the burst to
  // finishDiscovery() (as this used to) left that on screen for as long as the
  // per-id PARAM_INFO walk takes, which is seconds at ~200 params.
  //
  // It is affordable here: GET_PARAM packs 120 ids per frame, so the whole
  // registry is ~2 frames, against one request per id for the metadata. The
  // original worry — competing with the info flow for the firmware's small
  // command queue — is about those 200 requests, not about 2 sent before the
  // pump is even primed.
  requestAllParamValues();

  if (m_pendingInfoIds.isEmpty()) {
    finishDiscovery();
    return;
  }
  m_discoveryStartMs = nowMs();
  pumpInfoRequests();          // prime the window
  m_infoRequestTimer.start();  // keep it topped up + reap timeouts
}

void SynthController::sendInfoRequest(quint16 id) {
  const quint8 seq = nextSeq();
  m_infoSeqToId.insert(seq, id);
  m_infoInflight.insert(id, nowMs());
  sendWithSeq(OP_PARAM_INFO, seq, payloadParamInfo(id), false);
}

// Flow-controlled pump: reap timed-out in-flight requests, then keep the window
// (kInfoWindow) topped up from the work queue. Responses (handleParamInfoSingle)
// call back into here to fill the freed slot immediately, so throughput tracks
// the link/firmware speed without ever flooding it.
void SynthController::pumpInfoRequests() {
  if (!m_discovering) return;

  if (m_pendingInfoIds.isEmpty()) {
    finishDiscovery();
    return;
  }
  if (nowMs() - m_discoveryStartMs > kDiscoveryBudgetMs) {
    qDebug() << "Synth | Discovery: budget spent;" << m_pendingInfoIds.size()
             << "infos still missing.";
    finishDiscovery();
    return;
  }

  const qint64 now = nowMs();

  // Reap timed-out in-flight requests back onto the queue for another try.
  for (auto it = m_infoInflight.begin(); it != m_infoInflight.end();) {
    if (now - it.value() > kInfoTimeoutMs) {
      const quint16 id = it.key();
      it = m_infoInflight.erase(it);
      if (m_pendingInfoIds.contains(id) && !m_infoQueue.contains(id)) m_infoQueue.append(id);
    } else {
      ++it;
    }
  }

  // Respect a BUSY back-off before issuing new requests.
  if (now < m_infoBackoffUntilMs) return;

  // Top the window up.
  while (m_infoInflight.size() < kInfoWindow && !m_infoQueue.isEmpty()) {
    const quint16 id = m_infoQueue.takeFirst();
    if (!m_pendingInfoIds.contains(id)) continue;   // already answered
    if (m_infoInflight.contains(id)) continue;      // already outstanding
    sendInfoRequest(id);
  }
}

void SynthController::onInfoBusy(quint8 seq) {
  // Firmware queue full: back off, and re-queue the id this request was for.
  m_infoBackoffUntilMs = nowMs() + kInfoBusyBackoffMs;
  const auto it = m_infoSeqToId.constFind(seq);
  if (it == m_infoSeqToId.constEnd()) return;
  const quint16 id = it.value();
  m_infoSeqToId.remove(seq);
  m_infoInflight.remove(id);
  if (m_pendingInfoIds.contains(id) && !m_infoQueue.contains(id)) m_infoQueue.append(id);
}

void SynthController::onInfoBadArg(quint8 seq) {
  // The id isn't registered (e.g. it vanished in an engine switch). Stop chasing
  // it so discovery can complete; a fresh EVT_ENGINE will re-discover anyway.
  const auto it = m_infoSeqToId.constFind(seq);
  if (it == m_infoSeqToId.constEnd()) return;
  const quint16 id = it.value();
  m_infoSeqToId.remove(seq);
  m_infoInflight.remove(id);
  m_pendingInfoIds.remove(id);
  if (m_discovering && m_pendingInfoIds.isEmpty()) finishDiscovery();
}

void SynthController::finishDiscovery() {
  m_discovering = false;
  m_infoRequestTimer.stop();
  m_infoInflight.clear();
  m_infoSeqToId.clear();
  m_infoQueue.clear();
  scheduleParamsDiscovered();
  // A second value sweep. onParamListComplete() already fetched them so the UI
  // was never showing defaults; this catches anything the synth changed during
  // the metadata walk (a MIDI CC, a preset load, the sequencer's telemetry),
  // which at ~200 params is a few seconds of opportunity.
  requestAllParamValues();
  listPresets(m_engine);
  // The sequencer and kit are not parameters, so discovery does not reach
  // them; ask once the parameter traffic has been queued.
  refreshSequencer();
  refreshKit();
  send(OP_SEQ_SONG, payloadSeqSongGet(), true);
  // Capability probe: the firmware ignores velocity 0, so this is silent where
  // the opcode exists and answers UNKNOWN_OP where it does not.
  m_drumTrigOpcode = true;
  send(OP_DRUM_TRIG, payloadDrumTrig(0, 0), true);
}

void SynthController::scheduleParamsDiscovered() {
  if (!m_discoveredCoalesceTimer.isActive()) m_discoveredCoalesceTimer.start();
}

void SynthController::requestAllParamValues() {
  QList<quint16> chunk;
  for (quint16 id : std::as_const(m_paramOrder)) {
    chunk.append(id);
    if (chunk.size() >= kMaxGetIdsPerFrame) {
      send(OP_GET_PARAM, payloadGetParam(chunk), false);
      chunk.clear();
    }
  }
  if (!chunk.isEmpty()) send(OP_GET_PARAM, payloadGetParam(chunk), false);
}

/* ---------------------------------------------------------- frame decode */

void SynthController::onReceiveData(const QByteArray& data) {
  const Frame f = parseFrame(data);
  if (!f.valid) {
    qWarning() << "Synth | dropped malformed EVT frame of" << data.size() << "bytes";
    return;
  }
  handleFrame(f);
}

void SynthController::handleFrame(const Frame& f) {
  if (f.isEvent()) {
    switch (f.op) {
      case EVT_PARAMS: handleParamValues(f.payload); break;
      case EVT_ENGINE: handleEngineEvent(f.payload); break;
      default: break;
    }
    return;
  }

  if (f.status != ST_OK) {
    if (f.requestOp() == OP_DRUM_TRIG && f.status == ST_UNKNOWN_OP) {
      // Pre-S22 firmware: pads and audition buttons go through the parameter.
      if (m_drumTrigOpcode) {
        qDebug() << "Synth | no DRUM_TRIG opcode; using the drums.trig param";
      }
      m_drumTrigOpcode = false;
      return;
    }
    if (f.requestOp() == OP_PARAM_INFO) {
      // Flow control for the discovery pump: BUSY = firmware queue full (back off
      // and retry that id); BAD_ARG = id not registered (stop chasing it). Both
      // are expected under load / engine races, so they are handled, not warned.
      if (f.status == ST_BUSY) {
        onInfoBusy(f.seq);
        return;
      }
      if (f.status == ST_BAD_ARG) {
        onInfoBadArg(f.seq);
        return;
      }
    }
    qWarning() << "Synth | op" << Qt::hex << f.requestOp() << "status" << f.status;
    return;
  }

  switch (f.requestOp()) {
    case OP_PARAM_INFO:
      // Route to the list handler only while the list response is actually
      // pending; once it completes, later PARAM_INFO frames are per-id metadata
      // (even if a seq value happens to collide with the old list seq).
      if (m_awaitingInfoList && f.seq == m_infoListSeq) {
        handleParamInfoList(f.payload);
        if (!f.continuation) onParamListComplete();
      } else {
        m_infoSeqToId.remove(f.seq);  // this request is resolved
        handleParamInfoSingle(f.payload);
      }
      break;
    case OP_GET_PARAM:
      handleParamValues(f.payload);
      break;
    case OP_LIST_PRESETS:
      handlePresetList(f.payload, f.continuation);
      break;
    case OP_PING:
      // uptime u32 — informational only.
      break;
    case OP_SEQ_INFO:
      handleSeqInfo(f.payload);
      break;
    case OP_SEQ_STEPS:
      handleSeqSteps(f.payload, f.continuation);
      break;
    case OP_SEQ_TRACK:
      handleSeqTrack(f.payload);
      break;
    case OP_SEQ_PATTERN:
      handleSeqPattern(f.payload);
      break;
    case OP_SEQ_PLOCK:
      handleSeqPlock(f.payload, f.continuation);
      break;
    case OP_SEQ_SONG:
      handleSeqSong(f.payload, f.continuation);
      break;
    case OP_KIT_INFO:
      handleKitInfo(f.payload, f.continuation);
      break;
    case OP_SEQ_EDIT:
      // Status-only (or the toggle's one result byte); the optimistic local
      // edit already repainted the grid.
      break;
    default:
      break;
  }
}

void SynthController::handleParamInfoList(const QByteArray& payload) {
  const ParamInfoList list = parseParamInfoList(payload);
  m_infoListEngine = list.engine;
  m_infoListCaps = list.caps;
  for (quint16 id : list.ids) m_infoListAccum.append(id);
}

void SynthController::handleParamInfoSingle(const QByteArray& payload) {
  const ParamInfo pi = parseParamInfo(payload);
  if (!pi.valid) return;
  Param& p = m_params[pi.id];
  const bool wasUnknown = !p.infoKnown;
  p.info = pi;
  p.infoKnown = true;
  if (!m_paramOrder.contains(pi.id)) m_paramOrder.append(pi.id);
  const bool wasPending = m_pendingInfoIds.remove(pi.id);
  m_infoInflight.remove(pi.id);  // free its flow-control slot
  // Progressive fill: refresh the page groups as newly-known metadata arrives.
  if (wasUnknown) scheduleParamsDiscovered();
  if (m_discovering) {
    if (m_pendingInfoIds.isEmpty()) {
      finishDiscovery();
    } else if (wasPending) {
      pumpInfoRequests();  // a slot freed — send the next one right away
    }
  }
}

void SynthController::handleParamValues(const QByteArray& payload) {
  const QList<ParamValue> values = parseParamValues(payload);
  for (const ParamValue& pv : values) applyValue(pv.id, pv.value, /*echo=*/true);
}

void SynthController::applyValue(quint16 id, float value, bool echo) {
  Param& p = m_params[id];  // inserts an info-less entry if unknown; harmless
  p.value = value;
  p.valueKnown = true;
  if (echo) emit paramChanged(id, value);

  if (id == ID_PRESET_LOAD) {
    const int slot = int(std::lround(value));
    if (slot != m_presetSlot) {
      m_presetSlot = slot;
      updatePresetFromSlot();
      emit presetChanged();
    }
  } else if (id == ID_SEQ_POS) {
    const int pos = int(std::lround(value));
    if (pos != m_playhead) {
      m_playhead = pos;
      emit playheadChanged();
    }
  } else if (id == ID_SEQ_CURPAT) {
    // The firmware moved to another pattern (song chain, or a queued switch
    // reaching its bar boundary). Follow it with the editor so the grid keeps
    // showing what is actually playing — but debounced: a song chain of short
    // patterns fires this every bar, and a full re-read per bar would swamp
    // the link with traffic nobody can read that fast.
    const int pat = int(std::lround(value));
    if (pat != m_editPattern && pat >= 0 &&
        (!m_seqInfo.valid || pat < m_seqInfo.patterns)) {
      m_editPattern = pat;
      emit editTargetChanged();
      m_followPatternTimer.start();
    }
  }
}

void SynthController::handleEngineEvent(const QByteArray& payload) {
  const EngineEvent e = parseEngineEvent(payload);
  setEngineCaps(e.engine, e.caps);
  // (Re)discover: the 0x02xx map is re-registered per engine.
  beginDiscovery();
}

void SynthController::setEngineCaps(quint8 engine, quint8 caps) {
  const bool engineChangedNow = (int(engine) != m_engine);
  if (engineChangedNow) {
    // The engine-specific 0x02xx range changes meaning — drop its stale infos
    // so discovery re-fetches them.
    for (auto it = m_paramOrder.begin(); it != m_paramOrder.end();) {
      if (*it >= 0x0200 && *it <= 0x02FF) {
        m_params.remove(*it);
        it = m_paramOrder.erase(it);
      } else {
        ++it;
      }
    }
  }
  const bool changed = engineChangedNow || int(caps) != m_caps;
  m_engine = engine;
  m_caps = caps;
  if (changed) emit engineChanged();
  if (engineChangedNow) {
    updatePresetFromSlot();
    emit presetChanged();
  }
}

/* --------------------------------------------------------------- presets */

void SynthController::handlePresetList(const QByteArray& payload, bool more) {
  const PresetList pl = parsePresetList(payload);
  QList<PresetEntry>& accum = m_presetsAccum[pl.engine];
  for (const PresetEntry& e : pl.entries) accum.append(e);
  if (!more) {
    m_presets[pl.engine] = accum;
    m_presetsAccum.remove(pl.engine);
    emit presetsChanged(pl.engine);
    if (int(pl.engine) == m_engine) {
      updatePresetFromSlot();
      emit presetChanged();
    }
  }
}

void SynthController::updatePresetFromSlot() {
  m_presetName.clear();
  m_presetIsFactory = false;
  const auto it = m_presets.constFind(m_engine);
  if (it == m_presets.constEnd()) return;
  for (const PresetEntry& e : it.value()) {
    if (int(e.slot) == m_presetSlot) {
      m_presetName = e.name;
      m_presetIsFactory = e.factory;
      return;
    }
  }
}

QVariantList SynthController::presetsFor(int engine) const {
  QVariantList out;
  const auto it = m_presets.constFind(engine);
  if (it == m_presets.constEnd()) return out;
  for (const PresetEntry& e : it.value()) {
    out.append(QVariantMap{{"slot", e.slot}, {"name", e.name}, {"factory", e.factory}});
  }
  return out;
}

/* -------------------------------------------------------- param accessors */

bool SynthController::paramExists(int id) const {
  const auto it = m_params.constFind(quint16(id));
  return it != m_params.constEnd() && it->infoKnown;
}

double SynthController::paramValue(int id) const {
  const auto it = m_params.constFind(quint16(id));
  if (it == m_params.constEnd()) return 0.0;
  if (it->valueKnown) return it->value;
  return it->infoKnown ? it->info.def : 0.0;
}

bool SynthController::paramValueKnown(int id) const {
  const auto it = m_params.constFind(quint16(id));
  return it != m_params.constEnd() && it->valueKnown;
}

QVariantMap SynthController::paramMeta(int id) const {
  QVariantMap m;
  const auto it = m_params.constFind(quint16(id));
  const bool exists = it != m_params.constEnd() && it->infoKnown;
  m.insert("exists", exists);
  if (!exists) return m;
  const ParamInfo& pi = it->info;
  m.insert("id", pi.id);
  m.insert("name", pi.name);
  m.insert("type", pi.type);
  m.insert("curve", pi.curve);
  m.insert("min", pi.min);
  m.insert("max", pi.max);
  m.insert("def", pi.def);
  m.insert("enumNames", pi.enumNames);
  return m;
}

QVariantList SynthController::paramIds() const {
  QVariantList out;
  for (quint16 id : m_paramOrder) out.append(id);
  return out;
}

int SynthController::paramIdForName(const QString& name) const {
  for (quint16 id : m_paramOrder) {
    const Param& p = m_params.value(id);
    if (p.infoKnown && p.info.name == name) return id;
  }
  return -1;
}

QString SynthController::paramName(int id) const {
  const auto it = m_params.constFind(quint16(id));
  return (it != m_params.constEnd() && it->infoKnown) ? it->info.name : QString();
}

QVariantList SynthController::paramIdsByPrefix(const QString& prefix) const {
  QVariantList out;
  for (quint16 id : m_paramOrder) {
    const Param& p = m_params.value(id);
    if (p.infoKnown && p.info.name.startsWith(prefix)) out.append(id);
  }
  return out;
}

QVariantList SynthController::paramPickerList() const {
  QVariantList out;
  for (quint16 id : m_paramOrder) {
    const Param& p = m_params.value(id);
    if (p.infoKnown) out.append(QVariantMap{{"id", id}, {"name", p.info.name}});
  }
  return out;
}

/* --------------------------------------------------------------- commands */

void SynthController::setParam(int id, double value) {
  const quint16 pid = quint16(id);
  applyValue(pid, float(value), /*echo=*/true);  // optimistic, responsive UI
  m_pendingSets.insert(pid, float(value));
  if (!m_setTimer.isActive()) m_setTimer.start();
}

void SynthController::flushPendingSets() {
  if (m_pendingSets.isEmpty()) return;
  QByteArray payload;
  int pairs = 0;
  for (auto it = m_pendingSets.constBegin(); it != m_pendingSets.constEnd(); ++it) {
    appendSetParam(payload, it.key(), it.value());
    if (++pairs >= kMaxSetPairsPerFrame) {
      send(OP_SET_PARAM, payload, /*withResponse=*/false);
      payload.clear();
      pairs = 0;
    }
  }
  if (pairs > 0) send(OP_SET_PARAM, payload, /*withResponse=*/false);
  m_pendingSets.clear();
}

void SynthController::refreshParam(int id) {
  send(OP_GET_PARAM, payloadGetParam({quint16(id)}), false);
}

void SynthController::selectEngine(int engine) {
  send(OP_SELECT_ENGINE, payloadSelectEngine(quint8(engine)), true);
}

void SynthController::loadPreset(int engine, int slot) {
  send(OP_LOAD_PRESET, payloadLoadPreset(quint8(engine), quint8(slot)), true);
}

void SynthController::savePreset(int engine, int slot, const QString& name) {
  send(OP_SAVE_PRESET, payloadSavePreset(quint8(engine), quint8(slot), name), true);
  // The occupied-slot list changed; refresh it shortly.
  QTimer::singleShot(200, this, [this, engine]() { listPresets(engine); });
}

void SynthController::listPresets(int engine) {
  if (!m_connected) return;
  send(OP_LIST_PRESETS, payloadListPresets(quint8(engine)), false);
}

void SynthController::noteOn(int note, int velocity) {
  send(OP_NOTE_ON, payloadNoteOn(quint8(note), quint8(velocity)), false);
}

void SynthController::noteOff(int note) {
  send(OP_NOTE_OFF, payloadNoteOff(quint8(note)), false);
}

void SynthController::allNotesOff() {
  for (int n = 0; n < 128; ++n) noteOff(n);
}

void SynthController::transport(int cmd, double tempo) {
  send(OP_TRANSPORT, payloadTransport(quint8(cmd), float(tempo)), true);
}

void SynthController::setArp(int enable, int mode, int octaves, int division) {
  send(OP_ARP, payloadArp(quint8(enable), quint8(mode), quint8(octaves), quint8(division)), true);
}

void SynthController::ping() { send(OP_PING, QByteArray(), true); }

/* --------------------------------------------------------- patch library */

// Parameters a patch must neither capture nor replay.
//
// Not every registered parameter is a sound setting. Some are *actions*, where
// the write itself is the operation; some are the transport; some are status
// the synth reports and owns. A snapshot took every parameter with a known
// value and load pushed all of them straight back, which performed those
// actions: a drum hit from drums.trig, flash writes from the preset and looper
// save triggers — loud enough to break up the audio, and they overwrite real
// user slots — and a wiped loop from loop.clear.
//
// PARAM_INFO carries no "this is an action" flag (only type/curve/range), so
// they go by name. The filter runs on load as well as on save, because patches
// saved before it existed still carry these rows.
static bool isNotPatchMaterial(const QString& name) {
  static const QSet<QString> kSkip = {
      // Actions: writing one performs it, whether or not the value moved.
      QStringLiteral("drums.trig"),          // fires a drum pad
      QStringLiteral("loop.clear"),          // wipes tracks; snaps back to none
      QStringLiteral("loop.save"),           // writes a looper set to flash/SD
      QStringLiteral("loop.load"),
      QStringLiteral("preset.save"),         // writes a synth preset slot
      QStringLiteral("preset.load"),
      QStringLiteral("preset.seq.save"),     // writes a sequencer pattern slot
      QStringLiteral("preset.seq.load"),
      QStringLiteral("preset.seqset.save"),  // writes a whole sequencer set
      QStringLiteral("preset.seqset.load"),
      // Transport (stop/play/rec): replaying one would start the synth playing
      // — or recording — behind the user.
      QStringLiteral("seq.mode"),
      QStringLiteral("loop.mode"),
      // Read-only status the synth reports; writing them means nothing.
      QStringLiteral("seq.pos"),
      QStringLiteral("seq.curpat"),
      QStringLiteral("loop.pos"),
      QStringLiteral("loop.len"),
      QStringLiteral("loop.filled"),
      QStringLiteral("loop.rectrk"),
      QStringLiteral("loop.armed"),
      QStringLiteral("loop.maxlen"),
  };
  return kSkip.contains(name);
}

int SynthController::saveCurrentAsPatch(const QString& name) {
  QList<QPair<int, double>> params;
  for (quint16 id : std::as_const(m_paramOrder)) {
    const Param& p = m_params.value(id);
    if (!p.valueKnown) continue;
    if (isNotPatchMaterial(paramName(id))) continue;
    params.append({int(id), double(p.value)});
  }
  if (params.isEmpty()) {
    emit showError(Translator::instance().t("Nothing to save yet — no parameters have been read."));
    return 0;
  }
  const int id = db().insertPatch(name, m_engine, params);
  if (id <= 0) emit showError(Translator::instance().t("Could not save the patch."));
  return id;
}

void SynthController::pushParams(const QList<QPair<int, double>>& params) {
  QByteArray payload;
  int pairs = 0;
  for (const auto& pv : params) {
    // Patches saved before saveCurrentAsPatch() started filtering still carry
    // rows for the action/status parameters, so drop them here too. An id whose
    // name has not arrived yet is written as before — patches saved from now on
    // never contain those rows anyway.
    if (isNotPatchMaterial(paramName(pv.first))) continue;
    appendSetParam(payload, quint16(pv.first), float(pv.second));
    applyValue(quint16(pv.first), float(pv.second), /*echo=*/true);
    if (++pairs >= kMaxSetPairsPerFrame) {
      send(OP_SET_PARAM, payload, false);
      payload.clear();
      pairs = 0;
    }
  }
  if (pairs > 0) send(OP_SET_PARAM, payload, false);
}

void SynthController::loadPatch(int patchId) {
  const QList<QPair<int, double>> params = db().getPatchParams(patchId);
  if (params.isEmpty()) return;

  // Find the patch's engine so we switch first if it differs from the live one.
  int patchEngine = m_engine;
  const QVariantList rows = db().getPatches(-1);
  for (const QVariant& r : rows) {
    const QVariantMap m = r.toMap();
    if (m.value("id").toInt() == patchId) {
      patchEngine = m.value("engine").toInt();
      break;
    }
  }

  if (patchEngine != m_engine) {
    selectEngine(patchEngine);
    m_pendingPatchParams = params;
    QTimer::singleShot(kEngineSwitchSettleMs, this, [this]() {
      pushParams(m_pendingPatchParams);
      m_pendingPatchParams.clear();
    });
  } else {
    pushParams(params);
  }
}

bool SynthController::renamePatch(int patchId, const QString& name) {
  return db().renamePatch(patchId, name);
}

bool SynthController::deletePatch(int patchId) { return db().deletePatch(patchId); }

QVariantList SynthController::patches(int engine) { return db().getPatches(engine); }

/* ======================================================================== */
/*  Sequencer (S23)                                                         */
/*                                                                          */
/*  Pattern data lives on the synth, not here: 8 tracks x 256 steps is far   */
/*  more than a BLE link wants to mirror, and the firmware is the only       */
/*  authority while the transport runs. The app caches exactly the slice the */
/*  grid shows — one pattern, one track — and re-reads it when either        */
/*  changes. Edits are optimistic: the local step updates immediately so the */
/*  grid never lags a tap, and the write goes out right after.               */
/* ======================================================================== */

namespace {
// A steps response repeats a 4-byte prefix per frame and carries 8 bytes per
// step, so a 247-byte MTU fits ~29. Ask for a round 24 at a time and let the
// chunker split further if the negotiated MTU is smaller.
constexpr int kStepWindow = 24;
}  // namespace

void SynthController::setEditPattern(int pattern) {
  if (pattern < 0 || (m_seqInfo.valid && pattern >= m_seqInfo.patterns)) return;
  if (pattern == m_editPattern) return;
  m_editPattern = pattern;
  emit editTargetChanged();
  // seq.pattern is the *playing* selection and the firmware queues it to a bar
  // boundary; the app's edit target follows it but is not the same thing, so
  // only the view is refreshed here.
  refreshSequencer();
}

void SynthController::setEditTrack(int track) {
  if (track < 0 || (m_seqInfo.valid && track >= m_seqInfo.tracks)) return;
  if (track == m_editTrack) return;
  m_editTrack = track;
  emit editTargetChanged();
  // Mirror into seq.edit.track so live recording and step input land on the
  // track the user is looking at.
  const int pid = paramIdForName(QStringLiteral("seq.edit.track"));
  if (pid > 0) setParam(pid, track + 1);
  refreshSequencer();
}

void SynthController::refreshSequencer() {
  if (!m_connected) return;
  send(OP_SEQ_INFO, QByteArray(), true);
  send(OP_SEQ_PATTERN, payloadSeqGetPattern(m_editPattern), true);
  send(OP_SEQ_TRACK, payloadSeqGetTrack(m_editPattern, m_editTrack), true);
  send(OP_SEQ_PLOCK, payloadSeqPlockListPattern(m_editPattern), true);
  m_stepsWindowNext = 0;
  requestSteps();
}

void SynthController::requestSteps() {
  if (m_stepsWindowNext < 0) return;
  const int len = m_trackCfg.length > 0 ? int(m_trackCfg.length) : 64;
  if (m_stepsWindowNext >= len) {
    m_stepsWindowNext = -1;
    return;
  }
  const int count = qMin(kStepWindow, len - m_stepsWindowNext);
  send(OP_SEQ_STEPS,
       payloadSeqGetSteps(m_editPattern, m_editTrack, m_stepsWindowNext, count),
       true);
}

QVariantList SynthController::steps() const {
  QVariantList out;
  out.reserve(m_steps.size());
  for (int i = 0; i < m_steps.size(); ++i) out.append(step(i));
  return out;
}

QVariantMap SynthController::step(int index) const {
  QVariantMap m;
  if (index < 0 || index >= m_steps.size()) {
    m["filled"] = false;
    return m;
  }
  const SeqStep& s = m_steps.at(index);
  m["index"] = index;
  m["note"] = s.note;
  m["vel"] = s.vel;
  m["gate"] = s.gate;
  m["prob"] = s.prob;
  m["micro"] = s.micro;
  m["ratchet"] = s.ratchet;
  m["cond"] = s.cond;
  m["flags"] = s.flags;
  m["filled"] = s.filled();
  m["accent"] = (s.flags & SF_ACCENT) != 0;
  m["slide"] = (s.flags & SF_SLIDE) != 0;
  m["muted"] = (s.flags & SF_MUTE) != 0;
  m["hasPlock"] = !m_plocks.value(plockKey(m_editTrack, index)).isEmpty();
  return m;
}

int SynthController::noteForSlot(int slot) const {
  for (const QVariant& v : m_kitSlots) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("slot")).toInt() == slot) {
      return m.value(QStringLiteral("note")).toInt();
    }
  }
  return -1;  // kit list not in yet, or the slot is empty
}

int SynthController::defaultDrumNote() const {
  // Kit order is musical, not arbitrary — the generator puts the kick first —
  // so the first populated slot is the right thing to start on.
  for (const QVariant& v : m_kitSlots) {
    const QVariantMap m = v.toMap();
    if (!m.value(QStringLiteral("name")).toString().isEmpty()) {
      return m.value(QStringLiteral("note")).toInt();
    }
  }
  return -1;
}

QString SynthController::drumNameForNote(int note) const {
  for (const QVariant& v : m_kitSlots) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("note")).toInt() == note) {
      return m.value(QStringLiteral("name")).toString();
    }
  }
  return QString();
}

int SynthController::noteForNewStep(int pickedNote) const {
  if (m_trackCfg.target != 1) return pickedNote;  // synth lane
  // "From step note": the note *is* the choice, so it is placed verbatim —
  // whatever the pads or the keyboard last picked.
  if (m_trackCfg.slot == SEQ_SLOT_FROM_NOTE) return pickedNote;
  // Fixed slot: the firmware ignores the note, so store the slot's own note
  // rather than a melodic pick that would mean nothing here — and would go
  // silent if the lane were later switched to "from step note".
  const int n = noteForSlot(m_trackCfg.slot);
  return n >= 0 ? n : pickedNote;
}

void SynthController::writeSteps(int first, int count) {
  // The frame cap allows ~30 step records; 24 keeps a margin for the header
  // and the prefix at any negotiated MTU.
  constexpr int kPerFrame = 24;
  for (int at = first; at < first + count; at += kPerFrame) {
    const int n = qMin(kPerFrame, first + count - at);
    QList<SeqStep> batch;
    batch.reserve(n);
    for (int i = 0; i < n; ++i) {
      if (at + i < m_steps.size()) batch.append(m_steps.at(at + i));
    }
    if (batch.isEmpty()) break;
    send(OP_SEQ_STEPS,
         payloadSeqSetSteps(m_editPattern, m_editTrack, at, batch), true);
  }
}

int SynthController::stampSlotNoteOnSteps(int slot) {
  const int note = noteForSlot(slot);
  if (note < 0) return 0;
  int changed = 0;
  int first = -1, last = -1;
  for (int i = 0; i < m_steps.size(); ++i) {
    if (!m_steps[i].filled() || m_steps[i].note == quint8(note)) continue;
    m_steps[i].note = quint8(note);
    ++changed;
    if (first < 0) first = i;
    last = i;
  }
  if (changed > 0) {
    writeSteps(first, last - first + 1);
    emit stepsChanged();
  }
  return changed;
}

void SynthController::toggleStep(int step, int note) {
  if (step < 0) return;
  if (step >= m_steps.size()) m_steps.resize(step + 1);
  SeqStep& s = m_steps[step];
  const bool wasFilled = s.filled();
  if (wasFilled) {
    s = SeqStep();  // vel 0 = empty
  } else {
    s.note = quint8(qBound(0, note, 127));
    s.vel = 100;
    s.gate = 16;
    s.prob = 100;
    s.micro = 0;
    s.ratchet = 1;
    s.cond = 0;
    s.flags = 0;
  }
  emit stepsChanged();

  // Send the resulting step, not the firmware's toggle sub-op. A toggle needs
  // both sides to agree on the *current* state; if this cache is stale — a
  // dropped window during a refresh, or an edit made from somewhere else —
  // the two flip to opposite states and stay inverted from then on. A set is
  // idempotent, so the app's view is authoritative and cannot drift.
  writeStep(step);

  if (wasFilled) {
    // Match the firmware's seq_step_clear(): clearing a step drops its locks.
    send(OP_SEQ_PLOCK,
         payloadSeqPlockClearStep(m_editPattern, m_editTrack, step), false);
    m_plocks.remove(plockKey(m_editTrack, step));
    emit plocksChanged();
  }
}

void SynthController::setStepField(int step, const QString& field, double value) {
  if (step < 0) return;
  if (step >= m_steps.size()) m_steps.resize(step + 1);
  SeqStep& s = m_steps[step];
  const int v = int(std::lround(value));
  if (field == QLatin1String("note")) {
    s.note = quint8(qBound(0, v, 127));
  } else if (field == QLatin1String("vel")) {
    s.vel = quint8(qBound(0, v, 127));
  } else if (field == QLatin1String("gate")) {
    s.gate = quint8(qBound(1, v, 255));
  } else if (field == QLatin1String("prob")) {
    s.prob = quint8(qBound(0, v, 100));
  } else if (field == QLatin1String("micro")) {
    s.micro = qint8(qBound(-50, v, 50));
  } else if (field == QLatin1String("ratchet")) {
    s.ratchet = quint8(qBound(1, v, 8));
  } else if (field == QLatin1String("cond")) {
    s.cond = quint8(qBound(0, v, seqCondNames().size() - 1));
  } else if (field == QLatin1String("accent")) {
    s.flags = quint8(v ? (s.flags | SF_ACCENT) : (s.flags & ~SF_ACCENT));
  } else if (field == QLatin1String("slide")) {
    s.flags = quint8(v ? (s.flags | SF_SLIDE) : (s.flags & ~SF_SLIDE));
  } else if (field == QLatin1String("mute")) {
    s.flags = quint8(v ? (s.flags | SF_MUTE) : (s.flags & ~SF_MUTE));
  } else {
    return;
  }
  emit stepsChanged();
  writeStep(step);
}

void SynthController::setStep(int step, const QVariantMap& fields) {
  for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
    setStepField(step, it.key(), it.value().toDouble());
  }
}

void SynthController::writeStep(int index) {
  if (index < 0 || index >= m_steps.size()) return;
  send(OP_SEQ_STEPS,
       payloadSeqSetSteps(m_editPattern, m_editTrack, index, {m_steps.at(index)}),
       false);
}

QVariantMap SynthController::trackConfig() const {
  QVariantMap m;
  m["target"] = m_trackCfg.target;
  m["slot"] = m_trackCfg.slot;
  m["length"] = m_trackCfg.length;
  m["div"] = m_trackCfg.div;
  m["dir"] = m_trackCfg.dir;
  m["transpose"] = m_trackCfg.transpose;
  m["swing"] = m_trackCfg.swing;
  m["gateScale"] = m_trackCfg.gateScale;
  m["velScale"] = m_trackCfg.velScale;
  m["probScale"] = m_trackCfg.probScale;
  m["humanize"] = m_trackCfg.humanize;
  m["scale"] = m_trackCfg.scale;
  m["root"] = m_trackCfg.root;
  m["followsPatternSwing"] = m_trackCfg.swing == 0xFF;
  m["followsPatternScale"] = m_trackCfg.scale == 0xFF;
  m["noteToSlot"] = m_trackCfg.slot == SEQ_SLOT_FROM_NOTE;
  return m;
}

void SynthController::setTrackField(const QString& field, double value) {
  const int v = int(std::lround(value));
  SeqTrackCfg& c = m_trackCfg;
  if (field == QLatin1String("target")) {
    c.target = quint8(qBound(0, v, 1));
  } else if (field == QLatin1String("slot")) {
    const int newSlot = qBound(0, v, 255);
    // Switching a fixed-slot drum lane to "from step note" changes what every
    // *existing* step plays: the note suddenly selects the drum, and the notes
    // already stored were irrelevant until now — a note no kit slot answers to
    // resolves to nothing and the step goes silent. Stamp the lane's outgoing
    // slot onto them so the switch keeps the pattern sounding exactly as it
    // did, and seed the paint note to match so the next placement continues
    // the same drum.
    if (newSlot == SEQ_SLOT_FROM_NOTE && c.slot != SEQ_SLOT_FROM_NOTE) {
      const int changed = stampSlotNoteOnSteps(c.slot);
      const int note = noteForSlot(c.slot);
      if (note >= 0) emit paintNoteSuggested(note);
      if (changed > 0) {
        emit showInfo(tr("Kept %1 step(s) on %2 — the note now picks the drum.")
                          .arg(changed)
                          .arg(drumNameForNote(note)));
      }
    }
    c.slot = quint8(newSlot);
  } else if (field == QLatin1String("length")) {
    c.length = quint16(qBound(1, v, m_seqInfo.maxSteps ? m_seqInfo.maxSteps : 256));
  } else if (field == QLatin1String("div")) {
    c.div = quint8(qBound(0, v, int(seqDivNames().size()) - 1));
  } else if (field == QLatin1String("dir")) {
    c.dir = quint8(qBound(0, v, int(seqDirNames().size()) - 1));
  } else if (field == QLatin1String("transpose")) {
    c.transpose = qint8(qBound(-24, v, 24));
  } else if (field == QLatin1String("swing")) {
    // Negative means "follow the pattern" — the wire encoding for that is 0xFF.
    c.swing = quint8(v < 0 ? 0xFF : qBound(0, v, 75));
  } else if (field == QLatin1String("gateScale")) {
    c.gateScale = quint8(qBound(0, v, 200));
  } else if (field == QLatin1String("velScale")) {
    c.velScale = quint8(qBound(0, v, 200));
  } else if (field == QLatin1String("probScale")) {
    c.probScale = quint8(qBound(0, v, 100));
  } else if (field == QLatin1String("humanize")) {
    c.humanize = quint8(qBound(0, v, 100));
  } else if (field == QLatin1String("scale")) {
    c.scale = quint8(v < 0 ? 0xFF : qBound(0, v, 11));
  } else if (field == QLatin1String("root")) {
    c.root = quint8(v < 0 ? 0xFF : qBound(0, v, 11));
  } else {
    return;
  }
  emit trackConfigChanged();
  send(OP_SEQ_TRACK, payloadSeqSetTrack(m_editPattern, m_editTrack, c), true);
  // A length change moves the grid's extent: re-read from the top.
  if (field == QLatin1String("length")) {
    m_steps.resize(c.length);
    m_stepsWindowNext = 0;
    requestSteps();
  }
}

QVariantMap SynthController::patternConfig() const {
  QVariantMap m;
  m["length"] = m_patternLength;
  m["scale"] = m_patternScale;
  m["root"] = m_patternRoot;
  m["swing"] = m_patternSwing;
  m["name"] = m_patternName;
  return m;
}

void SynthController::setPatternField(const QString& field, double value) {
  const int v = int(std::lround(value));
  if (field == QLatin1String("length")) {
    m_patternLength = qBound(1, v, m_seqInfo.maxSteps ? m_seqInfo.maxSteps : 256);
  } else if (field == QLatin1String("scale")) {
    m_patternScale = qBound(0, v, 11);
  } else if (field == QLatin1String("root")) {
    m_patternRoot = qBound(0, v, 11);
  } else if (field == QLatin1String("swing")) {
    m_patternSwing = qBound(0, v, 75);
  } else {
    return;
  }
  send(OP_SEQ_PATTERN,
       payloadSeqSetPattern(m_editPattern, m_patternLength, m_patternScale,
                            m_patternRoot, m_patternSwing, m_patternName),
       true);
}

void SynthController::setPatternName(const QString& name) {
  m_patternName = name.left(11);
  send(OP_SEQ_PATTERN,
       payloadSeqSetPattern(m_editPattern, m_patternLength, m_patternScale,
                            m_patternRoot, m_patternSwing, m_patternName),
       true);
}

void SynthController::clearPattern(int pattern) {
  const int p = pattern < 0 ? m_editPattern : pattern;
  send(OP_SEQ_EDIT, payloadSeqClearPattern(p), true);
  if (p == m_editPattern) refreshSequencer();
}

void SynthController::clearTrack(int track) {
  const int t = track < 0 ? m_editTrack : track;
  send(OP_SEQ_EDIT, payloadSeqClearTrack(m_editPattern, t), true);
  if (t == m_editTrack) refreshSequencer();
}

void SynthController::copyPattern(int src, int dst) {
  send(OP_SEQ_EDIT, payloadSeqCopyPattern(src, dst), true);
  if (dst == m_editPattern) refreshSequencer();
}

void SynthController::rotateTrack(int delta) {
  send(OP_SEQ_EDIT, payloadSeqRotate(m_editPattern, m_editTrack, delta), true);
  refreshSequencer();
}

void SynthController::euclidFill(int pulses, int steps, int rotate, int note,
                                 int velocity) {
  send(OP_SEQ_EDIT,
       payloadSeqEuclid(m_editPattern, m_editTrack, pulses, steps, rotate, note,
                        velocity),
       true);
  refreshSequencer();
}

void SynthController::humanizeTrack(int amount) {
  send(OP_SEQ_EDIT, payloadSeqHumanize(m_editPattern, m_editTrack, amount), true);
  refreshSequencer();
}

QVariantList SynthController::plocksForStep(int step) const {
  return m_plocks.value(plockKey(m_editTrack, step));
}

void SynthController::setPlock(int step, int pid, double value) {
  send(OP_SEQ_PLOCK,
       payloadSeqPlockSet(m_editPattern, m_editTrack, step, pid, float(value)),
       true);
  // Reflect locally so the grid shades the step without a round trip.
  QVariantList& locks = m_plocks[plockKey(m_editTrack, step)];
  for (int i = 0; i < locks.size(); ++i) {
    if (locks.at(i).toMap().value("pid").toInt() == pid) {
      QVariantMap m = locks.at(i).toMap();
      m["value"] = value;
      locks[i] = m;
      emit plocksChanged();
      emit stepsChanged();
      return;
    }
  }
  QVariantMap m;
  m["pid"] = pid;
  m["value"] = value;
  m["name"] = paramName(pid);
  locks.append(m);
  emit plocksChanged();
  emit stepsChanged();
}

void SynthController::clearPlock(int step, int pid) {
  // NaN is the firmware's "remove this lock" value.
  send(OP_SEQ_PLOCK,
       payloadSeqPlockSet(m_editPattern, m_editTrack, step, pid,
                          std::numeric_limits<float>::quiet_NaN()),
       true);
  QVariantList& locks = m_plocks[plockKey(m_editTrack, step)];
  for (int i = 0; i < locks.size(); ++i) {
    if (locks.at(i).toMap().value("pid").toInt() == pid) {
      locks.removeAt(i);
      break;
    }
  }
  emit plocksChanged();
  emit stepsChanged();
}

void SynthController::clearStepPlocks(int step) {
  send(OP_SEQ_PLOCK, payloadSeqPlockClearStep(m_editPattern, m_editTrack, step),
       true);
  m_plocks.remove(plockKey(m_editTrack, step));
  emit plocksChanged();
  emit stepsChanged();
}

QVariantList SynthController::song() const { return m_song; }

void SynthController::setSong(const QVariantList& chain) {
  QList<QPair<int, int>> entries;
  for (const QVariant& v : chain) {
    const QVariantMap m = v.toMap();
    entries.append({m.value("pattern").toInt(), qMax(1, m.value("repeats").toInt())});
  }
  send(OP_SEQ_SONG, payloadSeqSongSet(entries), true);
  m_song = chain;
  emit songChanged();
}

/* -------------------------------------------------------------- drum kit */

void SynthController::refreshKit() {
  if (!m_connected) return;
  send(OP_KIT_INFO, payloadKitInfo(0), true);  // selectable kits
  send(OP_KIT_INFO, payloadKitInfo(1), true);  // the current kit's slots
}

void SynthController::selectKit(int index) {
  const int pid = paramIdForName(QStringLiteral("drums.kit"));
  if (pid > 0) setParam(pid, index);
  // The firmware loads asynchronously (SD I/O on a control task); re-read the
  // slots once it has had a chance to publish the new kit.
  QTimer::singleShot(600, this, [this]() { refreshKit(); });
}

void SynthController::triggerDrum(int slot, int velocity) {
  if (slot < 0) return;
  // Write-without-response either way: a pad hit is the one thing in this app
  // where latency is audible, and there is nothing useful to do with an ack.
  if (m_drumTrigOpcode) {
    send(OP_DRUM_TRIG, payloadDrumTrig(slot, qBound(1, velocity, 127)), false);
    return;
  }
  // Firmware without the opcode: the drums.trig parameter. It carries no
  // velocity (a parameter is one float), and it has to bypass setParam's
  // coalescing — two hits on the same pad inside one 40 ms batch would
  // collapse into a single write. A repeated write of the same value is fine:
  // ParamStore::set() notifies its listeners whether or not the value moved.
  const int pid = paramIdForName(QStringLiteral("drums.trig"));
  if (pid <= 0) return;
  QByteArray p;
  appendSetParam(p, quint16(pid), float(slot));
  send(OP_SET_PARAM, p, false);
}

QStringList SynthController::scaleNames() const {
  // Prefer the firmware's own enum labels when discovery has them.
  const int pid = paramIdForName(QStringLiteral("seq.scale"));
  if (pid > 0) {
    const Param& p = m_params.value(quint16(pid));
    if (p.infoKnown && !p.info.enumNames.isEmpty()) return p.info.enumNames;
  }
  return {"chromatic", "major",      "minor",     "dorian",
          "phrygian",  "lydian",     "mixolydian", "locrian",
          "harm minor", "penta maj", "penta min", "blues"};
}

QString SynthController::noteName(int note) const {
  static const char* kNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                   "F#", "G",  "G#", "A",  "A#", "B"};
  if (note < 0 || note > 127) return QString();
  // C4 = 60 (Yamaha): octave = note/12 - 1.
  return QStringLiteral("%1%2").arg(QLatin1String(kNames[note % 12])).arg(note / 12 - 1);
}

/* ------------------------------------------------- sequencer frame decode */

void SynthController::handleSeqInfo(const QByteArray& payload) {
  const SeqInfo info = parseSeqInfo(payload);
  if (!info.valid) return;
  const bool sizingChanged = info.tracks != m_seqInfo.tracks ||
                             info.patterns != m_seqInfo.patterns ||
                             info.maxSteps != m_seqInfo.maxSteps;
  m_seqInfo = info;
  if (m_editTrack >= info.tracks) m_editTrack = 0;
  if (m_editPattern >= info.patterns) m_editPattern = 0;
  emit seqInfoChanged();
  if (sizingChanged) emit editTargetChanged();
}

void SynthController::handleSeqSteps(const QByteArray& payload, bool more) {
  Reader r(payload);
  const int pattern = r.u8();
  const int track = r.u8();
  const int first = r.u16();
  if (!r.ok) return;
  // A response for a target the user has since navigated away from is stale.
  if (pattern != m_editPattern || track != m_editTrack) return;

  if (m_stepsAccum.isEmpty()) m_stepsAccumFirst = first;
  while (r.remaining >= 8) m_stepsAccum.append(readStep(r));
  if (more) return;

  const int len = m_trackCfg.length > 0 ? int(m_trackCfg.length) : 64;
  if (m_steps.size() != len) m_steps.resize(len);
  for (int i = 0; i < m_stepsAccum.size(); ++i) {
    const int at = m_stepsAccumFirst + i;
    if (at >= 0 && at < m_steps.size()) m_steps[at] = m_stepsAccum.at(i);
  }
  const int next = m_stepsAccumFirst + int(m_stepsAccum.size());
  m_stepsAccum.clear();
  emit stepsChanged();

  // Walk to the next window until the whole track is in.
  if (m_stepsWindowNext >= 0) {
    m_stepsWindowNext = next;
    requestSteps();
  }
}

void SynthController::handleSeqTrack(const QByteArray& payload) {
  Reader r(payload);
  const int pattern = r.u8();
  const int track = r.u8();
  if (!r.ok) return;
  const SeqTrackCfg cfg = readTrackCfg(r);
  if (!r.ok || pattern != m_editPattern || track != m_editTrack) return;
  const bool lengthChanged = cfg.length != m_trackCfg.length;
  m_trackCfg = cfg;
  emit trackConfigChanged();
  if (lengthChanged) {
    m_steps.resize(cfg.length);
    m_stepsWindowNext = 0;
    requestSteps();
    emit stepsChanged();
  }
}

void SynthController::handleSeqPattern(const QByteArray& payload) {
  Reader r(payload);
  const int pattern = r.u8();
  const int length = r.u16();
  const int scale = r.u8();
  const int root = r.u8();
  const int swing = r.u8();
  if (!r.ok || pattern != m_editPattern) return;
  m_patternLength = length;
  m_patternScale = scale;
  m_patternRoot = root;
  m_patternSwing = swing;
  m_patternName = r.cstr();
  emit trackConfigChanged();
}

void SynthController::handleSeqPlock(const QByteArray& payload, bool more) {
  Reader r(payload);
  const quint8 kind = r.u8();
  if (kind != 4) {
    // Per-step listing: {kind=0, pattern, track, step(u16)} then {pid, value}
    // records. The kind byte leads so a pattern index of 4 cannot be read as
    // the pattern-wide listing below.
    const int pattern = r.u8();
    const int track = r.u8();
    const int step = r.u16();
    if (!r.ok || pattern != m_editPattern) return;
    QVariantList locks;
    while (r.remaining >= 6) {
      QVariantMap m;
      m["pid"] = r.u16();
      m["value"] = r.f32();
      m["name"] = paramName(m.value("pid").toInt());
      locks.append(m);
    }
    m_plocks.insert(plockKey(track, step), locks);
    if (!more) {
      emit plocksChanged();
      emit stepsChanged();
    }
    return;
  }

  const int pattern = r.u8();
  if (!r.ok || pattern != m_editPattern) return;
  while (r.remaining >= 9) {
    const int track = r.u8();
    const int step = r.u16();
    QVariantMap m;
    m["pid"] = r.u16();
    m["value"] = r.f32();
    m["name"] = paramName(m.value("pid").toInt());
    if (!r.ok) break;
    m_plocksAccum[plockKey(track, step)].append(m);
  }
  if (more) return;
  m_plocks = m_plocksAccum;
  m_plocksAccum.clear();
  emit plocksChanged();
  emit stepsChanged();
}

void SynthController::handleSeqSong(const QByteArray& payload, bool more) {
  Reader r(payload);
  r.u8();  // chain-length prefix; the records below are authoritative
  while (r.remaining >= 2) {
    QVariantMap m;
    m["pattern"] = r.u8();
    m["repeats"] = r.u8();
    if (!r.ok) break;
    m_songAccum.append(m);
  }
  if (more) return;
  m_song = m_songAccum;
  m_songAccum.clear();
  emit songChanged();
}

void SynthController::handleKitInfo(const QByteArray& payload, bool more) {
  Reader r(payload);
  const quint8 what = r.u8();
  if (what == 0) {
    m_currentKit = r.u8();
    r.u8();  // count; the records are authoritative
    while (r.remaining >= 1 + 24) {
      QVariantMap m;
      m["index"] = r.u8();
      QByteArray raw(reinterpret_cast<const char*>(r.p), 24);
      r.p += 24;
      r.remaining -= 24;
      const int nul = raw.indexOf('\0');
      if (nul >= 0) raw.truncate(nul);
      m["name"] = QString::fromUtf8(raw);
      m_kitsAccum.append(m);
    }
    if (more) return;
    m_kits = m_kitsAccum;
    m_kitsAccum.clear();
    emit kitChanged();
    return;
  }

  r.u8();  // slot count; the records are authoritative
  r.u8();  // reserved
  while (r.remaining >= 2 + 12) {
    QVariantMap m;
    m["slot"] = r.u8();
    m["note"] = r.u8();
    QByteArray raw(reinterpret_cast<const char*>(r.p), 12);
    r.p += 12;
    r.remaining -= 12;
    const int nul = raw.indexOf('\0');
    if (nul >= 0) raw.truncate(nul);
    m["name"] = QString::fromUtf8(raw);
    m_kitSlotsAccum.append(m);
  }
  if (more) return;
  m_kitSlots = m_kitSlotsAccum;
  m_kitSlotsAccum.clear();
  emit kitChanged();
}
