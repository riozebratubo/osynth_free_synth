#ifndef SEQTYPES_H
#define SEQTYPES_H

#include <QMetaType>
#include <QString>
#include <QtQml/qqmlregistration.h>

// Sequencer value types handed to QML, replacing the QVariantMaps that
// trackConfig() and patternConfig() used to build. See src/paramtypes.h for
// why: a QVariantMap field read is an untyped hash lookup, so every binding
// touching one fell back to interpreted byte code.
//
// The defaults below deliberately mirror SynthProto::SeqTrackCfg and
// SynthController's pattern members, because QML used to spell those same
// defaults out by hand — `cfg.length !== undefined ? cfg.length : 64` and
// seventeen more like it. Those guards only existed because the property
// started life as an empty object; with a default-constructed gadget the field
// is already the value the guard was substituting, so the guards are gone and
// the defaults here are what keeps that true.

// One track's configuration, as SEQ_TRACK reports it.
class TrackConfig {
  Q_GADGET
  QML_VALUE_TYPE(trackConfig)

  Q_PROPERTY(int target MEMBER target FINAL)
  // Drum slot, or SEQ_SLOT_FROM_NOTE (0xFF) when the step's note picks it.
  Q_PROPERTY(int slot MEMBER slot FINAL)
  Q_PROPERTY(int length MEMBER length FINAL)
  Q_PROPERTY(int div MEMBER div FINAL)
  Q_PROPERTY(int dir MEMBER dir FINAL)
  Q_PROPERTY(int transpose MEMBER transpose FINAL)
  // 0xFF means "follow the pattern"; followsPatternSwing says so directly.
  Q_PROPERTY(int swing MEMBER swing FINAL)
  Q_PROPERTY(int gateScale MEMBER gateScale FINAL)
  Q_PROPERTY(int velScale MEMBER velScale FINAL)
  Q_PROPERTY(int probScale MEMBER probScale FINAL)
  Q_PROPERTY(int humanize MEMBER humanize FINAL)
  Q_PROPERTY(int scale MEMBER scale FINAL)
  Q_PROPERTY(int root MEMBER root FINAL)
  // Chord mode expands this track's notes (SEQ_TRACK_F_CHORD, S41). A bool
  // rather than the raw `flags` byte, because the other two bits in it are
  // mute and solo — parameters the firmware owns, which a client must not
  // send back through SEQ_TRACK. Exposing only this one makes that
  // impossible rather than merely documented.
  Q_PROPERTY(bool chord MEMBER chord FINAL)
  // Derived on the C++ side so QML never has to know 0xFF is the sentinel.
  Q_PROPERTY(bool followsPatternSwing MEMBER followsPatternSwing FINAL)
  Q_PROPERTY(bool followsPatternScale MEMBER followsPatternScale FINAL)
  Q_PROPERTY(bool noteToSlot MEMBER noteToSlot FINAL)

 public:
  int target = 0;
  int slot = 0;
  int length = 64;
  int div = 6;
  int dir = 0;
  int transpose = 0;
  int swing = 0xFF;
  int gateScale = 100;
  int velScale = 100;
  int probScale = 100;
  int humanize = 0;
  int scale = 0xFF;
  int root = 0xFF;
  bool chord = false;
  bool followsPatternSwing = true;
  bool followsPatternScale = true;
  bool noteToSlot = false;
};

// The edited pattern's own settings, which a track can defer to.
class PatternConfig {
  Q_GADGET
  QML_VALUE_TYPE(patternConfig)

  Q_PROPERTY(int length MEMBER length FINAL)
  Q_PROPERTY(int scale MEMBER scale FINAL)
  Q_PROPERTY(int root MEMBER root FINAL)
  Q_PROPERTY(int swing MEMBER swing FINAL)
  Q_PROPERTY(QString name MEMBER name FINAL)

 public:
  int length = 64;
  int scale = 0;
  int root = 0;
  int swing = 50;
  QString name;
};

Q_DECLARE_METATYPE(TrackConfig)
Q_DECLARE_METATYPE(PatternConfig)

#endif  // SEQTYPES_H
