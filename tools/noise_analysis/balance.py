"""Cheap bracket-balance check on a C++ file, comment/string aware.
Not a compiler - just catches a botched textual patch before a build."""
import io, sys

BS = chr(92)   # backslash, kept out of the literals below


def strip(s):
    out = []
    i, n, st = 0, len(s), 0
    while i < n:
        c = s[i]
        if st == 0:
            if s.startswith('/*', i):
                st = 1; i += 2; continue
            if s.startswith('//', i):
                st = 2; i += 2; continue
            if c == '"':
                st = 3; i += 1; continue
            if c == "'":
                st = 4; i += 1; continue
            out.append(c)
        elif st == 1:
            if s.startswith('*/', i):
                st = 0; i += 2; continue
        elif st == 2:
            if c == chr(10):
                st = 0; out.append(c)
        elif st == 3:
            if c == BS:
                i += 2; continue
            if c == '"':
                st = 0
        elif st == 4:
            if c == BS:
                i += 2; continue
            if c == "'":
                st = 0
        i += 1
    return ''.join(out)


t = strip(io.open(sys.argv[1], encoding='utf-8').read())
bad = False
for a, b, nm in [('{', '}', 'brace'), ('(', ')', 'paren'), ('[', ']', 'bracket')]:
    ok = t.count(a) == t.count(b)
    bad |= not ok
    print(f"  {nm:8s} {t.count(a):5d} {t.count(b):5d}  {'OK' if ok else 'MISMATCH'}")
# running depth must never go negative and must end at zero
d = 0
for ch in t:
    if ch == '{':
        d += 1
    elif ch == '}':
        d -= 1
        if d < 0:
            print('  depth went negative'); bad = True; break
print(f"  final brace depth {d}  {'OK' if d == 0 else 'BAD'}")
sys.exit(1 if bad or d else 0)
