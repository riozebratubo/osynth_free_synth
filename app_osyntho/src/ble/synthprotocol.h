#ifndef SYNTHPROTOCOL_H
#define SYNTHPROTOCOL_H

// SynthCtl v1 codec — pure, Qt-only (no threading, no GUI). Encodes command
// frames for the CTRL characteristic and decodes response/event frames from
// EVT, plus the INFO characteristic read. See docs/BLE_PROTOCOL.md.
//
// All multi-byte fields are little-endian; floats are IEEE-754 binary32.

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtEndian>

#include <cstdint>
#include <cstring>

namespace SynthProto {

// --- GATT UUIDs (128-bit) ------------------------------------------------
inline constexpr const char* kServiceUuid = "395f2e00-7c4f-4a48-94e7-dafcf25ef34a";
inline constexpr const char* kCtrlUuid    = "395f2e01-7c4f-4a48-94e7-dafcf25ef34a";
inline constexpr const char* kEvtUuid     = "395f2e02-7c4f-4a48-94e7-dafcf25ef34a";
inline constexpr const char* kBulkUuid    = "395f2e03-7c4f-4a48-94e7-dafcf25ef34a";
inline constexpr const char* kInfoUuid    = "395f2e04-7c4f-4a48-94e7-dafcf25ef34a";

// Advertised device name; the app auto-connects to the first match.
inline constexpr const char* kDeviceName = "osynth";

// Preferred ATT MTU. Response payloads size to the live MTU, but PARAM_INFO
// wants the full frame. Neither platform backend can *request* an MTU (Qt
// negotiates it internally; WinRT does it in the OS), so this is what the
// app assumes when the link cannot tell it what was negotiated — see
// SynthController::maxPayloadBytes(), which sizes every outgoing frame from
// the live value where one is available.
inline constexpr int kPreferredMtu = 247;

// Bytes of ATT payload a write can carry at `mtu` (3 bytes of ATT header),
// capped at the firmware's 256-byte rx ceiling.
inline constexpr int attPayloadFor(int mtu) {
  const int m = mtu > 0 ? mtu : kPreferredMtu;
  const int payload = (m < 23 ? 23 : m) - 3;
  return payload > 256 ? 256 : payload;
}

// --- Opcodes (app -> synth) ---------------------------------------------
enum Op : quint8 {
  OP_SET_PARAM     = 0x01,
  OP_GET_PARAM     = 0x02,
  OP_PARAM_INFO    = 0x03,
  OP_SELECT_ENGINE = 0x04,
  OP_LOAD_PRESET   = 0x05,
  OP_SAVE_PRESET   = 0x06,
  OP_LIST_PRESETS  = 0x07,
  OP_TRANSPORT     = 0x10,
  OP_ARP           = 0x11,
  OP_NOTE_ON       = 0x20,
  OP_NOTE_OFF      = 0x21,
  // Sequencer + drum kit (S23). Pattern data is far too big to be parameters,
  // so it moves over these. Ops that read and write take a leading direction
  // byte (0 = get, 1 = set) instead of two opcodes each.
  OP_SEQ_INFO      = 0x30,
  OP_SEQ_STEPS     = 0x31,
  OP_SEQ_TRACK     = 0x32,
  OP_SEQ_PATTERN   = 0x33,
  OP_SEQ_PLOCK     = 0x34,
  OP_SEQ_EDIT      = 0x35,
  OP_SEQ_SONG      = 0x36,
  OP_KIT_INFO      = 0x37,
  // Velocity-carrying drum hit for the on-screen pads. `drums.trig` can only
  // say which slot (a parameter is one float), so velocity needs its own op.
  OP_DRUM_TRIG     = 0x38,
  // Modular patch graph (S28). Node *values* are ordinary parameters
  // (positional: slot k owns 0x0200 + 16k); the graph's structure travels
  // here. A firmware without the modular engine answers all four with
  // ST_UNSUPPORTED, which is how the app decides whether to offer the page.
  OP_GRAPH_INFO    = 0x39,
  OP_GRAPH_KIND    = 0x3A,
  OP_GRAPH_NODES   = 0x3B,
  OP_GRAPH_EDIT    = 0x3C,
  OP_PING          = 0x7F,
};

// Response opcodes are the request op | RESPONSE_FLAG; events sit at 0xC0+.
inline constexpr quint8 RESPONSE_FLAG = 0x80;
enum Evt : quint8 {
  EVT_PARAMS = 0xC0,
  EVT_ENGINE = 0xC1,
};

// Low 7 bits of the status byte; bit 7 is the continuation flag.
enum Status : quint8 {
  ST_OK          = 0,
  ST_MALFORMED   = 1,
  ST_UNKNOWN_OP  = 2,
  ST_BAD_ARG     = 3,
  ST_UNSUPPORTED = 4,
  ST_BUSY        = 5,
};
inline constexpr quint8 CONTINUATION_FLAG = 0x80;

enum Engine : quint8 {
  ENG_SUBTRACTIVE = 0,
  ENG_ADDITIVE    = 1,
  ENG_FM          = 2,
  ENG_WAVETABLE   = 3,
  // Kconfig-gated in firmware and therefore last, so a build without it just
  // has a shorter enum. Never assume it exists from this constant alone —
  // SynthController::graphEngineIndex (GRAPH_INFO) is what says whether the
  // connected firmware actually has it, and is the index to select.
  ENG_MODULAR     = 4,
};

// PARAM_INFO caps mask — which optional modules the active engine exposes, so
// the app can hide dead controls.
enum Caps : quint8 {
  CAP_FILTER    = 1 << 0,
  CAP_ENV2      = 1 << 1,
  CAP_LFO1      = 1 << 2,
  CAP_LFO2      = 1 << 3,
  CAP_MIXER     = 1 << 4,
  CAP_MODMATRIX = 1 << 5,
};

enum ParamType : quint8 { PT_FLOAT = 0, PT_INT = 1, PT_ENUM = 2, PT_BOOL = 3 };
enum ParamCurve : quint8 { PC_LINEAR = 0, PC_EXP = 1, PC_LOG = 2 };

// Sentinel id for PARAM_INFO "list all registered ids".
inline constexpr quint16 PARAM_INFO_LIST_ALL = 0xFFFF;

// Truncates `s` to at most `maxBytes` of UTF-8 *without splitting a code
// point*. A plain toUtf8().left(n) cuts on a byte boundary, so a name whose
// last character straddles the limit is stored as a half sequence — the
// firmware keeps the bytes verbatim and hands them back on the next listing,
// where QString::fromUtf8 turns them into U+FFFD. Names arrive here from a
// free-text field, so this is reachable with any accented character.
inline QByteArray utf8Clamped(const QString& s, int maxBytes) {
  QByteArray b = s.toUtf8();
  if (b.size() <= maxBytes) return b;
  // b[maxBytes] is a continuation byte (10xxxxxx) exactly when the cut would
  // land inside a sequence; walk back to that sequence's start and drop it.
  int cut = maxBytes;
  while (cut > 0 && (quint8(b.at(cut)) & 0xC0) == 0x80) --cut;
  b.truncate(cut);
  return b;
}

// --- little-endian append helpers ---------------------------------------
inline void appendU8(QByteArray& b, quint8 v) { b.append(char(v)); }

inline void appendU16(QByteArray& b, quint16 v) {
  char tmp[2];
  qToLittleEndian<quint16>(v, tmp);
  b.append(tmp, 2);
}

inline void appendU32(QByteArray& b, quint32 v) {
  char tmp[4];
  qToLittleEndian<quint32>(v, tmp);
  b.append(tmp, 4);
}

inline void appendF32(QByteArray& b, float v) {
  quint32 bits;
  std::memcpy(&bits, &v, 4);
  appendU32(b, bits);
}

// --- little-endian reader ------------------------------------------------
struct Reader {
  const quint8* p;
  int remaining;
  bool ok = true;

