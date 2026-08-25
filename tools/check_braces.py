#!/usr/bin/env python3
"""Cheap structural sanity check for C/C++ sources: balanced braces, parens
and brackets, outside strings, chars and comments.

Not a parser and not a substitute for the compiler. It exists for the case the
build policy creates here -- edits are made without building -- where the most
likely mechanical mistake in a large scripted patch is an unbalanced block, and
that is worth catching in a second rather than at the next build.

Usage:  python tools/check_braces.py <file> [<file> ...]
        python tools/check_braces.py --changed      (git-modified C/C++ files)
"""
import subprocess
import sys

PAIRS = {'}': '{', ')': '(', ']': '['}
OPENERS = set(PAIRS.values())


def scan(path):
    src = open(path, encoding='utf-8', errors='replace').read()
    stack = []
    i, n = 0, len(src)
    line = 1
    errors = []
    while i < n:
        c = src[i]
        if c == '\n':
            line += 1
            i += 1
            continue
        # comments
        if c == '/' and i + 1 < n:
            if src[i + 1] == '/':
                j = src.find('\n', i)
                i = n if j < 0 else j
                continue
            if src[i + 1] == '*':
                j = src.find('*/', i + 2)
                if j < 0:
                    errors.append((line, 'unterminated /* comment'))
                    break
                line += src.count('\n', i, j)
                i = j + 2
                continue
        # strings and chars
        if c in '"\'':
            quote = c
            i += 1
            while i < n:
                if src[i] == '\\':
                    i += 2
                    continue
                if src[i] == quote:
                    i += 1
                    break
                if src[i] == '\n':
                    line += 1
                i += 1
            continue
        if c in OPENERS:
            stack.append((c, line))
        elif c in PAIRS:
            if not stack:
                errors.append((line, 'unmatched %s' % c))
            elif stack[-1][0] != PAIRS[c]:
                errors.append((line, 'expected closer for %s opened at line %d, got %s'
                               % (stack[-1][0], stack[-1][1], c)))
                stack.pop()
            else:
                stack.pop()
        i += 1
    for ch, ln in stack:
        errors.append((ln, 'unclosed %s' % ch))
    return errors


def main(argv):
    files = argv[1:]
    if files[:1] == ['--changed']:
        out = subprocess.run(['git', 'status', '--porcelain'],
                             capture_output=True, text=True).stdout
        files = []
        for row in out.splitlines():
            path = row[3:].strip().strip('"')
            if path.endswith(('.c', '.cpp', '.h', '.hpp')):
                files.append(path)
    bad = 0
    for f in files:
        errs = scan(f)
        if errs:
            bad += 1
            print('%s:' % f)
            for ln, msg in errs[:10]:
                print('  line %d: %s' % (ln, msg))
        else:
            print('%s: balanced' % f)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
