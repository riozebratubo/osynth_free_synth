"""Document the S44 sampler in private_docs/PARAM_MAP.md and BLE_PROTOCOL.md."""
import io

# ------------------------------------------------------------ PARAM_MAP ----
P = 'private_docs/PARAM_MAP.md'
s = io.open(P, encoding='utf-8').read()
if 'smp.arm' in s:
    raise SystemExit('PARAM_MAP already patched')

old = """NRPN decimal ids: 1792 level, 1793 send, 1794 choke, 1795 kit, 1796 trig,
1797 midich; per slot s: 1808+4s level, 1809+4s pan, 1810+4s tune,
1811+4s decay."""
new = """NRPN decimal ids: 1792 level, 1793 send, 1794 choke, 1795 kit, 1796 trig,
1797 midich; per slot s: 1808+4s level, 1809+4s pan, 1810+4s tune,
1811+4s decay.

### Sampler / recorder (0x0750-0x0767, S44)

`drums.kit` now selects between the factory kit (0) and `OSYNTH_SAMPLE_KITS`
recordable kits (1..N), **all of them resident at once**. That residency is
what makes the rest of this block possible: a pad can be recorded into a kit
that is not the one playing, and selecting a kit is an atomic pointer store
rather than a load from a card.

The recorder captures from `smp.src` into a staging buffer and commits on the
falling edge of `smp.rec`. Three things are worth knowing before reading the
table:

* **`smp.arm` and `smp.rec` are separate on purpose.** The destination and the
  gate are set by different gestures at different times -- tap a pad, then hold
  a button, or the reverse -- and one combined "record into slot N" write could
  express only one of those orderings.
* **Pre-roll is not optional plumbing.** A short ring is filled every block
  whether or not anything is armed, so a take can start *before* the button
  arrived. Over BLE the press is 50-150 ms behind the sound that caused it;
  without this, sampling a transient means anticipating it.
* **The read-only four at the end are the truth.** The firmware decides when a
  threshold-armed take actually starts and when the ceiling stops it, so a
  control surface that inferred its state from its own writes would show
  "recording" through a take that never triggered.

| ID | Name | Type | Range | Default | Notes |
|----|------|------|-------|---------|-------|
| `0x0750` | `smp.src`       | enum  | input/bus | input | `bus` records the synth's own output, captured after the looper and before the metronome -- so resampling catches loops that are playing and never catches a count-in |
| `0x0751` | `smp.arm`       | int   | -1..15 | -1 | destination pad; -1 = nothing armed |
| `0x0752` | `smp.rec`       | bool  | 0/1   | 0 | the gate. Rising edge arms the capture (after `smp.countin`), falling edge commits |
| `0x0753` | `smp.erase`     | int   | -1..15 | -1 | **trigger**: empties that pad, stashing it for undo |
| `0x0754` | `smp.undo`      | bool  | 0/1   | 0 | **trigger**: puts back whatever the last record/erase/copy displaced. One operation deep, but an operation may be several pads -- a sliced take undoes as one |
| `0x0755` | `smp.thresh`    | float | 0..0.5 | 0 | 0 starts on the gate; above that the take starts on the first sample over the threshold, with pre-roll measured back from *that* |
| `0x0756` | `smp.trim`      | bool  | 0/1   | 1 | drop the tail below -60 dBFS |
| `0x0757` | `smp.norm`      | bool  | 0/1   | 1 | normalise the samples themselves (not the slot gain -- a quiet take scaled at playback keeps a quiet take's bit depth) |
| `0x0758` | `smp.preroll`   | float | 0..250 ms | 120 | how much of the ring to prepend |
| `0x0759` | `smp.maxsec`    | float | 0.1..MAX | MAX | the player's take ceiling, inside the build's `OSYNTH_SAMPLE_MAX_SEC` staging buffer |
| `0x075A` | `smp.slices`    | int   | 1..16 | 1 | 1 = one pad. Above that the take is split across consecutive pads from `smp.arm`, wrapping |
| `0x075B` | `smp.slicemode` | enum  | even/transient | transient | a detector that finds nothing it is confident about falls back to even, rather than refusing |
| `0x075C` | `smp.monitor`   | bool  | 0/1   | 1 | while armed, borrow `in.route` for `mon` **if it was off**. A route the player set on purpose is left alone |
| `0x075D` | `smp.countin`   | int   | 0..8  | 0 | beats of metronome before the gate opens, at `seq.tempo` |
| `0x075E` | `smp.save`      | bool  | 0/1   | 0 | **trigger**: write the bound kit to storage |
| `0x075F` | `smp.gain`      | float | 0..8  | 1.0 | record trim, applied on capture (so it is in the ring, and in the take) |
| `0x0760` | `smp.copyfrom`  | int   | -1..15 | -1 | source pad for a copy |
| `0x0761` | `smp.copykit`   | int   | -1..N | -1 | source kit; -1 = the bound one |
| `0x0762` | `smp.copyto`    | int   | -1..15 | -1 | **trigger**: writing it performs the copy |
| `0x0763` | `smp.dupkit`    | int   | -1..N | -1 | **trigger**: duplicate the bound kit onto that index. Refuses outright rather than duplicating half a kit if the pool cannot hold it |
| `0x0764` | `smp.state`     | int   | 0..4  | 0 | *read-only*: idle, armed, waiting, recording, committing |
| `0x0765` | `smp.pos`       | float | 0..1  | 0 | *read-only*: through `smp.maxsec` |
| `0x0766` | `smp.free`      | float | KB    | pool | *read-only*: sample pool still available |
| `0x0767` | `smp.peak`      | float | 0..1  | 0 | *read-only*: loudest sample of the live take |

**Not parameters.** The per-pad playback settings -- play mode (one-shot /
gate / loop), reverse, start offset, choke group, MIDI note and name -- are
kit data, not patch data, and travel over `OP_KIT_EDIT` instead. Two reasons,
either sufficient: a parameter does not follow a kit switch, so `drum7.rev`
would describe pad 7 of whichever kit happened to be bound; and 16 pads x 4
settings is 64 ids against a `kMaxParams` of 448 with ~394 already in use.

### Sampler engine (0x02xx, S44)

`engine.type` = 6. Plays the pads of the bound kit from the keyboard; see
`components/engines/include/engine_sampler.h` for why it carries no filter,
LFO or mod matrix.

| ID | Name | Type | Range | Default | Notes |
|----|------|------|-------|---------|-------|
| `0x0200` | `smp.mode`     | enum  | pads/pitched | pads | `pads` follows the kit's note map, one pad per key; `pitched` plays `smp.pad` chromatically against `smp.root` |
| `0x0201` | `smp.pad`      | int   | 0..15 | 0  | which pad `pitched` plays |
| `0x0202` | `smp.root`     | int   | 0..127 | 60 | the key that plays it at 1x |
| `0x0203` | `smp.start`    | float | 0..0.999 | 0 | extra start offset, on top of each pad's own |
| `0x0204` | `smp.veldepth` | float | 0..1  | 1.0 | how much velocity shapes level |
| `0x0205` | `smp.level`    | float | 0..2  | 1.0 | engine output trim |
| `0x0206` | `smp.spread`   | float | 0..1  | 1.0 | how far each pad's stored pan is honoured -- a kit panned for a drum bus is usually wider than you want under a melodic part |
| `0x0207`-`0x020A` | `env1.attack/decay/sustain/release` | float | | 0.001 / 0.5 / 1.0 / 0.08 | amplitude envelope |"""
assert old in s, 'PARAM_MAP anchor'
s = s.replace(old, new, 1)