  explicit Reader(const QByteArray& a)
      : p(reinterpret_cast<const quint8*>(a.constData())), remaining(a.size()) {}

  quint8 u8() {
    if (remaining < 1) { ok = false; return 0; }
    quint8 v = p[0];
    p += 1; remaining -= 1;
    return v;
  }
  quint16 u16() {
    if (remaining < 2) { ok = false; return 0; }
    quint16 v = qFromLittleEndian<quint16>(p);
    p += 2; remaining -= 2;
    return v;
  }
  quint32 u32() {
    if (remaining < 4) { ok = false; return 0; }
    quint32 v = qFromLittleEndian<quint32>(p);
    p += 4; remaining -= 4;
    return v;
  }
  float f32() {
    quint32 bits = u32();
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
  }
  // Consumes up to `max` bytes of a NUL-terminated string (or until the end).
  QString cstr() {
    QByteArray out;
    while (remaining > 0) {
      quint8 c = p[0];
      p += 1; remaining -= 1;
      if (c == 0) break;
      out.append(char(c));
    }
    return QString::fromUtf8(out);
  }
};

// --- frame build (CTRL) --------------------------------------------------
// [u8 opcode][u8 seq][u16 len][payload]
inline QByteArray buildFrame(quint8 op, quint8 seq, const QByteArray& payload = {}) {
  QByteArray f;
  f.reserve(4 + payload.size());
  appendU8(f, op);
  appendU8(f, seq);
  appendU16(f, quint16(payload.size()));
  f.append(payload);
  return f;
}

// Command payload builders (return the payload, not the whole frame).
inline QByteArray payloadSetParam(quint16 id, float value) {
  QByteArray p;
  appendU16(p, id);
  appendF32(p, value);
  return p;
}
inline void appendSetParam(QByteArray& p, quint16 id, float value) {
  appendU16(p, id);
  appendF32(p, value);
}
inline QByteArray payloadGetParam(const QList<quint16>& ids) {
  QByteArray p;
  for (quint16 id : ids) appendU16(p, id);
  return p;
}
inline QByteArray payloadParamInfo(quint16 id) {
  QByteArray p;
  appendU16(p, id);
  return p;
}
inline QByteArray payloadSelectEngine(quint8 engine) {
  QByteArray p;
  appendU8(p, engine);
  return p;
}
inline QByteArray payloadLoadPreset(quint8 engine, quint8 slot) {
  QByteArray p;
  appendU8(p, engine);
  appendU8(p, slot);
  return p;
}
inline QByteArray payloadSavePreset(quint8 engine, quint8 slot, const QString& name) {
  QByteArray p;
  appendU8(p, engine);
  appendU8(p, slot);
  p.append(utf8Clamped(name, 23));  // rest of frame, 0..23 bytes
  return p;
}
inline QByteArray payloadListPresets(quint8 engine) {
  QByteArray p;
  appendU8(p, engine);
  return p;
}
// cmd: 0 stop / 1 play / 2 rec. tempo > 0 also writes seq.tempo.
inline QByteArray payloadTransport(quint8 cmd, float tempo = 0.0f) {
  QByteArray p;
  appendU8(p, cmd);
  if (tempo > 0.0f) appendF32(p, tempo);
  return p;
}
inline QByteArray payloadArp(quint8 enable, quint8 mode, quint8 octaves, quint8 division) {
  QByteArray p;
  appendU8(p, enable);
  appendU8(p, mode);
  appendU8(p, octaves);
  appendU8(p, division);
  return p;
}
inline QByteArray payloadNoteOn(quint8 note, quint8 velocity) {
  QByteArray p;
  appendU8(p, note);
  appendU8(p, velocity);
  return p;
}
inline QByteArray payloadNoteOff(quint8 note) {
  QByteArray p;
  appendU8(p, note);
  return p;
}

// --- frame parse (EVT: responses and events) -----------------------------
// [u8 opcode'][u8 seq][u16 len][u8 status][payload...]  (len counts status)
struct Frame {
  bool valid = false;
  quint8 op = 0;      // opcode' as received (response = req|0x80, event = 0xC0+)
  quint8 seq = 0;
  quint8 status = 0;  // low 7 bits
  bool continuation = false;
  QByteArray payload;  // bytes after the status byte

