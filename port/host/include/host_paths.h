/*
 * osynth host port — where persistent data lives.
 *
 * The firmware has three storage areas backed by three different mechanisms:
 * NVS for settings, a LittleFS partition for presets and the working state,
 * and either a raw flash region or an SD card for looper takes. A host has one
 * filesystem, so all three land in subdirectories of one root.
 *
 * The root is settable, and has to be. The default below is right for a
 * desktop and wrong everywhere else: on Android and iOS an app may only write
 * inside the sandbox the OS hands it, which the app learns at runtime from
 * QStandardPaths and nothing here can guess. So the embedding app sets it
 * before bringing the engine up, and the default is what a standalone
 * desktop build gets if nobody does.
 *
 * Nothing here creates the root eagerly. It is created on first use by
 * osynth_host_subdir(), so a build that never persists anything never leaves a
 * directory behind.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Overrides the data root. Call before any component initialises; a later call
 * has no effect on paths already resolved. Passing NULL or an empty string
 * restores the platform default. */
void osynth_host_set_data_dir(const char* path);

/* The data root, without a trailing separator. Never NULL.
 *
 * Default per platform:
 *   Windows  %LOCALAPPDATA%\osynth
 *   macOS    ~/Library/Application Support/osynth
 *   Linux    $XDG_DATA_HOME/osynth, else ~/.local/share/osynth
 * and the working directory as a last resort, if the environment says nothing.
 */
const char* osynth_host_data_dir(void);

/* Resolves `<root>/<name>` into `out`, creating it (and the root) if needed.
 * Returns false and leaves `out` empty if it could not be created -- every
 * caller treats that the way the firmware treats a failed mount: the feature
 * degrades to "no storage" rather than failing to boot.
 *
 * `name` may be NULL or empty for the root itself. */
bool osynth_host_subdir(const char* name, char* out, size_t out_len);

#ifdef __cplusplus
}
#endif
