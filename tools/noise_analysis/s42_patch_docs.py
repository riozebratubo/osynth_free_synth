"""S42 — mic noise reduction: PARAM_MAP.md, plus the S39c corrections the
firmware changes made true (defaults, the bank, the estimator, the creep)."""
import io
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
p = ROOT / 'private_docs/PARAM_MAP.md'
s = p.read_text(encoding='utf-8')

# ---------------------------------------------------------------- heading --
old = "### Noise reduction (0x03Ex + 0x03Fx, S39 — part of the FX bus)\n\nTwo units at the head of the bus, added so a P4 build can be used as a plain"
new = ("### Noise reduction (0x0344-0x0349 + 0x03Ex + 0x03Fx, S39/S42 — part of the FX bus)\n\n"
       "Three units at the head of the bus, added so a P4 build can be used as a plain")
assert s.count(old) == 1, 'heading'
s = s.replace(old, new)

old = """cleanup stage it was missing. Neither unit is microphone-specific; both are
ordinary bus units and will do the same to a hissy line input or a noisy
sampled loop.

Each has a **`src`** selector, and it is the control that decides whether
these are microphone cleanup or an effect on the instrument:

* `bus` (default) — the finished mix, which is right for a noisy sampled loop
  and wrong for a synth that was never noisy in the first place."""
new = """cleanup stage it was missing. None of the three is microphone-specific; all
are ordinary bus units and will do the same to a hissy line input or a noisy
sampled loop.

**Which one to use.** They are alternatives, not a stack — see the warning
below — and the choice is decided by one question: *is the noise still audible
underneath the voice, or only between words?*

| | removes noise in the gaps | removes noise **under the voice** | SNR gain |
| --- | --- | --- | --- |
| `fx.nr`  (expander, S39) | 5.8 dB | **0.4 dB** | **0.0 dB** |
| `fx.anr` (12 bands, S39) | 14.3 dB | 4.5 dB | +2.4 dB |
| `fx.mnr` (129 bins, S42) | **23.7 dB** | **4.7 dB** | **+3.2 dB** |

Measured on a real osynth capture (mic at `in.micgain` 1.33, ambient room)
with a voice mixed over it at 14.5 dB SNR. `fx.nr`'s 0.0 dB is not a tuning
failure and cannot be tuned away: a downward expander computes **one** gain
for the whole spectrum from one detector, so the instant the voice crosses the
threshold that gain is 1 and the hiss returns at full level. It cleans the
gaps, which is all any gate has ever done. `fx.mnr` resolves 129 bins and
exploits the fact that speech is *sparse in frequency* — at any instant the
voice occupies a fraction of the spectrum, and every other bin can still be
pushed down mid-syllable.

**Do not run two of them at `src` = `input` at once.** Each computes its
correction against the same untouched input block and adds it to the bus, so
two units remove the input's contribution twice and what is left is a
subtraction nobody asked for. This is a real hazard rather than a theoretical
one, because all three now default to `input`.

Each has a **`src`** selector, and it is the control that decides whether
these are microphone cleanup or an effect on the instrument:

* `bus` — the finished mix, which is right for a noisy sampled loop
  and wrong for a synth that was never noisy in the first place."""
assert s.count(old) == 1, 'which one'
s = s.replace(old, new)

old = """  leaves the output identical to the unit being off. Exact, not approximate,
  because
  both units are *corrections*: the adaptive one's band sum is already (g-1)
  times each band, and the fixed one's is `g*filt(x) - x`. Neither ever needed
  the bus to work, only something to be a difference from, so each runs on the
  block `audio_io_in_fx_block()` hands back and adds the result to the bus."""
new = """  leaves the output identical to the unit being off. Exact, not approximate,
  because all three are *corrections*: the adaptive one's band sum is already
  (g-1) times each band, the fixed one's is `g*filt(x) - x`, and the mic one's
  is `cleaned(t-N) - x(t)`. None ever needed the bus to work, only something
  to be a difference from, so each runs on the block
  `audio_io_in_fx_block()` hands back and adds the result to the bus.

  `input` is the **default** for all three since S42. Cleaning a microphone is
  what anyone switching one of these on is trying to do; denoising the synth's
  own bus is the specialist case, and it is the one with a held pad to lose."""
assert s.count(old) == 1, 'src exactness'
s = s.replace(old, new)

old = """They run **before the vocoder**, `fx.anr` first and `fx.nr` second, and that
order does not commute:"""
new = """They run **before the vocoder**, `fx.mnr` first, then `fx.anr`, then `fx.nr`,
and the last two do not commute:"""
assert s.count(old) == 1, 'order'
s = s.replace(old, new)

old = """Both are off by default and both are ordinary preset values, with one
exception: `fx.anr.learn` is on the preset skip list, like `fx.voc.freeze`."""
new = """All three are off by default and are ordinary preset values, with two
exceptions: `fx.anr.learn` and `fx.mnr.learn` are on the preset skip list,
like `fx.voc.freeze`.

> **Presets save sparsely** — only values that differ from their default are
> stored. The S39c/S42 default changes (`src` on both S39 units, `fx.nr.thresh`
> -45 to -24, `fx.anr.adapt` 8 s to 3 s) therefore change what an *existing*
> patch does if it switched one of these units on and left those controls
> alone. No factory preset touches any of them."""
assert s.count(old) == 1, 'defaults note'
s = s.replace(old, new)

# ------------------------------------------------------------ setup table --
old = """| 3 | `fx.anr.src` = `input`, `fx.anr.on` = 1, then hold **Hold to learn** for a second in a quiet room |
| 4 | `fx.nr.src` = `input`, `fx.nr.on` = 1, `fx.nr.thresh` just under the quietest thing worth keeping |
| 5 | on the computer, pick the **osynth audio** capture device |

Step 3's button is optional: left alone, `fx.anr` listens for `fx.anr.adapt`
seconds after being switched on and starts working by itself. The button is
the short cut, and the answer when the room has changed since."""
new = """| 3 | `fx.mnr.on` = 1 (`src` already defaults to `input`), then hold **Hold to learn** for a second in a quiet room |
| 4 | on the computer, pick the **osynth audio** capture device |

Step 3's button is optional: left alone, `fx.mnr` tracks the floor by itself
within `fx.mnr.adapt`. The button is the short cut, and the answer when the
room has changed since.

Use `fx.anr` instead of `fx.mnr` at step 3 if the 5.3 ms of latency matters
more than the noise does, and `fx.nr` **in addition** only with its `src` set
to `bus` — as a final gate on the gaps, after the mic unit has already cleaned
what is in them."""
assert s.count(old) == 1, 'setup table'
s = s.replace(old, new)

p.write_text(s, encoding='utf-8', newline='')
print('PARAM_MAP.md: shared section patched')
