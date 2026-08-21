#!/usr/bin/env python3
"""S37 mic-input change: check #if/#endif nesting and Kconfig choice pairing.

A cheap structural check for the files touched by the microphone-input work,
for use before handing a build over. It does not compile anything — it only
proves that every conditional block opened is closed, and at the depth it was
opened at, which is the failure mode a large #if-guarded edit actually has.

Usage:  python tools/s37_check_ppdirectives.py
Exit code is non-zero if anything is unbalanced.
"""
import io
import re
import sys

C_FILES = [
    "components/audio_io/audio_io.cpp",
    "components/audio_io/source_mic.cpp",
    "components/audio_io/audio_sink.h",
    "components/audio_io/include/audio_io.h",
    "components/synth_core/include/synth_config.h",
    "components/synth_core/include/synth_params.h",
    "main/main.cpp",
]
KCONFIG_FILES = ["components/synth_core/Kconfig.projbuild"]

OPEN = re.compile(r"^\s*#\s*(if|ifdef|ifndef)\b")
MID = re.compile(r"^\s*#\s*(else|elif)\b")
CLOSE = re.compile(r"^\s*#\s*endif\b")


def check_c(path):
    depth = 0
    stack = []
    problems = []
    for n, line in enumerate(io.open(path, encoding="utf-8"), 1):
        if OPEN.match(line):
            stack.append((n, line.strip()))
            depth += 1
        elif CLOSE.match(line):
            depth -= 1
            if depth < 0:
                problems.append("%s:%d  #endif with no matching #if" % (path, n))
                depth = 0
            else:
                stack.pop()
        elif MID.match(line) and depth == 0:
            problems.append("%s:%d  #else/#elif outside any #if" % (path, n))
    for n, text in stack:
        problems.append("%s:%d  unclosed %s" % (path, n, text[:60]))
    return problems


def check_kconfig(path):
    """choice/endchoice, menu/endmenu, if/endif pairing.

    Help text has to be skipped or this reads prose as syntax: a help line
    beginning "if the line output is..." is not an `if` block, and the first
    version of this script duly reported the file as broken.  A help block runs
    from the `help` keyword until the next non-blank line indented no further
    than the keyword itself, which is Kconfig's own rule for it.
    """
    pairs = {"choice": "endchoice", "menu": "endmenu", "if": "endif"}
    closers = set(pairs.values())
    stack = []
    problems = []
    help_indent = None
    for n, line in enumerate(io.open(path, encoding="utf-8"), 1):
        stripped = line.strip()
        indent = len(line) - len(line.lstrip())
        if help_indent is not None:
            if stripped and indent <= help_indent:
                help_indent = None  # fall through and read this line normally
            else:
                continue
        if stripped in ("help", "---help---"):
            help_indent = indent
            continue
        head = stripped.split(" ")[0].split("\t")[0]
        if head in pairs and not stripped.startswith("#"):
            stack.append((n, head))
        elif head in closers:
            if not stack:
                problems.append("%s:%d  %s with nothing open" % (path, n, head))
            elif pairs[stack[-1][1]] != head:
                problems.append(
                    "%s:%d  %s closes a %s opened at line %d"
                    % (path, n, head, stack[-1][1], stack[-1][0])
                )
                stack.pop()
            else:
                stack.pop()
    for n, head in stack:
        problems.append("%s:%d  unclosed %s" % (path, n, head))
    return problems


def main():
    problems = []
    for p in C_FILES:
        problems += check_c(p)
    for p in KCONFIG_FILES:
        problems += check_kconfig(p)
    if problems:
        for p in problems:
            print("FAIL " + p)
        return 1
    print("ok: %d C/C++ files, %d Kconfig files balanced"
          % (len(C_FILES), len(KCONFIG_FILES)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
