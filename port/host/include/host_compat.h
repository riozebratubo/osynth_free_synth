/*
 * osynth host port — MSVC compatibility, force-included.
 *
 * The firmware is built by GCC on all three ESP targets and by Clang on the
 * Apple and Android hosts, so it uses GCC spellings freely. MSVC is the one
 * toolchain in the set that does not know them. This header is injected ahead
 * of every translation unit (`/FI`, see port/host/CMakeLists.txt) so those
 * spellings resolve without a single #ifdef appearing in the DSP sources.
 *
 * It is force-included rather than #included from somewhere central because
 * the identifiers below appear in headers that no ordinary include order can
 * be relied on to reach first -- synth_smooth.h uses SYNTH_UNLIKELY, and
 * `__restrict__` appears in 70 declarations across a dozen files.
 *
 * Everything here is a no-op on GCC and Clang, which take this header too so
 * that all four toolchains see the same translation units.
 *
 * What is deliberately NOT here: __attribute__((vector_size)) and
 * __builtin_convertvector. Those need real types rather than a rename, so
 * synth_simd.h carries its own MSVC branch -- see the note there.
 */
#pragma once

#ifdef _MSC_VER

/* MSVC spells the same qualifier without the trailing underscores. It means
 * what GCC's means: these pointers do not alias, which is what lets the
 * 4-wide kernels hoist their loads. */
#ifndef __restrict__
#define __restrict__ __restrict
#endif

/* synth_config.h guards its own definitions with `#ifndef SYNTH_LIKELY`, so
 * defining them here suppresses the __builtin_expect versions before that
 * header is ever reached.
 *
 * MSVC has no branch-hint builtin, and C++20's [[likely]] is a statement
 * attribute that cannot sit inside `if (SYNTH_LIKELY(x))`. So the hint is
 * dropped and only the value is kept. That costs a branch prediction hint in
 * the smoothers and nothing else: every use in the tree is a hint, never a
 * correctness claim. */
#ifndef SYNTH_LIKELY
#define SYNTH_LIKELY(x)   (!!(x))
#define SYNTH_UNLIKELY(x) (!!(x))
#endif

/* strlcpy() -- BSD, provided by newlib on the ESP targets and by the Apple and
 * Android libcs, absent on MSVC. Twenty call sites in drums and the sampler,
 * all of them copying a name into a fixed-size field.
 *
 * The full contract is implemented rather than the part currently used: it
 * always NUL-terminates when size > 0, truncates rather than overflowing, and
 * returns the length of `src` so a caller can detect truncation. None of the
 * twenty check the return today, but a half-implemented standard function is
 * a trap for the twenty-first.
 *
 * Not strncpy_s or strcpy_s: those have different truncation behaviour (and by
 * default invoke the invalid-parameter handler rather than truncating), which
 * would turn a long kit name into a crash instead of a short name.
 *
 * static inline, so every translation unit that needs it gets its own copy and
 * none that does not pays for it. */
/* __builtin_ctz -- count trailing zeros. One use, in the protocol's event
 * flush: the dirty-parameter bitmap is walked a set bit at a time, and this is
 * how each bit's index is found.
 *
 * MSVC spells it _BitScanForward. Both are undefined for an input of zero, and
 * the caller never passes one (it loops while bits != 0), so the shim keeps
 * that contract rather than inventing a safer one that would hide a future
 * caller getting it wrong.
 *
 * Declared as a function rather than a macro so it cannot capture an
 * unparenthesised argument, and so the name still resolves if something takes
 * its address. */
#ifndef __builtin_ctz
#include <intrin.h>

static inline int __builtin_ctz(unsigned int x) {
    unsigned long index;
    _BitScanForward(&index, (unsigned long)x);
    return (int)index;
}
#endif

/* __builtin_popcount -- population count. Three uses in the looper, all of
 * them counting how many of the eight tracks a bitmask has set.
 *
 * __popcnt is an SSE4.2 instruction intrinsic and faults on a CPU without it.
 * That is fine for the callers here (none is in the render path, and every
 * x64 CPU this port targets has SSE4.2), but the fallback is written out
 * anyway rather than assuming: it is four instructions on a cold path. */
#ifndef __builtin_popcount
#include <intrin.h>
#include <isa_availability.h>

static inline int __builtin_popcount(unsigned int x) {
    int n = 0;
    while (x) {
        x &= x - 1;
        ++n;
    }
    return n;
}
#endif

/* POSIX file-mode helpers and mkdir(). MSVC has the underscore-prefixed
 * equivalents in <sys/stat.h> and <direct.h> but neither the macros nor the
 * two-argument mkdir. The mode argument is dropped, which is what it means on
 * Windows: permissions come from the parent directory's ACL, and there is
 * nothing for 0775 to select. */
#include <direct.h>
#include <sys/stat.h>

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef mkdir
#define mkdir(path, mode) _mkdir(path)
#endif

#ifndef strlcpy
#include <stddef.h>
#include <string.h>

static inline size_t strlcpy(char* dst, const char* src, size_t size) {
    const size_t len = strlen(src);
    if (size != 0) {
        const size_t n = (len < size - 1) ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}
#endif

#endif /* _MSC_VER */
