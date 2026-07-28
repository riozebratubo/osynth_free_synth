#ifndef THEME_H
#define THEME_H

#include <QMetaType>
#include <QString>

#include <array>

// Value-type theme exposed to QML as App.theme.<prop>. A Q_GADGET (not a
// QObject) so it can be returned by value from App's `theme` property; QML reads
// its sub-properties directly. App drives it from the stored theme_type +
// theme_preset settings (see App::applyThemePreset / App::setThemeType).
class Theme {
  Q_GADGET
  Q_PROPERTY(QString type READ type)
  Q_PROPERTY(QString primaryColor READ primaryColor)
  Q_PROPERTY(QString primaryBgColor READ primaryBgColor)
  Q_PROPERTY(QString materialAccent READ materialAccent)

 public:
  struct Preset {
    QString primaryColor;   // toolbar foreground
    QString darkBgColor;    // toolbar background (dark theme)
    QString lightBgColor;   // toolbar background (light theme)
    QString materialAccent; // Material accent / swatch
  };
  static const std::array<Preset, 8> presets;

  QString type() const { return m_type; }
  QString primaryColor() const { return m_primaryColor; }
  QString primaryBgColor() const { return m_primaryBgColor; }
  QString materialAccent() const { return m_materialAccent; }

  void setType(const QString& v) { m_type = v; }
  void setPrimaryColor(const QString& v) { m_primaryColor = v; }
  void setPrimaryBgColor(const QString& v) { m_primaryBgColor = v; }
  void setMaterialAccent(const QString& v) { m_materialAccent = v; }

 private:
  QString m_type = "dark";
  QString m_primaryColor = "#FFFFFFFF";
  QString m_primaryBgColor = "#FF673AB7";
  QString m_materialAccent = "#FF673AB7";
};

Q_DECLARE_METATYPE(Theme)

#endif  // THEME_H
