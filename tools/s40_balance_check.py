import re
import sys

files = sys.argv[1:]
for f in files:
    s = open(f, encoding='utf-8').read()
    s2 = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    s2 = re.sub(r'//[^\n]*', '', s2)
    s2 = re.sub(r'"(\\.|[^"\\])*"', 'S', s2)
    s2 = re.sub(r"'(\\.|[^'\\])*'", 'C', s2)
    print(f, 'braces', s2.count('{') - s2.count('}'),
          'parens', s2.count('(') - s2.count(')'))