old = "| `0x0700-0x07FF` | Drum bus: kit selection, bus sends, per-slot mixer (S22) |"
new = "| `0x0700-0x07FF` | Drum / sample bus: kit selection, bus sends, per-slot mixer (S22); the sampler/recorder at `0x0750-0x0767` (S44) |"
assert old in s, 'PARAM_MAP index row'
s = s.replace(old, new, 1)

old = "| `0x0703` | `drums.kit`    | int   | 0..N-1  | 0       | 0 = factory, 1.. = kits found on SD     |"
new = "| `0x0703` | `drums.kit`    | int   | 0..N-1  | 0       | 0 = factory (flash, read-only), 1..N = the recordable kits (S44), all resident |"
assert old in s, 'PARAM_MAP kit row'
s = s.replace(old, new, 1)

io.open(P, 'w', encoding='utf-8').write(s)
print('PARAM_MAP ok')

# -------------------------------------------------------- BLE_PROTOCOL ----
B = 'private_docs/BLE_PROTOCOL.md'
b = io.open(B, encoding='utf-8').read()
if 'KIT_EDIT' in b:
    raise SystemExit('BLE_PROTOCOL already patched')

marker = '`0x3E`'
idx = b.find(marker)
if idx < 0:
    print('BLE_PROTOCOL: no 0x3E row found, appending a section instead')
    insert_at = len(b)
