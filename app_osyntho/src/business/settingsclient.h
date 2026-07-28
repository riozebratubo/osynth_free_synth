#ifndef SETTINGSCLIENT_H
#define SETTINGSCLIENT_H

#include "src/business/isettings.h"
#include "src/business/settings.h"

// Mixin that gives any class an injectable settings dependency.
//
// Defaults to the production Settings singleton, so objects that are
// default-constructed (e.g. QML-instantiated view models) keep working with no
// wiring. Tests call setSettings() to inject a Settings built on an in-memory
// Database (or a fake ISettings) before exercising the object.
//
// Mirrors DatabaseClient: intentionally NOT a QObject, so it can be inherited
// alongside a QObject/QAbstractListModel base without a second QObject base.
class SettingsClient {
 public:
  virtual ~SettingsClient() = default;

  // Virtual so composite owners can override to forward injection down to any
  // embedded children.
  virtual void setSettings(ISettings* settings) { m_injectedSettings = settings; }

 protected:
  ISettings& settings() const {
    return m_injectedSettings ? *m_injectedSettings : Settings::instance();
  }

 private:
  ISettings* m_injectedSettings = nullptr;
};

#endif  // SETTINGSCLIENT_H