  // Events are the two literal opcodes 0xC0/0xC1 — NOT the range above 0xC0.
  // A response is `request | 0x80`, so OP_PING (0x7F) answers with 0xFF: a
  // `op >= 0xC0` test swallowed every PING reply as an unknown event and made
  // the OP_PING response case below dead code. Matching exact opcodes is also
  // what keeps this honest as the opcode map grows; only commands 0x40/0x41
  // are unusable, and neither is allocated.
  bool isEvent() const { return op == EVT_PARAMS || op == EVT_ENGINE; }
  bool isResponse() const { return (op & RESPONSE_FLAG) && !isEvent(); }
  quint8 requestOp() const { return op & ~RESPONSE_FLAG; }
};

inline Frame parseFrame(const QByteArray& a) {
  Frame f;
  if (a.size() < 5) return f;  // op + seq + len(2) + status
  Reader r(a);
  f.op = r.u8();
  f.seq = r.u8();
  const quint16 len = r.u16();
  const quint8 rawStatus = r.u8();
  f.continuation = (rawStatus & CONTINUATION_FLAG) != 0;
  f.status = rawStatus & 0x7F;
  // len counts the status byte + payload; payload is len-1 bytes.
  if (len < 1) return f;
  const int payloadLen = len - 1;
  if (r.remaining < payloadLen) return f;  // truncated
  f.payload = QByteArray(reinterpret_cast<const char*>(r.p), payloadLen);
  f.valid = true;
  return f;
}

// --- decoded payload structures -----------------------------------------
struct ParamValue {
  quint16 id;
  float value;
};

// GET_PARAM response / EVT_PARAMS payload: n × { u16 id, f32 value }.
inline QList<ParamValue> parseParamValues(const QByteArray& payload) {
  QList<ParamValue> out;
  Reader r(payload);
  while (r.remaining >= 6) {
    ParamValue pv;
    pv.id = r.u16();
    pv.value = r.f32();
    if (!r.ok) break;
    out.append(pv);
  }
  return out;
}

// PARAM_INFO list response payload: [u8 engine][u8 caps] + n × u16 id.
struct ParamInfoList {
  quint8 engine = 0;
  quint8 caps = 0;
  QList<quint16> ids;
};
inline ParamInfoList parseParamInfoList(const QByteArray& payload) {
  ParamInfoList out;
  Reader r(payload);
  out.engine = r.u8();
  out.caps = r.u8();
  while (r.remaining >= 2) {
    quint16 id = r.u16();
    if (!r.ok) break;
    out.ids.append(id);
  }
  return out;
}

// PARAM_INFO single response payload:
// [u16 id][u8 type][u8 curve][u8 enum_count][f32 min][f32 max][f32 def]
// [name NUL][enum_count × NUL-terminated names]
struct ParamInfo {
  bool valid = false;
  quint16 id = 0;
  quint8 type = 0;
  quint8 curve = 0;
  float min = 0, max = 1, def = 0;
  QString name;
  QStringList enumNames;
};
inline ParamInfo parseParamInfo(const QByteArray& payload) {
  ParamInfo out;
  Reader r(payload);
  out.id = r.u16();
  out.type = r.u8();
  out.curve = r.u8();
  const quint8 enumCount = r.u8();
  out.min = r.f32();
  out.max = r.f32();
  out.def = r.f32();
  if (!r.ok) return out;
  out.name = r.cstr();
  for (quint8 i = 0; i < enumCount; ++i) out.enumNames.append(r.cstr());
  out.valid = true;
  return out;
}

// LIST_PRESETS response: prefix [u8 engine] then n × { u8 slot, u8 flags, char[24] name }.
struct PresetEntry {
  quint8 slot;
  bool factory;
  QString name;
};
struct PresetList {
  quint8 engine = 0;
  QList<PresetEntry> entries;
};
inline PresetList parsePresetList(const QByteArray& payload) {
  PresetList out;
  Reader r(payload);
  out.engine = r.u8();
  while (r.remaining >= 2 + 24) {
    PresetEntry e;
    e.slot = r.u8();
    const quint8 flags = r.u8();
    e.factory = (flags & 0x01) != 0;
    // Fixed 24-byte name field (NUL-padded).
    QByteArray raw(reinterpret_cast<const char*>(r.p), 24);
    r.p += 24;
    r.remaining -= 24;
    int nul = raw.indexOf('\0');
    if (nul >= 0) raw.truncate(nul);
    e.name = QString::fromUtf8(raw);
    out.entries.append(e);
  }
  return out;
}

// INFO characteristic read:
// [u8 proto_ver][u8 fw_major][u8 fw_minor][u8 fw_patch][target NUL-terminated]
struct DeviceInfo {
  bool valid = false;
  quint8 protoVersion = 0;
  quint8 fwMajor = 0, fwMinor = 0, fwPatch = 0;
  QString target;  // "esp32s3" | "esp32"
};
inline DeviceInfo parseInfo(const QByteArray& a) {
  DeviceInfo out;
  Reader r(a);
  out.protoVersion = r.u8();
  out.fwMajor = r.u8();
  out.fwMinor = r.u8();
  out.fwPatch = r.u8();
  if (!r.ok) return out;
  out.target = r.cstr();
  out.valid = true;
  return out;
}

// --- sequencer (S23) -----------------------------------------------------

// One step, wire layout (8 bytes, no padding — see the static_asserts in the
// firmware's ble_ctrl.cpp). `vel == 0` is the empty/rest encoding.
struct SeqStep {
  quint8 note = 60;
  quint8 vel = 0;
  quint8 gate = 16;   // 1/16 of a step; 16 = exactly one step
  quint8 prob = 100;  // %
  qint8 micro = 0;    // % of a step, -50..+50
  quint8 ratchet = 1; // 1..8
  quint8 cond = 0;    // SeqCond
  quint8 flags = 0;   // SeqStepFlag

