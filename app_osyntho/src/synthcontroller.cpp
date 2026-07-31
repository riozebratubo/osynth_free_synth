#include "synthcontroller.h"

#include "translator.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

using namespace SynthProto;

namespace {
constexpr int kSetCoalesceMs = 40;         // ~25 Hz knob-write batches
constexpr int kEngineSwitchSettleMs = 400; // let 0x02xx re-register before a patch push
constexpr int kPresetLoadSettleMs = 500;   // firmware finishes loading a preset slot
constexpr int kPresetReadSettleMs = 400;   // GET_PARAM answers land before snapshotting
// Ceilings at the documented MTU (247 -> 240 payload bytes): 40×6B of set
// pairs, 120×2B of get ids, 24 step records. maxPayloadBytes() scales these
// down on a link that negotiated less.
constexpr int kMaxSetPairsPerFrame = 40;
constexpr int kMaxGetIdsPerFrame = 120;
constexpr int kMaxStepsPerFrame = 24;

// Paced SET_PARAM output. 15 ms between frames keeps a burst well inside what
// the firmware's 4-deep command queue can drain, while an idle queue still
// sends immediately — a knob or a pad hit is never delayed by this.
constexpr int kSetDrainIntervalMs = 15;
constexpr int kSetBusyBackoffMs = 200;
constexpr int kSetSentHistory = 8;  // frames kept for a BUSY re-queue

// Resend of a request frame the firmware dropped. Its command queue is four
// deep and answers BUSY *instead of* handling the frame, and discovery ends by
// firing about a dozen requests back to back (two value sweeps, the preset
// list, four sequencer reads, two kit reads, the song chain, the DRUM_TRIG
// probe). Whatever lost that race used to be gone for good, leaving that page
// blank until the user found the Refresh button.
constexpr int kRequestRetryDelayMs = 120;
constexpr int kRequestRetries = 3;
constexpr int kSentRequestHistory = 16;

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
// Metadata the app's chrome is gated on, and which therefore must not wait its
// turn in the walk. The toolbar's volume slider and the home page's Master card
// both key off PARAM_INFO for master.volume — until it lands there is no range
// to draw a slider against, so both hide themselves.
//
// The firmware lists ids in ascending order, so id 0 is asked first anyway;
// what made it appear seconds into a sync is what happens when that first
// request is refused (BUSY, the queue still holding the value sweep sent
// alongside it) or lost: the id went back to the *end* of a ~250-id queue.
// These ids are requested ahead of the list response and re-queued at the
// front, so a bad first attempt costs one retry rather than the whole walk.
constexpr quint16 kPriorityInfoIds[] = {0x0000};  // master.volume
// Settle time before re-reading a pattern the firmware switched to itself.
constexpr int kFollowPatternMs = 500;
// If refreshSequencer's SEQ_TRACK get goes unanswered, start the step walk
// anyway on the cached length rather than leaving the grid empty.
constexpr int kTrackGetFallbackMs = 1500;
// Watchdog on each step window of that walk. The next window is requested only
// by the previous one's response, so one lost notification stalls the rest of
// the pattern; re-ask rather than leave the grid half drawn. Generous, because
// a window is up to 24 step records and the link may be busy with a value sweep.
constexpr int kStepsWindowTimeoutMs = 1500;
constexpr int kStepsWindowRetries = 3;

// Well-known ids the controller tracks directly.
constexpr quint16 ID_PRESET_LOAD = 0x0002;
// The engine-specific block: whatever engine is bound owns it, and its meaning
// changes wholesale on a switch. Everything outside it survives one.
constexpr quint16 ID_ENGINE_FIRST = 0x0200;
constexpr quint16 ID_ENGINE_LAST = 0x02FF;
// seq.pos: firmware-written playhead, -1 while stopped. It arrives in the
// ~20 Hz EVT_PARAMS batches like any other change, so the grid highlight
// costs no extra traffic.
constexpr quint16 ID_SEQ_POS = 0x040B;
constexpr quint16 ID_SEQ_CURPAT = 0x040C;
// Modular graph telemetry (S28). graph.rev is how the app learns the patch
// changed without a dedicated event opcode — including changes it did not
// cause, such as a preset load replacing the whole graph.
constexpr quint16 ID_GRAPH_COST = 0x02C0;
constexpr quint16 ID_GRAPH_REV = 0x02C1;

bool isPriorityInfoId(quint16 id) {
  for (quint16 pid : kPriorityInfoIds) {
    if (pid == id) return true;
  }
  return false;
}

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
}  // namespace

