#include "app.h"

#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QStandardPaths>
#include <QSysInfo>
#include <QVariant>

#include "src/apputils.h"
#include "src/nativefiledialog.h"
#include "src/translator.h"

#if QT_CONFIG(permissions)
#include <QPermissions>
#endif

#ifdef Q_OS_WINDOWS
#include <windows.h>  // SetThreadExecutionState
#endif

#ifdef Q_OS_ANDROID
#include <QScopeGuard>
#include <QtCore/private/qandroidextras_p.h>
#include <QtCore/qjniobject.h>

// FileProvider authority — must match assets/android-build/AndroidManifest.xml.
static constexpr const char* kFileProviderAuthority = "org.osynth.osyntho.qtprovider";

static QString mimeTypeForExtension(const QString& ext) {
  if (ext == "zip") return "application/zip";
  if (ext == "db") return "application/octet-stream";
  if (ext == "txt") return "text/plain";
  return "application/octet-stream";
}
#endif

constexpr const char* globalFirmwareFileName = "firmware";

// Is the focused object something the user is typing into?
//
// The whole class chain is walked, not just the leaf: a QML TextField focuses a
// QQuickTextField and a TextArea a QQuickTextArea, and neither of those names
// carries the word — it is their base class (QQuickTextInput / QQuickTextEdit)
// that does. Checking only the leaf name was letting the piano keys eat every
// letter typed into the patch/preset name fields.
static bool focusedObjectTakesText(QObject* fo) {
  for (const QMetaObject* mo = fo ? fo->metaObject() : nullptr; mo; mo = mo->superClass()) {
    const QString name = QString::fromLatin1(mo->className());
    if (name.contains("TextInput") or name.contains("TextEdit")) return true;
  }
  return false;
}

App& App::instance() {
  static App myInstance;
  return myInstance;
}

App::App() : App(Database::instance(), BluetoothManager::instance(), Settings::instance()) {}

App::App(IDatabase& db, IBluetoothManager& btm, ISettings& st)
  : database{db}, bluetoothManager{btm}, settings{st} {
  // Route the controller's persistence through the injected dependency. Only
  // the database: the controller reads no setting, and wiring one in anyway
  // (as this used to) gave it a dependency nothing ever asked a question
  // through.
  synth.setDatabase(&database);

  connect(this, &App::bluetoothAvailable, &bluetoothManager,
          &IBluetoothManager::onBluetoothAvailable);

  connect(&bluetoothManager, &IBluetoothManager::connectedChanged, this,
          [this](bool isConnected) {
            synth.setConnected(isConnected);
            // After setConnected: it resets the controller's link state, so
            // the MTU has to be pushed on top of the clean slate. The
            // controller sizes every batched frame from it.
            if (isConnected) synth.setLinkMtu(bluetoothManager.mtu());
          });

  connect(&bluetoothManager, &IBluetoothManager::receivedData, &synth,
          &SynthController::onReceiveData, Qt::QueuedConnection);

  connect(&bluetoothManager, &IBluetoothManager::infoRead, &synth,
          &SynthController::onInfoRead, Qt::QueuedConnection);

  connect(&bluetoothManager, &IBluetoothManager::updateConnectedBluetoothDevice, this,
          &App::updateConnectedBluetoothDevice, Qt::QueuedConnection);

  // Command frames flow synth -> BLE. Queued so a write emitted from a timer or
  // event handler always lands on the manager's event loop.
  connect(&synth, &SynthController::writeToSynth, this,
          [this](const QByteArray& data, bool withResponse) {
            bluetoothManager.write(data, withResponse);
          }, Qt::QueuedConnection);

  // Read before requestPermissions(): on the already-Granted path (every run
  // after the first) it emits bluetoothAvailable() synchronously, and the
  // manager's onBluetoothAvailable() initialises the stack unconditionally.
  // Reading these afterwards meant that emit tested the *header default*
  // (true), so a user who had turned Bluetooth off still got a scanning radio
  // behind an off toggle.
  m_bluetoothEnabled = settingIsTrue("bluetooth_enabled");
  m_bluetoothSelectedDeviceName = setting("bluetooth_selected_device_name");
  m_bluetoothSelectedDeviceAddress = setting("bluetooth_selected_device_address");

  requestPermissions();

  // Global key filter for the computer-keyboard piano (desktop). Guarded so
  // DI/test construction without a running app doesn't touch qApp.
  if (qApp) {
    qApp->installEventFilter(this);
    // Clicking into a name field while holding a key hands that key's release
    // to the field, which would leave the note sounding forever.
    connect(qApp, &QGuiApplication::focusObjectChanged, this, [this](QObject* focusObject) {
      if (m_keyboardCaptureEnabled and focusedObjectTakesText(focusObject))
        emit computerKeysAllReleased();
    });
  }

  // Theme from stored settings.
  {
    const int presetIdx =
        qBound(0, setting("theme_preset").toInt(), static_cast<int>(Theme::presets.size()) - 1);
    const QString type = (setting("theme_type") == "light") ? "light" : "dark";
    m_themePresetIndex = presetIdx;
    const auto& p = Theme::presets[presetIdx];
    m_theme.setType(type);
    m_theme.setPrimaryColor(p.primaryColor);
    m_theme.setPrimaryBgColor(type == "dark" ? p.darkBgColor : p.lightBgColor);
    m_theme.setMaterialAccent(p.materialAccent);
  }

  deleteTemporaryAppFiles();
}

