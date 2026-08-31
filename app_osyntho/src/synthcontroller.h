#ifndef SYNTHCONTROLLER_H
#define SYNTHCONTROLLER_H

#include <QByteArray>
#include <QHash>
#include <QJsonDocument>
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
#include "src/loopwav.h"
#include "src/chordtypes.h"
#include "src/graphtypes.h"
#include "src/paramtypes.h"
#include "src/seqtypes.h"

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
// (No SettingsClient: this class reads no setting. It used to inherit the mixin
// and App wired it up, which cost a dependency nothing ever asked a question
// through — and made it look, to anyone reading, as though some behaviour here
// were user-configurable.)
class SynthController final : public QObject, public DatabaseClient {
  Q_OBJECT

  Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged FINAL)
  Q_PROPERTY(bool ready READ ready NOTIFY readyChanged FINAL)

  Q_PROPERTY(int engine READ engine NOTIFY engineChanged FINAL)
  Q_PROPERTY(QString engineName READ engineName NOTIFY engineChanged FINAL)
  Q_PROPERTY(int caps READ caps NOTIFY engineChanged FINAL)

  // [{n, e}] for the engine picker: which engines the *connected* firmware
  // actually has, so the app never offers a slot that cannot be bound. Derived
  // from engine.type's enum labels rather than from a list here, which is what
  // lets one app build talk to firmware older than itself.
  Q_PROPERTY(QVariantList engineList READ engineList NOTIFY engineListChanged FINAL)

  Q_PROPERTY(int presetSlot READ presetSlot NOTIFY presetChanged FINAL)
  Q_PROPERTY(QString presetName READ presetName NOTIFY presetChanged FINAL)
  Q_PROPERTY(bool presetIsFactory READ presetIsFactory NOTIFY presetChanged FINAL)

  // Which row of the app's own patch library the live sound came from, or -1.
  // App-side state by necessity: the firmware knows nothing about the library,
  // so nothing but this can say a stored patch is what is playing — and that is
  // exactly why it has to be dropped the moment something else claims the sound
  // (a preset load, an engine change that is not this patch's own, a fresh
  // connection, or the row being deleted).
  Q_PROPERTY(int libraryPatchId READ libraryPatchId NOTIFY libraryPatchChanged FINAL)

  Q_PROPERTY(QString synthTarget READ synthTarget NOTIFY infoChanged FINAL)
  Q_PROPERTY(QString firmwareVersion READ firmwareVersion NOTIFY infoChanged FINAL)
  Q_PROPERTY(int protocolVersion READ protocolVersion NOTIFY infoChanged FINAL)

  // Kept for a future osynth firmware-update capability (none today).
  Q_PROPERTY(bool firmwareUpdateSupported READ firmwareUpdateSupported CONSTANT FINAL)
  Q_PROPERTY(bool isUpdatingFirmware READ isUpdatingFirmware NOTIFY isUpdatingFirmwareChanged FINAL)

  // --- chord mode (S41) ---
  // The nineteen chord.* parameters need nothing here — they are ordinary
  // parameters and the page reads them like any other. Only the user set does:
  // twelve slots of intervals are not parameter space, so they travel over
  // CHORD_SET. chordAvailable stays false until that opcode has answered, so
  // the page hides its user-set editor on firmware that predates chord mode
  // rather than offering an editor for something that is not there.
  Q_PROPERTY(bool chordAvailable READ chordAvailable NOTIFY chordSetChanged FINAL)
  Q_PROPERTY(QList<ChordSlot> chordSet READ chordSet NOTIFY chordSetChanged FINAL)

  // --- sequencer (S23) ---
  // The firmware's compile-time sizing: a PSRAM build has 8 tracks x 8
  // patterns, a classic ESP32 half that. The grid binds to these rather than
  // assuming, so one UI serves both.
  Q_PROPERTY(bool seqAvailable READ seqAvailable NOTIFY seqInfoChanged FINAL)
  Q_PROPERTY(int seqTracks READ seqTracks NOTIFY seqInfoChanged FINAL)
  Q_PROPERTY(int seqPatterns READ seqPatterns NOTIFY seqInfoChanged FINAL)
  Q_PROPERTY(int seqMaxSteps READ seqMaxSteps NOTIFY seqInfoChanged FINAL)
  Q_PROPERTY(int seqPlockCapacity READ seqPlockCapacity NOTIFY seqInfoChanged FINAL)
  Q_PROPERTY(int seqPlockUsed READ seqPlockUsed NOTIFY seqInfoChanged FINAL)
  // Which pattern/track the grid is showing (app-side view state).
  Q_PROPERTY(int editPattern READ editPattern WRITE setEditPattern NOTIFY editTargetChanged FINAL)
  Q_PROPERTY(int editTrack READ editTrack WRITE setEditTrack NOTIFY editTargetChanged FINAL)
  // Live playhead, mirrored from seq.pos (-1 when stopped).
  Q_PROPERTY(int playhead READ playhead NOTIFY playheadChanged FINAL)
  Q_PROPERTY(bool playing READ playing NOTIFY playheadChanged FINAL)

  // --- drum kit (S22) ---
  Q_PROPERTY(QVariantList kitSlots READ kitSlots NOTIFY kitChanged FINAL)
  Q_PROPERTY(QVariantList kits READ kits NOTIFY kitChanged FINAL)
  Q_PROPERTY(int currentKit READ currentKit NOTIFY kitChanged FINAL)
  // Where the firmware is persisting recordable kits: "sd", "lfs" or "none"
  // (S44). The page shows it, and it is what decides whether a Save control is
  // offered at all -- a button that provably cannot work is worse than one
  // that is not there.
  Q_PROPERTY(QString kitStorage READ kitStorage NOTIFY kitChanged FINAL)
  // Note of the kit's first populated slot — the kick in the factory kit.
  // The sensible starting pick for drum placement, since the melodic
  // default (60) answers to no slot at all. -1 before a kit arrives.
  Q_PROPERTY(int defaultDrumNote READ defaultDrumNote NOTIFY kitChanged FINAL)

  // ---- modular patch graph (S28) ----
  // graphAvailable stays false until GRAPH_INFO answers OK, so the patch page
  // hides itself on firmware built without the modular engine rather than
  // offering controls that would all fail.
  Q_PROPERTY(bool graphAvailable READ graphAvailable NOTIFY graphInfoChanged FINAL)
  Q_PROPERTY(int graphMaxNodes READ graphMaxNodes NOTIFY graphInfoChanged FINAL)
  Q_PROPERTY(int graphMaxInputs READ graphMaxInputs NOTIFY graphInfoChanged FINAL)
  Q_PROPERTY(int graphOutSlot READ graphOutSlot NOTIFY graphInfoChanged FINAL)
  Q_PROPERTY(int graphEngineIndex READ graphEngineIndex NOTIFY graphInfoChanged FINAL)
  Q_PROPERTY(int graphCost READ graphCost NOTIFY graphCostChanged FINAL)
  Q_PROPERTY(int graphCostBudget READ graphCostBudget NOTIFY graphInfoChanged FINAL)
  // The kind table, indexed by kind — see src/graphtypes.h. Grown to stay
  // indexable, so entries the synth has not answered for yet are present and
  // invalid rather than absent.
  Q_PROPERTY(QList<GraphKindDesc> graphKinds READ graphKinds NOTIFY graphKindsChanged FINAL)
  // One entry per slot, empty slots included, so graphNodes[slot] is the slot.
  Q_PROPERTY(QList<GraphSlot> graphNodes READ graphNodes NOTIFY graphChanged FINAL)
  Q_PROPERTY(QString graphError READ graphError NOTIFY graphErrorChanged FINAL)

  // ---- loop track download (S33) ----
  // False until an OP_LOOP_DUMP request has actually been answered, so the
  // looper page keeps its download controls off the screen on firmware that
  // has no such opcode rather than showing a button that can only fail.
  Q_PROPERTY(bool loopExportSupported READ loopExportSupported NOTIFY loopExportChanged FINAL)
  Q_PROPERTY(bool loopExportActive READ loopExportActive NOTIFY loopExportChanged FINAL)
  Q_PROPERTY(double loopExportProgress READ loopExportProgress NOTIFY loopExportChanged FINAL)
  // What the last probe found: {valid,source,slot,filled,tracks,frames,rate,
  // seconds,codec,mono,trackBytes}. `filled` is a bitmask, track 1 = bit 0.
  Q_PROPERTY(QVariantMap loopExportInfo READ loopExportInfo NOTIFY loopExportInfoChanged FINAL)

  // ---- USB role (S35) ----
  // What the OTG port is doing, straight from the synth. `usbHostSupported`
  // is false on firmware that cannot take the host role — no USB-OTG, or a
  // build where the USB sink is the audio clock and giving up the device role
  // would leave the synth silent — and is what the osynth page tests before
  // offering the control at all.
  //
  // `usbRestartRequired` is the synth's own comparison of the stored setting
  // against the live role, so it survives a disconnect: the app never has to
  // remember that a change is pending, it just asks again.
  Q_PROPERTY(bool usbStatusKnown READ usbStatusKnown NOTIFY usbStatusChanged FINAL)
  Q_PROPERTY(bool usbHostSupported READ usbHostSupported NOTIFY usbStatusChanged FINAL)
  Q_PROPERTY(int usbActiveMode READ usbActiveMode NOTIFY usbStatusChanged FINAL)
  Q_PROPERTY(int usbRequestedMode READ usbRequestedMode NOTIFY usbStatusChanged FINAL)
  Q_PROPERTY(bool usbRestartRequired READ usbRestartRequired NOTIFY usbStatusChanged FINAL)
  Q_PROPERTY(int usbAttachedCount READ usbAttachedCount NOTIFY usbStatusChanged FINAL)
  Q_PROPERTY(QString usbAttachedName READ usbAttachedName NOTIFY usbStatusChanged FINAL)
  // True from the moment a restart is asked for until the link is back and
  // answering. Drives the page's "restarting…" state; see restartSynth().
  Q_PROPERTY(bool restarting READ restarting NOTIFY restartingChanged FINAL)

 public:
  explicit SynthController(QObject* parent = nullptr);
  ~SynthController() override;

  bool connected() const { return m_connected; }
  bool ready() const { return m_ready; }

  int engine() const { return m_engine; }
  QString engineName() const;
  int caps() const { return m_caps; }

  // The per-engine 0-111 slot the synth is on, or -1 — including whenever
  // the slot preset.load rests on belongs to a different engine. It rests on
  // the last preset *loaded*, and a plain engine switch does not move it, so
  // the raw number would otherwise name a preset the live engine has not got.
  int presetSlot() const {
    return (m_presetEngine == m_engine) ? m_presetSlot : -1;
  }
  QString presetName() const { return m_presetName; }
  bool presetIsFactory() const { return m_presetIsFactory; }
  int libraryPatchId() const { return m_libraryPatchId; }

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
  // A default-constructed ParamMeta (exists == false) for any id the synth
  // has not described, including an out-of-range one — so callers can bind
  // straight to it without a >= 0 guard.
  Q_INVOKABLE ParamMeta paramMeta(int id) const;
  Q_INVOKABLE QList<int> paramIds() const;
  QVariantList engineList() const;
  // UI helpers: resolve by name, or gather every registered id whose name starts
  // with `prefix` (registration order). paramPickerList returns [{id,name}] for
  // the mod-matrix destination picker.
  Q_INVOKABLE int paramIdForName(const QString& name) const;
  Q_INVOKABLE QString paramName(int id) const;
  Q_INVOKABLE QList<int> paramIdsByPrefix(const QString& prefix) const;
  Q_INVOKABLE QVariantList paramPickerList() const;

  // --- commands ----------------------------------------------------------
  // Knob edit: updates the local value, echoes paramChanged, and queues a
  // coalesced write-without-response batch (~20 Hz).
  Q_INVOKABLE void setParam(int id, double value);
  // Momentary gesture: writes `id` straight away, outside the coalescing batch.
  //
  // setParam() keeps only the LAST value written to an id inside its ~40 ms
  // window, which is what a knob wants and the opposite of what a press/release
  // pair wants — those are two values of the same id milliseconds apart, and a
  // tap shorter than the window collapsed to the release alone. The synth then
  // never saw the press: the sequencer's Fill did nothing, intermittently, with
  // nothing on screen saying so. Still paced, so a burst cannot overrun the
  // firmware's command queue.
  Q_INVOKABLE void setParamNow(int id, double value);
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
  Q_INVOKABLE TrackConfig trackConfig() const;
  Q_INVOKABLE void setTrackField(const QString& field, double value);
  Q_INVOKABLE PatternConfig patternConfig() const;
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

  // --- chord mode (S41) --------------------------------------------------
  //
  // The three below answer "what would this key play", for the degree strip
  // and the live readout on the Chord page. They are a *display* copy of the
  // firmware's construction (components/chord/chord.cpp), which stays the
  // sole authority on what the instrument actually sounds — see the comment
  // over chordNotesFor() in the .cpp for what has to stay in step and what
  // deliberately does not.
  //
  // Why a copy at all: both surfaces redraw while a control is moving, and a
  // BLE round trip per redraw is not something that can be made to feel
  // right on a 7.5 ms connection interval.
  Q_INVOKABLE QList<int> chordNotesFor(int note) const;
  // "Cmaj7", "Dm7", "G7" — or the note names when the stack is not a chord
  // any standard name fits, which scale mode reaches on the pentatonics.
  Q_INVOKABLE QString chordNameFor(int note) const;
  // The keys that play degree 0..n-1 of the current scale, for `octaves`
  // octaves of it, so the board can label itself without knowing what
  // `chord.keymap` means — in `degrees` the keys run one semitone apart, in
  // `chromatic` they are the scale's own pitches.
  Q_INVOKABLE QList<int> chordDegreeKeys(int octaves = 1) const;

  // The user chord set: twelve slots, one per pitch class above chord.root.
  Q_INVOKABLE void refreshChordSet();
  Q_INVOKABLE QList<ChordSlot> chordSet() const { return m_chordSet; }
  // Two primitive-argument forms rather than one taking a ChordSlot: QML has
  // to be able to *build* the new value, and a JS object literal converted to
  // a Q_GADGET is both fragile and invisible to qmlcachegen, which drops the
  // whole binding to interpreted byte code. Ints and a list of ints are types
  // it can see all the way through.
  Q_INVOKABLE void setChordSlot(int slot, int transpose,
                                const QList<int>& intervals);
  Q_INVOKABLE void clearChordSlot(int slot);
  // Fills a slot from one of the chord.type qualities, which is how the
  // editor stays usable without asking anyone to type interval numbers.
  Q_INVOKABLE void setChordSlotQuality(int slot, int quality, int transpose);
  // Chord-quality labels, in the order chord.type numbers them. Taken from
  // PARAM_INFO when discovery has it, so the picker cannot disagree with the
  // parameter it writes.
  Q_INVOKABLE QStringList chordQualityNames() const;
  bool chordAvailable() const { return m_chordAvailable; }

  // --- drum kit (S22) ----------------------------------------------------
  QVariantList kitSlots() const { return m_kitSlots; }
  QVariantList kits() const { return m_kits; }
  int currentKit() const { return m_currentKit; }
  QString kitStorage() const { return m_kitStorage; }
  int defaultDrumNote() const;
  Q_INVOKABLE void refreshKit();
  Q_INVOKABLE void selectKit(int index);
  // Fire a slot without touching the sequencer — the audition buttons and the
  // on-screen pads. Velocity needs a dedicated opcode: `drums.trig` is a
  // parameter and can only carry the slot number.
  Q_INVOKABLE void triggerDrum(int slot, int velocity = 100);
  // Let go of a pad. Only gate and loop pads (S44) hold anything, so this is a
  // no-op on every other kind and the pad surfaces can send it on every
  // touch-up without first asking what the pad is.
  Q_INVOKABLE void releaseDrum(int slot);

  // ---- sample-kit editing (S44) ----
  //
  // These write kit data rather than parameters, so they do not go through
  // setParam's coalescing batch and they do not appear in a preset. `field`
  // is KitPadField in synthprotocol.h. Each is followed by a kit re-read,
  // because the firmware is the authority on what a pad ended up as -- it
  // clamps, and it refuses outright on the factory kit.
  Q_INVOKABLE void setPadField(int slot, int field, double value);
  Q_INVOKABLE void renameKit(int kit, const QString& name);
  Q_INVOKABLE void renamePad(int slot, const QString& name);

  // --- modular patch graph (S28) -----------------------------------------
  bool graphAvailable() const { return m_graphAvailable; }
  int graphMaxNodes() const { return m_graphMaxNodes; }
  int graphMaxInputs() const { return m_graphMaxInputs; }
  int graphOutSlot() const { return m_graphOutSlot; }
  int graphEngineIndex() const { return m_graphEngineIndex; }
  int graphCost() const { return m_graphCost; }
  int graphCostBudget() const { return m_graphCostBudget; }
  QList<GraphKindDesc> graphKinds() const { return m_graphKinds; }
  QList<GraphSlot> graphNodes() const { return m_graphNodes; }
  QString graphError() const { return m_graphError; }

  // Probes GRAPH_INFO, then the kind table (once per connection — it is
  // build-constant), then the model.
  Q_INVOKABLE void refreshGraph();
  // Just the model + cost, for after an edit or a preset load.
  Q_INVOKABLE void refreshGraphModel();

  Q_INVOKABLE void graphSetKind(int slot, int kind);
  Q_INVOKABLE void graphConnect(int dst, int port, int src);  // src < 0 unpatches
  Q_INVOKABLE void graphSetNodePos(int slot, int x, int y);

  // Parameter id of node `slot`'s parameter `index` — the positional scheme
  // (0x0200 + 16*slot + index). Exposed rather than recomputed in QML so the
  // layout constant lives in exactly one place on this side too.
  Q_INVOKABLE int graphNodeParamId(int slot, int index) const;
  // Source slot patched into `port` of `slot`, or -1 when nothing is.
  // Deliberately not left to QML as graphNodes[slot].sources[port]: see the
  // definition for the codegen trap that forces it, and for why the two jack
  // *bindings* still read .sources directly.
  Q_INVOKABLE int graphSource(int slot, int port) const;
  // Kind descriptor by index. Returns an invalid one for a kind outside the
  // table rather than nothing, so the canvas can read .name and .inputs off
  // the answer unconditionally.
  Q_INVOKABLE GraphKindDesc graphKind(int kind) const;
  // Lowest empty slot, or -1 when the graph is full — what "add node" uses.
  Q_INVOKABLE int graphFreeSlot() const;
  Q_INVOKABLE void clearGraphError();

  // --- loop track download (S33) -----------------------------------------
  // Sources: 0 = the live set (in PSRAM or streamed off the card — the app
  // cannot tell and does not need to), 1 = a saved slot.
  bool loopExportSupported() const { return m_expSupported; }
  bool loopExportActive() const;
  double loopExportProgress() const;
  QVariantMap loopExportInfo() const { return m_expInfoMap; }

  // Asks what is recorded in one source, for the page's track picker. Cheap
  // and idempotent; ignored while a transfer is running (it would be asking
  // about the thing it is already downloading).
  Q_INVOKABLE void probeLoopExport(int source, int slot = 0);
  // Downloads one track (0-based) and decodes it to a WAV held in memory,
  // announced by loopExportReady(). The track is fetched in windows, so this
  // takes as long as the link and the loop length say — tens of seconds for a
  // long take — and reports loopExportProgress throughout. The track comes out
  // as recorded: loop.lvlN is a mix control and has no business in it.
  Q_INVOKABLE void startLoopExport(int source, int slot, int track);
  // Every recorded track, summed into one WAV at the levels the looper page is
  // showing (loop.lvlN) — the set as it sounds. Downloads each track in turn,
  // so it costs as many transfers as there are tracks; tracks at level 0 are
  // skipped rather than fetched to be multiplied away.
  Q_INVOKABLE void startLoopMixExport(int source, int slot);
  Q_INVOKABLE void cancelLoopExport();
  // Writes the WAV loopExportReady() announced. Kept until the next transfer
  // starts, so a user who cancels the save picker can be offered it again.
  Q_INVOKABLE bool saveLoopExportTo(const QString& path);

  // --- USB role (S35) ----------------------------------------------------
  bool usbStatusKnown() const { return m_usb.valid; }
  bool usbHostSupported() const { return m_usb.supported; }
  int usbActiveMode() const { return m_usb.active; }
  int usbRequestedMode() const { return m_usb.requested; }
  bool usbRestartRequired() const { return m_usb.restartRequired(); }
  int usbAttachedCount() const { return m_usb.attached; }
  QString usbAttachedName() const { return m_usb.product; }
  bool restarting() const { return m_restarting; }

  // Re-reads the port's state. Cheap, and the only way to learn that a
  // controller was plugged in — there is no event for it, on purpose: a
  // notification the app would only act on while one page is visible is worth
  // less than a poll that page can run itself.
  Q_INVOKABLE void refreshUsbStatus();
  // Asks the synth to restart so a pending `usb.mode` change takes effect.
  // The firmware flushes the persisted settings first, so the value written a
  // moment ago is not lost to the write coalescing. The link drops either way;
  // `restarting` stays true until it is back and answering.
  Q_INVOKABLE void restartSynth();

  // --- local patch library ----------------------------------------------
  // A library patch is the whole synth, not only its knobs: every parameter
  // that is not an action, plus — on the modular engine — the graph the
  // parameters hang off. Without the graph a modular patch comes back applied
  // to whatever happened to be cabled up, because a node's parameter ids only
  // mean what the kind in that slot says they mean.
  Q_INVOKABLE int saveCurrentAsPatch(const QString& name);  // snapshot live params
  // The two levels are opt-in, and default to off. They are in the snapshot —
  // there is no reason to lose them — but they describe the room and the
  // wiring rather than the sound: master volume is the level you are
  // monitoring at, out.level is set once by ear for what is plugged into the
  // jack. A patch that moved either behind the player is the surprise the
  // firmware's own presets exclude them to avoid, so replaying them is the
  // Lib page's switches, not the default.
  Q_INVOKABLE void loadPatch(int patchId, bool withMasterVolume = false,
                             bool withOutLevel = false);
  Q_INVOKABLE bool renamePatch(int patchId, const QString& name);
  Q_INVOKABLE bool deletePatch(int patchId);
  Q_INVOKABLE QVariantList patches(int engine = -1);  // -1 = all engines

  // --- patch interchange (JSON, format version 1) -------------------------
  // Parameters travel by NAME ("flt.cutoff"), because ids are registration
  // order and shift between engines and firmware builds. The id is written
  // alongside as a fallback for files exported while disconnected, when the
  // stored patch's names cannot be resolved.
  //
  // A modular patch also carries a "graph" array. Deliberately not a version
  // bump: it is an added key, files without one still read exactly as before,
  // and a build that does not know about it ignores it — which is the whole
  // reason to add a key rather than change a shape. Bumping the version would
  // instead make every older app refuse the file outright (see the version
  // check in importPatchJson).
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
  // Parses a patch file and stores every patch it holds in the local library,
  // leaving the live sound alone — the Lib page's Import, as against the
  // Presets page's, which applies. Returns { ok, error, imported, skipped }.
  Q_INVOKABLE QVariantMap importPatchesToLibrary(const QString& text);

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
  void engineListChanged();
  void presetChanged();
  void libraryPatchChanged();
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
  void chordSetChanged();
  void stepsChanged();
  void trackConfigChanged();
  void editTargetChanged();
  void playheadChanged();
  void plocksChanged();
  void songChanged();
  void kitChanged();
  void graphInfoChanged();
  void graphKindsChanged();
  void graphChanged();
  void graphCostChanged();
  void graphErrorChanged();

  // Loop download: supported/active/progress, the probe result, and the two
  // endings. `suggestedName` is a file name, not a path — where it goes is the
  // platform's business (a save picker on desktop, the share sheet on Android).
  void loopExportChanged();
  void loopExportInfoChanged();

  // USB role: one signal for the whole status block — it arrives as one frame
  // and every consumer reads several fields of it.
  void usbStatusChanged();
  void restartingChanged();
  // The restart did not complete inside the timeout. The page turns this into
  // a message; the link may still come back on its own afterwards.
  void restartTimedOut();
  void loopExportReady(const QString& suggestedName);
  void loopExportFailed(const QString& reason);
  // Emitted when switching a drum lane to "from step note": the lane's old
  // fixed slot becomes the sensible note to keep placing, so QML seeds
  // UI.paintNote from it rather than leaving the melodic default (60), which
  // no kit slot answers to and which would therefore place silent steps.
  void paintNoteSuggested(int note);

  // A panic sweep went out (allNotesOff). Surfaces that track what they
  // believe is sounding — the on-screen keyboard's lit and latched sets — must
  // drop it, or they keep painting notes over silence. They must NOT re-send
  // note-offs for it: the sweep already covers every note.
  void allNotesOffSent();

  void showError(const QString& msg);
  void showInfo(const QString& msg);

 private:
  struct Param {
    SynthProto::ParamInfo info;
    bool infoKnown = false;
    float value = 0.0f;
    bool valueKnown = false;
  };

  // Where a loop download is. Probe is a standalone "what is in there?" for
  // the page's pickers; Info is the same question asked as the first step of a
  // transfer, and only that one goes on to Read. Kept apart so a probe landing
  // late cannot be mistaken for the start of a download that was cancelled.
  enum class ExpStage { Idle, Probe, Info, Read };

  quint8 nextSeq();
  void send(quint8 op, const QByteArray& payload, bool withResponse);
  void sendWithSeq(quint8 op, quint8 seq, const QByteArray& payload, bool withResponse);

  // A request frame kept briefly so a BUSY can resend it. The firmware answers
  // BUSY *instead of* running the frame, so an unretried request never happened.
  struct PendingRequest {
    QByteArray frame;
    bool withResponse = true;
    int retriesLeft = 0;
  };
  void trackRequest(quint8 seq, const QByteArray& frame, bool withResponse,
                    int retriesLeft);
  void forgetRequest(quint8 seq);  // answered or definitively rejected
  void onRequestBusy(quint8 seq);
  void drainRequestRetryQueue();

  // Value the synth will actually store for `id`, given its discovered range
  // and type. Own writes are echo-suppressed, so the app has to apply the same
  // rounding/clamping locally or its cache drifts from the hardware.
  float conformValue(quint16 id, float value) const;
  // Applies a parameter the app wrote through a convenience opcode (TRANSPORT,
  // ARP) rather than SET_PARAM, resolving it by name.
  void mirrorLocal(const QString& name, double value);

  void drainNoteOffQueue();

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

  // How much a discovery pass has to re-read. A fresh connection knows
  // nothing, so it reads everything. An engine switch only re-registers the
  // engine-specific 0x02xx range — the sequencer, the kit, the patch graph and
  // the song chain are all untouched by it — so re-reading them was pure cost:
  // ~80 blocking frames on the Windows write path, during which nothing else
  // (a note-off, say) could get out. EngineParams keeps the parameter work and
  // the per-engine preset list, and skips the rest.
  enum class DiscoveryScope { Full, EngineParams };
  void beginDiscovery(DiscoveryScope scope = DiscoveryScope::Full);
  void onParamListComplete();     // ids known: register, go ready, start info pump
  void pumpInfoRequests();        // flow-controlled: keeps a small window in flight
  void queueInfoId(quint16 id);      // enqueue for the pump; priority ids first
  void sendInfoRequest(quint16 id);  // one PARAM_INFO, tracked for flow control
  void onInfoBusy(quint8 seq);    // BUSY (queue full): back off, re-queue that id
  void onInfoBadArg(quint8 seq);  // BAD_ARG: id not registered; stop retrying it
  void finishDiscovery();         // all infos in (or budget spent): stop the pump
  // A few ids' metadata, outside a discovery pass. Same queue, same flow
  // control, no id list and no budget — for parameters the firmware registers
  // *after* discovery has been and gone (see syncGraphParamInfo).
  void requestInfoTopUp(const QList<quint16>& ids);
  void requestAllParamValues();   // GET_PARAM for every registered id
  void requestParamValues(const QList<quint16>& ids);  // GET_PARAM, batched
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
  void handleUsbStatus(const QByteArray& payload);

  // Pushes m_editTrack into the firmware's seq.edit.track, which is what the
  // recorder writes to. Call after anything that moves m_editTrack, and on
  // connect — the synth keeps its own selection across a disconnect.
  void syncEditTrack();

  // Records that this app just wrote pattern data, so a revision bump caused by
  // that write does not trigger a re-read that undoes it.
  void noteLocalSeqEdit();
  // seq.mode == rec. A revision bump means something different then.
  bool seqRecording() const;
  // What a settled seq.rev change re-reads, which depends on the above.
  void onSeqRevSettled();

  // Sequencer/kit frame handlers, all chunked like the preset list.
  void handleSeqInfo(const QByteArray& payload);
  void handleChordSet(const QByteArray& payload);
  // Live value of one chord.* parameter, by name, falling back to
  // `fallback` before discovery has run. The display-side chord math
  // reads every setting through this, so "not connected yet" is one
  // answer in one place rather than a guard at every call site.
  double chordParam(const char* leaf, double fallback) const;
  void chordScaleRoot(int* scale, int* root) const;
  void handleSeqSteps(const QByteArray& payload, bool more);
  void handleSeqTrack(const QByteArray& payload, quint8 seq);
  void handleSeqPattern(const QByteArray& payload);
  void handleSeqPlock(const QByteArray& payload, bool more);
  void handleSeqSong(const QByteArray& payload, bool more);
  void handleKitInfo(const QByteArray& payload, bool more);
  void handleGraphInfo(const QByteArray& payload);
  void handleGraphKind(const QByteArray& payload);
  void handleGraphNodes(const QByteArray& payload);
  void handleGraphEdit(const QByteArray& payload, quint8 status);

  // Loop download. The transfer is a two-step conversation — INFO to learn the
  // codec, the length and how many bytes a track is, then a walk of READ
  // windows — and its own retries: a lost window is re-asked from the first
  // byte still missing, which is why every data frame carries its offset.
  void handleLoopDump(const QByteArray& payload, bool more);
  void handleLoopDumpStatus(quint8 status);
  void sendLoopDumpInfo(int source, int slot);
  void requestLoopWindow();          // the window starting at m_expOffset
  void onLoopWindowTimeout();
  // One track has arrived in full: mix it in and move to the next, or — for a
  // single-track export, and for the last track of a mix — build the file.
  void finishLoopTrack();
  void startLoopExportCommon(int source, int slot);  // shared reset + INFO
  void failLoopExport(const QString& reason);
  void setLoopExportStage(ExpStage stage);
  // The level the looper page is showing for a track (loop.lvlN, 1-based in
  // the parameter's name), or 1.0 on firmware that does not register it.
  double loopTrackLevel(int track) const;
  void rebuildGraphNodes(const SynthProto::GraphModel& m);
  // Fetches metadata and values for the node parameters this app does not know
  // about yet. A node's parameters only exist once its slot has a kind, which
  // is long after discovery listed the ids — so without this a node added
  // during the session has controls the UI cannot draw.
  void syncGraphParamInfo();
  // Forgets one slot's parameter block, for when its kind changes: the ids stay
  // the same and stop meaning what the app recorded about them.
  void forgetNodeParamInfo(int slot);
  // Requests the edited track's steps; the firmware caps one response at the
  // MTU, so this walks the track in windows.
  void requestSteps();
  // Starts that walk at step 0, with a fresh retry budget for its first window.
  void beginStepWalk();
  void writeStep(int index);
  // Writes a run of cached steps back, batched to the frame cap.
  void writeSteps(int first, int count);
  // Stamps `slot`'s kit note onto every filled step of the edited track.
  // Returns how many it changed.
  int stampSlotNoteOnSteps(int slot);

  void applyValue(quint16 id, float value, bool echo);
  void setEngineCaps(quint8 engine, quint8 caps);
  void updatePresetFromSlot();
  // -1 clears the mark. `engine` is the engine the patch was stored for, and is
  // what lets setEngineCaps() tell loadPatch()'s own engine switch apart from
  // the player picking a different engine.
  void setLibraryPatch(int patchId, int engine);

  bool m_connected = false;
  bool m_ready = false;
  int m_linkMtu = 0;  // 0 = unknown; maxPayloadBytes() then assumes 247
  int m_engine = 0;
  int m_caps = 0;

  int m_presetSlot = -1;
  // The engine half of preset.load's linear value; -1 until one is reported.
  // Kept apart from m_engine so presetSlot() can tell "no preset on this
  // engine" from "slot 0".
  int m_presetEngine = -1;
  QString m_presetName;
  bool m_presetIsFactory = false;

  int m_libraryPatchId = -1;
  int m_libraryPatchEngine = -1;

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
  // A top-up is running: the pump has work but no list to wait for, no budget
  // to spend and no discovery to declare finished. Never set while
  // m_discovering — a pass in flight already covers every registered id.
  bool m_infoTopUp = false;
  // Deadline for the running top-up. A discovery pass has kDiscoveryBudgetMs to
  // stop it retrying an id that will never answer; a top-up needs its own, or a
  // node whose slot was cleared under it would keep the pump spinning for the
  // rest of the session.
  qint64 m_infoTopUpUntilMs = 0;
  DiscoveryScope m_discoveryScope = DiscoveryScope::Full;
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

  // Retry pool for every other request op (listings, sequencer/kit reads,
  // preset and engine commands). Keyed by seq, which the resend reuses so the
  // response stays routable by m_infoListSeq / m_trackGetSeq.
  QHash<quint8, PendingRequest> m_sentRequests;
  QList<quint8> m_sentRequestOrder;
  QList<PendingRequest> m_requestRetryQueue;
  QTimer m_requestRetryTimer;

  // Notes the app believes are sounding, so allNotesOff() sends only those —
  // and paces them, because 128 back-to-back frames would overrun the
  // firmware's 4-deep command queue and mostly be dropped.
  QSet<int> m_heldNotes;
  QList<int> m_noteOffQueue;
  QTimer m_noteOffTimer;

  // A patch load may need to switch engine first; the snapshot waits here.
  QList<QPair<int, double>> m_pendingPatchParams;
  void pushParams(const QList<QPair<int, double>>& params);

  // --- modular graph, as a library patch stores it ------------------------
  // JSON array of { kind, in: [...], x, y }, index = slot. Empty when the
  // graph is unavailable (no modular engine on this firmware) or the live
  // engine is not Modular.
  QString graphJson() const;
  static QList<SynthProto::GraphNode> graphFromJson(const QString& text);
  // Pushes a whole graph. One GRAPH_EDIT with the model blob where the
  // firmware supports it; a replay of per-node edits where it does not, which
  // is slower and audibly steps through the intermediate patches — hence the
  // preference. Empty list = nothing to do.
  void pushGraph(const QList<SynthProto::GraphNode>& nodes);
  void pushGraphNodeByNode(const QList<SynthProto::GraphNode>& nodes);
  // Set while a whole-model push is in flight, so the ST_BAD_ARG that older
  // firmware answers sub-op 3 with can be told from a rejected ordinary edit
  // and turned into the fallback.
  QList<SynthProto::GraphNode> m_pendingGraphPush;
  bool m_graphLoadModelInFlight = false;
  // False once firmware has refused sub-op 3, so the rest of the session goes
  // straight to the per-node path instead of paying for the probe every time.
  bool m_graphLoadModelSupported = true;

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
                              const QList<QPair<int, double>>& params,
                              const QString& graph = QString()) const;
  // Shape detection, shared by the two import routes: a file holds one patch,
  // a library envelope, or a bare array. Answers the patch objects in file
  // order, skipping anything without a "params" array.
  static QList<QJsonObject> patchObjectsFrom(const QJsonDocument& doc);
  // One patch object's parameters, minus what a patch must never carry.
  static QList<NamedParam> namedParamsFrom(const QJsonObject& patch);
  void resolveAndPushImport(const QList<NamedParam>& items);

  // Cached preset lists per engine, and accumulation across chunked frames.
  QHash<int, QList<SynthProto::PresetEntry>> m_presets;
  QHash<int, QList<SynthProto::PresetEntry>> m_presetsAccum;

  // --- sequencer state ---------------------------------------------------
  // The grid shows one pattern/track at a time, so only that slice is cached.
  // Fetching all 8x256 steps up front would be ~16 KB over BLE for a view
  // that shows 64 of them.
  SynthProto::SeqInfo m_seqInfo;
  // The user chord set as the synth last reported it. CHORD_SET answers every
  // read *and* every write with the whole set, so this is never a guess about
  // what landed.
  QList<ChordSlot> m_chordSet;
  bool m_chordAvailable = false;
  int m_editPattern = 0;
  int m_editTrack = 0;
  int m_playhead = -1;
  QList<SynthProto::SeqStep> m_steps;       // the edited track, index = step
  QList<SynthProto::SeqStep> m_stepsAccum;  // across chunked frames
  int m_stepsAccumFirst = 0;
  int m_stepsWindowNext = 0;                // next window to request, -1 = idle
  // Watchdog for the window in flight. Each window is asked for only by the
  // *previous* window's response, so a single lost notification used to park
  // the walk for good — the grid then showed the first 24 steps of the pattern
  // and nothing after them, until the user happened to switch track or pattern.
  // Only the SEQ_TRACK get that starts the walk had a fallback; this covers the
  // rest of it.
  QTimer m_stepsRetryTimer;
  int m_stepsWindowRetries = 0;
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

  // Pattern-data revision the app last saw (seq.rev), -1 before the first
  // value. Only compared for inequality — the firmware bumps it once per
  // changed step, and the size of a jump means nothing.
  int m_seqRevision = -1;
  QTimer m_seqRevTimer;  // debounces the re-read a revision change triggers
  // When this app last wrote pattern data. A revision bump inside the guard
  // window is this app's own echo, and re-reading on it would fight the
  // optimistic local edit that a grid drag depends on.
  qint64 m_lastLocalSeqEditMs = 0;
  // Steps written locally since the current read cycle began. A read already in
  // flight predates those writes and must not merge over them — see
  // handleSeqSteps(). Cleared by refreshSequencer().
  QSet<int> m_localStepEdits;

  QVariantList m_kitSlots;
  QVariantList m_kitSlotsAccum;
  QVariantList m_kits;
  QVariantList m_kitsAccum;
  int m_currentKit = 0;
  QString m_kitStorage = QStringLiteral("none");

  // --- modular patch graph (S28) ---
  bool m_graphAvailable = false;
  int m_graphMaxNodes = 0;
  int m_graphParamsPerNode = 16;
  int m_graphMaxInputs = 4;
  int m_graphOutSlot = 0;
  int m_graphEngineIndex = -1;
  int m_graphCost = 0;
  int m_graphCostBudget = 0;
  int m_graphRevision = -1;
  int m_graphKindCount = 0;
  QList<GraphKindDesc> m_graphKinds;
  QList<GraphSlot> m_graphNodes;
  QString m_graphError;
  // DRUM_TRIG (0x38) is newer than the rest of the drum bus, so a device may
  // be running firmware that has the kit but not the opcode. Assume it works,
  // probe once at discovery, and fall back to the drums.trig parameter if the
  // synth answers UNKNOWN_OP.
  bool m_drumTrigOpcode = true;

  // --- loop track download (S33) ---
  // The opposite default to m_drumTrigOpcode above, because the consequence is
  // the opposite: a wrong guess there is a pad that does not sound until the
  // probe answers, a wrong guess here is a panel that appears and then
  // vanishes. So the page stays quiet until a LOOP_DUMP request comes back.
  bool m_expSupported = false;
  ExpStage m_expStage = ExpStage::Idle;
  int m_expSource = 0;
  int m_expSlot = 0;
  int m_expTrack = 0;
  quint32 m_expOffset = 0;  // first byte still missing — where a retry resumes
  quint32 m_expTotal = 0;   // bytes in a full pass of this track
  quint32 m_expFrames = 0;
  int m_expCodec = SynthProto::LOOP_CODEC_ADPCM;
  int m_expRate = 48000;
  QByteArray m_expData;     // track bytes as they arrive, in the synth's codec
  QByteArray m_expWav;      // the finished file, held for saveLoopExportTo
  QString m_expName;
  int m_expRetries = 0;
  QTimer m_expTimer;        // window watchdog; a lost frame stalls nothing else
  QVariantMap m_expInfoMap;
  // Mixdown. m_expQueue is what is left to fetch (one entry for a single-track
  // export), m_expQueueTotal what it started as — progress spans the whole run,
  // not each track's own transfer. The sum lives in a wide accumulator so
  // eight tracks are never decoded side by side (loopwav.h).
  bool m_expMix = false;
  QList<int> m_expQueue;
  int m_expQueueTotal = 1;
  int m_expQueueDone = 0;
  LoopWav::Accumulator m_expMixAcc;
  int m_expChannels = 2;

  // --- USB role (S35) ---
  // Everything the page shows comes from this one struct, refilled by each
  // USB_STATUS response. It is deliberately *not* cleared on disconnect: the
  // last known role is still the best guess while the synth reboots, and
  // blanking the page for the seconds a restart takes only makes the wait look
  // like a failure. `valid` is what says it was ever answered at all.
  SynthProto::UsbStatus m_usb;
  bool m_restarting = false;
  // Retry pump for the reconnect after a restart. The synth is gone for a
  // second or two on the S3 and longer on the P4, where BLE comes back through
  // the C6 companion, so this waits rather than declaring failure at the first
  // missed poll.
  QTimer m_restartTimer;
  qint64 m_restartDeadlineMs = 0;

  static quint32 plockKey(int track, int step) {
    return (quint32(track) << 16) | quint32(step);
  }
};

#endif  // SYNTHCONTROLLER_H
