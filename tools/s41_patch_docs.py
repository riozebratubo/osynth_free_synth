#!/usr/bin/env python3
"""Document chord mode (S41) in PARAM_MAP.md, BLE_PROTOCOL.md and README.md.

Kept per the project's intermediary-artifacts policy. Idempotent: each edit
checks for its own marker first.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

PARAM_SECTION = """
### Chord mode (0x044x-0x045x, S41 — registered at boot)

One key in, several notes out. The expansion happens in the MIDI router, which
is the one place every note source meets — the app's keyboard over BLE, a USB
MIDI controller, DIN MIDI, the arpeggiator and the sequencer — so a plugged-in
controller gets chords for exactly the same reason the on-screen keys do.
Drums never do: `midi_route_channel_message()` gives notes on `drums.midich`
to the drum bus before chord mode is reached.

Three modes. **free** puts one quality (`chord.type`) under every key,
transposed. **scale** lets the key pick a *degree* of `chord.scale` /
`chord.root` and stacks the chord in scale thirds, so the quality follows the
degree with nothing to choose: I maj, ii min, V dom7, vii dim. **user** reads
twelve slots of arbitrary intervals, one per pitch class above the root, which
travel over the `CHORD_SET` opcode rather than living here — twelve interval
lists are not parameter space.

`chord.size` is how many notes are stacked, and **1 is a legal answer**: a
single note, still snapped into the scale. Plain scale-locking is the same
mechanism with one fewer tone.

`chord.route` decides where chord mode sits relative to the arpeggiator.
*pre-arp* expands before the note tap, so the arpeggiator's held-note list
fills with the chord's tones and one key gives a running arpeggio of that
chord. *post-arp* expands after it, so the arpeggiator steps through the keys
you are holding and each step comes out as a block chord. The two are mutually
exclusive by construction.

Sequencer tracks are chorded **one at a time**, not globally: the per-track
`SEQ_TRACK_F_CHORD` flag (bit 2 of `seq_track_cfg_t::flags`, on the app's track
sheet) is what turns a one-note-per-step bassline into a chord progression
while the lead lane beside it keeps playing what is written.

**Polyphony.** `CONFIG_OSYNTH_VOICES` is 8, so a triad is two keys and a third
steals. `chord.keys` = mono releases the previous key's chord when a new key
lands, which is what makes eight voices generous rather than tight; poly is the
default and leaves the voice manager's oldest-note stealing exactly as it was.

**Persistence.** Every parameter here is a *performance* setting: kept by the
working state, so it survives a power cycle, and **skipped by presets and by
sequencer sets** — the same treatment `master.volume` and the line input get.
Loading a patch called "Bell" mid-set must not change what your keyboard plays
a triad of, and the reverse matters more: chord mode is most useful while you
are auditioning sounds, and a patch change that switched it off every time
would make it unusable there. The user chord set rides in the working-state
file as its own section.

Being ordinary parameters, all of these are **p-lockable per step**, reachable
over MIDI NRPN and usable as mod-matrix destinations — a `chord.type` locked to
one step costs nothing extra.

| ID       | Name             | Type  | Range   | Default | Notes                                  |
| -------- | ---------------- | ----- | ------- | ------- | -------------------------------------- |
| `0x0440` | `chord.enable`   | bool  | 0/1     | 0       | the toggle. Off means the router behaves exactly as it did before chord mode existed |
| `0x0441` | `chord.mode`     | enum  | 0..2    | free    | free / scale / user                    |
| `0x0442` | `chord.type`     | enum  | 0..24   | maj     | free mode's quality: single, 5th, oct, sus2, sus4, maj, min, dim, aug, maj6, min6, 7, maj7, min7, m7b5, dim7, mmaj7, add9, 6/9, 9, maj9, min9, 11, 13, quartal. Ordered thin to rich, which is also the order the chord *namer* matches in — the simplest name that fits is the one printed |
| `0x0443` | `chord.scale`    | enum  | 0..11   | major   | the same twelve scales `seq.scale` numbers, and deliberately the same enum: `chord.follow` mirrors one into the other by value, which is only meaningful while they agree. Chromatic in scale mode stacks whole tones — exotic, but the consistent answer to the same rule |
| `0x0444` | `chord.root`     | int   | 0..11   | 0       | 0 = C                                  |
| `0x0445` | `chord.follow`   | bool  | 0/1     | 0       | read `seq.scale`/`seq.root` instead, so the whole instrument can be locked to one key |
| `0x0446` | `chord.keymap`   | enum  | 0..1    | degrees | **degrees**: every semitone advances one scale degree, so no key is dead or doubled and the two drawn octaves cover ~3.4 octaves of a seven-note scale — this is what actually delivers "you cannot play a wrong chord". **chromatic**: the key keeps its pitch, snapped into the scale (`seq_quantize`), and the five black keys double their neighbours |
| `0x0447` | `chord.size`     | int   | 1..7    | 3       | tones stacked in scale mode: 1 single note / 2 third / 3 triad / 4 seventh / 5 ninth / 6 eleventh / 7 thirteenth. Free and user modes take their count from the quality or the slot |
| `0x0448` | `chord.inv`      | int   | 0..3    | 0       | inversions; the lowest tone moves up an octave each time. Overridden while `chord.lead` is on |
| `0x0449` | `chord.voicing`  | enum  | 0..3    | close   | close / drop2 / drop3 / open. Open drops every other tone an octave, which is what stops a five- or six-note stack sounding like a cluster |
| `0x044A` | `chord.bass`     | enum  | 0..2    | off     | off / −1 oct / −2 oct. Added last, so it cannot be inverted, dropped or folded away — being the lowest thing sounding is the whole point of it |
| `0x044B` | `chord.strum`    | float | 0..200 ms | 0     | between the notes of a chord. 0 emits them inline on the calling task and costs nothing; above 0 they are scheduled on the chord task (1 ms resolution at `CONFIG_FREERTOS_HZ` = 1000) |
| `0x044C` | `chord.strumdir` | enum  | 0..3    | up      | up / down / alt / random. `alt` flips per key press |
| `0x044D` | `chord.vel`      | float | 0..1    | 0       | velocity falloff toward the top of the chord — quietest on top, which is how a hand plays one. Never below 1: 0 is a note-off in MIDI |
| `0x044E` | `chord.lead`     | bool  | 0/1     | 0       | auto voice-leading: score every inversion against the chord just played and take the closest, so a progression moves by the shortest path instead of jumping register with the root |
| `0x044F` | `chord.range`    | int   | 0..48   | 0       | semitones above the lowest tone; anything higher folds down by octaves. 0 = no limit. Applied after voicing, and what keeps the top of the keyboard playable in scale mode where a stacked 13th would run off the end |
| `0x0450` | `chord.route`    | enum  | 0..1    | pre-arp | pre-arp / post-arp — see above          |
| `0x0451` | `chord.keys`     | enum  | 0..1    | poly    | poly / mono — see **Polyphony** above   |
| `0x0452` | `chord.rev`      | int   | 0..2^24-1 | 0     | **read-only.** Bumped by every edit to the user chord set. The set is not parameter space and has no event opcode, so this is how a client learns of a change it did not make — a working-state restore, a `state.reset`. Only inequality is meaningful; wraps. Same role `seq.rev` and `graph.rev` play |

