/*
 * osynth host port — unistd.h
 *
 * MSVC has no such header. One caller: drum_kit.cpp deletes a kit's files with
 * unlink(), which MSVC spells _unlink() in <io.h>.
 *
 * Only what is used is here, as with dirent.h and strings.h beside it.
 * Implementing the rest of unistd would be inventing a contract nothing tests,
 * and on Windows most of it has no honest answer anyway.
 *
 * On a POSIX host this hands straight back to the platform header; it exists
 * only because this directory is first on the include path.
 */
#pragma once

#if !defined(_MSC_VER)

#include_next <unistd.h>

#else

#include <io.h>

/* Same signature and the same 0/-1 return, which is what the caller tests.
 * Not remove(): that also removes empty directories, and unlink() is a
 * statement that the target is a file. */
#ifndef unlink
#define unlink(path) _unlink(path)
#endif

#endif /* _MSC_VER */
