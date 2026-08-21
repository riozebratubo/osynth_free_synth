#!/usr/bin/env python3
"""S37e fixup - spell out RegVal::keep on every kInit entry.

Adding a third member to RegVal relied on aggregate initialisation zeroing it,
which is correct C++ and is a build error here: the project compiles with
-Werror=missing-field-initializers, so every `{REG, VAL}` line in the table has
to name all three fields. This appends `, 0` to the entries that do not.

Kept per the project's artifacts rule.
"""
import io
import re
import sys

PATH = 'components/codec/codec_es8311.cpp'

with io.open(PATH, encoding='utf-8', newline='') as f:
    lines = io.open(PATH, encoding='utf-8', newline='').read().split('\n')


def top_level_commas(text):
    """Commas in `text` that are not inside brackets."""
    depth = 0
    n = 0
    for ch in text:
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        elif ch == ',' and depth == 0:
            n += 1
    return n


start = end = None
for i, line in enumerate(lines):
    if line.startswith('const RegVal kInit[] = {'):
        start = i
    elif start is not None and line == '};':
        end = i
        break
if start is None or end is None:
    sys.exit('%s: kInit table not found' % PATH)

ENTRY = re.compile(r'^(\s*\{)(.*)(\},.*)$')
changed = 0
for i in range(start + 1, end):
    m = ENTRY.match(lines[i])
    if not m:
        continue
    body = m.group(2)
    if top_level_commas(body) != 1:
        continue  # already has keep, or is not a two-field entry
    lines[i] = m.group(1) + body + ', 0' + m.group(3)
    changed += 1

if changed:
    with io.open(PATH, 'w', encoding='utf-8', newline='') as f:
        f.write('\n'.join(lines))
print('%s: spelled out keep on %d entries' % (PATH, changed))
