#!/usr/bin/env python3
"""Balance check for C/C++ sources — the sibling of tools/qml_brace_check.py.

Not a parser and not a substitute for the compiler: it strips comments,
string and character literals, then verifies that (), [] and {} nest and
close. Its whole job is to catch the one class of mistake a hand-edited
region can carry into a build that then fails a hundred lines later with an
unhelpful message.

Usage:  python tools/c_brace_check.py <file> [<file>...]
Exit 0 when everything balances, 1 otherwise.
"""
import sys

PAIRS = {')': '(', ']': '[', '}': '{'}
OPENERS = set(PAIRS.values())


def strip(src: str) -> str:
    """Blank out comments and literals, preserving newlines for line numbers."""
    out = []
    i, n = 0, len(src)
    line_comment = block_comment = False
    quote = None  # "'" or '"' while inside a literal
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ''
        if line_comment:
            if c == '\n':
                line_comment = False
                out.append(c)
            else:
                out.append(' ')
            i += 1
        elif block_comment:
            if c == '*' and nxt == '/':
                block_comment = False
                out.append('  ')
                i += 2
            else:
                out.append(c if c == '\n' else ' ')
                i += 1
        elif quote:
            if c == '\\':
                out.append('  ')
                i += 2
                continue
            if c == quote:
                quote = None
            out.append(c if c == '\n' else ' ')
            i += 1
        elif c == '/' and nxt == '/':
            line_comment = True
            out.append('  ')
            i += 2
        elif c == '/' and nxt == '*':
            block_comment = True
            out.append('  ')
            i += 2
        elif c in '"\'':
            quote = c
            out.append(' ')
            i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def check(path: str) -> bool:
    with open(path, 'r', encoding='utf-8', errors='replace') as fh:
        src = fh.read()
    stack = []
    line = 1
    for ch in strip(src):
        if ch == '\n':
            line += 1
        elif ch in OPENERS:
            stack.append((ch, line))
        elif ch in PAIRS:
            if not stack:
                print(f'{path}:{line}: unmatched closing {ch!r}')
                return False
            want = PAIRS[ch]
            got, at = stack.pop()
            if got != want:
                print(f'{path}:{line}: {ch!r} closes {got!r} opened at line {at}')
                return False
    if stack:
        got, at = stack[-1]
        print(f'{path}: unclosed {got!r} opened at line {at}')
        return False
    print(f'{path}: balanced')
    return True


def main(argv) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    return 0 if all(check(p) for p in argv[1:]) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
