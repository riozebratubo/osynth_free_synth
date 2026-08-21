#!/usr/bin/env python3
"""S36: document the enable switches and the reverb algorithms in PARAM_MAP.md.

private_docs/ is gitignored, so this is the only durable record of what the
S36 documentation edit was. Idempotent. Run from the repo root:
    python tools/s36_patch_param_map.py
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DOC = ROOT / "private_docs" / "PARAM_MAP.md"

# New table rows, keyed by the row they are inserted *before*.
ROWS = {
    "| `0x0300` | `fx.cho.mix`":
        "| `0x0303` | `fx.cho.on`       | bool  | 0/1               | 0       | enable (S36); gates the mix below        |\n",
    "| `0x0310` | `fx.dly.mix`":
        "| `0x0317` | `fx.dly.on`       | bool  | 0/1               | 0       | enable (S36)                             |\n",
    "| `0x0320` | `fx.rev.mix`":
        "| `0x0324` | `fx.rev.on`       | bool  | 0/1               | 0       | enable (S36)                             |\n"
        "| `0x0325` | `fx.rev.algo`     | enum  | 0..1 (0..3 GPL)   | freeverb| algorithm; see Reverb algorithms below   |\n",
    "| `0x0330` | `fx.grn.mix`":
        "| `0x0337` | `fx.grn.on`       | bool  | 0/1               | 0       | enable (S36)                             |\n",
    "| `0x0340` | `fx.crush.mix`":
        "| `0x0343` | `fx.crush.on`     | bool  | 0/1               | 0       | enable (S36)                             |\n",
    "| `0x0360` | `fx.drv.mix`":
        "| `0x0365` | `fx.drv.on`       | bool  | 0/1               | 0       | enable (S36)                             |\n",
    "| `0x0370` | `fx.phs.mix`":
        "| `0x0377` | `fx.phs.on`       | bool  | 0/1               | 0       | enable (S36)                             |\n",
    "| `0x0380` | `fx.flg.mix`":
        "| `0x0386` | `fx.flg.on`       | bool  | 0/1               | 0       | enable (S36)                             |\n",
}

# The five shared reverb stages go after fx.rev.comp, which closes that unit.
AFTER_REV_COMP = (
    "| `0x0326` | `fx.rev.pre`      | float | 0..120 ms         | 0       | pre-delay, shared by every algorithm     |\n"
    "| `0x0327` | `fx.rev.tone`     | float | 500..20k Hz (exp) | 20000   | wet low-pass; max = off (bypassed)       |\n"
    "| `0x0328` | `fx.rev.width`    | float | 0..2              | 1       | wet stereo width; 1 = as rendered        |\n"
    "| `0x0329` | `fx.rev.diff`     | float | 0..1              | 0.7     | diffusion/density; freeverb ignores it   |\n"
    "| `0x032A` | `fx.rev.early`    | float | 0..1              | 0.4     | early/late balance; freeverb ignores it  |\n"
)

SECTION = """
#### Per-effect enable switches (S36)

Eight units gained a `fx.<unit>.on` switch: chorus, delay, granular, reverb,
bitcrush, drive, phaser and flanger. Filter, EQ and compressor already had
one and are unchanged; `fx.st` deliberately has none, because it is the
master output stage rather than an effect and bypassing it would mute the
instrument.

The switch gates the unit's `mix` — it does not replace it. Both go through
the same `unit_gate()` smoother, so:

- `on = 0` silences and skips the unit, whatever `mix` says;
- `mix = 0` silences and skips the unit, whatever `on` says (the pre-S36
  behaviour, unchanged);
- toggling either crossfades over ~90 ms rather than stepping;
- a bypassed unit's tail lines are scrubbed a chunk per block, so re-enabling
  never replays stale audio.

A bypassed unit keeps every one of its other parameter values, and presets
store them, so switching an effect back on returns it to the exact state it
was left in.

