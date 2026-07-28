#include "nativefiledialog.h"

#include <QDir>
#include <QFileInfo>
#include <QtSystemDetection>

#ifdef NFD_AVAILABLE
#include <QDebug>
#include <QStandardPaths>
#include <cstdlib>

#include "src/apputils.h"
#include "nfd.h"
#endif

#ifdef NFD_AVAILABLE
namespace {

// QML hands paths over as file: URLs (StandardPaths, FileDialog), while nfd
// wants a plain native one.
QString toDialogPath(const QString& path) {
  QString native = AppUtils::getFilePathFromCanonical(path);
  if (native.isEmpty()) return native;

#ifdef Q_OS_WINDOWS
  // SHCreateItemFromParsingName does not parse the forward slashes Qt uses, and
  // silently starts the dialog wherever it likes if handed them.
  return QDir::toNativeSeparators(native);
#else
  // zenity reads "--filename=/home/me/Documents" as a file named "Documents"
  // inside /home/me; the trailing separator is what makes it open the folder.
  if (not native.endsWith(QLatin1Char('/')) and QFileInfo{native}.isDir())
    native += QLatin1Char('/');
  return native;
#endif
}

// Takes ownership of an nfd result: it allocates with malloc(), and there is no
// NFD_FreePath in this version of the API.
QString takePath(nfdchar_t* out) {
  if (out == nullptr) return QString();
  const QString path = QDir::fromNativeSeparators(QString::fromUtf8(out));
  std::free(out);
  return path;
}

// nfd takes NULL, not "", to mean "no filter"/"no default path".
const nfdchar_t* orNull(const QByteArray& utf8) {
  return utf8.isEmpty() ? nullptr : utf8.constData();
}

}  // namespace
#endif

bool NativeFileDialog::isAvailable() {
#ifndef NFD_AVAILABLE
  return false;
#elif defined(Q_OS_WINDOWS)
  return true;
#else
  // The zenity backend execvp()s the binary, and without it every dialog comes
  // back NFD_ERROR having shown nothing. Report unavailable so the caller falls
  // back to its Qt dialog rather than to a picker that never opens.
  static const bool haveZenity = not QStandardPaths::findExecutable("zenity").isEmpty();
  return haveZenity;
#endif
}

QString NativeFileDialog::openFile(const QString& filters, const QString& defaultPath) {
#ifdef NFD_AVAILABLE
  if (not isAvailable()) return QString();

  const QByteArray filterList = filters.toUtf8();
  const QByteArray startPath = toDialogPath(defaultPath).toUtf8();

  nfdchar_t* out = nullptr;
  const nfdresult_t result = NFD_OpenDialog(orNull(filterList), orNull(startPath), &out);
  if (result == NFD_ERROR) qWarning() << "NativeFileDialog | open failed:" << NFD_GetError();
  if (result != NFD_OKAY) {
    // Nothing is expected in `out` unless the result was NFD_OKAY, but freeing
    // it costs nothing and a backend that fills it in anyway would leak.
    if (out != nullptr) std::free(out);
    return QString();
  }
  return takePath(out);
#else
  Q_UNUSED(filters);
  Q_UNUSED(defaultPath);
  return QString();
#endif
}

QString NativeFileDialog::saveFile(const QString& filters, const QString& defaultPath,
                                   const QString& defaultSuffix) {
#ifdef NFD_AVAILABLE
  if (not isAvailable()) return QString();

  const QByteArray filterList = filters.toUtf8();
  const QByteArray startPath = toDialogPath(defaultPath).toUtf8();

  nfdchar_t* out = nullptr;
  const nfdresult_t result = NFD_SaveDialog(orNull(filterList), orNull(startPath), &out);
  if (result == NFD_ERROR) qWarning() << "NativeFileDialog | save failed:" << NFD_GetError();
  if (result != NFD_OKAY) {
    if (out != nullptr) std::free(out);
    return QString();
  }

  QString path = takePath(out);
  // nfd applies no default extension of its own, so a name typed without one
  // would land on disk as an extension-less file. Only a name with no suffix at
  // all is completed -- "kit.old" is left as the user spelled it.
  if (not path.isEmpty() and not defaultSuffix.isEmpty() and QFileInfo{path}.suffix().isEmpty())
    path += QLatin1Char('.') + defaultSuffix;
  return path;
#else
  Q_UNUSED(filters);
  Q_UNUSED(defaultPath);
  Q_UNUSED(defaultSuffix);
  return QString();
#endif
}
