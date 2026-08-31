#ifndef GRAPHTYPES_H
#define GRAPHTYPES_H

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

// Modular patch graph value types (S28), replacing the QVariantMaps that
// graphNodes and graphKinds used to carry. The reason is the one src/
// paramtypes.h gives: a map field read is an untyped hash lookup whose result
// type is unknown, so every binding touching one fell back to interpreted byte
// code and took the surrounding expression with it. On this page that was the
// whole canvas — node position, colour, jack count, cable routing, all of it.
//
// It stopped being only a speed question on Linux, where one of GraphScreen's
// generated bindings segfaulted inside QMetaObject::indexOfProperty: the AOT
// code resolves a value type by name at run time and dereferences the result
// without checking it. Types the compiler can see all the way through are what
// keeps it off that path.
//
// Named apart from SynthProto::GraphNode and SynthProto::GraphKind on purpose.
// Those are the wire forms and synthcontroller.cpp does `using namespace
// SynthProto`, so a second GraphNode in scope there is an ambiguity rather than
// a convenience. The same split already exists twice: ParamMeta beside
// SynthProto::ParamInfo, TrackConfig beside SynthProto::SeqTrackCfg.

// One slot of the live model: what occupies it, what is patched into it, and
// where the canvas draws it. Every slot appears, empty ones included, so the
// list stays addressable as graphNodes[slot].
class GraphSlot {
  Q_GADGET
  QML_VALUE_TYPE(graphSlot)

  // False only for a slot outside the model — what nodeAt() answers for an
  // out-of-range index. A slot of the live model is valid whether or not
  // anything is in it; `kind === 0` is what empty means. Modelled on
  // ParamMeta::exists, so QML reads the fields off a default-constructed value
  // instead of testing for null first.
  Q_PROPERTY(bool valid MEMBER valid FINAL)
  Q_PROPERTY(int slot MEMBER slot FINAL)
  // Index into the kind table; 0 is the empty kind.
  Q_PROPERTY(int kind MEMBER kind FINAL)
  // Source slot per input port, -1 where nothing is patched. Not called `in`
  // as the wire form is: `in` is a JavaScript operator, and a property only
  // ever reachable as `obj.in` is one to keep out of compiled bindings.
  Q_PROPERTY(QList<int> sources MEMBER sources FINAL)
  // Canvas coordinates. The firmware stores them, so they mean the same thing
  // on every device that opens the patch.
  Q_PROPERTY(int x MEMBER x FINAL)
  Q_PROPERTY(int y MEMBER y FINAL)

 public:
  bool valid = false;
  int slot = 0;
  int kind = 0;
  QList<int> sources;
  int x = 0;
  int y = 0;
};

// One entry of the kind table: what a node of this kind is called, what it
// costs, and the ports and parameters it declares. Build-constant, so it is
// read once per connection.
class GraphKindDesc {
  Q_GADGET
  QML_VALUE_TYPE(graphKindDesc)

  // False for a kind the table has no entry for yet. GRAPH_KIND replies
  // arrive out of order and the list is grown to stay indexable by kind, so
  // the gaps in between are real and the page draws them rather than crashing
  // on them.
  Q_PROPERTY(bool valid MEMBER valid FINAL)
  Q_PROPERTY(int kind MEMBER kind FINAL)
  Q_PROPERTY(QString name MEMBER name FINAL)
  // 0 = control, 1 = audio. What decides a cable's colour on the canvas.
  Q_PROPERTY(int rate MEMBER rate FINAL)
  Q_PROPERTY(int cost MEMBER cost FINAL)
  // Port names in port order; the count is how many input jacks to draw.
  Q_PROPERTY(QStringList inputs MEMBER inputs FINAL)
  // Parameter name suffixes in id order; the count is how many of the slot's
  // positional ids this kind actually uses (see graphNodeParamId).
  Q_PROPERTY(QStringList params MEMBER params FINAL)

 public:
  bool valid = false;
  int kind = 0;
  QString name;
  int rate = 0;
  int cost = 0;
  QStringList inputs;
  QStringList params;
};

Q_DECLARE_METATYPE(GraphSlot)
Q_DECLARE_METATYPE(GraphKindDesc)

#endif  // GRAPHTYPES_H
