/*
 * osynth — byte-exact struct packing for the on-disk and on-wire formats.
 *
 * Seven structs in this tree describe bytes that outlive the process: preset
 * files, the working state, the persisted settings blob, and the legacy S12
 * sequence file. All of them were written as `struct __attribute__((packed))`,
 * which is correct for GCC and Clang — the compilers that build all three ESP
 * targets and the Apple and Android hosts.
 *
 * MSVC does not understand that attribute, and it does not warn: it parses the
 * declaration as something else entirely, or ignores the packing and inserts
 * padding. Either way the struct silently changes size, and a preset written by
 * one build stops loading in the other. That is a data bug wearing the clothes
 * of a build that succeeded, which is the worst shape a portability problem
 * can take.
 *
 * So the packing is spelled through these three macros instead:
 *
 *     OSYNTH_PACK_PUSH
 *     struct OSYNTH_PACKED Header { ... };
 *     static_assert(sizeof(Header) == 8, "on-disk layout");
 *     OSYNTH_PACK_POP
 *
 * On GCC and Clang the push/pop vanish and OSYNTH_PACKED is the attribute, so
 * every ESP target compiles exactly what it compiled before. On MSVC the
 * attribute vanishes and `#pragma pack(1)` does the work.
 *
 * The static_assert is not decoration and is not optional. It is the only
 * thing standing between a mispacked struct and a corrupt file, it costs
 * nothing, and it is what turned this into a build error rather than a support
 * question. Every struct wrapped in these macros must carry one.
 */
#pragma once

#if defined(_MSC_VER)
#define OSYNTH_PACK_PUSH __pragma(pack(push, 1))
#define OSYNTH_PACK_POP  __pragma(pack(pop))
#define OSYNTH_PACKED
#else
#define OSYNTH_PACK_PUSH
#define OSYNTH_PACK_POP
#define OSYNTH_PACKED __attribute__((packed))
#endif
