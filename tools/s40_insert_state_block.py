import io

p = 'components/presets/presets.cpp'
s = open(p, encoding='utf-8').read()

anchor = "void preset_task(void*) {"
assert anchor in s

block = open('tools/s40_state_block.cpp.txt', encoding='utf-8').read()

s = s.replace(anchor, block + anchor, 1)
open(p, 'w', encoding='utf-8', newline='').write(s)
print("ok")
