#include "settings.h"

#include <QDebug>
#include <QSysInfo>

#include "src/business/database.h"

Settings::Settings(IDatabase& db)
  : db{db} {
  reloadCurrentSettings();
}

Settings::Settings()
  : Settings(Database::instance()) {}

Settings& Settings::instance() {
  static Settings myInstance;
  return myInstance;
}

int Settings::saveSetting(const QString& name, const QString& value) {
  bool saved = db.saveSetting(name, value);
  if (saved) {
    settingsCache[name] = value;
  }
  return saved;
}

void Settings::reloadCurrentSettings() {
  fillSettingsCacheDefaultValues();
  fillSettingsCacheCurrentValues();
}

QString Settings::setting(const QString& name) const {
  if (settingsCache.contains(name)) {
    return settingsCache[name];
  }

  QString value;
  bool got = db.getSetting(name, value);
  if (not got) {
    qWarning() << "Settings | Failed to get setting from db: " << name;
  }

  // qDebug() << ">>> asking for setting " << name << ".";
  // qDebug() << ">>> setting has value " << value << ".";

  return value;
}

bool Settings::settingIsTrue(const QString& name) const { return setting(name) == "true"; }

void Settings::fillSettingsCacheDefaultValues() {
  settingsCache["app_font_size"] = []() {
    QString system = QSysInfo::productType();
    if (system == "android") {
      return "17";
    } else if (system == "windows") {
      return "11";
    }
    return "11";
  }();
  settingsCache["theme_type"] = "dark";
  settingsCache["theme_preset"] = "1";
  settingsCache["force_app_language"] = "en";

  // Bluetooth
  settingsCache["bluetooth_enabled"] = "true";
  settingsCache["bluetooth_prefix"] = "osynth";
  settingsCache["bluetooth_scan_time"] = []() {
    QString system = QSysInfo::productType();
    if (system == "windows") {
      return "10";
    }
    return "14";
  }();
  settingsCache["bluetooth_use_selected"] = "false";
  settingsCache["bluetooth_selected_device_name"] = "";
  settingsCache["bluetooth_selected_device_address"] = "";

  // Synth UI
  settingsCache["show_developer_tools"] = "false";
  // "tiled" packs the parameter panels left-to-right at the width each needs,
  // "rows" gives every panel its own full-width line
  settingsCache["panel_layout"] = "tiled";
  settingsCache["keyboard_octave"] = "4";       // base octave of the on-screen keys
  settingsCache["keyboard_velocity"] = "100";   // 1..127
  settingsCache["keyboard_hold"] = "false";     // latch notes (app-side sustain)
  settingsCache["keyboard_show_note_names"] = "true";  // label keys with note names (C4, D4...)
  settingsCache["keyboard_height"] = "118";     // px height of the on-screen key area
  settingsCache["keyboard_resize_mode"] = "divider";  // "divider" (drag handle) | "slider"
  settingsCache["keyboard_divider_thickness"] = "5";  // px height of the drag divider
  // Computer keys play the synth (desktop). Off hands every key back to the
  // app, which is what you want while driving it from a MIDI/BLE controller
  // or when a stray Z is not meant to sound a note.
  settingsCache["keyboard_computer_keys"] = "true";
  // Top letter/number rows fire the 4x4 drum pads instead of a second
  // octave of keys. On by default: one octave covers most parts, and
  // playing drums and a part together is the point of the pads.
  settingsCache["keyboard_top_row_drums"] = "true";
  settingsCache["android_immersive"] = "true";  // fullscreen/immersive on Android
  // Which page the app opens on: a SwipeView page index ("0" = Home), or "last"
  // to reopen wherever the previous run was left. The index space is UI.screens
  // in Main.qml/UI.qml; an out-of-range value falls back to Home there.
  settingsCache["startup_screen"] = "0";
  settingsCache["last_swipeview_index"] = "0";  // page left on the last run
}

void Settings::fillSettingsCacheCurrentValues() {
  const QHash<QString, QString> currentValues = db.getSettingsCurrentValues();
  for (auto it = currentValues.cbegin(); it != currentValues.cend(); ++it) {
    settingsCache[it.key()] = it.value();
  }
}
