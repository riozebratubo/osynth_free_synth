#!/usr/bin/env python3
"""Resolve every #include in the host port's source list against its include path.

Why this exists
---------------
The host port (port/host/) works by shadowing the ESP-IDF headers the firmware
includes -- esp_log.h, freertos/task.h, sdkconfig.h and the rest -- with shims
of the same name. The failure mode that costs the most time is a *missing*
shim: the compiler reports one "no such file" thirty seconds into a build, you
add it, and the next one appears. This walks the whole source list first and
lists them all at once.

It is a text scan, not a preprocessor. It therefore cannot tell an include
inside a disabled #if from a live one, so it reports the nesting conditions
alongside each unresolved header and leaves the judgement to a reader. An
include reported under, say, `#if SYNTH_ENABLE_MIC_IN` on a build with no
microphone is not a missing shim.

Usage:
    python tools/check_host_includes.py            # step-1 source list
    python tools/check_host_includes.py --all      # every component source
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
COMPONENTS = ROOT / "components"
PORT = ROOT / "port" / "host"

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
COND_RE = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$')

# Mirrors target_include_directories() in port/host/CMakeLists.txt. The port's
# own include dir is first for the same reason it is first there.
INCLUDE_DIRS = [
    PORT / "include",
    COMPONENTS / "synth_core" / "include",
    COMPONENTS / "engines" / "include",
    COMPONENTS / "fx" / "include",
    COMPONENTS / "fx_gpl" / "include",
    COMPONENTS / "graph" / "include",
    COMPONENTS / "drums" / "include",
    COMPONENTS / "looper" / "include",
    COMPONENTS / "seqarp" / "include",
    COMPONENTS / "chord" / "include",
    COMPONENTS / "ctrl_proto" / "include",
    COMPONENTS / "midi" / "include",
    COMPONENTS / "presets" / "include",
    COMPONENTS / "persist" / "include",
    COMPONENTS / "audio_io" / "include",
    COMPONENTS / "codec" / "include",
    # Headers only in the host build; see port/host/CMakeLists.txt.
    COMPONENTS / "usb_dev" / "include",
    COMPONENTS / "usb_host_midi" / "include",
    # audio_sink.h is private to the component, included by bare name.
    COMPONENTS / "audio_io",
]

# Generated at build time by tools/gen_wavetables.py into the build tree; there
# is nothing on disk to resolve it against before a build has run.
GENERATED = {"factory_wavetables.h"}

# The C and C++ standard library. Anything here is the toolchain's problem, not
# the port's.
STDLIB = {
    "assert.h", "ctype.h", "errno.h", "float.h", "inttypes.h", "limits.h",
    "locale.h", "math.h", "setjmp.h", "signal.h", "stdarg.h", "stdbool.h",
    "stddef.h", "stdint.h", "stdio.h", "stdlib.h", "string.h", "time.h",
    "wchar.h", "wctype.h",
    "algorithm", "array", "atomic", "bit", "cassert", "cerrno", "cfloat",
    "chrono", "cmath", "condition_variable", "cstdarg", "cstddef", "cstdint",
    "cstdio", "cstdlib", "cstring", "functional", "initializer_list",
    "limits", "memory", "mutex", "new", "numeric", "optional", "semaphore",
    "span", "string", "string_view", "thread", "type_traits", "utility",
    "vector",
    # POSIX-ish, available on every host target here (MSVC included, via its
    # CRT) -- used by the file-backed storage paths.
    "sys/stat.h", "sys/types.h", "dirent.h", "unistd.h", "fcntl.h",
}

STEP1_SOURCES = [
    "synth_core/synth_params.cpp",
    "synth_core/synth_voice.cpp",
    "synth_core/synth_dsp.cpp",
    "synth_core/synth_mod.cpp",
    "synth_core/synth_warn.cpp",
    "engines/engines.cpp",
    "engines/engine_subtractive.cpp",
    "engines/engine_additive.cpp",
    "engines/engine_fm.cpp",
    "engines/engine_wavetable.cpp",
    "engines/engine_granular.cpp",
    "engines/engine_sampler.cpp",
    "fx/fx.cpp",
    "fx/fx_reverb_wet.cpp",
    "fx/fx_fft.cpp",
    "graph/graph_model.cpp",
    "graph/graph_compile.cpp",
    "graph/graph_render.cpp",
    "graph/graph_engine.cpp",
    "seqarp/seqarp.cpp",
    "seqarp/seq_model.cpp",
    "seqarp/seq_play.cpp",
    "chord/chord.cpp",
    "midi/midi.c",
    "audio_io/audio_io.cpp",
    "audio_io/sink_null.cpp",
    "usb_host_midi/usb_mode.cpp",
    "usb_host_midi/usb_host_midi.c",
    "midi/midi_serial.c",
    "drums/drums.cpp",
    "drums/drum_kit.cpp",
    "drums/sampler.cpp",
    "ctrl_proto/ctrl_proto.cpp",
    "presets/presets.cpp",
    "presets/presets_factory.cpp",
    "persist/persist.cpp",
    "looper/looper.cpp",
    "looper/loop_store.cpp",
    "looper/loop_stream.cpp",
]


def resolve(header: str, origin: Path) -> Path | None:
    """Quote-form includes look beside the including file first, as the
    compiler does; that is how the components reach their own private headers
    (drums_priv.h, fx_fft.h, loop_store.h)."""
    local = origin.parent / header
    if local.is_file():
        return local
    for d in INCLUDE_DIRS:
        candidate = d / header
        if candidate.is_file():
            return candidate
    return None


def scan(path: Path):
    """Yield (line_no, header, condition_stack) for each include."""
    stack: list[str] = []
    text = path.read_text(encoding="utf-8", errors="replace")
    for n, line in enumerate(text.splitlines(), 1):
        cond = COND_RE.match(line)
        if cond:
            kind, rest = cond.group(1), cond.group(2).strip()
            if kind in ("if", "ifdef", "ifndef"):
                stack.append(f"#{kind} {rest}".strip())
            elif kind in ("elif", "else"):
                if stack:
                    stack[-1] = f"#{kind} {rest}".strip()
            elif kind == "endif":
                if stack:
                    stack.pop()
            continue
        inc = INCLUDE_RE.match(line)
        if inc:
            yield n, inc.group(1), list(stack)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true",
                    help="scan every component source, not just the host list")
    args = ap.parse_args()

    if args.all:
        sources = sorted(
            p for p in COMPONENTS.rglob("*")
            if p.suffix in (".c", ".cpp") and p.is_file()
        )
    else:
        sources = [COMPONENTS / s for s in STEP1_SOURCES]

    missing_dirs = [d for d in INCLUDE_DIRS if not d.is_dir()]
    if missing_dirs:
        for d in missing_dirs:
            print(f"include dir does not exist: {d}", file=sys.stderr)
        return 2

    # header -> list of (source, line, conditions)
    unresolved: dict[str, list[tuple[str, int, list[str]]]] = {}
    seen_files = 0

    for src in sources:
        if not src.is_file():
            print(f"source does not exist: {src}", file=sys.stderr)
            return 2
        seen_files += 1
        # Follow the transitive closure of quote/angle includes we can resolve,
        # so a shim missing three headers deep is still reported.
        pending = [(src, list(scan(src)))]
        visited = {src}
        while pending:
            origin, includes = pending.pop()
            for line_no, header, conds in includes:
                if header in STDLIB or header in GENERATED:
                    continue
                found = resolve(header, origin)
                if found is None:
                    rel = origin.relative_to(ROOT).as_posix()
                    unresolved.setdefault(header, []).append(
                        (rel, line_no, conds))
                    continue
                if found not in visited:
                    visited.add(found)
                    pending.append((found, list(scan(found))))

    print(f"scanned {seen_files} source files "
          f"({'all components' if args.all else 'host step-1 list'})")

    if not unresolved:
        print("all includes resolve against the host include path")
        return 0

    print(f"\n{len(unresolved)} header(s) do not resolve:\n")
    for header in sorted(unresolved):
        sites = unresolved[header]
        print(f"  {header}")
        for rel, line_no, conds in sites[:4]:
            where = f"    {rel}:{line_no}"
            if conds:
                where += "   under " + " / ".join(conds)
            print(where)
        if len(sites) > 4:
            print(f"    ... and {len(sites) - 4} more")
        print()

    print("An include reached only through a condition this build turns off is")
    print("not a missing shim -- check the `under` clause before adding one.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
