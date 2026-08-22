p = 'private_docs/PARAM_MAP.md'
s = open(p, encoding='utf-8').read()

old = """
### Engine-common (0x01xx) — registered by the voice manager, all engines
"""
new = """| `0x000F` | `state.reset`     | int   | 0..1   | 0       | **trigger** (S40): writing 1 puts the synth back to the state a first boot would have produced — every value the working state covers to its registered default, the sequencer patterns and the song chain cleared, the modular graph reset, and `/lfs/state.osw` removed. Reflects itself back to 0 when it is done. Deliberately **not** a factory reset: the persisted NVS settings (master volume, the input, `out.level`, `usb.mode`) and the looper are left exactly as they are. Skipped by presets and by the working state — a snapshot that fired this on load would erase the sequencer of whoever selected it |

### Engine-common (0x01xx) — registered by the voice manager, all engines
"""
assert old in s
s = s.replace(old, new, 1)

old = """**Factory presets** (16 × 4, authored in"""
new = """**The working state** (S40) is a seventh file, `/lfs/state.osw`, and nothing
asks for it. The preset task writes it whenever the synth has been left
alone for 5 s *and* `audio_io_quiet_ms()` says the output has gone quiet
(forced after 3 min if it never does — the persist.h rules, at a longer
settle because this write is kilobytes rather than a couple of hundred
bytes), and reads it back once at boot from `main()`, after
`audio_io_start()` because applying it may switch engines and the S6
detach handshake needs render boundaries to hand over on.

It carries: every patch parameter, plus `engine.type`, `drums.kit` and
`seq.pattern` — the three a *named* snapshot must not move behind the
player — plus the modular graph blob, plus the whole sequencer (every
pattern and the song chain), plus the last loaded preset slot, so the
app's "current preset" readout survives the power cycle too. `engine.type`
and the preset slot live in the header rather than the pair list, because
both have to be read before anything else can be applied.

Left out: anything `persist_owns()` (two owners writing one setting from
two files is a value that depends on which one lost the race), the
transport and `seq.clock` (a synth that boots playing, or boots slaved to
a clock that is not there, is a trap), the looper (minutes of audio is not
a setting), and the momentary controls and telemetry `skip_id()` already
names.

Two things keep it cheap. Parameter locks do **not** mark it dirty —
`ParamOrigin::Internal` is filtered out, except for `seq.rev` and
`graph.rev`, which are how pattern data and the graph say they changed at
all — so a p-locked pattern playing does not hold the state permanently
dirty and force a write through its own playback. And every save builds
the blob twice: a probe pass that only hashes it, and the real write, done
only when the hash moved. A knob dragged back where it started costs
nothing.

**Factory presets** (16 × 4, authored in"""
assert old in s
s = s.replace(old, new, 1)
open(p, 'w', encoding='utf-8', newline='').write(s)
print("ok")
