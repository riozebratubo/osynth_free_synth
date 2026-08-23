#ifndef QMLFOREIGN_H
#define QMLFOREIGN_H

#include <QtQml/qqmlregistration.h>

#include "src/ibluetoothmanager.h"
#include "src/synthcontroller.h"

QT_BEGIN_NAMESPACE
class QQmlEngine;
class QJSEngine;
QT_END_NAMESPACE

// QML singleton registrations that cannot live on the classes themselves.
//
// Both are here for the same underlying reason: QQmlPrivate::
// singletonConstructionMode() picks how to make the singleton, and it tests in
// this order --
//
//     FactoryWrapper   (a *foreign* type with a create())
//     Constructor      (T is default-constructible)
//     Factory          (T itself has a create())
//
// so a default-constructible type never reaches its own create(). Registering
// through a foreign wrapper is the only way to force the factory when T can
// still be default-constructed.

// SynthController is default-constructible -- App holds one as a member, and
// its constructor takes an optional parent. Registered directly it therefore
// landed in Constructor mode, and QML's `Synth` would have been a *second*,
// freshly built controller: no BLE link, no parameter table, every binding
// reading a disconnected object. It also forced QQmlElement<SynthController>,
// which does not compile against a `final` class.
//
// The wrapper hands QML the live controller App owns, and keeps the class
// final.
struct SynthForeign {
  Q_GADGET
  QML_FOREIGN(SynthController)
  QML_NAMED_ELEMENT(Synth)
  QML_SINGLETON

 public:
  static SynthController* create(QQmlEngine*, QJSEngine*);
};

// IBluetoothManager is abstract, and deliberately knows nothing about the
// backends behind it — Qt Bluetooth on Android/Linux/macOS, SimpleBLE on
// Windows, both spelled `BluetoothManager` in their own headers. A
// QML_SINGLETON needs a create() that names a concrete instance, and putting
// one on the interface would point it straight at the implementation it exists
// to hide.
//
// QML sees the interface — which is where every Q_PROPERTY and Q_INVOKABLE the
// UI binds to is declared — and the platform switch stays in one .cpp.
struct BluetoothManagerForeign {
  Q_GADGET
  QML_FOREIGN(IBluetoothManager)
  QML_NAMED_ELEMENT(BluetoothManager)
  QML_SINGLETON

 public:
  static IBluetoothManager* create(QQmlEngine*, QJSEngine*);
};

#endif  // QMLFOREIGN_H
