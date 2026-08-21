#!/usr/bin/env python3
"""Find broken C/C++ block comments — the sibling of tools/c_brace_check.py.

Why this exists (S38): editing the *text* of a long block comment is easy to
get wrong in a way nothing catches until the compiler does, and then reports
from the wrong place. Appending paragraphs to a comment whose `*/` was left in
the middle leaves the tail as code:

    /* ... first paragraph. */
     *
     * second paragraph — now parsed as an expression
     */

GCC answers that with "stray '`'", "extended character is not valid in an
identifier", and a pile of undeclared-variable errors for the *real* code that
follows, because the statements it could not parse are discarded. None of it
points at the comment. c_brace_check.py strips comments before counting, so it
calls the file balanced.

Three findings, all of them unambiguous:
  * `*/` outside a comment
  * end of file inside a block comment
  * an orphan continuation line (` * ...`) outside a comment — the signature
    of the mistake above

Usage:  python tools/check_comment_blocks.py <file> [<file>...]
Exit 0 when everything is well formed, 1 otherwise.
"""
import re
import sys


def scan(path):
    try:
        src = open(path, encoding="utf-8").read()
    except OSError as exc:
        return [(0, f"cannot read: {exc}")]

    bad = []
    i, n = 0, len(src)
    line = 1
    in_block = False
    block_start = 0
    # Which lines are inside a block comment, so the continuation-line pass
    # below can tell an orphan from a legitimate middle-of-comment line.
    inside = set()

    while i < n:
        ch = src[i]
        nxt = src[i + 1] if i + 1 < n else ""

        if ch == "\n":
            line += 1
            i += 1
            if in_block:
                inside.add(line)
            continue

        if in_block:
            if ch == "*" and nxt == "/":
                in_block = False
                i += 2
                continue
            i += 1
            continue

        # Not in a block comment: skip the things that can contain */ legally.
        if ch == "/" and nxt == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue
        if ch == "/" and nxt == "*":
            in_block = True
            block_start = line
            inside.add(line)
            i += 2
            continue
        if ch in "\"'":
            quote = ch
            i += 1
            while i < n and src[i] != quote:
                if src[i] == "\\":
                    i += 1
                elif src[i] == "\n":  # unterminated literal; let the compiler say so
                    break
                i += 1
            i += 1
            continue
        if ch == "*" and nxt == "/":
            bad.append((line, "stray '*/' outside a comment"))
            i += 2
            continue
        i += 1

    if in_block:
        bad.append((block_start, "block comment opened here is never closed"))

    # Orphan continuation lines: ` * text` or a bare ` *` that is not inside a
    # comment.
    #
    # One legitimate shape looks identical and has to be excluded: a
    # backslash-continued macro whose next line begins with a multiplication,
    # which usb_descriptors.h does —
    #
    #     #define OSYNTH_USB_BYTES_PER_MS (OSYNTH_USB_SAMPLE_RATE / 1000 \
    #                                      * OSYNTH_USB_CHANNELS * ...)
    #
    # so a line is only suspect when the one before it is not continued.
    lines = src.splitlines()
    for num, text in enumerate(lines, start=1):
        if num in inside:
            continue
        prev = lines[num - 2] if num >= 2 else ""
        if prev.rstrip().endswith("\\"):
            continue
        if re.match(r"^\s+\*(\s|$)", text):
            bad.append((num, "orphan comment continuation line outside a comment"))

    return bad


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    failed = 0
    for path in argv:
        bad = scan(path)
        if bad:
            failed = 1
            for num, why in sorted(bad):
                print(f"{path}:{num}: {why}")
        else:
            print(f"{path}: comments ok")
    print("" if failed else "\nall files well formed")
    return failed


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