void App::deleteTemporaryAppFiles() {
  for (const QString& ext : {QString("bin"), QString("hex")}) {
    m_firmwareFileExtension = ext;
    QFile::remove(getFirmwareFileLocation());
  }
  m_firmwareFileExtension = {};
}

void App::requestPermissions() {
#if QT_CONFIG(permissions) || defined(Q_OS_APPLE)
  auto checkBtPermission = [this]() {
    QBluetoothPermission btPermission;
    // Central-role access only (scan/connect); the default also asks for
    // BLUETOOTH_ADVERTISE, which the app doesn't declare or need.
    btPermission.setCommunicationModes(QBluetoothPermission::Access);
    switch (qApp->checkPermission(btPermission)) {
      case Qt::PermissionStatus::Undetermined:
        qApp->requestPermission(btPermission, this, [this](const QPermission&) {
          if (m_bluetoothEnabled) emit bluetoothAvailable();
        });
        break;
      case Qt::PermissionStatus::Denied:
        break;
      case Qt::PermissionStatus::Granted:
        // Already granted (e.g. a later run): make sure the BLE stack starts,
        // since the manager's startup check may have run before this resolved.
        if (m_bluetoothEnabled) emit bluetoothAvailable();
        break;
    }
  };

  auto checkLocationPermission = [this, checkBtPermission]() {
    QLocationPermission locationPermission;
    locationPermission.setAccuracy(QLocationPermission::Precise);
    locationPermission.setAvailability(QLocationPermission::WhenInUse);
    switch (qApp->checkPermission(locationPermission)) {
      case Qt::PermissionStatus::Undetermined:
        qApp->requestPermission(locationPermission, this,
                                [checkBtPermission](const QPermission&) { checkBtPermission(); });
        break;
      case Qt::PermissionStatus::Denied:
      case Qt::PermissionStatus::Granted:
        checkBtPermission();
        break;
    }
  };

#if defined(Q_OS_MACOS)
  checkBtPermission();
#else
  checkLocationPermission();
#endif
#endif
}

void App::setKeepScreenOn(bool on) {
#if defined(Q_OS_WINDOWS)
  if (on) {
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
  } else {
    SetThreadExecutionState(ES_CONTINUOUS);
  }
#else
  Q_UNUSED(on);
#endif
}

