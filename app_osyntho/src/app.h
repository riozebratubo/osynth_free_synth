#ifndef APP_H
#define APP_H

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QtSystemDetection>

#include "src/business/database.h"
#include "src/business/idatabase.h"
#include "src/business/isettings.h"
#include "src/business/settings.h"
#include "src/ibluetoothmanager.h"
#include "src/theme.h"

#ifdef Q_OS_WINDOWS
#include "src/bluetoothmanager2.h"
#else
#include "src/bluetoothmanager.h"
#endif

#include "src/synthcontroller.h"
#include <QtQml/qqmlregistration.h>

// Composition root + QML façade. Owns the SynthController and wires it to the
// platform BLE manager; exposes settings, theme, backup/restore and the
// (future) firmware-file plumbing to QML.
class App final : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_SINGLETON

  Q_PROPERTY(QString fontAwesomeName READ getFontAwesomeFontName WRITE setFontAwesomeFontName NOTIFY
                 fontAwesomeFontNameChanged FINAL)
  Q_PROPERTY(QString fontAwesomeRegularName READ getFontAwesomeRegularFontName WRITE
                 setFontAwesomeRegularFontName NOTIFY fontAwesomeRegularFontNameChanged FINAL)
  Q_PROPERTY(Theme theme READ getTheme NOTIFY themeChanged FINAL)
  Q_PROPERTY(int themePresetIndex READ getThemePresetIndex NOTIFY themeChanged FINAL)
  Q_PROPERTY(bool bluetoothEnabled READ getBluetoothEnabled WRITE setBluetoothEnabled NOTIFY
                 bluetoothEnabledChanged FINAL)
  Q_PROPERTY(QString bluetoothSelectedDeviceName READ getBluetoothSelectedDeviceName NOTIFY
                 bluetoothSelectedDeviceChanged FINAL)
  Q_PROPERTY(QString bluetoothSelectedDeviceAddress READ getBluetoothSelectedDeviceAddress NOTIFY
                 bluetoothSelectedDeviceChanged FINAL)
  // When true (desktop, keyboard visible), maps computer keys to piano notes via
  // a global event filter. The on-screen Keyboard binds this to its visibility.
  Q_PROPERTY(bool keyboardCaptureEnabled READ keyboardCaptureEnabled WRITE setKeyboardCaptureEnabled
                 NOTIFY keyboardCaptureEnabledChanged FINAL)

 public:
  static App& instance();

  // QML singleton factory. The engine never owns App -- instance() does --
  // so ownership is pinned to C++ before the pointer is handed over.
  static App* create(QQmlEngine*, QJSEngine*);

  // Composition-root constructor. Production uses instance() (which injects the
  // singletons); tests can construct App with a fake/in-memory IDatabase and
  // IBluetoothManager.
  App(IDatabase& db, IBluetoothManager& btm, ISettings& st);

  // settings helpers
  Q_INVOKABLE QString setting(const QString& name) const;
  Q_INVOKABLE bool settingIsTrue(const QString& name) const;
  Q_INVOKABLE int saveSetting(const QString& name, const QString& value);

  Q_INVOKABLE void setKeepScreenOn(bool on);

  /* Immersive fullscreen is deliberately NOT here. It used to be a JNI call
   * writing View.setSystemUiVisibility() directly, which fought Qt's own
   * Android handling: the window kept *available* geometry instead of the
   * screen's, and poking the decor flags behind the platform plugin's back
   * left the insets stale, so ApplicationWindow's automatic safe-area padding
   * added a second band. Qt 6.11 does all of it from Qt::WindowFullScreen, so
   * this is one line of QML — the `visibility` binding in Main.qml, which also
   * explains why it must be declared rather than assigned.
   *
   * (It still hides the status/nav bars, and still tends to make MIUI/HyperOS
   * stop intercepting multi-finger gestures over the app — see Keyboard.qml.) */

  Q_INVOKABLE QString getVersionNumber();
  Q_INVOKABLE QString getVersionFull();

  Q_INVOKABLE QString getOsName();
  Q_INVOKABLE bool isDesktop();
  Q_INVOKABLE bool isMobile();
  Q_INVOKABLE bool isAndroid();
  Q_INVOKABLE bool isWindows();
  Q_INVOKABLE bool isLinux();

  Q_INVOKABLE QString getWritablePath();
  Q_INVOKABLE bool copyFileTo(const QString& from, const QString& to);

  Q_INVOKABLE QString getDatabaseFileLocation();
  // False if the backup could not be written, so QML can say so.
  Q_INVOKABLE bool saveBackupTo(const QString& fileFullPath);
  // False if the file was not a usable backup or could not be put in place, in
  // which case restoreFailed() carries the reason and databaseRestored() is NOT
  // emitted — the app is left on the data it already had. Every route (desktop
  // picker, Android SAF) goes through this one call, so the file is validated
  // the same way on all of them.
  Q_INVOKABLE bool restoreBackupFrom(const QString& fileFullPath);
  Q_INVOKABLE void emitDatabaseRestored();
  Q_INVOKABLE void forceCloseDatabase();
  Q_INVOKABLE void forceOpenDatabase();

  // Patch interchange files. Both take either a plain path or the file:// URL a
  // QML FileDialog hands back. exportFileLocation() is the app-private staging
  // path Android exports are written to before being handed to shareFile().
  Q_INVOKABLE bool writeTextFile(const QString& fileFullPath, const QString& text);
  Q_INVOKABLE QString readTextFile(const QString& fileFullPath);
  Q_INVOKABLE QString exportFileLocation(const QString& fileName);

  // Kept for a future osynth firmware-update capability (none today).
  Q_INVOKABLE void setFirmwareFileExtension(const QString& extension);
  Q_INVOKABLE QString getFirmwareFileExtension();
  Q_INVOKABLE QString getFirmwareFileLocation();
  Q_INVOKABLE void emitFirmwareFileLoaded();

  // Android SAF flows: no-ops on every other platform.
  Q_INVOKABLE void shareFile(const QString& fileFullPath);
  Q_INVOKABLE void selectFile(const QString& destination, const QString& extensions = "*");

  // Native open/save pickers, via nfd. Available on Windows and Linux (there,
  // only when zenity is installed); QML keeps its own FileDialog for the rest
  // and must check hasNativeFileDialogs() before calling either. Both block
  // until the dialog closes and answer "" when nothing was chosen. See
  // src/nativefiledialog.h for the filter format.
  Q_INVOKABLE bool hasNativeFileDialogs();
  Q_INVOKABLE QString openFileDialog(const QString& filters, const QString& defaultPath);
  Q_INVOKABLE QString saveFileDialog(const QString& filters, const QString& defaultPath,
                                     const QString& defaultSuffix = QString());

  void setQmlEngine(QQmlEngine* engine);

  QString getFontAwesomeFontName() const;
  void setFontAwesomeFontName(const QString& newFontAwesomeName);

  QString getFontAwesomeRegularFontName() const;
  void setFontAwesomeRegularFontName(const QString& newFontAwesomeRegularName);

  Theme getTheme() const;
  int getThemePresetIndex() const;
  Q_INVOKABLE void setThemeType(const QString& type);
  Q_INVOKABLE void applyThemePreset(int index);

  SynthController& getSynth();

  bool getBluetoothEnabled() const;
  void setBluetoothEnabled(bool enabled);

  QString getBluetoothSelectedDeviceName() const;
  QString getBluetoothSelectedDeviceAddress() const;
  Q_INVOKABLE void setBluetoothSelectedDevice(const QString& name, const QString& address);

  bool keyboardCaptureEnabled() const;
  void setKeyboardCaptureEnabled(bool on);

 protected:
  // Global key filter: on desktop, translates the tracker-style computer-keyboard
  // rows into piano notes (semitone offsets from the on-screen base octave),
  // skipping events aimed at focused text inputs.
  bool eventFilter(QObject* watched, QEvent* event) override;

 signals:
  // A restore actually landed. Emitted only on success — it used to fire on
  // every attempt, so a restore that failed its checks, or that rolled back to
  // the old database, still showed the user a "Backup restored." toast.
  void databaseRestored();
  // Why a restore did not happen; already translated and meant to be shown.
  void restoreFailed(QString reason);
  void firmwareFileLoaded();

  // A setting was written through saveSetting(). setting() is a plain
  // invokable with no change notification, so components that mirror a setting
  // into a property (Keyboard, DrumPads) re-read on this instead of only
  // sampling it once at creation.
  void settingChanged(QString name);

  void fontAwesomeFontNameChanged();
  void fontAwesomeRegularFontNameChanged();
  void themeChanged();

  void selectFileSelected(QString fileName);
  // Emitted when the native picker of selectFile() ends WITHOUT a selection so
  // QML can roll back state armed before opening it (e.g. reopen a DB a restore
  // closed).
  void selectFileCanceled();

  void bluetoothAvailable();
  void bluetoothEnabledChanged();
  void bluetoothSelectedDeviceChanged();

  void keyboardCaptureEnabledChanged();
  // Emitted for a mapped key press/release (semitone offset from the base
  // octave's C). The Keyboard applies base octave + velocity.
  void computerKeyPressed(int semitone);
  void computerKeyReleased(int semitone);
  // A top-row key mapped to a drum pad (0..15), when keyboard_top_row_drums
  // is on. One-shot: there is no matching release.
  void computerDrumPadPressed(int pad);
  // Focus moved into a text field, so the key-up of anything held right then
  // goes to the field instead of here. Asks the Keyboard to drop what is
  // sounding rather than leave a stranded note.
  void computerKeysAllReleased();

 public slots:
  void onDatabaseRestoredAction();
  void updateConnectedBluetoothDevice(const QString& name, const QString& address);

 private:
  App();

  void requestPermissions();
  void deleteTemporaryAppFiles();

  QString m_fontAwesomeName;
  QString m_fontAwesomeRegularName;
  QString m_firmwareFileExtension;
  Theme m_theme;
  int m_themePresetIndex = 0;

  IDatabase& database;
  IBluetoothManager& bluetoothManager;
  ISettings& settings;
  SynthController synth;

  QQmlEngine* engine = nullptr;

  bool m_bluetoothEnabled = true;
  QString m_bluetoothSelectedDeviceName;
  QString m_bluetoothSelectedDeviceAddress;

  bool m_keyboardCaptureEnabled = false;
  // Semitone offset for a computer key, or -1 if the key isn't a piano key.
  static int semitoneForKey(int key);
  // Top-row key -> drum pad index (0..15), or -1. Shadows the upper
  // octave when keyboard_top_row_drums is on.
  static int drumPadForKey(int key);
};

#endif  // APP_H
