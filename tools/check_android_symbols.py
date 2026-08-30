#!/usr/bin/env python3
"""Find symbols libosynth_core.a references but nothing will define.

Why this exists: a static archive links happily with unresolved symbols. The
Android osynth_core built clean for weeks while referencing two symbols that did
not exist -- `_binary_drumkit_bin_start`/`_end`, from a #if that keyed on the
compiler instead of the build -- and nothing said a word until the app tried to
make a shared object out of it. The MSVC side has check_host_symbols.py for the
same reason; this is its NDK counterpart.

Method: undefined minus defined within the archive, then minus everything the
NDK sysroot and the C++ runtime export. What is left has to be provided by the
target that links the archive, and every one of those is listed below by name.
An unexpected name is a link error waiting for someone to build the app.

Usage: python tools/check_android_symbols.py [--abi arm64-v8a] [--build DIR]
"""
import argparse
import pathlib
import re
import subprocess
import sys

NDK = pathlib.Path("C:/Android/Sdk/ndk/27.2.12479018")
BIN = NDK / "toolchains/llvm/prebuilt/windows-x86_64/bin"
NM = BIN / "llvm-nm.exe"

# API level and arch dirs under the sysroot, per ABI.
ABI_TRIPLE = {
    "arm64-v8a": "aarch64-linux-android",
    "armeabi-v7a": "arm-linux-androideabi",
    "x86_64": "x86_64-linux-android",
    "x86": "i686-linux-android",
}

# compiler-rt names its archives by architecture, and the name is neither the
# ABI nor a substring of the triple -- armeabi-v7a's triple is
# arm-linux-androideabi but its builtins are libclang_rt.builtins-arm-android.a.
# Deriving it by string surgery got that wrong and made every __aeabi_ helper
# look unresolved, so the mapping is spelled out.
ABI_RT_ARCH = {
    "arm64-v8a": "aarch64",
    "armeabi-v7a": "arm",
    "x86_64": "x86_64",
    "x86": "i386",
}

# Symbols the *app* target defines and the archive calls into. Each is a real
# seam, not an accident, so each is named rather than pattern-matched.
EXPECTED_FROM_APP = {
    # The host port: the sink, the source, NVS, logging, paths, the shims.
    # (These live in the app target on Android because port/host/src is
    # compiled there, not into osynth_core.)
}

def nm(args, tolerant=False):
    out = subprocess.run([str(NM)] + args, capture_output=True, text=True)
    if out.returncode != 0:
        # Some sysroot entries are linker scripts, not archives. Reading what
        # can be read and moving on is right: a symbol wrongly listed as
        # unmet is noise a human resolves in seconds, and the alternative --
        # bailing out -- means the check never runs at all.
        if tolerant:
            return out.stdout
        sys.exit(f"llvm-nm failed: {out.stderr.strip()}")
    return out.stdout

def syms(path, undefined, tolerant=False):
    flag = "--undefined-only" if undefined else "--defined-only"
    names = set()
    for line in nm([flag, "--no-sort", str(path)], tolerant).splitlines():
        line = line.strip()
        if not line or line.endswith(":"):
            continue
        parts = line.split()
        if undefined:
            # "                 U name"  (or "U name")
            if parts[0] == "U" or (len(parts) > 1 and parts[-2] == "U"):
                names.add(parts[-1])
        else:
            if len(parts) >= 2 and parts[-2] not in ("U", "u"):
                names.add(parts[-1])
    return names

def dynsyms(path):
    names = set()
    out = nm(["--dynamic", "--defined-only", "--no-sort", str(path)], True)
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            names.add(parts[-1])
    return names

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--abi", default="arm64-v8a", choices=sorted(ABI_TRIPLE))
    ap.add_argument("--build", default=None)
    ap.add_argument("--api", default="24")
    args = ap.parse_args()

    build = args.build or (
        f"app_osyntho/build/Qt_6_11_0_for_Android_{args.abi.replace('-', '_')}-Debug"
    )
    archive = pathlib.Path(build) / "osynth_core" / "libosynth_core.a"
    if not archive.exists():
        # The multi-abi dirs name the abi differently.
        alt = list(pathlib.Path("app_osyntho/build").glob(
            f"*{args.abi}*/osynth_core/libosynth_core.a"))
        if not alt:
            sys.exit(f"no archive at {archive} (build the Android target first)")
        archive = alt[0]

    print(f"archive : {archive}")
    undef = syms(archive, True)
    defined = syms(archive, False)
    missing = undef - defined
    print(f"          {len(undef)} undefined, {len(defined)} defined, "
          f"{len(missing)} unresolved within the archive")

    triple = ABI_TRIPLE[args.abi]
    llvm = NDK / "toolchains/llvm/prebuilt/windows-x86_64"
    sysroot = llvm / "sysroot/usr/lib" / triple
    provided = set()
    libdirs = [sysroot / args.api, sysroot]
    for d in libdirs:
        if not d.is_dir():
            continue
        for so in sorted(d.glob("*.so")):
            provided |= dynsyms(so)
        for a in sorted(d.glob("*.a")):
            provided |= syms(a, False, tolerant=True)
        # crtbegin defines __dso_handle. Clang links these implicitly, so a
        # reference to one is not a missing symbol.
        for o in sorted(d.glob("crt*.o")):
            provided |= syms(o, False, tolerant=True)
    print(f"sysroot : {sysroot} -> {len(provided)} exported symbols")

    # compiler-rt: the outline atomics (__aarch64_swp4_acq and friends, which
    # every std::atomic on ARM64 goes through) and the soft-float long-double
    # helpers (__multf3, __eqtf2) live here, not in the sysroot. Clang links
    # this archive implicitly too, and leaving it out made 35 perfectly normal
    # builtins look like findings and buried the one that was real.
    arch = ABI_RT_ARCH[args.abi]
    rtdir = llvm / "lib/clang"
    rt = list(rtdir.glob(f"*/lib/linux/libclang_rt.builtins-{arch}-android.a"))
    # libatomic and libunwind sit in a per-arch subdirectory. libunwind defines
    # _Unwind_Resume, which every function with a destructor and a call that can
    # throw references; both are linked implicitly.
    rt += list(rtdir.glob(f"*/lib/linux/{arch}/libatomic.a"))
    rt += list(rtdir.glob(f"*/lib/linux/{arch}/libunwind.a"))
    if not rt:
        sys.exit(f"no compiler-rt builtins for {args.abi} under {rtdir} -- the "
                 f"NDK layout changed, and without them this check is noise")
    for a in rt:
        provided |= syms(a, False, tolerant=True)
    print(f"builtins: {len(rt)} archive(s) -> {len(provided)} total")

    unmet = sorted(s for s in missing if s not in provided)
    unmet = [s for s in unmet if s not in EXPECTED_FROM_APP]

    if not unmet:
        print("\nOK: every symbol resolves in the archive or the NDK sysroot.")
        return 0

    print(f"\n{len(unmet)} symbol(s) nothing in the archive or the platform defines:")
    refs = nm(["--undefined-only", "--print-file-name", "--no-sort", str(archive)],
              True).splitlines()
    for s in unmet:
        where = sorted({l.split(":")[1].strip() for l in refs
                        if l.rstrip().endswith(" " + s)})
        print(f"  {s}")
        if where:
            print(f"      referenced by {', '.join(where[:4])}"
                  + (" ..." if len(where) > 4 else ""))
    print("\nEach must be defined by the target that links osynth_core, or it is a bug.")
    return 1

if __name__ == "__main__":
    sys.exit(main())
