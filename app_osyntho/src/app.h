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

// Composition root + QML façade. Owns the SynthController and wires it to the
// platform BLE manager; exposes settings, theme, backup/restore and the
// (future) firmware-file plumbing to QML.
class App : public QObject {
  Q_OBJECT

  Q_PROPERTY(QString fontAwesomeName READ getFontAwesomeFontName WRITE setFontAwesomeFontName NOTIFY
                 fontAwesomeFontNameChanged)
  Q_PROPERTY(QString fontAwesomeRegularName READ getFontAwesomeRegularFontName WRITE
                 setFontAwesomeRegularFontName NOTIFY fontAwesomeRegularFontNameChanged)
  Q_PROPERTY(Theme theme READ getTheme NOTIFY themeChanged)
  Q_PROPERTY(int themePresetIndex READ getThemePresetIndex NOTIFY themeChanged)
  Q_PROPERTY(bool bluetoothEnabled READ getBluetoothEnabled WRITE setBluetoothEnabled NOTIFY
                 bluetoothEnabledChanged)
  Q_PROPERTY(QString bluetoothSelectedDeviceName READ getBluetoothSelectedDeviceName NOTIFY
                 bluetoothSelectedDeviceChanged)
  Q_PROPERTY(QString bluetoothSelectedDeviceAddress READ getBluetoothSelectedDeviceAddress NOTIFY
                 bluetoothSelectedDeviceChanged)
  // When true (desktop, keyboard visible), maps computer keys to piano notes via
  // a global event filter. The on-screen Keyboard binds this to its visibility.
  Q_PROPERTY(bool keyboardCaptureEnabled READ keyboardCaptureEnabled WRITE setKeyboardCaptureEnabled
                 NOTIFY keyboardCaptureEnabledChanged)

 public:
  static App& instance();

  // Composition-root constructor. Production uses instance() (which injects the
  // singletons); tests can construct App with a fake/in-memory IDatabase and
  // IBluetoothManager.
  App(IDatabase& db, IBluetoothManager& btm, ISettings& st);

  // settings helpers
  Q_INVOKABLE QString setting(const QString& name) const;
  Q_INVOKABLE bool settingIsTrue(const QString& name) const;
  Q_INVOKABLE int saveSetting(const QString& name, const QString& value);

  Q_INVOKABLE void setKeepScreenOn(bool on);
  // Android only: enable immersive-sticky fullscreen (hides the status/nav bars).
  // Also tends to make MIUI/HyperOS stop intercepting multi-finger system
  // gestures over the app. No-op elsewhere.
  Q_INVOKABLE void setAndroidImmersiveMode(bool enabled);
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
  Q_INVOKABLE void restoreBackupFrom(const QString& fileFullPath);
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

  // Platform file flows (Windows nfd / Android SAF); desktop-Qt callers use the
  // QML FileDialog fallback instead.
  Q_INVOKABLE void shareFile(const QString& fileFullPath);
  Q_INVOKABLE void selectFile(const QString& destination, const QString& extensions = "*");

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
  void databaseRestored();
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
