#include "apputils.h"

#include <QUrl>

AppUtils::AppUtils() {}

// QML's FileDialog hands paths back as file: URLs; the native pickers, the
// database location and the settings hand back bare paths. Convert only the
// former, and through QUrl rather than by string surgery:
//
//   - the old replace() stripped EVERY "file:///" occurrence rather than the
//     prefix, so a path containing that sequence anywhere was mangled;
//   - a Windows UNC url ("file://server/share/patch.json") matched neither the
//     two- nor the three-slash spelling, and reached QFile with its scheme
//     still attached;
//   - and nothing percent-decoded, so a file picked from a folder with a space
//     in its name arrived as ".../My%20Patches/..." and simply failed to open.
//
// A bare path is returned untouched: QUrl parses "C:/dev/x.db" with a scheme of
// "c", which isLocalFile() correctly refuses.
QString AppUtils::getFilePathFromCanonical(const QString& fileFullPath) {
  const QUrl url{fileFullPath};
  if (url.isLocalFile()) return url.toLocalFile();
  return fileFullPath;
}
