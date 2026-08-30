/*
 * osynth host port — dirent.h
 *
 * POSIX directory reading, which MSVC does not provide. Two callers:
 * presets.cpp builds its slot cache with one readdir pass, and drum_kit.cpp
 * scans for kit files.
 *
 * Only what those two use is here -- DIR, opendir, readdir, closedir, and
 * d_name. No rewinddir, no seekdir, no d_type: implementing the parts nothing
 * calls would be inventing a contract nothing tests, and d_type in particular
 * is not portable even between POSIX filesystems (both callers already stat or
 * parse the name instead, which is why they never reach for it).
 *
 * On a POSIX host this shim hands straight back to the platform header; it
 * exists only because this directory is first on the include path.
 */
#pragma once

#if !defined(_MSC_VER)

#include_next <dirent.h>

#else

#include <io.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MAX_PATH is 260, and a long name can reach it. Sized to match rather than
 * guessed, since a short buffer here truncates a filename and the callers
 * parse those names to decide which slot a file belongs to. */
struct dirent {
    char d_name[260];
};

typedef struct DIR_impl {
    intptr_t handle;         /* _findfirst/_findnext, -1 when exhausted */
    struct _finddata_t data;
    struct dirent entry;
    int first;               /* _findfirst already produced an entry */
} DIR;

static inline DIR* opendir(const char* path) {
    if (path == NULL) return NULL;
    /* _findfirst wants a wildcard, not a directory. */
    const size_t n = strlen(path);
    char* pattern = (char*)malloc(n + 3);
    if (pattern == NULL) return NULL;
    memcpy(pattern, path, n);
    size_t at = n;
    if (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/') pattern[at++] = '\\';
    pattern[at++] = '*';
    pattern[at] = '\0';

    DIR* d = (DIR*)calloc(1, sizeof(DIR));
    if (d == NULL) {
        free(pattern);
        return NULL;
    }
    d->handle = _findfirst(pattern, &d->data);
    free(pattern);
    if (d->handle == -1) { /* empty or missing: both are "cannot list" */
        free(d);
        return NULL;
    }
    d->first = 1;
    return d;
}

static inline struct dirent* readdir(DIR* d) {
    if (d == NULL || d->handle == -1) return NULL;
    if (d->first) {
        d->first = 0;
    } else if (_findnext(d->handle, &d->data) != 0) {
        return NULL;
    }
    strncpy(d->entry.d_name, d->data.name, sizeof(d->entry.d_name) - 1);
    d->entry.d_name[sizeof(d->entry.d_name) - 1] = '\0';
    return &d->entry;
}

static inline int closedir(DIR* d) {
    if (d == NULL) return -1;
    if (d->handle != -1) _findclose(d->handle);
    free(d);
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* _MSC_VER */