NRPN decimal ids: 1088 enable, 1089 mode, 1090 type, 1091 scale, 1092 root,
1093 follow, 1094 keymap, 1095 size, 1096 inv, 1097 voicing, 1098 bass,
1099 strum, 1100 strumdir, 1101 vel, 1102 lead, 1103 range, 1104 route,
1105 keys, 1106 rev.

"""

BLE_SECTION = """
### `0x3E` CHORD_SET

The user chord set (S41): twelve slots of eight bytes, addressed by
`(played note − chord.root) mod 12`, so one set is the same progression in
every key. Twelve interval lists are not parameter space — a parameter is one
float — and the whole set is small enough to travel in a single frame either
way, so a write answers with the entire set rather than an ack. The editor
draws all twelve at once and can never fall out of step with the synth.

```
get:  [u8 dir = 0]
set:  [u8 dir = 1][u8 slot 0-11][slot: 8 bytes]

response (both):  [u8 slots][slots x 8 bytes]

slot: [i8 transpose][u8 count][i8 iv[6]]
```

`transpose` (−24..24) is added to the played key before the intervals are
stacked; `iv[0..count-1]` (−24..36) are semitones above that. `count` 0 is a
legal and useful entry — that key is **silent**, which is how a five-chord set
stays quiet on the seven keys it does not use. Bytes past `count` are ignored
on read and sent as zero.

`chord.rev` moves on every write, so a second client on the link learns of the
change through the ordinary ~20 Hz parameter batch rather than by polling.

Firmware without chord mode answers `ST_UNKNOWN_OP`, which is how the app
decides whether to offer the user-set editor at all.

"""


def patch(rel, anchor, insert, marker):
    path = REPO / rel
    src = path.read_text(encoding="utf-8")
    if marker in src:
        print("%s: already patched" % rel)
        return
    if src.count(anchor) != 1:
        sys.exit("%s: anchor appears %d times" % (rel, src.count(anchor)))
    path.write_text(src.replace(anchor, insert + anchor, 1), encoding="utf-8")
    print("%s: patched" % rel)


patch("private_docs/PARAM_MAP.md",
      "### Drum bus (0x07xx, S22 — registered at boot on both targets)",
      PARAM_SECTION.lstrip("\n") + "\n",
      "### Chord mode (0x044x")

patch("private_docs/BLE_PROTOCOL.md",
      "### `0x37` KIT_INFO",
      BLE_SECTION.lstrip("\n") + "\n",
      "### `0x3E` CHORD_SET")

# The opcode table near the top of BLE_PROTOCOL.md.
path = REPO / "private_docs/BLE_PROTOCOL.md"
src = path.read_text(encoding="utf-8")
row_anchor = "| `0x38` | DRUM_TRIG"
if "| `0x3E` | CHORD_SET" not in src:
    if src.count(row_anchor) != 1:
        sys.exit("BLE opcode table: anchor appears %d times" % src.count(row_anchor))
    line_start = src.index(row_anchor)
    new_row = ("| `0x3E` | CHORD_SET      | `u8 dir` [+ `u8 slot, 8 B slot`]          "
               "| the whole 12-slot user chord set (S41) |\n")
    src = src[:line_start] + new_row + src[line_start:]
    path.write_text(src, encoding="utf-8")
    print("BLE opcode table: patched")
else:
    print("BLE opcode table: already patched")

# Namespace table in PARAM_MAP.md.
path = REPO / "private_docs/PARAM_MAP.md"
src = path.read_text(encoding="utf-8")
ns_anchor = "| `0x0400-0x04FF` | Sequencer + arpeggiator                            |"
if "0x0440-0x045F" not in src.split("## Registered parameters")[0]:
    if src.count(ns_anchor) != 1:
        sys.exit("namespace table: anchor appears %d times" % src.count(ns_anchor))
    src = src.replace(
        ns_anchor,
        "| `0x0400-0x043F` | Sequencer + arpeggiator                            |\n"
        "| `0x0440-0x045F` | Chord mode (S41), inside the 0x04xx block          |",
        1)
    path.write_text(src, encoding="utf-8")
    print("namespace table: patched")
else:
    print("namespace table: already patched")