void App::setAndroidImmersiveMode(bool enabled) {
#if defined(Q_OS_ANDROID)
  // runOnAndroidMainThread takes a std::function<QVariant()>, so the lambda
  // returns a QVariant.
  QNativeInterface::QAndroidApplication::runOnAndroidMainThread([enabled]() -> QVariant {
    QJniObject activity = QNativeInterface::QAndroidApplication::context();
    if (not activity.isValid()) return {};
    QJniObject window = activity.callObjectMethod("getWindow", "()Landroid/view/Window;");
    if (not window.isValid()) return {};
    QJniObject decorView = window.callObjectMethod("getDecorView", "()Landroid/view/View;");
    if (not decorView.isValid()) return {};

    // Deprecated in API 30 but still functional and simplest across versions:
    // IMMERSIVE_STICKY | LAYOUT_STABLE | LAYOUT_HIDE_NAVIGATION |
    // LAYOUT_FULLSCREEN | HIDE_NAVIGATION | FULLSCREEN.
    const jint immersiveFlags = 0x1000 | 0x100 | 0x200 | 0x400 | 0x2 | 0x4;
    decorView.callMethod<void>("setSystemUiVisibility", "(I)V", enabled ? immersiveFlags : 0);
    return {};
  });
#else
  Q_UNUSED(enabled);
#endif
}

QString App::getVersionNumber() {
#ifdef APP_VERSION_NUMBER
  return APP_VERSION_NUMBER;
#else
  return "?";
#endif
}

QString App::getVersionFull() {
#ifdef APP_VERSION
  return APP_VERSION;
#else
  return "?";
#endif
}

QString App::getOsName() { return QSysInfo::productType(); }

bool App::isDesktop() { return not isMobile(); }

bool App::isMobile() {
  const QString os = QSysInfo::productType().toLower();
  return os == "android" or os == "ios" or os == "watchos";
}

bool App::isAndroid() { return QSysInfo::productType().toLower() == "android"; }

bool App::isWindows() { return QSysInfo::productType().toLower() == "windows"; }

bool App::isLinux() {
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
  return true;
#else
  return false;
#endif
}

