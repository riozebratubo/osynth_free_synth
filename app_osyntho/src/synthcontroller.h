#ifndef SYNTHCONTROLLER_H
#define SYNTHCONTROLLER_H

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include "src/ble/synthprotocol.h"
#include "src/business/databaseclient.h"
#include "src/business/settingsclient.h"

// Drives one connected osynth over SynthCtl v1.
//
// - Owns the runtime parameter table, discovered via PARAM_INFO after every
//   connect / engine switch, with per-engine module caps so the UI can hide
//   dead controls.
// - Coalesces knob writes to ~20 Hz and sends them write-without-response for
//   the lowest control jitter; discovery/preset commands go reliably.
// - Reflects non-BLE changes (MIDI, preset loads) from EVT_PARAMS/EVT_ENGINE.
// - Persists/loads app-side patches (full parameter snapshots) via the DB.
//
// Wiring (in App): manager.receivedData -> onReceiveData, manager.infoRead ->
// onInfoRead, writeToSynth -> manager.write, manager.connectedChanged ->
// setConnected.
class SynthController : public QObject, public DatabaseClient, public SettingsClient {
  Q_OBJECT

  Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
  Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)

  Q_PROPERTY(int engine READ engine NOTIFY engineChanged)
  Q_PROPERTY(QString engineName READ engineName NOTIFY engineChanged)
  Q_PROPERTY(int caps READ caps NOTIFY engineChanged)

  Q_PROPERTY(int presetSlot READ presetSlot NOTIFY presetChanged)
  Q_PROPERTY(QString presetName READ presetName NOTIFY presetChanged)
  Q_PROPERTY(bool presetIsFactory READ presetIsFactory NOTIFY presetChanged)

  Q_PROPERTY(QString synthTarget READ synthTarget NOTIFY infoChanged)
  Q_PROPERTY(QString firmwareVersion READ firmwareVersion NOTIFY infoChanged)
  Q_PROPERTY(int protocolVersion READ protocolVersion NOTIFY infoChanged)

  // Kept for a future osynth firmware-update capability (none today).
  Q_PROPERTY(bool firmwareUpdateSupported READ firmwareUpdateSupported CONSTANT)
  Q_PROPERTY(bool isUpdatingFirmware READ isUpdatingFirmware NOTIFY isUpdatingFirmwareChanged)

  // --- sequencer (S23) ---
  // The firmware's compile-time sizing: a PSRAM build has 8 tracks x 8
  // patterns, a classic ESP32 half that. The grid binds to these rather than
  // assuming, so one UI serves both.
  Q_PROPERTY(bool seqAvailable READ seqAvailable NOTIFY seqInfoChanged)
  Q_PROPERTY(int seqTracks READ seqTracks NOTIFY seqInfoChanged)
  Q_PROPERTY(int seqPatterns READ seqPatterns NOTIFY seqInfoChanged)
  Q_PROPERTY(int seqMaxSteps READ seqMaxSteps NOTIFY seqInfoChanged)
  Q_PROPERTY(int seqPlockCapacity READ seqPlockCapacity NOTIFY seqInfoChanged)
  Q_PROPERTY(int seqPlockUsed READ seqPlockUsed NOTIFY seqInfoChanged)
  // Which pattern/track the grid is showing (app-side view state).
  Q_PROPERTY(int editPattern READ editPattern WRITE setEditPattern NOTIFY editTargetChanged)
  Q_PROPERTY(int editTrack READ editTrack WRITE setEditTrack NOTIFY editTargetChanged)
  // Live playhead, mirrored from seq.pos (-1 when stopped).
  Q_PROPERTY(int playhead READ playhead NOTIFY playheadChanged)
  Q_PROPERTY(bool playing READ playing NOTIFY playheadChanged)

  // --- drum kit (S22) ---
  Q_PROPERTY(QVariantList kitSlots READ kitSlots NOTIFY kitChanged)
  Q_PROPERTY(QVariantList kits READ kits NOTIFY kitChanged)
  Q_PROPERTY(int currentKit READ currentKit NOTIFY kitChanged)
  // Note of the kit's first populated slot — the kick in the factory kit.
  // The sensible starting pick for drum placement, since the melodic
  // default (60) answers to no slot at all. -1 before a kit arrives.
  Q_PROPERTY(int defaultDrumNote READ defaultDrumNote NOTIFY kitChanged)

 public:
  explicit SynthController(QObject* parent = nullptr);
  ~SynthController() override;

  bool connected() const { return m_connected; }
  bool ready() const { return m_ready; }

  int engine() const { return m_engine; }
  QString engineName() const;
  int caps() const { return m_caps; }

  int presetSlot() const { return m_presetSlot; }
  QString presetName() const { return m_presetName; }
  bool presetIsFactory() const { return m_presetIsFactory; }

  QString synthTarget() const { return m_target; }
  QString firmwareVersion() const { return m_fwVersion; }
  int protocolVersion() const { return m_protoVersion; }

  bool firmwareUpdateSupported() const { return false; }
  bool isUpdatingFirmware() const { return m_isUpdatingFirmware; }

  // --- parameter access for QML (curated pages bind by id) ---------------
  Q_INVOKABLE bool paramExists(int id) const;
  Q_INVOKABLE double paramValue(int id) const;
  // Whether paramValue() is a value the synth actually reported. It
  // falls back to the parameter's *default* otherwise, which for
  // firmware-written status params (loop.filled, seq.pos …) is a
  // legitimate-looking zero — so anything that re-reads on a signal
  // must check this before overwriting state learned from an event.
  Q_INVOKABLE bool paramValueKnown(int id) const;
  Q_INVOKABLE QVariantMap paramMeta(int id) const;  // exists,name,type,curve,min,max,def,enumNames
  Q_INVOKABLE QVariantList paramIds() const;
  // UI helpers: resolve by name, or gather every registered id whose name starts
  // with `prefix` (registration order). paramPickerList returns [{id,name}] for
  // the mod-matrix destination picker.
  Q_INVOKABLE int paramIdForName(const QString& name) const;
  Q_INVOKABLE QString paramName(int id) const;
  Q_INVOKABLE QVariantList paramIdsByPrefix(const QString& prefix) const;
  Q_INVOKABLE QVariantList paramPickerList() const;

  // --- commands ----------------------------------------------------------
  // Knob edit: updates the local value, echoes paramChanged, and queues a
  // coalesced write-without-response batch (~20 Hz).
  Q_INVOKABLE void setParam(int id, double value);
  // Immediate reliable refresh of a single id.
  Q_INVOKABLE void refreshParam(int id);

  Q_INVOKABLE void selectEngine(int engine);
  Q_INVOKABLE void loadPreset(int engine, int slot);
  Q_INVOKABLE void savePreset(int engine, int slot, const QString& name);
  Q_INVOKABLE void listPresets(int engine);
  Q_INVOKABLE QVariantList presetsFor(int engine) const;

  Q_INVOKABLE void noteOn(int note, int velocity);
  Q_INVOKABLE void noteOff(int note);
  Q_INVOKABLE void allNotesOff();

  Q_INVOKABLE void transport(int cmd, double tempo = 0.0);
  Q_INVOKABLE void setArp(int enable, int mode, int octaves, int division);
  Q_INVOKABLE void ping();

  // --- sequencer (S23) ---------------------------------------------------
  bool seqAvailable() const { return m_seqInfo.valid && m_seqInfo.modelReady; }
  int seqTracks() const { return m_seqInfo.tracks; }
  int seqPatterns() const { return m_seqInfo.patterns; }
  int seqMaxSteps() const { return m_seqInfo.maxSteps; }
  int seqPlockCapacity() const { return m_seqInfo.plockCapacity; }
  int seqPlockUsed() const { return m_seqInfo.plockUsed; }
  int editPattern() const { return m_editPattern; }
  int editTrack() const { return m_editTrack; }
  void setEditPattern(int pattern);
  void setEditTrack(int track);
  int playhead() const { return m_playhead; }
  bool playing() const { return m_playhead >= 0; }

  // Refresh the cached view of the current pattern/track (steps + config).
  Q_INVOKABLE void refreshSequencer();
  // Steps of the edited track, as [{note,vel,gate,prob,micro,ratchet,cond,
  // flags,filled}] — index is the step number. Empty until the first refresh.
  Q_INVOKABLE QVariantList steps() const;
  Q_INVOKABLE QVariantMap step(int index) const;
  // Grid gesture: fills an empty step with defaults, clears a filled one.
  Q_INVOKABLE void toggleStep(int step, int note = 60);
  // The note a newly placed step should carry on the edited track. A synth
  // lane — or a drum lane that picks its slot from the note — uses whatever
  // the user chose. A drum lane bound to a fixed slot uses *that slot's* note
  // instead: the firmware ignores the note on such a lane, so writing the
  // user's melodic pick there would store something meaningless, and the step
  // would play the wrong drum if the lane were later switched to note-to-slot.
  Q_INVOKABLE int noteForNewStep(int pickedNote) const;
  // Name of the drum a note selects through the current kit's map, or "" when
  // no slot answers to it. The grid uses it to label — and to flag — steps on
  // a lane that picks its slot from the step's note, where an unmapped note is
  // simply silent and would otherwise look identical to a sounding one.
  Q_INVOKABLE QString drumNameForNote(int note) const;
  // Kit note a slot answers to, or -1 when the slot is empty or the kit
  // list has not arrived yet.
  int noteForSlot(int slot) const;
  // Edits one field of one step and writes it back ("note", "vel", "gate",
  // "prob", "micro", "ratchet", "cond", "accent", "slide", "mute").
  Q_INVOKABLE void setStepField(int step, const QString& field, double value);
  Q_INVOKABLE void setStep(int step, const QVariantMap& fields);

  // Track configuration of the edited track, as a map with the same keys the
  // setters below take.
  Q_INVOKABLE QVariantMap trackConfig() const;
  Q_INVOKABLE void setTrackField(const QString& field, double value);
  Q_INVOKABLE QVariantMap patternConfig() const;
  Q_INVOKABLE void setPatternField(const QString& field, double value);
  Q_INVOKABLE void setPatternName(const QString& name);

  // Editing helpers, all served by the firmware so the pattern never has to
  // travel both ways for an operation it can do in place.
  Q_INVOKABLE void clearPattern(int pattern = -1);
  Q_INVOKABLE void clearTrack(int track = -1);
  Q_INVOKABLE void copyPattern(int src, int dst);
  Q_INVOKABLE void rotateTrack(int delta);
  Q_INVOKABLE void euclidFill(int pulses, int steps, int rotate, int note = 60,
                              int velocity = 100);
  Q_INVOKABLE void humanizeTrack(int amount);

  // Parameter locks on the edited track.
  Q_INVOKABLE QVariantList plocksForStep(int step) const;
  Q_INVOKABLE void setPlock(int step, int pid, double value);
  Q_INVOKABLE void clearPlock(int step, int pid);
  Q_INVOKABLE void clearStepPlocks(int step);

  // Song chain: [{pattern, repeats}].
  Q_INVOKABLE QVariantList song() const;
  Q_INVOKABLE void setSong(const QVariantList& chain);

  // Enum label lists, mirroring the firmware's seq_model.h order. Exposed so
  // the QML never hard-codes them (and so a firmware that grows a condition
  // only needs this one list updated).
  Q_INVOKABLE QStringList condNames() const { return SynthProto::seqCondNames(); }
  Q_INVOKABLE QStringList divNames() const { return SynthProto::seqDivNames(); }
  Q_INVOKABLE QStringList dirNames() const { return SynthProto::seqDirNames(); }
  Q_INVOKABLE QStringList targetNames() const { return SynthProto::seqTargetNames(); }
  // Scale names come from the firmware via PARAM_INFO on seq.scale, which is
  // the authority; this is the fallback when that has not arrived yet.
  Q_INVOKABLE QStringList scaleNames() const;
  // "C4", "F#3" — MIDI note number to name, C4 = 60 (Yamaha convention).
  Q_INVOKABLE QString noteName(int note) const;

  // --- drum kit (S22) ----------------------------------------------------
  QVariantList kitSlots() const { return m_kitSlots; }
  QVariantList kits() const { return m_kits; }
  int currentKit() const { return m_currentKit; }
  int defaultDrumNote() const;
  Q_INVOKABLE void refreshKit();
  Q_INVOKABLE void selectKit(int index);
  // Fire a slot without touching the sequencer — the audition buttons and the
  // on-screen pads. Velocity needs a dedicated opcode: `drums.trig` is a
  // parameter and can only carry the slot number.
  Q_INVOKABLE void triggerDrum(int slot, int velocity = 100);

  // --- local patch library ----------------------------------------------
  Q_INVOKABLE int saveCurrentAsPatch(const QString& name);  // snapshot live params
  Q_INVOKABLE void loadPatch(int patchId);                  // push snapshot to synth
  Q_INVOKABLE bool renamePatch(int patchId, const QString& name);
  Q_INVOKABLE bool deletePatch(int patchId);
  Q_INVOKABLE QVariantList patches(int engine = -1);  // -1 = all engines

  // --- patch interchange (JSON, format version 1) -------------------------
  // Parameters travel by NAME ("flt.cutoff"), because ids are registration
  // order and shift between engines and firmware builds. The id is written
  // alongside as a fallback for files exported while disconnected, when the
  // stored patch's names cannot be resolved.
  static constexpr int kPatchJsonVersion = 1;
  Q_INVOKABLE QString patchToJson(int patchId) const;  // one stored library patch
  Q_INVOKABLE QString libraryToJson() const;           // every stored patch, as an array
  // Reads a synth preset slot: loads it, re-reads the values, then hands the
  // JSON back through presetJsonReady(). Loading is the only way to see a
  // preset's parameters — the synth does not serve slots it has not loaded —
  // so this *does* change the live sound.
  Q_INVOKABLE void exportPresetJson(int engine, int slot);
  // Parses a patch file and pushes it to the synth. Returns
  // { ok, error, name }; the applied/skipped counts arrive as a showInfo().
  Q_INVOKABLE QVariantMap importPatchJson(const QString& text);

  static QString engineNameFor(int engine);

 public slots:
  void setConnected(bool connected);
  // Negotiated ATT MTU of the live link (0 = unknown). Every batched frame is
  // sized from this; see maxPayloadBytes().
  void setLinkMtu(int mtu);
  void onReceiveData(const QByteArray& data);
  void onInfoRead(const QByteArray& info);

 signals:
  // Bytes to send to CTRL; withResponse=false is the low-jitter path.
  void writeToSynth(const QByteArray& data, bool withResponse);

  void connectedChanged();
  void readyChanged();
  void engineChanged();
  void presetChanged();
  void infoChanged();
  void isUpdatingFirmwareChanged();

  // A parameter's live value changed (own edit, GET_PARAM, or EVT_PARAMS).
  void paramChanged(int id, double value);
  // Parameter metadata (the registered set / ranges / enum names) was updated
  // after a discovery pass; pages should rebind.
  void paramsDiscovered();
  // A preset list arrived for an engine.
  void presetsChanged(int engine);
  // exportPresetJson() finished reading a slot: the patch file text, and the
  // preset's name for the suggested file name.
  void presetJsonReady(const QString& json, const QString& name);

  // Sequencer: sizing/capacity, the cached steps or track config, the edited
  // target, and the playhead (which moves ~20 Hz and drives only the grid's
  // highlight — kept separate so a step edit never repaints the whole page).
  void seqInfoChanged();
  void stepsChanged();
  void trackConfigChanged();
  void editTargetChanged();
  void playheadChanged();
  void plocksChanged();
  void songChanged();
  void kitChanged();
  // Emitted when switching a drum lane to "from step note": the lane's old
  // fixed slot becomes the sensible note to keep placing, so QML seeds
  // UI.paintNote from it rather than leaving the melodic default (60), which
  // no kit slot answers to and which would therefore place silent steps.
  void paintNoteSuggested(int note);

  void showError(const QString& msg);
  void showInfo(const QString& msg);

 private:
  struct Param {
    SynthProto::ParamInfo info;
    bool infoKnown = false;
    float value = 0.0f;
    bool valueKnown = false;
  };

  quint8 nextSeq();
  void send(quint8 op, const QByteArray& payload, bool withResponse);
  void sendWithSeq(quint8 op, quint8 seq, const QByteArray& payload, bool withResponse);

  // --- frame sizing ------------------------------------------------------
  // Payload bytes that fit one command frame on the live link (ATT payload
  // minus the 4-byte frame header). The batch limits below derive from it, so
  // a link that negotiated less than the documented 247 packs smaller frames
  // instead of sending oversized ones the firmware rejects as MALFORMED.
  int maxPayloadBytes() const;
  int maxSetPairsPerFrame() const;
  int maxGetIdsPerFrame() const;
  int maxStepsPerFrame() const;

  void resetState();
  void beginDiscovery();          // PARAM_INFO 0xFFFF
  void onParamListComplete();     // ids known: register, go ready, start info pump
  void pumpInfoRequests();        // flow-controlled: keeps a small window in flight
  void sendInfoRequest(quint16 id);  // one PARAM_INFO, tracked for flow control
  void onInfoBusy(quint8 seq);    // BUSY (queue full): back off, re-queue that id
  void onInfoBadArg(quint8 seq);  // BAD_ARG: id not registered; stop retrying it
  void finishDiscovery();         // all infos in (or budget spent): stop the pump
  void requestAllParamValues();   // GET_PARAM for every registered id
  void flushPendingSets();        // coalesced SET_PARAM batch
  void scheduleParamsDiscovered(); // coalesce the paramsDiscovered signal

  // Paced SET_PARAM output. The firmware's command queue is four frames deep
  // and answers BUSY when it overflows, dropping the frame — so a patch push
  // (~5 back-to-back full frames) used to apply only part of itself, silently.
  // Frames go through this queue instead: the first goes out immediately when
  // the queue is idle (no added latency for a knob or a pad hit), the rest are
  // spaced, and a BUSY re-queues the exact frame that was dropped.
  void queueSetFrame(const QByteArray& payload);
  void drainSetQueue();
  void onSetBusy(quint8 seq);

  void handleFrame(const SynthProto::Frame& f);
  void handleParamInfoList(const QByteArray& payload);
  void handleParamInfoSingle(const QByteArray& payload);
  void handleParamValues(const QByteArray& payload);  // GET_PARAM + EVT_PARAMS
  void handlePresetList(const QByteArray& payload, bool more);
  void handleEngineEvent(const QByteArray& payload);

  // Sequencer/kit frame handlers, all chunked like the preset list.
  void handleSeqInfo(const QByteArray& payload);
  void handleSeqSteps(const QByteArray& payload, bool more);
  void handleSeqTrack(const QByteArray& payload, quint8 seq);
  void handleSeqPattern(const QByteArray& payload);
  void handleSeqPlock(const QByteArray& payload, bool more);
  void handleSeqSong(const QByteArray& payload, bool more);
  void handleKitInfo(const QByteArray& payload, bool more);
  // Requests the edited track's steps; the firmware caps one response at the
  // MTU, so this walks the track in windows.
  void requestSteps();
  void writeStep(int index);
  // Writes a run of cached steps back, batched to the frame cap.
  void writeSteps(int first, int count);
  // Stamps `slot`'s kit note onto every filled step of the edited track.
  // Returns how many it changed.
  int stampSlotNoteOnSteps(int slot);

  void applyValue(quint16 id, float value, bool echo);
  void setEngineCaps(quint8 engine, quint8 caps);
  void updatePresetFromSlot();

  bool m_connected = false;
  bool m_ready = false;
  int m_linkMtu = 0;  // 0 = unknown; maxPayloadBytes() then assumes 247
  int m_engine = 0;
  int m_caps = 0;

  int m_presetSlot = -1;
  QString m_presetName;
  bool m_presetIsFactory = false;

  QString m_target;
  QString m_fwVersion;
  int m_protoVersion = 0;
  bool m_isUpdatingFirmware = false;

  QHash<quint16, Param> m_params;
  QList<quint16> m_paramOrder;

  // Discovery bookkeeping.
  quint8 m_seq = 0;
  quint8 m_infoListSeq = 0;        // seq of the in-flight PARAM_INFO 0xFFFF
  bool m_awaitingInfoList = false; // true only while the list response is pending
  quint8 m_infoListEngine = 0;     // engine/caps carried by the list response
  quint8 m_infoListCaps = 0;
  QList<quint16> m_infoListAccum;  // ids accumulated across chunked list frames
  QSet<quint16> m_pendingInfoIds;  // ids still missing PARAM_INFO metadata
  // Flow-controlled per-id PARAM_INFO discovery. The firmware has a small command
  // queue and returns BUSY when flooded, so we keep only a few requests in flight
  // and send the next as each response arrives. Ordered work list + in-flight
  // map (id -> send time ms) + seq->id map to attribute error (BUSY/BAD_ARG)
  // responses back to their id.
  QList<quint16> m_infoQueue;           // ids waiting to be requested
  QHash<quint16, qint64> m_infoInflight;  // id -> send time (ms since epoch)
  QHash<quint8, quint16> m_infoSeqToId;   // request seq -> id
  qint64 m_infoBackoffUntilMs = 0;      // pause new sends until this time (BUSY)
  qint64 m_discoveryStartMs = 0;        // for the overall safety budget
  QTimer m_infoRequestTimer;            // drives the flow-controlled pump
  bool m_discovering = false;
  QTimer m_discoveryTimer;              // list-response watchdog (single-shot)
  int m_listRetries = 0;                // remaining list-request resends

  // paramsDiscovered fires often during progressive fill; coalesce it so the
  // page groups rebuild at most ~once per burst instead of per-id.
  QTimer m_discoveredCoalesceTimer;

  // Coalesced knob writes: latest value per id, flushed by m_setTimer.
  QHash<quint16, float> m_pendingSets;
  QTimer m_setTimer;

  // Paced SET_PARAM output (queueSetFrame). m_setSent keeps the last few sent
  // frames so a BUSY — the only feedback a write-without-response ever gets —
  // can put the dropped one back at the head of the queue.
  QList<QByteArray> m_setQueue;
  QHash<quint8, QByteArray> m_setSent;
  QList<quint8> m_setSentOrder;
  qint64 m_setBackoffUntilMs = 0;
  qint64 m_setNextSendMs = 0;  // earliest next send; paces a multi-frame burst
  QTimer m_setDrainTimer;

  // A patch load may need to switch engine first; the snapshot waits here.
  QList<QPair<int, double>> m_pendingPatchParams;
  void pushParams(const QList<QPair<int, double>>& params);

  // --- patch interchange helpers -----------------------------------------
  // One parameter as it travels in a file: name is authoritative, id the
  // fallback for files written with no parameter table to name them by.
  struct NamedParam {
    QString name;
    int id = -1;
    double value = 0.0;
  };
  // Built after an engine switch, when the ids the names resolve to exist.
  QList<NamedParam> m_pendingImport;
  QJsonObject patchJsonObject(const QString& name,
                              int engine,
                              const QString& created,
                              const QList<QPair<int, double>>& params) const;
  void resolveAndPushImport(const QList<NamedParam>& items);

  // Cached preset lists per engine, and accumulation across chunked frames.
  QHash<int, QList<SynthProto::PresetEntry>> m_presets;
  QHash<int, QList<SynthProto::PresetEntry>> m_presetsAccum;

  // --- sequencer state ---------------------------------------------------
  // The grid shows one pattern/track at a time, so only that slice is cached.
  // Fetching all 8x256 steps up front would be ~16 KB over BLE for a view
  // that shows 64 of them.
  SynthProto::SeqInfo m_seqInfo;
  int m_editPattern = 0;
  int m_editTrack = 0;
  int m_playhead = -1;
  QList<SynthProto::SeqStep> m_steps;       // the edited track, index = step
  QList<SynthProto::SeqStep> m_stepsAccum;  // across chunked frames
  int m_stepsAccumFirst = 0;
  int m_stepsWindowNext = 0;                // next window to request, -1 = idle
  // The step walk needs the track's length, which only the SEQ_TRACK response
  // carries — so refreshSequencer() does not start it, it waits for that
  // response and starts it there. Without this the walk ran twice on every
  // track switch: once with the *previous* track's length, then again when the
  // real one arrived. The seq identifies the refresh's own SEQ_TRACK get, so
  // the echo of a track-field edit does not restart it.
  quint8 m_trackGetSeq = 0;
  bool m_awaitingTrackGet = false;
  QTimer m_trackGetFallbackTimer;  // start the walk anyway if that get is lost
  SynthProto::SeqTrackCfg m_trackCfg;
  int m_patternLength = 64;
  int m_patternScale = 0;
  int m_patternRoot = 0;
  int m_patternSwing = 50;
  QString m_patternName;
  // Locks of the edited pattern: (track, step) -> [{pid, value}].
  QHash<quint32, QVariantList> m_plocks;
  QHash<quint32, QVariantList> m_plocksAccum;
  QVariantList m_song;
  QVariantList m_songAccum;
  // Debounces the "follow the playing pattern" re-read: a song chain of short
  // patterns advances every bar, and one full pattern re-read per bar would
  // saturate the link.
  QTimer m_followPatternTimer;

  QVariantList m_kitSlots;
  QVariantList m_kitSlotsAccum;
  QVariantList m_kits;
  QVariantList m_kitsAccum;
  int m_currentKit = 0;
  // DRUM_TRIG (0x38) is newer than the rest of the drum bus, so a device may
  // be running firmware that has the kit but not the opcode. Assume it works,
  // probe once at discovery, and fall back to the drums.trig parameter if the
  // synth answers UNKNOWN_OP.
  bool m_drumTrigOpcode = true;

  static quint32 plockKey(int track, int step) {
    return (quint32(track) << 16) | quint32(step);
  }
};

#endif  // SYNTHCONTROLLER_H
