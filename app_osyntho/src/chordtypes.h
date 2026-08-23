#ifndef CHORDTYPES_H
#define CHORDTYPES_H

#include <QList>
#include <QMetaType>
#include <QString>
#include <QtQml/qqmlregistration.h>

// One slot of the user chord set (S41), as CHORD_SET reports it — a value
// type rather than a QVariantMap for the reason src/paramtypes.h gives: a map
// field read is an untyped hash lookup, and every binding touching one falls
// back to interpreted byte code instead of being compiled by qmlcachegen.
//
// The wire form is SynthProto::ChordUserSlot (eight bytes). This is what the
// editor binds to, so it carries two things the wire does not: the intervals
// as a proper list rather than a fixed array with a separate count, and the
// name the page prints. Both are derived on the C++ side, which is what keeps
// the QML declarative.
class ChordSlot {
  Q_GADGET
  QML_VALUE_TYPE(chordSlot)

  // Semitones added to the played key before the intervals are stacked. What
  // makes a slot able to say "this key plays the chord a fourth above".
  Q_PROPERTY(int transpose MEMBER transpose FINAL)
  // Semitones above the transposed key. Empty means the slot is silent —
  // a legitimate entry, and how a five-chord set stays quiet on the seven
  // keys it does not use.
  Q_PROPERTY(QList<int> intervals MEMBER intervals FINAL)
  Q_PROPERTY(bool silent READ silent FINAL)
  // "maj7", "m7b5", or the interval list when nothing standard fits.
  Q_PROPERTY(QString label MEMBER label FINAL)

 public:
  int transpose = 0;
  QList<int> intervals;
  QString label;

  bool silent() const { return intervals.isEmpty(); }
};

Q_DECLARE_METATYPE(ChordSlot)

#endif  // CHORDTYPES_H
