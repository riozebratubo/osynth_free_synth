#ifndef TRANSLATOR_H
#define TRANSLATOR_H

#include <QHash>
#include <QObject>
#include <QString>

class Translator : public QObject {
  Q_OBJECT

 public:
  static Translator& instance();

  Q_INVOKABLE void setActiveLanguage(const QString& language);
  Q_INVOKABLE QString t(const QString& str);
  Q_INVOKABLE QString ts(const QString& str,
                         const QString& param1,
                         const QString& param2 = "",
                         const QString& param3 = "");

 signals:

 public slots:

 private:
  Translator();

  QHash<QString, int> languagesMap;
  QVector<QHash<QString, QString>> translations;
  QHash<QString, QString> dummyTranslations;
  QHash<QString, QString>& currentTranslations;
};

#endif  // TRANSLATOR_H
