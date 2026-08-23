#ifndef PARAMTYPES_H
#define PARAMTYPES_H

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

// Value types the parameter API hands to QML.
//
// These used to be QVariantMap. That reads the same from QML but is opaque to
// the compiler: `meta.exists` on a QVariantMap is a run-time hash lookup whose
// result type is unknown, so every binding that touched one fell back to
// interpreted byte code — and took the surrounding expression with it. As a
// Q_GADGET the fields have real types and the whole binding compiles.

// One parameter's discovered metadata: what PARAM_INFO reported for an id.
//
// `exists` is false both for an id the synth never registered and for one whose
// PARAM_INFO has not arrived yet, which is why every consumer tests it before
// reading anything else. A default-constructed ParamMeta is exactly that "not
// known" answer, so SynthController::paramMeta() can return one by value for a
// bad id and QML needs no null check.
class ParamMeta {
  Q_GADGET
  QML_VALUE_TYPE(paramMeta)

  Q_PROPERTY(bool exists MEMBER exists FINAL)
  Q_PROPERTY(int id MEMBER id FINAL)
  Q_PROPERTY(QString name MEMBER name FINAL)
  // SynthProto::ParamType — 0 float, 1 int, 2 enum, 3 bool.
  Q_PROPERTY(int type MEMBER type FINAL)
  // SynthProto::ParamCurve — 0 linear, 1 exponential, 2 logarithmic.
  Q_PROPERTY(int curve MEMBER curve FINAL)
  Q_PROPERTY(qreal min MEMBER min FINAL)
  Q_PROPERTY(qreal max MEMBER max FINAL)
  Q_PROPERTY(qreal def MEMBER def FINAL)
  // Labels for an enum parameter, in value order; empty for every other type.
  Q_PROPERTY(QStringList enumNames MEMBER enumNames FINAL)

 public:
  bool exists = false;
  int id = 0;
  QString name;
  int type = 0;
  int curve = 0;
  // qreal, not float: QML numbers are doubles, and declaring these float made
  // every read a narrowing conversion the compiler has to guard.
  qreal min = 0;
  qreal max = 1;
  qreal def = 0;
  QStringList enumNames;
};

Q_DECLARE_METATYPE(ParamMeta)

#endif  // PARAMTYPES_H
