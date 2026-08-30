/*
 * osynth host port — strings.h
 *
 * POSIX puts strcasecmp() here; MSVC has no such header and spells the
 * function _stricmp() in <string.h>. drum_kit.cpp includes <strings.h>
 * unconditionally and uses strcasecmp() in three places, so this shims the
 * header by name exactly as the esp_* headers beside it do.
 *
 * On a POSIX host the real header is still what is wanted -- this directory is
 * first on the include path, so without the #include_next below this shim
 * would shadow it and quietly remove every other declaration it carries.
 */
#pragma once

#if defined(_MSC_VER)

#include <string.h>

#define strcasecmp  _stricmp
#define strncasecmp _strnicmp

#else

/* Hand back to the platform's own <strings.h>, which this file is shadowing
 * only because it sits earlier on the include path. */
#include_next <strings.h>

#endif
