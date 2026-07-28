#include "apputils.h"

AppUtils::AppUtils() {}

QString AppUtils::getFilePathFromCanonical(const QString& fileFullPath) {
  QString where = fileFullPath;
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
  where.replace("file://", "");
#else
  where.replace("file:///", "");
#endif
  return where;
}
