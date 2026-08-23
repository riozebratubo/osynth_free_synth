#include "src/qmlforeign.h"

#include <QtQml/qqmlengine.h>
#include <QtSystemDetection>

#include "src/app.h"

#ifdef Q_OS_WINDOWS
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
  IBluetoothManager* manager = &BluetoothManager::instance();
  QJSEngine::setObjectOwnership(manager, QJSEngine::CppOwnership);
  return manager;
}
