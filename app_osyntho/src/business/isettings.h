#ifndef ISETTINGS_H
#define ISETTINGS_H

#include <QString>

class ISettings {
 public:
  virtual ~ISettings() = default;

  /* api */
  virtual QString setting(const QString& name) const = 0;
  virtual bool settingIsTrue(const QString& name) const = 0;
  virtual int saveSetting(const QString& name, const QString& value) = 0;
  virtual void reloadCurrentSettings() = 0;
};

#endif  // ISETTINGS_H
