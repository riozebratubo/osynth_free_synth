p = 'app_osyntho/src/ble/synthprotocol.h'
s = open(p, encoding='utf-8').read()

start_marker = "// The whole model in one edit (S40)."
end_marker = "struct GraphInfo {"
a = s.index(start_marker)
b = s.index(end_marker)
block = s[a:b]
s = s[:a] + s[b:]

anchor = "// GRAPH_EDIT response: { u16 revision, u16 cost }"
i = s.index(anchor)
s = s[:i] + block + s[i:]
open(p, 'w', encoding='utf-8', newline='').write(s)
print("ok")