QString App::getWritablePath() {
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

bool App::copyFileTo(const QString& from, const QString& to) {
  QString newTo = AppUtils::getFilePathFromCanonical(to);
  if (QFile::exists(newTo)) QFile::remove(newTo);
  return QFile::copy(from, newTo);
}

QString App::getDatabaseFileLocation() { return database.getDatabaseFileFullPath(); }

QString App::getFirmwareFileLocation() {
  QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  return path + (((path.right(1) == "\\") or (path.right(1) == "/")) ? "" : "/") +
         QString{globalFirmwareFileName} + "." + m_firmwareFileExtension;
}

void App::emitFirmwareFileLoaded() { emit firmwareFileLoaded(); }

bool App::saveBackupTo(const QString& fileFullPath) {
  return database.saveDatabaseBackupTo(fileFullPath);
}

bool App::restoreBackupFrom(const QString& fileFullPath) {
  QString error;
  if (not database.restoreDatabaseFrom(fileFullPath, &error)) {
    emit restoreFailed(error.isEmpty()
                           ? Translator::instance().t("Could not restore the backup.")
                           : error);
    return false;
  }
  emit databaseRestored();
  return true;
}

void App::emitDatabaseRestored() { emit databaseRestored(); }

void App::forceCloseDatabase() { database.forceCloseDatabase(); }

void App::forceOpenDatabase() { database.forceOpenOrCreateDatabase(); }

bool App::writeTextFile(const QString& fileFullPath, const QString& text) {
  const QString path = AppUtils::getFilePathFromCanonical(fileFullPath);
  QFile file{path};
  // No QIODevice::Text: the bytes are written verbatim, so what lands on disk is
  // exactly the UTF-8 that was handed in — no CRLF translation to reason about
  // when the same file is read back on another platform.
  if (not file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    qWarning() << "App | Could not open for writing:" << path << file.errorString();
    return false;
  }
  const QByteArray utf8 = text.toUtf8();
  const bool ok = file.write(utf8) == utf8.size();
  file.close();
  return ok;
}

QString App::readTextFile(const QString& fileFullPath) {
  const QString path = AppUtils::getFilePathFromCanonical(fileFullPath);
  QFile file{path};
  if (not file.open(QIODevice::ReadOnly)) {
    qWarning() << "App | Could not open for reading:" << path << file.errorString();
    return QString();
  }
  const QString text = QString::fromUtf8(file.readAll());
  file.close();
  return text;
}

QString App::exportFileLocation(const QString& fileName) {
  const QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  QDir{}.mkpath(path);  // first export of a fresh install: the dir may not exist
  return path + (((path.right(1) == "\\") or (path.right(1) == "/")) ? "" : "/") + fileName;
}

void App::setFirmwareFileExtension(const QString& extension) { m_firmwareFileExtension = extension; }

QString App::getFirmwareFileExtension() { return m_firmwareFileExtension; }

// Android's Storage Access Framework flows: shareFile exports via ACTION_SEND +
// FileProvider, selectFile imports via ACTION_GET_CONTENT and copies the picked
// stream to `destination`. Desktop picks files through the native dialogs below,
// or the QML FileDialog where those are unavailable (see Main.qml).
void App::shareFile(const QString& fileFullPath) {
#if defined(Q_OS_ANDROID)
  const QString ext = QFileInfo(fileFullPath).suffix().toLower();
  if (not QFile::exists(fileFullPath)) {
    qWarning() << "File does not exist: " << fileFullPath;
    return;
  }

  const auto valid = [](const QJniObject& obj, const QString& msg) {
    if (not obj.isValid()) {
      qWarning() << msg;
      return false;
    }
    return true;
  };

  const QJniObject actionSend =
      QJniObject::getStaticObjectField<jstring>("android/content/Intent", "ACTION_SEND");
  if (not valid(actionSend, "ACTION_SEND")) return;

  const QJniObject intent("android/content/Intent", "(Ljava/lang/String;)V",
                          actionSend.object<jstring>());
  if (not valid(intent, "Intent")) return;

  intent.callObjectMethod("setType", "(Ljava/lang/String;)Landroid/content/Intent;",
                          QJniObject::fromString(mimeTypeForExtension(ext)).object());

  QJniObject file("java/io/File", "(Ljava/lang/String;)V",
                  QJniObject::fromString(fileFullPath).object());
  if (not valid(file, "File")) return;

  auto uri = QJniObject::callStaticObjectMethod(
      "androidx/core/content/FileProvider", "getUriForFile",
      "(Landroid/content/Context;Ljava/lang/String;Ljava/io/File;)Landroid/net/Uri;",
      QNativeInterface::QAndroidApplication::context().object(),
      QJniObject::fromString(kFileProviderAuthority).object(), file.object());
  if (not valid(uri, "FileProvider Uri")) return;

  const QJniObject extraStream =
      QJniObject::getStaticObjectField<jstring>("android/content/Intent", "EXTRA_STREAM");
  intent.callObjectMethod("putExtra",
                          "(Ljava/lang/String;Landroid/os/Parcelable;)Landroid/content/Intent;",
                          extraStream.object(), uri.object());

  const jint grantRead =
      QJniObject::getStaticField<jint>("android/content/Intent", "FLAG_GRANT_READ_URI_PERMISSION");
  intent.callObjectMethod("addFlags", "(I)Landroid/content/Intent;", grantRead);

  QJniObject chooser = QJniObject::callStaticObjectMethod(
      "android/content/Intent", "createChooser",
      "(Landroid/content/Intent;Ljava/lang/CharSequence;)Landroid/content/Intent;",
      intent.object<jobject>(), QJniObject::fromString("Share file").object<jstring>());
  if (not valid(chooser, "createChooser")) return;

  QtAndroidPrivate::startActivity(chooser, 1001, nullptr);
#else
  Q_UNUSED(fileFullPath);
#endif
}

void App::selectFile(const QString& destination, const QString& extensions) {
#if defined(Q_OS_ANDROID)
  Q_UNUSED(extensions);
  const auto valid = [](const QJniObject& obj, const QString& msg) {
    if (not obj.isValid()) {
      qWarning() << msg;
      return false;
    }
    return true;
  };

  const QJniObject actionGetContent =
      QJniObject::getStaticObjectField<jstring>("android/content/Intent", "ACTION_GET_CONTENT");
  if (not valid(actionGetContent, "ACTION_GET_CONTENT")) return;
  const QJniObject categoryOpenable =
      QJniObject::getStaticObjectField<jstring>("android/content/Intent", "CATEGORY_OPENABLE");

  const QJniObject intent("android/content/Intent", "(Ljava/lang/String;)V",
                          actionGetContent.object<jstring>());
  if (not valid(intent, "Intent")) return;

  intent.callObjectMethod("addCategory", "(Ljava/lang/String;)Landroid/content/Intent;",
                          categoryOpenable.object<jstring>());
  intent.callObjectMethod("setType", "(Ljava/lang/String;)Landroid/content/Intent;",
                          QJniObject::fromString("*/*").object());

  QJniObject chooser = QJniObject::callStaticObjectMethod(
      "android/content/Intent", "createChooser",
      "(Landroid/content/Intent;Ljava/lang/CharSequence;)Landroid/content/Intent;",
      intent.object<jobject>(), QJniObject::fromString("Choose a file").object<jstring>());
  if (not valid(chooser, "createChooser")) return;

  QtAndroidPrivate::startActivity(
      chooser, 1002, [this, destination](int actionCode, int, const QJniObject& resultIntent) {
        if (actionCode != 1002) return;

        // Any exit without a successful copy must tell QML the selection did not
        // happen so it can roll back state armed before the picker (a restore
        // closed the DB first).
        bool fileWasSelected = false;
        auto cancelNotifier = qScopeGuard([this, &fileWasSelected]() {
          if (not fileWasSelected) emit selectFileCanceled();
        });

        if (not resultIntent.isValid()) return;
        QJniObject uri = resultIntent.callObjectMethod("getData", "()Landroid/net/Uri;");
        if (not uri.isValid()) return;

        QJniObject scheme = uri.callObjectMethod("getScheme", "()Ljava/lang/String;");
        if (not scheme.isValid() or scheme.toString() != "content") return;

        QJniObject activity = QNativeInterface::QAndroidApplication::context();
        if (not activity.isValid()) return;
        QJniObject resolver =
            activity.callObjectMethod("getContentResolver", "()Landroid/content/ContentResolver;");
        if (not resolver.isValid()) return;

        QJniObject cursor = resolver.callObjectMethod(
            "query",
            "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/"
            "String;)Landroid/database/Cursor;",
            uri.object(), nullptr, nullptr, nullptr, nullptr);
        QString displayName;
        if (cursor.isValid() and cursor.callMethod<jboolean>("moveToFirst", "()Z")) {
          jint idx = cursor.callMethod<jint>("getColumnIndex", "(Ljava/lang/String;)I",
                                             QJniObject::fromString("_display_name").object());
          if (idx >= 0) {
            QJniObject name = cursor.callObjectMethod("getString", "(I)Ljava/lang/String;", idx);
            if (name.isValid()) displayName = name.toString();
          }
          cursor.callMethod<void>("close", "()V");
        }

        QJniObject inStream = resolver.callObjectMethod(
            "openInputStream", "(Landroid/net/Uri;)Ljava/io/InputStream;", uri.object());
        if (not inStream.isValid()) return;

        QJniObject fileTo("java/io/File", "(Ljava/lang/String;)V",
                          QJniObject::fromString(destination).object());
        QJniObject outStream("java/io/FileOutputStream", "(Ljava/io/File;)V", fileTo.object());
        if (not outStream.isValid()) return;

        jlong copied = QJniObject::callStaticMethod<jlong>(
            "android/os/FileUtils", "copy", "(Ljava/io/InputStream;Ljava/io/OutputStream;)J",
            inStream.object(), outStream.object());
        if (copied <= 0) return;

        fileWasSelected = true;
        emit selectFileSelected(displayName.isEmpty() ? destination : displayName);
      });
#else
  Q_UNUSED(destination);
  Q_UNUSED(extensions);
  emit selectFileCanceled();
#endif
}

bool App::hasNativeFileDialogs() { return NativeFileDialog::isAvailable(); }

QString App::openFileDialog(const QString& filters, const QString& defaultPath) {
  return NativeFileDialog::openFile(filters, defaultPath);
}

QString App::saveFileDialog(const QString& filters, const QString& defaultPath,
                            const QString& defaultSuffix) {
  return NativeFileDialog::saveFile(filters, defaultPath, defaultSuffix);
}

void App::updateConnectedBluetoothDevice(const QString& name, const QString& address) {
  database.updateConnectedBluetoothDevice(name, address);
}

void App::setQmlEngine(QQmlEngine* newEngine) { engine = newEngine; }

void App::onDatabaseRestoredAction() { settings.reloadCurrentSettings(); }

QString App::getFontAwesomeFontName() const { return m_fontAwesomeName; }

void App::setFontAwesomeFontName(const QString& newFontAwesomeName) {
  if (m_fontAwesomeName == newFontAwesomeName) return;
  m_fontAwesomeName = newFontAwesomeName;
  emit fontAwesomeFontNameChanged();
}

QString App::getFontAwesomeRegularFontName() const { return m_fontAwesomeRegularName; }

void App::setFontAwesomeRegularFontName(const QString& newFontAwesomeRegularName) {
  if (m_fontAwesomeRegularName == newFontAwesomeRegularName) return;
  m_fontAwesomeRegularName = newFontAwesomeRegularName;
  emit fontAwesomeRegularFontNameChanged();
}

SynthController& App::getSynth() { return synth; }

Theme App::getTheme() const { return m_theme; }

int App::getThemePresetIndex() const { return m_themePresetIndex; }

void App::setThemeType(const QString& type) {
  const QString t = (type == "light") ? "light" : "dark";
  const auto& p = Theme::presets[m_themePresetIndex];
  m_theme.setType(t);
  m_theme.setPrimaryBgColor(t == "dark" ? p.darkBgColor : p.lightBgColor);
  saveSetting("theme_type", t);
  emit themeChanged();
}

void App::applyThemePreset(int index) {
  if (index < 0 or index >= static_cast<int>(Theme::presets.size())) return;
  m_themePresetIndex = index;
  const auto& p = Theme::presets[index];
  m_theme.setPrimaryColor(p.primaryColor);
  m_theme.setPrimaryBgColor(m_theme.type() == "dark" ? p.darkBgColor : p.lightBgColor);
  m_theme.setMaterialAccent(p.materialAccent);
  saveSetting("theme_preset", QString::number(index));
  emit themeChanged();
}

QString App::setting(const QString& name) const { return settings.setting(name); }

bool App::settingIsTrue(const QString& name) const { return settings.settingIsTrue(name); }

int App::saveSetting(const QString& name, const QString& value) {
  const int saved = settings.saveSetting(name, value);
  // setting() is a plain invokable, so a QML binding onto it captures nothing
  // and never re-evaluates. This is the one funnel every write passes through,
  // so it is where components are told to re-read — without it, changing e.g.
  // the keyboard's base octave in Settings did nothing until an app restart.
  if (saved) emit settingChanged(name);
  return saved;
}

bool App::getBluetoothEnabled() const { return m_bluetoothEnabled; }

void App::setBluetoothEnabled(bool enabled) {
  m_bluetoothEnabled = enabled;
  saveSetting("bluetooth_enabled", enabled ? "true" : "false");
  bluetoothManager.setBluetoothEnabled(enabled);
  emit bluetoothEnabledChanged();
}

QString App::getBluetoothSelectedDeviceName() const { return m_bluetoothSelectedDeviceName; }

QString App::getBluetoothSelectedDeviceAddress() const { return m_bluetoothSelectedDeviceAddress; }

void App::setBluetoothSelectedDevice(const QString& name, const QString& address) {
  m_bluetoothSelectedDeviceName = name;
  m_bluetoothSelectedDeviceAddress = address;
  saveSetting("bluetooth_selected_device_name", name);
  saveSetting("bluetooth_selected_device_address", address);
  emit bluetoothSelectedDeviceChanged();
}

bool App::keyboardCaptureEnabled() const { return m_keyboardCaptureEnabled; }

void App::setKeyboardCaptureEnabled(bool on) {
  if (m_keyboardCaptureEnabled == on) return;
  m_keyboardCaptureEnabled = on;
  emit keyboardCaptureEnabledChanged();
}

// Two tracker-style rows: Z..M (+ comma) is the lower octave, Q..I the upper.
// Returns the semitone offset from the base octave's C, or -1 for non-piano keys.
int App::semitoneForKey(int key) {
  switch (key) {
    // lower row (white: Z X C V B N M ,  black: S D G H J)
    case Qt::Key_Z: return 0;
    case Qt::Key_S: return 1;
    case Qt::Key_X: return 2;
    case Qt::Key_D: return 3;
    case Qt::Key_C: return 4;
    case Qt::Key_V: return 5;
    case Qt::Key_G: return 6;
    case Qt::Key_B: return 7;
    case Qt::Key_H: return 8;
    case Qt::Key_N: return 9;
    case Qt::Key_J: return 10;
    case Qt::Key_M: return 11;
    case Qt::Key_Comma: return 12;
    // upper row (white: Q W E R T Y U I  black: 2 3 5 6 7). With the
    // keyboard_top_row_drums setting on (the default) these keys fire drum
    // pads instead — see drumPadForKey() and the check in eventFilter().
    case Qt::Key_Q: return 12;
    case Qt::Key_2: return 13;
    case Qt::Key_W: return 14;
    case Qt::Key_3: return 15;
    case Qt::Key_E: return 16;
    case Qt::Key_R: return 17;
    case Qt::Key_5: return 18;
    case Qt::Key_T: return 19;
    case Qt::Key_6: return 20;
    case Qt::Key_Y: return 21;
    case Qt::Key_7: return 22;
    case Qt::Key_U: return 23;
    case Qt::Key_I: return 24;
    default: return -1;
  }
}

// The top two rows drive the 4x4 drum pads instead of a second octave, when
// keyboard_top_row_drums is on (the default).
//
// Pads number left-to-right, bottom row first (DrumPads.qml uses MPC order),
// so the two key rows map onto the two halves of the grid in the same reading
// direction: Q..I is the bottom half of the pads — in the factory kit the
// kicks, snares, stick, clap and closed hats — and 1..8 is the top half, the
// open hat, toms, cymbals and percussion. Letters are the drums you play
// fastest, which is why they get the home row of the two.
int App::drumPadForKey(int key) {
  switch (key) {
    case Qt::Key_Q: return 0;
    case Qt::Key_W: return 1;
    case Qt::Key_E: return 2;
    case Qt::Key_R: return 3;
    case Qt::Key_T: return 4;
    case Qt::Key_Y: return 5;
    case Qt::Key_U: return 6;
    case Qt::Key_I: return 7;
    case Qt::Key_1: return 8;
    case Qt::Key_2: return 9;
    case Qt::Key_3: return 10;
    case Qt::Key_4: return 11;
    case Qt::Key_5: return 12;
    case Qt::Key_6: return 13;
    case Qt::Key_7: return 14;
    case Qt::Key_8: return 15;
    default: return -1;
  }
}

bool App::eventFilter(QObject* watched, QEvent* event) {
  const QEvent::Type t = event->type();
  if (m_keyboardCaptureEnabled and (t == QEvent::KeyPress or t == QEvent::KeyRelease)) {
    // Don't steal keys from a focused text input (preset/patch name fields).
    if (not focusedObjectTakesText(QGuiApplication::focusObject())) {
      auto* ke = static_cast<QKeyEvent*>(event);
      // A modified key is a shortcut, not a note. Ctrl+C / Ctrl+V / Ctrl+Z /
      // Ctrl+S all land on mapped piano keys (C, V, Z, S), so without this the
      // filter both sounded a note and swallowed the shortcut app-wide.
      // Shift is deliberately let through: it is not a shortcut on its own,
      // and holding it while playing must not go silent.
      if (ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        return QObject::eventFilter(watched, event);
      // Checked first, and it fully shadows the upper octave's keys: with the
      // setting on, Q..I and the number row must not also emit semitones.
      if (settingIsTrue(QStringLiteral("keyboard_top_row_drums"))) {
        const int pad = drumPadForKey(ke->key());
        if (pad >= 0) {
          if (not ke->isAutoRepeat()) {
            // A drum is a one-shot: there is no release to send. Auto-repeat
            // is dropped rather than machine-gunning the pad.
            if (t == QEvent::KeyPress) emit computerDrumPadPressed(pad);
          }
          return true;
        }
      }
      const int semi = semitoneForKey(ke->key());
      if (semi >= 0) {
        if (not ke->isAutoRepeat()) {
          if (t == QEvent::KeyPress)
            emit computerKeyPressed(semi);
          else
            emit computerKeyReleased(semi);
        }
        return true;  // consume mapped keys (including auto-repeat)
      }
    }
  }
  return QObject::eventFilter(watched, event);
}