**Default is off, and that required a preset-format change.** A `.osp` is a
sparse snapshot — `do_save()` writes only values that differ from their
default — so with the switches defaulting to 0, "no `fx.rev.on` pair in this
file" is ambiguous: on a file written before S36 it means the reverb was
governed by its mix alone and was audible; on a file written after, it means
the player bypassed it. `PresetHdr.version` disambiguates: 1 and 2 are the
legacy layouts, 3 and 4 are the same two layouts written by firmware that has
the switches. A legacy file gets `legacy_fx_enable()` run over it after its
pairs land, turning on every unit whose mix came out above zero — including
the reverb's 0.15 default, which no v1 file has to mention to have been
audible. Files are migrated on load and made permanent by the next save;
nothing walks the filesystem.

The app's own patch library (SQLite) needed the same treatment for the same
reason, at schema version 2 — see `Database::createTables()`. Factory presets
name their switches explicitly and need no migration.

#### Reverb algorithms (S36)

`fx.rev.algo` selects one of four topologies inside the single reverb unit.
One is allocated and run at a time; changing it mutes the unit for ~40 ms
while the new algorithm's lines are scrubbed, then fades back in.

| Value | Name        | Licence | Topology                                                     |
| ----- | ----------- | ------- | ------------------------------------------------------------ |
| 0     | `freeverb`  | MIT     | 8 LP-feedback combs + 4 allpasses per channel — the original |
| 1     | `wetreverb` | MIT     | half-rate Schroeder bank + tapped early field (WET Reverb)   |
| 2     | `mverb`     | GPL-3   | Dattorro figure-of-eight plate (MVerb)                       |
| 3     | `duskverb`  | GPL-3   | Dattorro tank + 12-deep density cascade (DuskVerb Plate)     |

**The list is append-only and 0 never moves.** The index is the stored value,
so entry 0 is not a favourite — it is the contract that lets every patch
saved before S36 keep its sound.

2 and 3 exist only when `CONFIG_OSYNTH_FX_GPL` is set. osynth is MIT and both
of those algorithms are GPL-3, so building them in makes the firmware image a
GPL-3 combined work; the option is off by default and they live in their own
component (`components/fx_gpl`, which contributes no object files when the
option is off). They are last in the enum precisely so that compiling them
out shortens the list rather than punching a hole in it — removing a middle
entry would renumber the tail. On an MIT build the registered range is 0..1,
so a patch asking for 2 or 3 clamps to `wetreverb`.

`fx.rev.mix`, `size` and `damp` mean the same thing to all four. `fx.rev.pre`,
`tone` and `width` are shared stages the bus runs around whichever algorithm
is selected, and each is an exact no-op at its default, so freeverb gained
three controls without any pre-S36 patch changing. `fx.rev.diff` and
`fx.rev.early` reach into the algorithm and do nothing under freeverb, which
has neither an adjustable diffusion coefficient nor an early field.
`fx.rev.comp` stays freeverb-only: it undoes one specific staging decision in
that algorithm and has nothing to undo in the others.
"""

ANCHOR_SECTION = "#### Level compensation (S35)"


def main() -> int:
    if not DOC.exists():
        print(f"skipped: {DOC} not found (private_docs is gitignored)")
        return 0
    doc = DOC.read_text(encoding="utf-8")
    if "fx.rev.algo" in doc:
        print("nothing to do: PARAM_MAP.md already documents S36")
        return 0

    for anchor, rows in ROWS.items():
        idx = doc.find(anchor)
        if idx < 0:
            print(f"warn: no row anchored at {anchor!r}; skipping")
            continue
        doc = doc[:idx] + rows + doc[idx:]

    comp = "| `0x0323` | `fx.rev.comp`"
    idx = doc.find(comp)
    if idx >= 0:
        eol = doc.index("\n", idx) + 1
        doc = doc[:eol] + AFTER_REV_COMP + doc[eol:]
    else:
        print("warn: fx.rev.comp row not found; shared stages not inserted")

    idx = doc.find(ANCHOR_SECTION)
    if idx < 0:
        print("warn: could not place the prose sections")
    else:
        doc = doc[:idx] + SECTION.lstrip("\n") + "\n" + doc[idx:]

    DOC.write_text(doc, encoding="utf-8")
    print("PARAM_MAP.md: S36 rows and prose added")
    return 0


if __name__ == "__main__":
    sys.exit(main())