else:
    # end of the line the 0x3E row sits on
    insert_at = b.find('\n', idx) + 1

section = """
## `0x3F` KIT_EDIT (S44)

Per-pad kit data. Everything the *recorder* does rides on ordinary parameters
(`smp.*`, PARAM_MAP.md); this carries what could not be one, because it has to
follow a kit switch and because there is no parameter space left for 64 more
ids.

Request payload, sub-op first:

| Sub | Payload | Meaning |
|-----|---------|---------|
| `0` | `[u8 kit][u8 slot][u8 field][f32 value]` | set one field. `kit` = `0xFF` means the bound one. `field`: 0 play mode (0 one-shot / 1 gate / 2 loop), 1 reverse, 2 start offset 0..1, 3 choke group 0..7, 4 MIDI note, 5 baked gain |
| `1` | `[u8 kit][char name[<=23]]` | rename a kit |
| `2` | `[u8 kit][u8 slot][char name[<=11]]` | rename a pad |

Response is a bare status: `ST_OK`, `ST_BAD_ARG`, or `ST_UNSUPPORTED` when the
target is the factory kit -- which is flash-mapped and cannot be written back,
so it must not appear to accept an edit either.

The app re-reads `KIT_INFO` afterwards rather than trusting the echo: the
firmware clamps, and what a pad ended up as is its answer to give.

### `KIT_INFO` changes in S44

* `what = 0` (the kit list) gained a fourth prefix byte -- the storage backend,
  `0` none / `1` SD / `2` LittleFS -- and a flags byte on each record before
  the name (`bit 0` = recordable). Records are 26 bytes, from 25.
* `what = 1` (the slot list) now sends its **record width** in the third prefix
  byte, which was reserved and zero. Records are 22 bytes, from 14:
  `[u8 slot][u8 note][u8 flags][u8 mode][u8 choke][u8 start][u32 frames][char name[12]]`,
  where `flags` bit 0 = the pad has audio and bit 1 = reverse. A reader should
  take the stride from the prefix and skip anything it does not recognise; a
  zero there means a pre-S44 firmware and the old 14-byte record.

## `0x38` DRUM_TRIG, release form (S44)

The payload may now be three bytes, `[u8 slot][u8 velocity][u8 release]`. With
`release` non-zero the pad is let go instead of struck, which is what gate and
loop pads need. A third byte rather than an overloaded velocity of 0, because
that value is already the capability probe the app fires at discovery -- and
that probe silencing a held pad would have been a genuinely confusing bug.
Two-byte payloads behave exactly as before.
"""
b = b[:insert_at] + section + b[insert_at:]
io.open(B, 'w', encoding='utf-8').write(b)
print('BLE_PROTOCOL ok')
