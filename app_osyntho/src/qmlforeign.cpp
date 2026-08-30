#include "src/qmlforeign.h"

#include <QtQml/qqmlengine.h>
#include <QtSystemDetection>

#include "src/app.h"

#ifdef OSYNTHO_EMBEDDED
#include "src/embeddedmanager.h"
#elif defined(Q_OS_WINDOWS)
#include "src/bluetoothmanager2.h"
#else
#include "src/bluetoothmanager.h"
#endif

// Both singletons outlive the engine — they are process-wide and owned by App
// (or by their own instance()) — so ownership stays in C++. Without that the
// engine adopts the object and deletes it at teardown, taking the live BLE
// link or the parameter table with it.

SynthController* SynthForeign::create(QQmlEngine*, QJSEngine*) {
  SynthController* synth = &App::instance().getSynth();
  QJSEngine::setObjectOwnership(synth, QJSEngine::CppOwnership);
  return synth;
}

IBluetoothManager* BluetoothManagerForeign::create(QQmlEngine*, QJSEngine*) {
  // The one place the transport is chosen. QML binds to IBluetoothManager and
  // never learns which of the three it got -- Qt Bluetooth, SimpleBLE, or the
  // engine compiled into this process.
#ifdef OSYNTHO_EMBEDDED
  IBluetoothManager* manager = &EmbeddedManager::instance();
#else
  IBluetoothManager* manager = &BluetoothManager::instance();
#endif
  QJSEngine::setObjectOwnership(manager, QJSEngine::CppOwnership);
  return manager;
}
