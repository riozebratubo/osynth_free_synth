#ifndef SETTINGS_H
#define SETTINGS_H

#include <QHash>

#include "src/business/idatabase.h"
#include "src/business/isettings.h"

class Settings : public ISettings {
 public:
  Settings(IDatabase&);
  static Settings& instance();

  /* api */
  QString setting(const QString& name) const override;
  bool settingIsTrue(const QString& name) const override;
  int saveSetting(const QString& name, const QString& value) override;
  void reloadCurrentSettings() override;

 private:
  Settings();
  void fillSettingsCacheDefaultValues();
  void fillSettingsCacheCurrentValues();
  void migrateScreenOrder();

  IDatabase& db;
  QHash<QString, QString> settingsCache;
};

#endif  // SETTINGS_H
