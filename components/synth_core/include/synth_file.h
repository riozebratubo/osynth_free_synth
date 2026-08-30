/*
 * osynth — atomically replacing one file with another.
 *
 * Five writers in this tree save by building a temporary file and putting it
 * over the live one: the three preset writers, the looper's slot save and its
 * take export. The pattern is what makes a write all-or-nothing — a power cut
 * mid-save leaves the old file intact rather than half a new one.
 *
 * On every POSIX filesystem, and on the LittleFS and FAT the firmware uses,
 * rename() is exactly that operation. On Windows it is not: the C runtime's
 * rename() *fails* if the destination exists, so the pattern worked once per
 * file and every save afterwards failed — reported as "filesystem full?" on a
 * disk with hundreds of gigabytes free.
 *
 * Hence a name of its own. Calling it at the five sites says what is wanted
 * ("replace") rather than what one platform happens to spell it, and leaves
 * the difference in one place instead of five.
 *
 * Not a macro over rename(): that was tried, and a macro by that name rewrites
 * the *declarations* in <corecrt_io.h> and <filesystem>, which both declare a
 * rename of their own.
 */
#pragma once

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
/* MoveFileEx with MOVEFILE_REPLACE_EXISTING; see port/host/src/esp_sys_host.cpp.
 * Deliberately not remove-then-rename, which opens exactly the window the
 * temporary file exists to close. */
int synth_replace_file(const char* tmp, const char* path);
#else
/* 0 on success, like rename() itself — which is what every caller tests. */
static inline int synth_replace_file(const char* tmp, const char* path) {
    return rename(tmp, path);
}
#endif

#ifdef __cplusplus
}
#endif