  bool filled() const { return vel != 0; }
};

enum SeqStepFlag : quint8 {
  SF_ACCENT = 1 << 0,
  SF_SLIDE  = 1 << 1,
  SF_MUTE   = 1 << 2,
  SF_PLOCK  = 1 << 3,
};

// Trig conditions, in firmware order (seq_model.h). The labels are what the
// UI shows; keep both in step with the firmware enum.
inline QStringList seqCondNames() {
  return {"always", "1:2", "2:2", "1:3", "2:3", "3:3", "1:4", "2:4", "3:4",
          "4:4",    "1:8", "2:8", "4:8", "8:8", "fill", "!fill", "prev",
          "!prev"};
}
inline QStringList seqDivNames() {
  return {"1/1", "1/2", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32",
          "1/32T"};
}
inline QStringList seqDirNames() {
  return {"forward", "reverse", "ping-pong", "random", "brownian"};
}
inline QStringList seqTargetNames() { return {"synth", "drums"}; }

// Per-track configuration, wire layout (16 bytes).
struct SeqTrackCfg {
  quint8 target = 0;
  quint8 slot = 0;       // drum slot, or 0xFF = the step's note picks it
  quint16 length = 64;
  quint8 div = 6;        // 1/16
  quint8 dir = 0;
  qint8 transpose = 0;
  quint8 swing = 0xFF;   // 0xFF = follow the pattern
  quint8 gateScale = 100;
  quint8 velScale = 100;
  quint8 probScale = 100;
  quint8 humanize = 0;
  quint8 flags = 0;
  quint8 scale = 0xFF;   // 0xFF = follow the pattern
  quint8 root = 0xFF;
  quint8 reserved = 0;
};

inline constexpr quint8 SEQ_SLOT_FROM_NOTE = 0xFF;

inline void appendStep(QByteArray& b, const SeqStep& s) {
  b.append(char(s.note));
  b.append(char(s.vel));
  b.append(char(s.gate));
  b.append(char(s.prob));
  b.append(char(quint8(s.micro)));
  b.append(char(s.ratchet));
  b.append(char(s.cond));
  b.append(char(s.flags));
}

inline SeqStep readStep(Reader& r) {
  SeqStep s;
  s.note = r.u8();
  s.vel = r.u8();
  s.gate = r.u8();
  s.prob = r.u8();
  s.micro = qint8(r.u8());
  s.ratchet = r.u8();
  s.cond = r.u8();
  s.flags = r.u8();
  return s;
}

inline void appendTrackCfg(QByteArray& b, const SeqTrackCfg& c) {
  b.append(char(c.target));
  b.append(char(c.slot));
  appendU16(b, c.length);
  b.append(char(c.div));
  b.append(char(c.dir));
  b.append(char(quint8(c.transpose)));
  b.append(char(c.swing));
  b.append(char(c.gateScale));
  b.append(char(c.velScale));
  b.append(char(c.probScale));
  b.append(char(c.humanize));
  b.append(char(c.flags));
  b.append(char(c.scale));
  b.append(char(c.root));
  b.append(char(c.reserved));
}

inline SeqTrackCfg readTrackCfg(Reader& r) {
  SeqTrackCfg c;
  c.target = r.u8();
  c.slot = r.u8();
  c.length = r.u16();
  c.div = r.u8();
  c.dir = r.u8();
  c.transpose = qint8(r.u8());
  c.swing = r.u8();
  c.gateScale = r.u8();
  c.velScale = r.u8();
  c.probScale = r.u8();
  c.humanize = r.u8();
  c.flags = r.u8();
  c.scale = r.u8();
  c.root = r.u8();
  c.reserved = r.u8();
  return c;
}

// SEQ_INFO response: the firmware's compile-time sizing, so the UI never
// assumes 8 tracks on a build that has 4.
struct SeqInfo {
  bool valid = false;
  int tracks = 0;
  int patterns = 0;
  int maxSteps = 0;
  int defaultSteps = 0;
  int songLength = 0;
  int plockCapacity = 0;
  int plockUsed = 0;
  int scaleCount = 0;
  int condCount = 0;
  int divCount = 0;
  int targetCount = 0;
  int dirCount = 0;
  bool modelReady = false;
};

inline SeqInfo parseSeqInfo(const QByteArray& payload) {
  SeqInfo i;
  Reader r(payload);
  i.tracks = r.u8();
  i.patterns = r.u8();
  i.maxSteps = r.u16();
  i.defaultSteps = r.u16();
  i.songLength = r.u8();
  i.plockCapacity = r.u16();
  i.plockUsed = r.u16();
  i.scaleCount = r.u8();
  i.condCount = r.u8();
  i.divCount = r.u8();
  i.targetCount = r.u8();
  i.dirCount = r.u8();
  i.modelReady = r.u8() != 0;
  i.valid = r.ok;
  return i;
}

// --- sequencer payload builders -----------------------------------------
inline QByteArray payloadSeqGetSteps(int pattern, int track, int first, int count) {
  QByteArray p;
  appendU8(p, 0);
  appendU8(p, quint8(pattern));
  appendU8(p, quint8(track));
  appendU16(p, quint16(first));
  appendU8(p, quint8(count));
  return p;
}
inline QByteArray payloadSeqSetSteps(int pattern, int track, int first,
                                     const QList<SeqStep>& steps) {
  QByteArray p;
  appendU8(p, 1);
  appendU8(p, quint8(pattern));
  appendU8(p, quint8(track));
  appendU16(p, quint16(first));
  appendU8(p, quint8(steps.size()));
  for (const SeqStep& s : steps) appendStep(p, s);
  return p;
}
inline QByteArray payloadSeqGetTrack(int pattern, int track) {
  QByteArray p;
  appendU8(p, 0);
  appendU8(p, quint8(pattern));
  appendU8(p, quint8(track));
  return p;
}
inline QByteArray payloadSeqSetTrack(int pattern, int track, const SeqTrackCfg& c) {
  QByteArray p;
  appendU8(p, 1);
  appendU8(p, quint8(pattern));
  appendU8(p, quint8(track));
  appendTrackCfg(p, c);
  return p;
}
inline QByteArray payloadSeqGetPattern(int pattern) {
  QByteArray p;
  appendU8(p, 0);
  appendU8(p, quint8(pattern));
  return p;
}
inline QByteArray payloadSeqSetPattern(int pattern, int length, int scale, int root,
                                       int swing, const QString& name) {
  QByteArray p;
  appendU8(p, 1);
  appendU8(p, quint8(pattern));
  appendU16(p, quint16(length));
  appendU8(p, quint8(scale));
  appendU8(p, quint8(root));
  appendU8(p, quint8(swing));
  p.append(utf8Clamped(name, 11));
  return p;
}
// Plock sub-ops: 1 set / 2 clear-step / 3 clear-pattern / 4 list-pattern.
// Sub-op 0 (list one step) has no builder: the grid always loads the whole
// pattern's locks in one request (sub-op 4) and serves single steps from that
// cache, so a per-step request would only be a slower way to learn the same
// thing.
inline QByteArray payloadSeqPlockSet(int pattern, int track, int step, int pid,
                                     float value) {
  QByteArray p;
  appendU8(p, 1);
  appendU8(p, quint8(pattern));
  appendU8(p, quint8(track));
  appendU16(p, quint16(step));
  appendU16(p, quint16(pid));
  appendF32(p, value);
  return p;
}
inline QByteArray payloadSeqPlockClearStep(int pattern, int track, int step) {
  QByteArray p;
  appendU8(p, 2);
  appendU8(p, quint8(pattern));
  appendU8(p, quint8(track));
  appendU16(p, quint16(step));
  return p;
}
inline QByteArray payloadSeqPlockClearPattern(int pattern) {
  QByteArray p;
  appendU8(p, 3);
  appendU8(p, quint8(pattern));
  return p;
}
inline QByteArray payloadSeqPlockListPattern(int pattern) {
  QByteArray p;
  appendU8(p, 4);
  appendU8(p, quint8(pattern));
  return p;
}
// Edit ops, matching the firmware's handle_seq_edit switch.
inline QByteArray payloadSeqClearPattern(int pattern) {
  QByteArray p; appendU8(p, 0); appendU8(p, quint8(pattern)); return p;
}
inline QByteArray payloadSeqClearTrack(int pattern, int track) {
  QByteArray p; appendU8(p, 1); appendU8(p, quint8(pattern)); appendU8(p, quint8(track)); return p;
}
inline QByteArray payloadSeqCopyPattern(int src, int dst) {
  QByteArray p; appendU8(p, 2); appendU8(p, quint8(src)); appendU8(p, quint8(dst)); return p;
}
inline QByteArray payloadSeqRotate(int pattern, int track, int delta) {
  QByteArray p; appendU8(p, 3); appendU8(p, quint8(pattern)); appendU8(p, quint8(track));
  appendU8(p, quint8(qint8(delta))); return p;
}
inline QByteArray payloadSeqEuclid(int pattern, int track, int pulses, int steps,
                                   int rotate, int note, int vel) {
  QByteArray p;
  appendU8(p, 4);
  appendU8(p, quint8(pattern));
  appendU8(p, quint8(track));
  appendU8(p, quint8(pulses));
  appendU16(p, quint16(steps));
  appendU8(p, quint8(qint8(rotate)));
  appendU8(p, quint8(note));
  appendU8(p, quint8(vel));
  return p;
}
inline QByteArray payloadSeqHumanize(int pattern, int track, int amount) {
  QByteArray p; appendU8(p, 5); appendU8(p, quint8(pattern)); appendU8(p, quint8(track));
  appendU8(p, quint8(amount)); return p;
}
inline QByteArray payloadSeqToggle(int pattern, int track, int step, int note) {
  QByteArray p;
  appendU8(p, 6);
  appendU8(p, quint8(pattern));
  appendU8(p, quint8(track));
  appendU16(p, quint16(step));
  appendU8(p, quint8(note));
  return p;
}
inline QByteArray payloadSeqSongGet() {
  QByteArray p; appendU8(p, 0); return p;
}
inline QByteArray payloadSeqSongSet(const QList<QPair<int, int>>& chain) {
  QByteArray p;
  appendU8(p, 1);
  appendU8(p, quint8(chain.size()));
  for (const auto& e : chain) {
    appendU8(p, quint8(e.first));
    appendU8(p, quint8(e.second));
  }
  return p;
}
// what: 0 = the selectable kits, 1 = the current kit's slots
inline QByteArray payloadKitInfo(int what) {
  QByteArray p; appendU8(p, quint8(what)); return p;
}
inline QByteArray payloadDrumTrig(int slot, int velocity) {
  QByteArray p;
  appendU8(p, quint8(slot));
  appendU8(p, quint8(velocity));
  return p;
}

struct KitEntry {
  int index = 0;
  QString name;
};
struct KitSlot {
  int slot = 0;
  int note = 0;
  QString name;
};

// ---- modular patch graph (S28) --------------------------------------------

inline QByteArray payloadGraphKind(int kind) {
  QByteArray p; appendU8(p, quint8(kind)); return p;
}
inline QByteArray payloadGraphNodesGet() {
  QByteArray p; appendU8(p, 0); return p;
}
inline QByteArray payloadGraphSetKind(int slot, int kind) {
  QByteArray p;
  appendU8(p, 0);  // cmd: set kind
  appendU8(p, quint8(slot));
  appendU8(p, quint8(kind));
  return p;
}
// src < 0 disconnects — the firmware reads 0xFF as "unpatched".
inline QByteArray payloadGraphConnect(int dst, int port, int src) {
  QByteArray p;
  appendU8(p, 1);  // cmd: connect
  appendU8(p, quint8(dst));
  appendU8(p, quint8(port));
  appendU8(p, quint8(src < 0 ? 0xFF : src));
  return p;
}
inline QByteArray payloadGraphSetPos(int slot, int x, int y) {
  QByteArray p;
  appendU8(p, 2);  // cmd: canvas position
  appendU8(p, quint8(slot));
  appendU16(p, quint16(qint16(x)));
  appendU16(p, quint16(qint16(y)));
  return p;
}

struct GraphInfo {
  bool valid = false;
  int maxNodes = 0;
  int paramsPerNode = 0;
  int maxInputs = 0;
  int outSlot = 0;
  int kindCount = 0;
  int costBudget = 0;
  int liveCost = 0;
  int revision = 0;
  int engineIndex = -1;
};
inline GraphInfo parseGraphInfo(const QByteArray& payload) {
  Reader r(payload);
  GraphInfo g;
  g.maxNodes = r.u8();
  g.paramsPerNode = r.u8();
  g.maxInputs = r.u8();
  g.outSlot = r.u8();
  g.kindCount = r.u8();
  g.costBudget = r.u16();
  g.liveCost = r.u16();
  g.revision = r.u16();
  g.engineIndex = r.u8();
  g.valid = r.ok && g.maxNodes > 0 && g.kindCount > 0;
  return g;
}

struct GraphKind {
  bool valid = false;
  int kind = 0;
  int rate = 0;  // 0 = control, 1 = audio
  int cost = 0;
  QString name;
  QStringList inputs;   // port names, in port order
  QStringList params;   // parameter name suffixes, in id order
};
inline GraphKind parseGraphKind(const QByteArray& payload) {
  Reader r(payload);
  GraphKind k;
  k.kind = r.u8();
  k.rate = r.u8();
  const int nIn = r.u8();
  const int nParam = r.u8();
  k.cost = r.u16();
  k.name = r.cstr();
  for (int i = 0; i < nIn && r.ok; ++i) k.inputs.append(r.cstr());
  for (int i = 0; i < nParam && r.ok; ++i) k.params.append(r.cstr());
  k.valid = r.ok && !k.name.isEmpty();
  return k;
}

struct GraphNode {
  int kind = 0;
  QList<int> in;  // source slot per port, -1 = unpatched
  int x = 0;
  int y = 0;
};
struct GraphModel {
  bool valid = false;
  int revision = 0;
  QList<GraphNode> nodes;
};
// Matches graph_model.cpp's v1 blob exactly — the same bytes a version-2
// preset file stores, which is what keeps the wire and the file from
// drifting apart.
inline GraphModel parseGraphModel(const QByteArray& payload) {
  Reader r(payload);
  GraphModel m;
  if (r.u32() != 0x3152474Fu) return m;  // 'OGR1'
  if (r.u8() != 1) return m;             // blob version
  const int count = r.u8();
  m.revision = int(r.u32());
  for (int i = 0; i < count && r.ok; ++i) {
    GraphNode n;
    n.kind = r.u8();
    for (int k = 0; k < 4; ++k) n.in.append(qint8(r.u8()));
    n.x = qint16(r.u16());
    n.y = qint16(r.u16());
    m.nodes.append(n);
  }
  m.valid = r.ok && !m.nodes.isEmpty();
  return m;
}

// GRAPH_EDIT response: { u16 revision, u16 cost } — sent on failure too, so
// a rejected edit still tells the app exactly where it stands.
struct GraphEditReply {
  int revision = 0;
  int cost = 0;
};
inline GraphEditReply parseGraphEditReply(const QByteArray& payload) {
  Reader r(payload);
  GraphEditReply e;
  e.revision = r.u16();
  e.cost = r.u16();
  return e;
}

// EVT_ENGINE payload: { u8 engine, u8 caps }.
struct EngineEvent {
  quint8 engine;
  quint8 caps;
};
inline EngineEvent parseEngineEvent(const QByteArray& payload) {
  Reader r(payload);
  EngineEvent e{};
  e.engine = r.u8();
  e.caps = r.u8();
  return e;
}

}  // namespace SynthProto

#endif  // SYNTHPROTOCOL_H
