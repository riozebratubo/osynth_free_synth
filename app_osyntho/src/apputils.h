#ifndef APPUTILS_H
#define APPUTILS_H

#include <QString>

class AppUtils {
 public:
  AppUtils();

  static QString getFilePathFromCanonical(const QString& fileFullPath);
};

#endif  // APPUTILS_H
