#ifndef NATIVEFILEDIALOG_H
#define NATIVEFILEDIALOG_H

#include <QString>

// Thin wrapper over the vendored Native File Dialog (nfd/), so the app can raise
// the platform's own open/save dialogs instead of Qt's non-native ones.
//
// Only compiled with a backend on Windows (IFileDialog) and Linux (zenity) --
// see the nfd block in CMakeLists.txt. Everywhere else isAvailable() answers
// false and the caller is expected to keep using its Qt FileDialog: Android has
// no picker to drive (it uses the Storage Access Framework flows in App), and
// macOS/iOS were left on Qt deliberately.
//
// Both calls are blocking and modal, like the native dialogs they wrap.
namespace NativeFileDialog {

// False when no backend was compiled in, and -- on Linux -- when the zenity
// binary the backend shells out to is not installed. Cheap to call repeatedly:
// the zenity lookup happens once.
bool isAvailable();

// Answer the chosen path, or an empty string if the user cancelled or the
// dialog could not be shown (the two are not distinguished: a dialog that fails
// to open leaves nothing selected either way).
//
// `filters` is nfd's own format -- extensions without dots, "," between the
// extensions of one filter and ";" between filters ("json", "png,jpg;pdf"). An
// "all files" entry is appended by nfd itself. `defaultPath` takes either a
// plain path or the file: URL QML hands over, and may name either the folder to
// open in or a file to pre-select.
QString openFile(const QString& filters, const QString& defaultPath);

// `defaultSuffix` (without the dot) is appended when the user types a name with
// no extension at all, which is what FileDialog.defaultSuffix used to do.
QString saveFile(const QString& filters, const QString& defaultPath,
                 const QString& defaultSuffix = QString());

}  // namespace NativeFileDialog

#endif  // NATIVEFILEDIALOG_H
