p = 'components/presets/presets.cpp'
s = open(p, encoding='utf-8').read()

start = s.index('bool state_id(uint16_t id) {')
end_marker = "/* FNV-1a over the blob as it is built"
end = s.index(end_marker)

new = open('tools/s40_state_pred.cpp.txt', encoding='utf-8').read() + "\n"
s = s[:start] + new + s[end:]

s = s.replace("    state_touch(id);\n", "    state_touch(id, origin);\n", 1)
open(p, 'w', encoding='utf-8', newline='').write(s)
print("ok")