SynthController::SynthController(QObject* parent) : QObject(parent) {
  m_setTimer.setSingleShot(true);
  m_setTimer.setInterval(kSetCoalesceMs);
  connect(&m_setTimer, &QTimer::timeout, this, &SynthController::flushPendingSets);

  m_setDrainTimer.setSingleShot(true);
  m_setDrainTimer.setInterval(kSetDrainIntervalMs);
  connect(&m_setDrainTimer, &QTimer::timeout, this, &SynthController::drainSetQueue);

  m_requestRetryTimer.setSingleShot(true);
  m_requestRetryTimer.setInterval(kRequestRetryDelayMs);
  connect(&m_requestRetryTimer, &QTimer::timeout, this,
          &SynthController::drainRequestRetryQueue);

  m_noteOffTimer.setSingleShot(true);
  m_noteOffTimer.setInterval(kSetDrainIntervalMs);
  connect(&m_noteOffTimer, &QTimer::timeout, this,
          &SynthController::drainNoteOffQueue);

  m_trackGetFallbackTimer.setSingleShot(true);
  m_trackGetFallbackTimer.setInterval(kTrackGetFallbackMs);
  connect(&m_trackGetFallbackTimer, &QTimer::timeout, this, [this]() {
    if (!m_awaitingTrackGet) return;
    m_awaitingTrackGet = false;
    beginStepWalk();
  });

  m_stepsRetryTimer.setSingleShot(true);
  m_stepsRetryTimer.setInterval(kStepsWindowTimeoutMs);
  connect(&m_stepsRetryTimer, &QTimer::timeout, this, [this]() {
    if (!m_connected || m_stepsWindowNext < 0) return;
    if (m_stepsWindowRetries <= 0) {
      qWarning() << "Synth | step window" << m_stepsWindowNext
                 << "unanswered after" << kStepsWindowRetries
                 << "tries; stopping the walk";
      m_stepsWindowNext = -1;
      return;
    }
    m_stepsWindowRetries--;
    m_stepsAccum.clear();  // a half-arrived window must not be resumed
    requestSteps();
  });

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

namespace {
// Which ops are worth resending after a BUSY.
//
// SET_PARAM has its own paced queue and PARAM_INFO its own discovery pump —
// both already re-queue on BUSY, and tracking them here would retry each twice.
// Notes and pad hits are excluded on purpose: by the time a resend went out the
// gesture is over, and a late note is worse than a missing one. PING answers
// nothing anyone waits on.
bool isRetryableRequest(quint8 op) {
  switch (op) {
    case OP_SET_PARAM:
    case OP_PARAM_INFO:
    case OP_NOTE_ON:
    case OP_NOTE_OFF:
    case OP_DRUM_TRIG:
    case OP_PING:
      return false;
    default:
      return true;
  }
}
}  // namespace

void SynthController::sendWithSeq(quint8 op, quint8 seq, const QByteArray& payload,
                                  bool withResponse) {
  if (!m_connected) return;
  const QByteArray frame = buildFrame(op, seq, payload);
  if (isRetryableRequest(op)) {
    trackRequest(seq, frame, withResponse, kRequestRetries);
  }
  emit writeToSynth(frame, withResponse);
}

void SynthController::trackRequest(quint8 seq, const QByteArray& frame,
                                   bool withResponse, int retriesLeft) {
  m_sentRequestOrder.removeAll(seq);  // a wrapped seq must not evict its successor
  m_sentRequests.insert(seq, PendingRequest{frame, withResponse, retriesLeft});
  m_sentRequestOrder.append(seq);
  while (m_sentRequestOrder.size() > kSentRequestHistory) {
    m_sentRequests.remove(m_sentRequestOrder.takeFirst());
  }
}

void SynthController::forgetRequest(quint8 seq) {
  if (m_sentRequests.remove(seq) > 0) m_sentRequestOrder.removeAll(seq);
}

void SynthController::onRequestBusy(quint8 seq) {
  const auto it = m_sentRequests.constFind(seq);
  if (it == m_sentRequests.constEnd()) return;  // aged out; nothing to resend
  PendingRequest req = it.value();
  forgetRequest(seq);
  if (req.retriesLeft <= 0) {
    qWarning() << "Synth | op" << Qt::hex << quint8(req.frame.at(0))
               << "still BUSY after" << kRequestRetries << "retries; giving up";
    return;
  }
  req.retriesLeft--;
  m_requestRetryQueue.append(req);
  if (!m_requestRetryTimer.isActive()) m_requestRetryTimer.start(kRequestRetryDelayMs);
}

void SynthController::drainRequestRetryQueue() {
  if (!m_connected) {
    m_requestRetryQueue.clear();
    m_requestRetryTimer.stop();
    return;
  }
  if (m_requestRetryQueue.isEmpty()) {
    m_requestRetryTimer.stop();
    return;
  }
  const PendingRequest req = m_requestRetryQueue.takeFirst();
  // The original bytes, so the resend keeps its seq. That matters: the
  // discovery list response is matched by m_infoListSeq and refreshSequencer's
  // track read by m_trackGetSeq, and a fresh seq would make both unroutable.
  const quint8 seq = quint8(req.frame.at(1));
  trackRequest(seq, req.frame, req.withResponse, req.retriesLeft);
  emit writeToSynth(req.frame, req.withResponse);
  if (!m_requestRetryQueue.isEmpty()) m_requestRetryTimer.start(kRequestRetryDelayMs);
}

void SynthController::send(quint8 op, const QByteArray& payload, bool withResponse) {
  sendWithSeq(op, nextSeq(), payload, withResponse);
}

/* ------------------------------------------------------------ frame sizing */

int SynthController::maxPayloadBytes() const {
  // ATT write payload is MTU-3; the SynthCtl frame header takes 4 more. The
  // floor keeps the batch helpers from degenerating to zero on a link stuck at
  // the 23-byte default (they clamp to at least one record anyway).
  return qMax(1, SynthProto::attPayloadFor(m_linkMtu) - 4);
}

int SynthController::maxSetPairsPerFrame() const {
  return qBound(1, maxPayloadBytes() / 6, kMaxSetPairsPerFrame);
}

int SynthController::maxGetIdsPerFrame() const {
  return qBound(1, maxPayloadBytes() / 2, kMaxGetIdsPerFrame);
}

int SynthController::maxStepsPerFrame() const {
  // A SET_STEPS payload carries a 6-byte prefix ahead of the step records.
  return qBound(1, (maxPayloadBytes() - 6) / 8, kMaxStepsPerFrame);
}

QString SynthController::engineNameFor(int engine) {
  switch (engine) {
    case ENG_SUBTRACTIVE: return QStringLiteral("Subtractive");
    case ENG_ADDITIVE:    return QStringLiteral("Additive");
    case ENG_FM:          return QStringLiteral("FM");
    case ENG_WAVETABLE:   return QStringLiteral("Wavetable");
    case ENG_MODULAR:     return QStringLiteral("Modular");
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

void SynthController::setLinkMtu(int mtu) {
  if (mtu == m_linkMtu) return;
  m_linkMtu = mtu;
  qDebug() << "Synth | link MTU" << mtu << "->" << maxPayloadBytes()
           << "payload bytes per frame";
}

void SynthController::resetState() {
  m_ready = false;
  m_linkMtu = 0;
  m_discovering = false;
  m_discoveryScope = DiscoveryScope::Full;
  m_awaitingInfoList = false;
  m_discoveryTimer.stop();
  m_infoRequestTimer.stop();
  m_discoveredCoalesceTimer.stop();
  m_followPatternTimer.stop();
  m_trackGetFallbackTimer.stop();
  m_stepsRetryTimer.stop();
  m_stepsWindowRetries = 0;
  m_awaitingTrackGet = false;
  m_setTimer.stop();
  m_setDrainTimer.stop();
  m_setQueue.clear();
  m_setSent.clear();
  m_setSentOrder.clear();
  m_setBackoffUntilMs = 0;
  m_setNextSendMs = 0;
  m_pendingSets.clear();
  m_requestRetryTimer.stop();
  m_requestRetryQueue.clear();
  m_sentRequests.clear();
  m_sentRequestOrder.clear();
  // The link is gone, so anything it was holding is released on the synth side
  // too (the firmware runs voice_manager_all_notes_off() on disconnect).
  m_noteOffTimer.stop();
  m_noteOffQueue.clear();
  m_heldNotes.clear();
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

void SynthController::beginDiscovery(DiscoveryScope scope) {
  m_discovering = true;
  m_discoveryScope = scope;
  m_awaitingInfoList = true;
  m_listRetries = kListRetries;
  m_infoListAccum.clear();
  m_infoQueue.clear();
  m_infoInflight.clear();
  m_infoSeqToId.clear();
  // Rebuilt from the id list this pass is about to fetch. Leaving the previous
  // pass's leftovers in place used to make them permanently un-discoverable:
  // onParamListComplete() only queues an id it is not already "pending" on, so
  // anything stranded by an aborted pass (budget spent, or an engine switch
  // mid-discovery) was never requested again — the pump then span for the full
  // 40 s budget on an empty queue and those controls stayed missing from the
  // UI for the rest of the session.
  m_pendingInfoIds.clear();
  m_infoBackoffUntilMs = 0;
  m_discoveryStartMs = nowMs();
  m_infoRequestTimer.stop();
  m_infoListSeq = nextSeq();
  // Write-without-response (non-blocking) even for this linchpin request: on an
  // unsettled link a with-response write can block the GUI until the ATT
  // timeout. If it (or its response) is dropped, the watchdog resends it.
  sendWithSeq(OP_PARAM_INFO, m_infoListSeq, payloadParamInfo(PARAM_INFO_LIST_ALL), false);
  m_discoveryTimer.start();  // list watchdog

  // The shell's own parameters, asked for now rather than after the list: the
  // master volume control is then on screen one round trip into the sync,
  // instead of waiting on a list response the watchdog may have to resend
  // (1.2 s a try) and then on its turn in the metadata walk. Their values go
  // with them — paramValue() falls back to the *default* until a value lands,
  // which for master.volume is a slider sitting at 0.8 wherever the synth
  // actually is. Two frames against a 16-deep firmware queue, and the pump
  // below skips whatever has already answered by the time it starts.
  //
  // Gated on what is actually missing rather than on the scope: these ids sit
  // outside the 0x02xx range an engine switch re-walks, so on that pass they
  // are already known and this costs nothing — while a connect pass gets the
  // prefetch whichever way it was kicked (the EVT_ENGINE greeting or the
  // fallback in setConnected).
  QList<quint16> priority;
  for (quint16 id : kPriorityInfoIds) {
    const auto it = m_params.constFind(id);
    if (it != m_params.constEnd() && it->infoKnown && it->valueKnown) continue;
    sendInfoRequest(id);
    priority.append(id);
  }
  if (!priority.isEmpty()) requestParamValues(priority);
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
      queueInfoId(id);
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

// Queue an id for the metadata pump. Priority ids (kPriorityInfoIds) go to the
// front — on the first pass because the shell is waiting on them, and on a
// re-queue after a BUSY or a timeout because that is the case that hurt: an id
// appended behind a full pass's ~250 others reappears seconds later, which is
// exactly how long the master volume control used to take to show up.
void SynthController::queueInfoId(quint16 id) {
  if (m_infoQueue.contains(id)) return;
  if (isPriorityInfoId(id))
    m_infoQueue.prepend(id);
  else
    m_infoQueue.append(id);
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
  // Nothing to pump — and nothing to conclude from an empty pending set —
  // until the id list has landed.
  if (m_awaitingInfoList) return;

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
      // Retire the seq that request went out with as well. nextSeq() cycles
      // through 255 values, and a discovery pass sends about as many requests
      // as there are parameters — so a mapping left behind by a timed-out
      // request can still be there when its seq comes round again, and would
      // then attribute the new request's BUSY or BAD_ARG to an unrelated id
      // (re-queueing one already answered, or striking one off for good).
      // A late answer to the retired request is dropped rather than misfiled;
      // the id is back on the queue below, so it is asked again either way.
      for (auto sit = m_infoSeqToId.begin(); sit != m_infoSeqToId.end();) {
        if (sit.value() == id)
          sit = m_infoSeqToId.erase(sit);
        else
          ++sit;
      }
      if (m_pendingInfoIds.contains(id)) queueInfoId(id);
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
  if (m_pendingInfoIds.contains(id)) queueInfoId(id);
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
  if (m_discovering && !m_awaitingInfoList && m_pendingInfoIds.isEmpty()) finishDiscovery();
}

void SynthController::finishDiscovery() {
  m_discovering = false;
  m_infoRequestTimer.stop();
  m_infoInflight.clear();
  m_infoSeqToId.clear();
  m_infoQueue.clear();
  // Also on the budget-spent path, where ids are still outstanding: a pass
  // that gives up must not leave them marked, or the next beginDiscovery()
  // would skip them (see the note there).
  m_pendingInfoIds.clear();
  scheduleParamsDiscovered();

  // A second value sweep. onParamListComplete() already fetched them so the UI
  // was never showing defaults; this catches anything the synth changed during
  // the metadata walk (a MIDI CC, a preset load, the sequencer's telemetry),
  // which at ~200 params is a few seconds of opportunity. An engine-switch pass
  // only walks the ~36 ids of the new engine, so its window is a fraction of
  // that and one frame covering that range is enough.
  if (m_discoveryScope == DiscoveryScope::EngineParams) {
    requestParamValues(engineParamIds());
  } else {
    requestAllParamValues();
  }
  // Presets are stored per engine, so this one *is* engine-scoped.
  listPresets(m_engine);

  if (m_discoveryScope == DiscoveryScope::EngineParams) return;

  // The sequencer and kit are not parameters, so discovery does not reach
  // them; ask once the parameter traffic has been queued.
  refreshSequencer();
  refreshKit();
  // Same reasoning: the patch graph's structure is not parameter space, so
  // discovery never sees it. This doubles as the capability probe — a
  // firmware without the modular engine refuses GRAPH_INFO and the patch
  // page stays hidden.
  refreshGraph();
  m_songAccum.clear();
  send(OP_SEQ_SONG, payloadSeqSongGet(), false);
  // Capability probe: the firmware ignores velocity 0, so this is silent where
  // the opcode exists and answers UNKNOWN_OP where it does not.
  m_drumTrigOpcode = true;
  send(OP_DRUM_TRIG, payloadDrumTrig(0, 0), false);
}

void SynthController::scheduleParamsDiscovered() {
  if (!m_discoveredCoalesceTimer.isActive()) m_discoveredCoalesceTimer.start();
}

void SynthController::requestAllParamValues() { requestParamValues(m_paramOrder); }

void SynthController::requestParamValues(const QList<quint16>& ids) {
  const int perFrame = maxGetIdsPerFrame();
  QList<quint16> chunk;
  for (quint16 id : ids) {
    chunk.append(id);
    if (chunk.size() >= perFrame) {
      send(OP_GET_PARAM, payloadGetParam(chunk), false);
      chunk.clear();
    }
  }
  if (!chunk.isEmpty()) send(OP_GET_PARAM, payloadGetParam(chunk), false);
}

QList<quint16> SynthController::engineParamIds() const {
  QList<quint16> out;
  for (quint16 id : m_paramOrder) {
    if (id >= ID_ENGINE_FIRST && id <= ID_ENGINE_LAST) out.append(id);
  }
  return out;
}

/* ------------------------------------------------- paced SET_PARAM output */

void SynthController::queueSetFrame(const QByteArray& payload) {
  if (payload.isEmpty() || !m_connected) return;
  m_setQueue.append(payload);
  drainSetQueue();
}

void SynthController::drainSetQueue() {
  if (m_setQueue.isEmpty()) {
    m_setDrainTimer.stop();
    return;
  }
  // The gate is a timestamp, not "is the timer running": queueSetFrame() calls
  // in here once per appended frame, so a five-frame patch push would
  // otherwise still leave as five back-to-back writes.
  const qint64 now = nowMs();
  const qint64 earliest = qMax(m_setNextSendMs, m_setBackoffUntilMs);
  if (now < earliest) {
    m_setDrainTimer.start(int(qMin<qint64>(earliest - now, kSetBusyBackoffMs)));
    return;
  }

  const QByteArray payload = m_setQueue.takeFirst();
  const quint8 seq = nextSeq();
  m_setSent.insert(seq, payload);
  m_setSentOrder.append(seq);
  while (m_setSentOrder.size() > kSetSentHistory) {
    m_setSent.remove(m_setSentOrder.takeFirst());
  }
  sendWithSeq(OP_SET_PARAM, seq, payload, /*withResponse=*/false);
  m_setNextSendMs = now + kSetDrainIntervalMs;

  if (!m_setQueue.isEmpty()) m_setDrainTimer.start(kSetDrainIntervalMs);
}

void SynthController::onSetBusy(quint8 seq) {
  m_setBackoffUntilMs = nowMs() + kSetBusyBackoffMs;
  const auto it = m_setSent.constFind(seq);
  if (it != m_setSent.constEnd()) {
    const QByteArray dropped = it.value();  // copy out before erasing the node
    m_setSent.remove(seq);
    m_setSentOrder.removeAll(seq);
    m_setQueue.prepend(dropped);  // at the head: parameter order matters
  }
  // Otherwise it has aged out of the history and the frame is simply lost —
  // all that is left to do is slow down. Eight frames of history covers any
  // burst this app produces, so that is a theoretical branch.
  m_setDrainTimer.start(kSetDrainIntervalMs);
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
    if (f.requestOp() == OP_SET_PARAM && f.status == ST_BUSY) {
      // The firmware's command queue overflowed and dropped this frame. It is
      // the only feedback a write-without-response ever gets, and ignoring it
      // meant a patch push silently applied only part of itself.
      onSetBusy(f.seq);
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
    if (f.status == ST_BUSY) {
      // Command queue full: the firmware answered BUSY *instead of* running
      // the frame, so this request simply did not happen. Resend it — without
      // this, a listing or a sequencer read lost to a discovery burst left its
      // page empty with nothing left to ask again.
      onRequestBusy(f.seq);
      return;
    }
    // A real rejection (MALFORMED / BAD_ARG / UNSUPPORTED): resending would
    // only be rejected again.
    forgetRequest(f.seq);
    qWarning() << "Synth | op" << Qt::hex << f.requestOp() << "status" << f.status;
    return;
  }

  // Answered. Chunked responses repeat the seq, and removing on the first
  // frame is right: the request has been served either way.
  forgetRequest(f.seq);

  switch (f.requestOp()) {
    case OP_PARAM_INFO:
      // Route by the seq the request went out with. The list response is the
      // one matching m_infoListSeq; per-id metadata is anything m_infoSeqToId
      // knows about. Anything else is stale and must be DROPPED, not guessed
      // at — most importantly the superseded list response after the watchdog
      // resent the request, which the old `else` branch fed to the single-id
      // parser. That parsed an id list as one parameter (registering a garbage
      // entry) and, because no per-id requests were outstanding yet, concluded
      // discovery was complete and tore it down before it had started: the app
      // ended up with ids and values but no metadata at all, i.e. every
      // parameter page blank.
      if (m_awaitingInfoList && f.seq == m_infoListSeq) {
        handleParamInfoList(f.payload);
        if (!f.continuation) onParamListComplete();
      } else if (m_infoSeqToId.contains(f.seq)) {
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
      handleSeqTrack(f.payload, f.seq);
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
    case OP_GRAPH_INFO:
      // A refusal here is the normal answer from firmware without the
      // modular engine, not an error: leave graphAvailable false and the
      // patch page stays hidden.
      if (f.status == 0) handleGraphInfo(f.payload);
      break;
    case OP_GRAPH_KIND:
      if (f.status == 0) handleGraphKind(f.payload);
      break;
    case OP_GRAPH_NODES:
      if (f.status == 0) handleGraphNodes(f.payload);
      break;
    case OP_GRAPH_EDIT:
      handleGraphEdit(f.payload, f.status);
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
  if (m_discovering && !m_awaitingInfoList) {
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

// What ParamStore::set() will do to this value on the synth: round anything
// that is not a Float, then clamp into the registered range (synth_params.cpp).
// The app has to reproduce it because its own writes are echo-suppressed — the
// synth never reports back the value it actually stored, so a local cache that
// kept the raw number drifted from the hardware on every out-of-range or
// fractional write. Unknown metadata means discovery has not reached this id
// yet; pass the value through, as before.
float SynthController::conformValue(quint16 id, float value) const {
  const auto it = m_params.constFind(id);
  if (it == m_params.constEnd() || !it->infoKnown) return value;
  const ParamInfo& pi = it->info;
  float v = (pi.type != PT_FLOAT) ? std::round(value) : value;
  if (v < pi.min) v = pi.min;
  if (v > pi.max) v = pi.max;
  return v;
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
  } else if (id == ID_GRAPH_REV) {
    // Only re-read when it actually moved, and never on the value that
    // arrives from our own discovery sweep before the first model read.
    const int rev = int(std::lround(value));
    if (m_graphAvailable && m_graphRevision >= 0 && rev != m_graphRevision) {
      refreshGraphModel();
    }
  } else if (id == ID_GRAPH_COST) {
    const int c = int(std::lround(value));
    if (c != m_graphCost) {
      m_graphCost = c;
      emit graphCostChanged();
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
  // (Re)discover, but only what the switch actually changed: the 0x02xx map is
  // re-registered per engine, and setEngineCaps() has just dropped the stale
  // half of it. Everything else the synth holds — the pattern data, the kit,
  // the patch graph — is engine-independent and does not need re-reading.
  //
  // Except on the greeting: the firmware announces engine + caps to every fresh
  // subscriber, and *that* EVT_ENGINE is the connect pass (see setConnected,
  // whose timer is only a fallback for when this one is missed). Whether it
  // lands before or after the manager publishes the connection is a race, and
  // on the side where it lands after, an engine-scoped pass skipped the
  // sequencer, kit, graph and song reads for the whole session — they are
  // requested from finishDiscovery() only on a full pass. Nothing known yet
  // means nothing to keep, so read everything.
  beginDiscovery(m_paramOrder.isEmpty() ? DiscoveryScope::Full
                                        : DiscoveryScope::EngineParams);
}

void SynthController::setEngineCaps(quint8 engine, quint8 caps) {
  const bool engineChangedNow = (int(engine) != m_engine);
  if (engineChangedNow) {
    // The engine-specific 0x02xx range changes meaning — drop its stale infos
    // so discovery re-fetches them.
    for (auto it = m_paramOrder.begin(); it != m_paramOrder.end();) {
      if (*it >= ID_ENGINE_FIRST && *it <= ID_ENGINE_LAST) {
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

// Status the firmware owns and rewrites: there is no control to offer for it.
//
// These are registered as ordinary Int/Float parameters — PARAM_INFO has no
// read-only flag — but the firmware headers mark them read-only (seqarp.h,
// looper.h) and the firmware writes them itself, seq.pos on *every sequencer
// step*. A ParamGroup drew each one as a draggable knob that jittered through
// playback and could not do anything: the write lands in the store and the
// next internal write overwrites it.
//
// Deliberately narrower than isNotPatchMaterial(): that list also holds the
// transport (seq.mode / loop.mode) and the action triggers, which are things a
// *patch* must not carry but which the UI does legitimately drive — the
// transport strips write seq.mode, and ArpSeqScreen offers it on purpose.
static bool isReadOnlyTelemetry(const QString& name) {
  static const QSet<QString> kTelemetry = {
      QStringLiteral("seq.pos"),      // playhead, -1 when stopped
      QStringLiteral("seq.curpat"),   // pattern currently playing
      QStringLiteral("loop.pos"),     // position in the loop, seconds
      QStringLiteral("loop.len"),     // recorded length
      QStringLiteral("loop.filled"),  // which tracks hold audio
      QStringLiteral("loop.rectrk"),  // track being recorded
      QStringLiteral("loop.armed"),   // count-in beats remaining
      QStringLiteral("loop.maxlen"),  // buffer capacity for this build
  };
  return kTelemetry.contains(name);
}

// Feeds ParamGroup, and nothing else — so the telemetry filter belongs here
// rather than at each call site. A screen that wants to *display* one of those
// values reads it through ParamValue, which is not a control.
QVariantList SynthController::paramIdsByPrefix(const QString& prefix) const {
  QVariantList out;
  for (quint16 id : m_paramOrder) {
    const Param& p = m_params.value(id);
    if (!p.infoKnown || !p.info.name.startsWith(prefix)) continue;
    if (isReadOnlyTelemetry(p.info.name)) continue;
    out.append(id);
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
  // Cache what the synth will store, not what the caller asked for: the write
  // is echo-suppressed, so a value the store rounds or clamps would otherwise
  // never be corrected here. The raw value still goes on the wire — the synth
  // conforms it identically.
  applyValue(pid, conformValue(pid, float(value)), /*echo=*/true);
  m_pendingSets.insert(pid, float(value));
  if (!m_setTimer.isActive()) m_setTimer.start();
}

void SynthController::flushPendingSets() {
  if (m_pendingSets.isEmpty()) return;
  // Ascending id, not QHash order. QHash iterates arbitrarily (and with a
  // per-process random seed), so the same batch packed differently on every
  // run — while onSetBusy() re-queues a dropped frame at the *head* precisely
  // because the order writes land in can matter. Ascending id is the order the
  // firmware registers them in, so it is the one worth being deterministic on.
  QList<quint16> ids = m_pendingSets.keys();
  std::sort(ids.begin(), ids.end());
  const int perFrame = maxSetPairsPerFrame();
  QByteArray payload;
  int pairs = 0;
  for (quint16 id : std::as_const(ids)) {
    appendSetParam(payload, id, m_pendingSets.value(id));
    if (++pairs >= perFrame) {
      queueSetFrame(payload);
      payload.clear();
      pairs = 0;
    }
  }
  if (pairs > 0) queueSetFrame(payload);
  m_pendingSets.clear();
}

// Momentary gesture, outside the coalescing batch — see the header note. The
// press and the release of a Fill button are two values of one id a few
// milliseconds apart, and m_pendingSets keeps only the last one written inside
// its window: a quick tap therefore sent the release and nothing else, so the
// firmware never saw the gesture at all. This is the same reasoning that has
// always kept triggerDrum() out of the batch.
void SynthController::setParamNow(int id, double value) {
  const quint16 pid = quint16(id);
  // Drop any batched write for this id: it holds an older value and would land
  // *after* this one.
  m_pendingSets.remove(pid);
  applyValue(pid, conformValue(pid, float(value)), /*echo=*/true);
  QByteArray p;
  appendSetParam(p, pid, float(value));
  // Through the paced queue, which is idle while playing — so the write still
  // goes out on the spot — but a gesture landing during a patch push is spaced
  // out instead of dropped by the firmware's 4-deep command queue.
  queueSetFrame(p);
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
  // 80 slots x 26 B is ~9 frames, so this one is always chunked; a listing
  // that lost its final frame would leave the accumulator populated and the
  // next Refresh would show every preset twice.
  m_presetsAccum.remove(engine);
  send(OP_LIST_PRESETS, payloadListPresets(quint8(engine)), false);
}

void SynthController::noteOn(int note, int velocity) {
  if (note < 0 || note > 127) return;
  // MIDI semantics, which the firmware also applies (it routes 0x90 with
  // velocity 0 as a note-off): keep the held set honest either way.
  if (velocity <= 0) {
    noteOff(note);
    return;
  }
  m_heldNotes.insert(note);
  send(OP_NOTE_ON, payloadNoteOn(quint8(note), quint8(velocity)), false);
}

void SynthController::noteOff(int note) {
  if (note < 0 || note > 127) return;
  m_heldNotes.remove(note);
  send(OP_NOTE_OFF, payloadNoteOff(quint8(note)), false);
}

// Panic: every note, not only the ones the app believes it started.
//
// The obvious 128-frame blast is still wrong — the firmware's command queue is
// four deep and answers BUSY *by dropping the frame*, so most of it would
// evaporate and the survivors would be the arbitrary few that landed between
// drains. So it is paced like the SET_PARAM queue.
//
// But it cannot be narrowed to m_heldNotes either, which is what a panic is
// for: noteOff() removes a note from that set and then writes
// *without response*, so a note-off the queue drops leaves a note sounding
// that nothing is tracking any more. Notes started over MIDI or left by the
// arpeggiator are outside the set for the same reason. The believed-held ones
// go first so the audible ones stop within a few frames; the rest follow as a
// sweep.
void SynthController::allNotesOff() {
  const QSet<int> held = m_heldNotes;
  m_heldNotes.clear();
  QList<int> ordered = held.values();
  std::sort(ordered.begin(), ordered.end());
  for (int n = 0; n < 128; ++n) {
    if (!held.contains(n)) ordered.append(n);
  }
  m_noteOffQueue = ordered;  // replaces any sweep already in flight
  emit allNotesOffSent();
  drainNoteOffQueue();
}

void SynthController::drainNoteOffQueue() {
  // A sweep left over from a link that went down has nothing to send: the
  // firmware runs voice_manager_all_notes_off() on disconnect anyway, and
  // otherwise this would tick 128 times writing into a closed socket.
  if (!m_connected) m_noteOffQueue.clear();
  if (m_noteOffQueue.isEmpty()) {
    m_noteOffTimer.stop();
    return;
  }
  send(OP_NOTE_OFF, payloadNoteOff(quint8(m_noteOffQueue.takeFirst())), false);
  if (!m_noteOffQueue.isEmpty()) m_noteOffTimer.start(kSetDrainIntervalMs);
}

// TRANSPORT and ARP are conveniences that land as ordinary parameter writes on
// the synth — with origin Ble, which is precisely what EVT_PARAMS suppresses.
// So nothing comes back, and a controller that only sent the frame kept serving
// its *previous* seq.mode for the rest of the session: the toolbar transport
// strip and the sequencer page's Rec button both bind that parameter, so they
// sat on a stale state after every press. (seq.mode is written back by the
// firmware on MIDI real-time start/stop, which is why this looked fine whenever
// an external clock was driving it.) Mirror locally, exactly as setParam does.
void SynthController::mirrorLocal(const QString& name, double value) {
  const int pid = paramIdForName(name);
  // >= 0, not > 0: paramIdForName() answers -1 for "no such name", and 0 is a
  // real parameter id — master.volume is PID 0x0000. No caller resolves to it
  // today, so `> 0` was silently correct; it would have gone wrong without a
  // symptom the first time one did.
  if (pid >= 0) applyValue(quint16(pid), conformValue(quint16(pid), float(value)),
                           /*echo=*/true);
}

void SynthController::transport(int cmd, double tempo) {
  send(OP_TRANSPORT, payloadTransport(quint8(cmd), float(tempo)), true);
  mirrorLocal(QStringLiteral("seq.mode"), cmd);
  // The firmware only writes the tempo when it is positive (handle_transport).
  if (tempo > 0.0) mirrorLocal(QStringLiteral("seq.tempo"), tempo);
}

void SynthController::setArp(int enable, int mode, int octaves, int division) {
  send(OP_ARP, payloadArp(quint8(enable), quint8(mode), quint8(octaves), quint8(division)), true);
  // Mirrors handle_arp(): enable 0 means mode 0, otherwise the mode is clamped
  // into 1..5 before it is stored. Octaves and division are clamped by the
  // store, which conformValue() reproduces from the discovered range.
  mirrorLocal(QStringLiteral("arp.mode"), enable == 0 ? 0 : qBound(1, mode, 5));
  // "arp.octaves", spelled exactly as seqarp.cpp registers it — an "arp.oct"
  // here resolved to no id at all, so this mirror silently did nothing.
  mirrorLocal(QStringLiteral("arp.octaves"), octaves);
  mirrorLocal(QStringLiteral("seq.div"), division);
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

// A whole patch is ~180 parameters, i.e. ~5 full frames. Blasting them
// back-to-back overran the firmware's 4-deep command queue and lost whole
// frames to BUSY, so a loaded patch applied only partly — the exact failure
// the discovery pump was rewritten to avoid. They go through the paced queue.
void SynthController::pushParams(const QList<QPair<int, double>>& params) {
  const int perFrame = maxSetPairsPerFrame();
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
    if (++pairs >= perFrame) {
      queueSetFrame(payload);
      payload.clear();
      pairs = 0;
    }
  }
  if (pairs > 0) queueSetFrame(payload);
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

/* ---------------------------------------------- patch interchange (JSON) */

// One patch as it travels in a file. `created` is written only when there is
// one to write (the library rows carry it; a live snapshot does not).
//
//   { "format": "osyntho.patch", "version": 1, "name": …, "engine": 2,
//     "engineName": "FM",
//     "params": [ { "name": "flt.cutoff", "id": 42, "value": 0.63 }, … ] }
//
// A parameter with no name is one this build has no metadata for — it still
// goes out with its id so a round trip on the same firmware is lossless.
QJsonObject SynthController::patchJsonObject(const QString& name,
                                             int engine,
                                             const QString& created,
                                             const QList<QPair<int, double>>& params) const {
  QJsonArray items;
  for (const auto& pv : params) {
    const QString pname = paramName(pv.first);
    // Skip what a patch must never carry, so a file can never make an importer
    // fire a drum or overwrite a preset slot. See isNotPatchMaterial().
    if (isNotPatchMaterial(pname)) continue;
    QJsonObject item;
    if (!pname.isEmpty()) item["name"] = pname;
    item["id"] = pv.first;
    item["value"] = pv.second;
    items.append(item);
  }

  QJsonObject out;
  out["format"] = QStringLiteral("osyntho.patch");
  out["version"] = kPatchJsonVersion;
  out["name"] = name;
  out["engine"] = engine;
  out["engineName"] = engineNameFor(engine);
  if (!created.isEmpty()) out["created"] = created;
  out["params"] = items;
  return out;
}

QString SynthController::patchToJson(int patchId) const {
  const QVariantList rows = db().getPatches(-1);
  for (const QVariant& r : rows) {
    const QVariantMap m = r.toMap();
    if (m.value("id").toInt() != patchId) continue;
    const QJsonObject obj = patchJsonObject(m.value("name").toString(),
                                            m.value("engine").toInt(),
                                            m.value("created").toString(),
                                            db().getPatchParams(patchId));
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Indented));
  }
  return QString();
}

// The whole library: the same per-patch objects, in an array, under an envelope
// that names and versions the collection itself.
QString SynthController::libraryToJson() const {
  QJsonArray patchArray;
  const QVariantList rows = db().getPatches(-1);
  for (const QVariant& r : rows) {
    const QVariantMap m = r.toMap();
    patchArray.append(patchJsonObject(m.value("name").toString(),
                                      m.value("engine").toInt(),
                                      m.value("created").toString(),
                                      db().getPatchParams(m.value("id").toInt())));
  }

  QJsonObject out;
  out["format"] = QStringLiteral("osyntho.patchlib");
  out["version"] = kPatchJsonVersion;
  out["exported"] = QDateTime::currentDateTime().toString(Qt::ISODate);
  out["patches"] = patchArray;
  return QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Indented));
}

void SynthController::exportPresetJson(int engine, int slot) {
  if (!m_connected || !m_ready) {
    emit showError(Translator::instance().t("Connect to the synth first."));
    return;
  }

  // A preset lives on the synth and there is no "read slot" opcode: the only
  // way to see its parameters is to load it and read back what changed. So the
  // export really does load the preset — the toast says so, because the sound
  // changing under the user would otherwise look like a bug.
  QString name;
  for (const PresetEntry& e : m_presets.value(engine)) {
    if (int(e.slot) == slot) {
      name = e.name;
      break;
    }
  }
  if (name.isEmpty()) name = QStringLiteral("preset-%1").arg(slot);

  emit showInfo(Translator::instance().ts("Loading preset %1 to read it…", QString::number(slot)));
  loadPreset(engine, slot);

  // Two hops: let the firmware finish loading the slot, then sweep the values
  // (EVT_PARAMS after a preset load is not guaranteed to reach us complete) and
  // let the answers land before snapshotting.
  QTimer::singleShot(kPresetLoadSettleMs, this, [this, engine, name]() {
    requestAllParamValues();
    QTimer::singleShot(kPresetReadSettleMs, this, [this, engine, name]() {
      QList<QPair<int, double>> params;
      for (quint16 id : std::as_const(m_paramOrder)) {
        const Param& p = m_params.value(id);
        if (p.valueKnown) params.append({int(id), double(p.value)});
      }
      if (params.isEmpty()) {
        emit showError(Translator::instance().t("Nothing to save yet — no parameters have been read."));
        return;
      }
      const QJsonObject obj = patchJsonObject(name, engine, QString(), params);
      emit presetJsonReady(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Indented)),
                           name);
    });
  });
}

// Names first. The id is used only for an entry that carries no name at all —
// a file exported with no parameter table to name it by — and only when this
// build has that id. A name this firmware does not know is dropped rather than
// followed to its id, which on another firmware is some unrelated parameter.
void SynthController::resolveAndPushImport(const QList<NamedParam>& items) {
  QList<QPair<int, double>> resolved;
  int skipped = 0;
  for (const NamedParam& item : items) {
    int id = item.name.isEmpty() ? -1 : paramIdForName(item.name);
    if (id < 0 && item.name.isEmpty() && item.id >= 0 && paramExists(item.id)) id = item.id;
    // A nameless entry can still resolve to an action parameter. pushParams()
    // drops it either way, but not counting it here would make the tally lie.
    if (id < 0 || isNotPatchMaterial(paramName(id))) {
      ++skipped;
      continue;
    }
    resolved.append({id, item.value});
  }

  if (resolved.isEmpty()) {
    emit showError(Translator::instance().t("No parameter in that file matches this synth."));
    return;
  }
  pushParams(resolved);
  if (skipped > 0) {
    emit showInfo(Translator::instance().ts("Applied %1 parameters; %2 in the file do not apply to this synth.",
                                            QString::number(resolved.size()),
                                            QString::number(skipped)));
  } else {
    emit showInfo(Translator::instance().ts("Applied %1 parameters.", QString::number(resolved.size())));
  }
}

// Accept a single patch, the library envelope, or a bare array of patches. The
// two importers differ only in what they do with these: push the first, or
// store them all.
QList<QJsonObject> SynthController::patchObjectsFrom(const QJsonDocument& doc) {
  QList<QJsonObject> out;
  const auto take = [&out](const QJsonArray& arr) {
    for (const QJsonValue& v : arr) {
      const QJsonObject o = v.toObject();
      if (o.value("params").isArray()) out.append(o);
    }
  };
  if (doc.isArray()) {
    take(doc.array());
  } else if (doc.isObject()) {
    const QJsonObject root = doc.object();
    if (root.value("patches").isArray())
      take(root.value("patches").toArray());
    else if (root.value("params").isArray())
      out.append(root);
  }
  return out;
}

// Actions are dropped here, on the way in, so neither route can be made to
// fire a drum or overwrite a preset slot by a hand-written file.
QList<SynthController::NamedParam> SynthController::namedParamsFrom(const QJsonObject& patch) {
  QList<NamedParam> items;
  const QJsonArray arr = patch.value("params").toArray();
  for (const QJsonValue& v : arr) {
    const QJsonObject o = v.toObject();
    if (!o.value("value").isDouble()) continue;
    NamedParam item;
    item.name = o.value("name").toString();
    item.id = o.value("id").isDouble() ? o.value("id").toInt() : -1;
    item.value = o.value("value").toDouble();
    if (isNotPatchMaterial(item.name)) continue;  // never replay an action
    items.append(item);
  }
  return items;
}

QVariantMap SynthController::importPatchJson(const QString& text) {
  // Reported as a toast as well as returned: every caller wants it shown, and
  // showError is already wired to one.
  const auto fail = [this](const QString& why) {
    emit showError(why);
    return QVariantMap{{"ok", false}, {"error", why}, {"name", QString()}};
  };

  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
  if (err.error != QJsonParseError::NoError)
    return fail(Translator::instance().t("That file is not valid JSON."));

  // A file may hold more than one patch, and importing here means "push one to
  // the synth", so the first is taken and the count is reported.
  const QList<QJsonObject> objects = patchObjectsFrom(doc);
  if (objects.isEmpty()) return fail(Translator::instance().t("That file holds no patch."));
  const QJsonObject patch = objects.first();
  const int total = objects.size();

  const int version = patch.value("version").toInt(0);
  if (version > kPatchJsonVersion)
    return fail(Translator::instance().ts("That patch file is version %1; this app reads up to %2.",
                                          QString::number(version),
                                          QString::number(kPatchJsonVersion)));

  const QList<NamedParam> items = namedParamsFrom(patch);
  if (items.isEmpty()) return fail(Translator::instance().t("That file holds no patch."));

  const QString name = patch.value("name").toString();
  const int engine = patch.value("engine").isDouble() ? patch.value("engine").toInt() : -1;

  if (engine >= 0 && engine != m_engine) {
    // The ids the names resolve to only exist after the new engine has
    // re-registered, so resolution waits with the push.
    selectEngine(engine);
    m_pendingImport = items;
    QTimer::singleShot(kEngineSwitchSettleMs, this, [this]() {
      resolveAndPushImport(m_pendingImport);
      m_pendingImport.clear();
    });
  } else {
    resolveAndPushImport(items);
  }

  if (total > 1) {
    emit showInfo(Translator::instance().ts("That file holds %1 patches; imported the first.",
                                            QString::number(total)));
  }
  return QVariantMap{{"ok", true}, {"error", QString()}, {"name", name}};
}

// The Lib page's import: every patch in the file is stored, and nothing is
// sent to the synth. A library export is a collection, so importing one gives
// the collection back rather than only its first entry.
//
// A stored row is (param id, value) — that is what loadPatch() pushes later —
// so the names in the file have to be resolved to ids now. That is only
// possible against the live parameter table, which describes the connected
// synth's current engine and nothing else; for any other engine, or with no
// synth at all, the file's own id is kept. It is what the exporter wrote and
// is right for a round trip on the same firmware build, and loadPatch()
// resolves nothing either way.
QVariantMap SynthController::importPatchesToLibrary(const QString& text) {
  const auto fail = [this](const QString& why) {
    emit showError(why);
    return QVariantMap{{"ok", false}, {"error", why}, {"imported", 0}, {"skipped", 0}};
  };

  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
  if (err.error != QJsonParseError::NoError)
    return fail(Translator::instance().t("That file is not valid JSON."));

  const QList<QJsonObject> objects = patchObjectsFrom(doc);
  if (objects.isEmpty()) return fail(Translator::instance().t("That file holds no patch."));

  int imported = 0;
  int skipped = 0;
  QString lastName;
  for (const QJsonObject& patch : objects) {
    if (patch.value("version").toInt(0) > kPatchJsonVersion) {
      ++skipped;
      continue;
    }

    // An engine-less file is taken as the live engine: the row needs one, and
    // the ids in it were written by whatever was playing when it was exported.
    const int engine = patch.value("engine").isDouble() ? patch.value("engine").toInt() : m_engine;
    // Unlike a push, which is gone once it lands, a row stays — and Load sends
    // its engine number straight to the synth. A number this build has no
    // engine for is refused rather than stored.
    if (engine < ENG_SUBTRACTIVE || engine > ENG_MODULAR) {
      ++skipped;
      continue;
    }
    const bool byName = m_ready && engine == m_engine;

    const QList<NamedParam> items = namedParamsFrom(patch);
    QList<QPair<int, double>> params;
    for (const NamedParam& item : items) {
      // With a table to resolve against, the name decides — and a name this
      // build does not know is dropped rather than followed to the file's id,
      // which on another firmware is some unrelated parameter. Without one,
      // the file's id is all there is.
      int id = (byName && !item.name.isEmpty()) ? paramIdForName(item.name) : item.id;
      // A nameless entry falling back to its file id gets the same check the
      // push route applies (resolveAndPushImport): with a table in hand, an id
      // this build has no parameter for is dropped rather than stored. Without
      // the check the row still went into the library and loadPatch() pushed it
      // at the synth later, addressing nothing.
      if (byName && item.name.isEmpty() && !paramExists(id)) id = -1;
      if (id < 0) continue;
      // A nameless entry can still land on an action. pushParams() drops those
      // at load time, but they have no business in a stored row either.
      if (byName && isNotPatchMaterial(paramName(id))) continue;
      params.append({id, item.value});
    }
    if (params.isEmpty()) {
      ++skipped;
      continue;
    }

    QString name = patch.value("name").toString().trimmed();
    if (name.isEmpty()) name = Translator::instance().t("Imported patch");
    if (db().insertPatch(name, engine, params) > 0) {
      ++imported;
      lastName = name;
    } else {
      ++skipped;
    }
  }

  if (imported == 0)
    return fail(Translator::instance().t("Nothing in that file could be added to the library."));

  if (imported == 1 && skipped == 0) {
    emit showInfo(Translator::instance().ts("Added \"%1\" to the library.", lastName));
  } else if (skipped > 0) {
    emit showInfo(Translator::instance().ts("Added %1 patches to the library; %2 skipped.",
                                            QString::number(imported),
                                            QString::number(skipped)));
  } else {
    emit showInfo(Translator::instance().ts("Added %1 patches to the library.",
                                            QString::number(imported)));
  }
  return QVariantMap{
      {"ok", true}, {"error", QString()}, {"imported", imported}, {"skipped", skipped}};
}

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

// Step windows (read and write) are sized by maxStepsPerFrame(), which
// derives from the negotiated MTU — a steps payload carries 8 bytes per step
// behind a 6-byte prefix, so the documented 247 fits the 24 this used to
// hard-code, and a smaller link packs fewer instead of overrunning the frame.

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
  // Drop whatever a superseded response left behind. Both of these
  // accumulators are abandoned mid-stream when their staleness check fires
  // (a pattern or track switch while frames are in flight), and appending the
  // next listing onto the remains showed up as duplicated p-locks — and locks
  // from the previous pattern — on the grid.
  m_stepsAccum.clear();
  m_stepsAccumFirst = 0;
  m_plocksAccum.clear();

  // Write-without-response, like every other read here: the EVT response *is*
  // the acknowledgement, so the ATT write-response round trip on top of it only
  // buys latency — and on Windows each one blocks the single write path for the
  // whole trip, which is what let a discovery burst strand a note-off.
  send(OP_SEQ_INFO, QByteArray(), false);
  send(OP_SEQ_PATTERN, payloadSeqGetPattern(m_editPattern), false);
  send(OP_SEQ_PLOCK, payloadSeqPlockListPattern(m_editPattern), false);

  // The step walk needs this track's length, and only this response carries
  // it — so it starts in handleSeqTrack(), not here. Starting it here meant
  // walking the whole track once on the *previous* track's length and then
  // again when the real one arrived: up to 22 wasted round trips per switch.
  m_stepsWindowNext = -1;
  m_stepsRetryTimer.stop();  // any walk still in flight is abandoned here
  m_trackGetSeq = nextSeq();
  m_awaitingTrackGet = true;
  m_trackGetFallbackTimer.start();
  sendWithSeq(OP_SEQ_TRACK, m_trackGetSeq,
              payloadSeqGetTrack(m_editPattern, m_editTrack), false);
}

void SynthController::requestSteps() {
  if (m_stepsWindowNext < 0) {
    m_stepsRetryTimer.stop();
    return;
  }
  const int len = m_trackCfg.length > 0 ? int(m_trackCfg.length) : 64;
  if (m_stepsWindowNext >= len) {  // the whole track is in
    m_stepsWindowNext = -1;
    m_stepsRetryTimer.stop();
    return;
  }
  m_stepsAccum.clear();  // each window is answered on its own
  const int count = qMin(maxStepsPerFrame(), len - m_stepsWindowNext);
  send(OP_SEQ_STEPS,
       payloadSeqGetSteps(m_editPattern, m_editTrack, m_stepsWindowNext, count),
       false);
  m_stepsRetryTimer.start();  // watchdog on this window
}

void SynthController::beginStepWalk() {
  m_stepsAccum.clear();
  m_stepsWindowNext = 0;
  m_stepsWindowRetries = kStepsWindowRetries;
  requestSteps();
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
    if (m.value(QStringLiteral("slot")).toInt() != slot) continue;
    // KIT_INFO has no "no note" encoding — the firmware sends 0 for a slot the
    // kit leaves empty — so an empty name is what identifies one. Returning
    // that 0 made noteForNewStep() stamp note 0 onto a lane bound to an empty
    // slot, and made drumNameForNote(0) answer with the first empty slot.
    if (m.value(QStringLiteral("name")).toString().isEmpty()) return -1;
    return m.value(QStringLiteral("note")).toInt();
  }
  return -1;  // kit list not in yet, or no such slot
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
    const QString name = m.value(QStringLiteral("name")).toString();
    // Empty slots all report note 0 (see noteForSlot), so they must not be
    // allowed to claim it.
    if (name.isEmpty()) continue;
    if (m.value(QStringLiteral("note")).toInt() == note) return name;
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
  const int perFrame = maxStepsPerFrame();
  for (int at = first; at < first + count; at += perFrame) {
    const int n = qMin(perFrame, first + count - at);
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
    beginStepWalk();
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
  // Clamp by UTF-8 bytes, not QChars — that is the limit the wire imposes, and
  // caching a name longer than the one actually sent made the field snap back
  // on the next read of the pattern.
  m_patternName = QString::fromUtf8(utf8Clamped(name, 11));
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
  // Same rule as the sequencer listings: a chunked response that never
  // finished (a dropped final frame) would otherwise have the next one
  // appended to it, duplicating every kit and slot in the pickers.
  m_kitsAccum.clear();
  m_kitSlotsAccum.clear();
  send(OP_KIT_INFO, payloadKitInfo(0), false);  // selectable kits
  send(OP_KIT_INFO, payloadKitInfo(1), false);  // the current kit's slots
}

// ---- modular patch graph (S28) --------------------------------------------

void SynthController::refreshGraph() {
  if (!m_connected) return;
  // GRAPH_INFO first and alone: it carries the sizing everything else is read
  // against, and its failure (ST_UNSUPPORTED on a firmware without the
  // modular engine) is what decides whether to ask for anything more at all.
  send(OP_GRAPH_INFO, QByteArray(), false);
}

void SynthController::refreshGraphModel() {
  if (!m_connected || !m_graphAvailable) return;
  send(OP_GRAPH_NODES, payloadGraphNodesGet(), false);
}

void SynthController::handleGraphInfo(const QByteArray& payload) {
  const GraphInfo g = parseGraphInfo(payload);
  if (!g.valid) return;
  const bool wasAvailable = m_graphAvailable;
  m_graphAvailable = true;
  m_graphMaxNodes = g.maxNodes;
  m_graphParamsPerNode = g.paramsPerNode;
  m_graphMaxInputs = g.maxInputs;
  m_graphOutSlot = g.outSlot;
  m_graphEngineIndex = g.engineIndex;
  m_graphCostBudget = g.costBudget;
  m_graphCost = g.liveCost;
  emit graphInfoChanged();
  emit graphCostChanged();

  // The kind table is build-constant, so fetch it once per connection. Doing
  // it on every refresh would put kindCount round trips in front of every
  // model read for data that cannot have changed.
  if (!wasAvailable || m_graphKinds.size() != g.kindCount) {
    m_graphKinds.clear();
    m_graphKindCount = g.kindCount;
    for (int k = 0; k < g.kindCount; ++k) {
      send(OP_GRAPH_KIND, payloadGraphKind(k), false);
    }
  }
  refreshGraphModel();
}

void SynthController::handleGraphKind(const QByteArray& payload) {
  const GraphKind k = parseGraphKind(payload);
  if (!k.valid) return;
  QVariantMap m;
  m["kind"] = k.kind;
  m["name"] = k.name;
  m["rate"] = k.rate;
  m["cost"] = k.cost;
  m["inputs"] = QVariant(k.inputs);
  m["params"] = QVariant(k.params);
  // Responses can arrive out of order; index by kind rather than appending,
  // so the list is always addressable as graphKinds[kind].
  while (m_graphKinds.size() <= k.kind) m_graphKinds.append(QVariantMap());
  m_graphKinds[k.kind] = m;
  emit graphKindsChanged();
}

void SynthController::rebuildGraphNodes(const GraphModel& gm) {
  m_graphNodes.clear();
  for (int i = 0; i < gm.nodes.size(); ++i) {
    const GraphNode& n = gm.nodes[i];
    QVariantMap m;
    m["slot"] = i;
    m["kind"] = n.kind;
    QVariantList ins;
    for (int v : n.in) ins.append(v);
    m["in"] = ins;
    m["x"] = n.x;
    m["y"] = n.y;
    m_graphNodes.append(m);
  }
  m_graphRevision = gm.revision;
  emit graphChanged();
}

void SynthController::handleGraphNodes(const QByteArray& payload) {
  const GraphModel gm = parseGraphModel(payload);
  if (!gm.valid) return;
  rebuildGraphNodes(gm);
}

void SynthController::handleGraphEdit(const QByteArray& payload, quint8 status) {
  const GraphEditReply e = parseGraphEditReply(payload);
  if (m_graphCost != e.cost) {
    m_graphCost = e.cost;
    emit graphCostChanged();
  }
  if (status != 0) {
    // The three refusals are different problems and deserve different words —
    // "too expensive" is a budget the user can see and work within, a cycle is
    // a shape mistake, and a bad argument is a bug in this app.
    switch (status & 0x7F) {
      case 4:
        m_graphError = tr("Patch too expensive: %1 of %2 CPU units")
                           .arg(e.cost)
                           .arg(m_graphCostBudget);
        break;
      case 5:
        m_graphError = tr("That cable would make a loop");
        break;
      default:
        m_graphError = tr("Edit refused");
        break;
    }
    emit graphErrorChanged();
    // The firmware did not change the model, so the canvas has to go back to
    // what is actually bound rather than keep the edit it drew optimistically.
    refreshGraphModel();
    return;
  }
  if (e.revision != m_graphRevision) refreshGraphModel();
}

int SynthController::graphNodeParamId(int slot, int index) const {
  if (slot < 0 || slot >= m_graphMaxNodes) return -1;
  if (index < 0 || index >= m_graphParamsPerNode) return -1;
  return 0x0200 + slot * m_graphParamsPerNode + index;
}

QVariantMap SynthController::graphKind(int kind) const {
  if (kind < 0 || kind >= m_graphKinds.size()) return QVariantMap();
  return m_graphKinds[kind].toMap();
}

int SynthController::graphFreeSlot() const {
  for (int i = 0; i < m_graphNodes.size(); ++i) {
    const QVariantMap m = m_graphNodes[i].toMap();
    if (i == m_graphOutSlot) continue;  // pinned to the output node
    if (m.value("kind").toInt() == 0) return i;
  }
  return -1;
}

void SynthController::graphSetKind(int slot, int kind) {
  if (!m_graphAvailable) return;
  send(OP_GRAPH_EDIT, payloadGraphSetKind(slot, kind), true);
}

void SynthController::graphConnect(int dst, int port, int src) {
  if (!m_graphAvailable) return;
  send(OP_GRAPH_EDIT, payloadGraphConnect(dst, port, src), true);
}

void SynthController::graphSetNodePos(int slot, int x, int y) {
  if (!m_graphAvailable) return;
  if (slot < 0 || slot >= m_graphNodes.size()) return;
  // Canvas-only on the firmware side: no recompile, no audio duck. Sent
  // without response so dragging a node never queues acks.
  send(OP_GRAPH_EDIT, payloadGraphSetPos(slot, x, y), false);
  // ...and applied locally, because a position edit deliberately does not
  // bump the graph revision, so nothing would ever push it back to us. The
  // app is the authority on layout; the firmware only stores it. Without
  // this the cable painter keeps reading the old coordinates and every wire
  // stays where the node used to be.
  QVariantMap m = m_graphNodes[slot].toMap();
  m["x"] = x;
  m["y"] = y;
  m_graphNodes[slot] = m;
  emit graphChanged();
}

void SynthController::clearGraphError() {
  if (m_graphError.isEmpty()) return;
  m_graphError.clear();
  emit graphErrorChanged();
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
  //
  // Through the paced queue, which is empty while playing, so the hit still
  // goes out on the spot — but a hit landing during a patch push is retried
  // instead of dropped.
  const int pid = paramIdForName(QStringLiteral("drums.trig"));
  if (pid <= 0) return;
  QByteArray p;
  appendSetParam(p, quint16(pid), float(slot));
  queueSetFrame(p);
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
  // Drop what it already contributed with it: leaving a half-accumulated
  // window behind would make the next response resume from *its* first index
  // and scatter the new track's steps across the wrong slots.
  if (pattern != m_editPattern || track != m_editTrack) {
    m_stepsAccum.clear();
    return;
  }
  // The window answered, so its watchdog is re-armed rather than left to fire:
  // a window chunked across several frames takes longer than one round trip,
  // and each frame is proof the walk is still moving.
  if (m_stepsWindowNext >= 0) m_stepsRetryTimer.start();

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
    if (next <= m_stepsWindowNext) {
      // A window that carried no step records at all: `next` is the index we
      // just asked for, so advancing to it would re-send the same request, be
      // answered the same way, and do that for as long as the link stayed up.
      // This firmware cannot produce it (handle_seq_steps clamps the count and
      // rejects an out-of-range start with BAD_ARG), but nothing in the walk
      // required it not to.
      qWarning() << "Synth | step window" << m_stepsWindowNext
                 << "carried no steps; stopping the walk";
      m_stepsWindowNext = -1;
      m_stepsRetryTimer.stop();
      return;
    }
    m_stepsWindowNext = next;
    m_stepsWindowRetries = kStepsWindowRetries;  // fresh budget per window
    requestSteps();
  }
}

void SynthController::handleSeqTrack(const QByteArray& payload, quint8 seq) {
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
    emit stepsChanged();
  }

  // This response is the answer to refreshSequencer's own SEQ_TRACK get: it
  // has just delivered the length the walk is sized by, so the walk starts
  // here. Every other SEQ_TRACK response is the echo of a track-field write —
  // restarting a full walk on each of those would re-read the whole track
  // while the user drags a swing slider.
  if (m_awaitingTrackGet && seq == m_trackGetSeq) {
    m_awaitingTrackGet = false;
    m_trackGetFallbackTimer.stop();
    beginStepWalk();
  } else if (lengthChanged) {
    beginStepWalk();
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
  if (!r.ok || pattern != m_editPattern) {
    m_plocksAccum.clear();  // abandoned mid-listing; do not carry it forward
    return;
  }
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
  // A *set* is answered with a bare status frame — no payload at all — and
  // without this guard that landed here as "the chain is now empty": the
  // accumulator is empty between listings, so `m_song = m_songAccum` wiped the
  // chain the app had just written and announced it with songChanged(). Every
  // sibling handler already refuses a payload it could not read; this one was
  // the hole.
  if (!r.ok) return;
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
