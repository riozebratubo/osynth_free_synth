p = 'private_docs/BLE_PROTOCOL.md'
s = open(p, encoding='utf-8').read()

old = """| 2 | `u8 slot, i16 x, i16 y` | canvas position — no audio effect, no recompile |

Response, **on success and on failure alike**: `[u16 revision][u16 cost]`."""
new = """| 2 | `u8 slot, i16 x, i16 y` | canvas position — no audio effect, no recompile |
| 3 | the v1 `OGR1` model blob | replace the **whole** model in one edit (S40) |

`cmd 3` takes byte for byte what `GRAPH_NODES` hands out and what a
version-4 preset file stores — magic, blob version, node count, revision,
then per node `{u8 kind, i8 in[4], i16 x, i16 y}` — so the app pushes back
exactly the shape it read and there is no third encoding to keep in step.
The blob is positional: send as many nodes as `GRAPH_INFO.maxNodes` says,
because a short one silently empties the tail. The revision field is
ignored; the firmware keeps its own.

It exists because replaying a patch node by node does not work in
practice. Every `cmd 0` is its own recompile and its own audio duck, a
twelve-node patch is a dozen of them, and the intermediate graphs are real
patches the synth renders on the way past. `cmd 3` is compiled and
cost-checked as a unit, so a patch that does not fit is refused whole
rather than half applied. Firmware without it answers `ST_BAD_ARG`, which
is the app's cue to fall back to the per-node path (kinds, then cables,
then positions — `cmd 0` drops every cable touching the slot it changes,
so a cable laid before both of its ends exist would be thrown away).

Response, **on success and on failure alike**: `[u16 revision][u16 cost]`."""
assert old in s
s = s.replace(old, new, 1)

old = """| `0x3C` | GRAPH_EDIT     | `u8 cmd, …`                               | one edit; replies with the resulting revision + cost |"""
new = """| `0x3C` | GRAPH_EDIT     | `u8 cmd, …`                               | one edit — or, with `cmd 3`, the whole model; replies with the resulting revision + cost |"""
assert old in s
s = s.replace(old, new, 1)

old = """`preset.seqset.load/save` 0x0006/7, `seq.mode`…)"""
new = """`preset.seqset.load/save` 0x0006/7, `state.reset` 0x000F, `seq.mode`…)"""
assert old in s
s = s.replace(old, new, 1)
open(p, 'w', encoding='utf-8', newline='').write(s)
print("ok")
