"""S42 — the per-unit doc subsections: a new one for fx.mnr, and the S39c
corrections that the firmware changes made necessary in the fx.anr/fx.nr ones."""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
p = ROOT / 'private_docs/PARAM_MAP.md'
s = p.read_text(encoding='utf-8')

MNR = """#### Mic — per bin, under the voice (0x0344-0x0349, S42)

Per-bin suppression in the STFT domain: the family a USB headset chip,
WebRTC's NS, Speex's preprocessor and the Ephraim-Malah literature all belong
to. The only one of the three units here that removes anything **while you are
talking**, for the reason given in the table above — 129 bins instead of one
gain or a dozen bands, and speech is sparse in frequency.

256-point frames, 50% overlap, sqrt-Hann in and out (WOLA, unity at this hop),
one frame per two audio blocks. 129 bins, 187 Hz apart. 512 was better in the
gaps and worse everywhere that matters at twice the latency; 128 gave up the
resolution that is the whole point; a 75% hop was within 0.5 dB of 50% on
every measure and cost twice the transforms.

Per bin, per frame: the noise power is tracked as a minimum over `adapt` in
four buckets (Martin's minimum statistics, the same structure `fx.anr` uses,
including the one-pole ahead of the minimum — a raw periodogram bin wanders by
tens of dB frame to frame on stationary noise, and the minimum of the wander
is not the level of anything). Then the a-priori SNR by the **decision-directed
estimator** — mostly the previous frame's *result*, a little of this frame's
raw excess — and the Wiener gain from that, floored at `fx.mnr.floor`.

That decision-directed line is what separates a spectral subtractor that
sounds like a denoiser from one that sounds like wind chimes. Estimating from
the current frame alone makes the gain swing on the noise's own fluctuation,
each bin opening and closing independently; leaning on the previous decision
holds a bin steady unless something really arrived in it.

**Mono.** It folds its source down, cleans that, and writes the result to both
channels — the vocoder's reasoning unchanged: it exists to clean a microphone
and osynth's is mono, and it halves the transform count, which is what makes
the unit affordable at all. `bus` mode will fold a stereo patch, which is the
other reason `input` is the default.

**Latency: 5.3 ms** (one frame) on whatever it is cleaning, and it is the only
unit on this bus that has any. At `src` = `input` that delays the input against
the synth beside it and nothing else — the undelayed copy is *removed* from the
bus rather than mixed with the delayed one, so there is no comb filtering.

| id | name | type | range | default | notes |
| --- | --- | --- | --- | --- | --- |
| `0x0344` | `fx.mnr.on`     | bool  | 0..1            | 0       |                                    |
| `0x0345` | `fx.mnr.src`    | enum  | 0..1            | input   | bus / input — see above; `input` needs `in.route` = fx |
| `0x0346` | `fx.mnr.amount` | float | 0..1            | 0.6     | over-subtraction: 1 subtracts 3x the estimated floor. Inflates the estimate rather than scaling the gain, so turning it up deepens the cut where there is noise and still leaves a loud bin alone |
| `0x0347` | `fx.mnr.floor`  | float | -48..0 dB       | -24     | deepest cut per bin. A bin taken to nothing is what a listener hears as an artefact; 24 dB down is heard as a quiet room |
| `0x0348` | `fx.mnr.adapt`  | float | 0.2..20 s (exp) | 1.5 s   | the minimum-tracking window. Shorter tracks a moving floor and starts eating sustained tones; 3 s and up gave up 7 dB in the gaps |
| `0x0349` | `fx.mnr.learn`  | bool  | 0..1            | 0       | momentary: the profile becomes what is arriving now, no minimum needed. Not stored in presets |

`0x034A`-`0x034F` are reserved for this unit. The ids sit in the bitcrush
block's unused tail because there was no free 0x03x0 block left, and
`fx_init()` unregisters this component with
`removeRange(PID_FX_BASE, PID_SEQARP_BASE)` — an id outside `0x03xx` would
register and then never be cleaned up.

Cost is two 128-point complex FFTs per 128 samples plus 129 bins of arithmetic,
i.e. one frame every two audio blocks, and about 4.5 KB of state. The
transform is `components/fx/fx_fft.cpp` — a self-contained radix-2 real FFT
rather than a new esp-dsp dependency for two transforms per frame; it is a
drop-in swap if it ever shows up in a profile.

"""

anchor = "#### Fixed — the USB-microphone chain (0x03Fx)"
assert s.count(anchor) == 1, 'mnr anchor'
s = s.replace(anchor, MNR + anchor)

# ---------------------------------------------- S39c corrections: the bank --
old = """Reconstruction is by **residual** — `y = x + sum (g-1) * band(x)` — so with
nothing to remove the output is the input, sample for sample, with no
filterbank colouration to explain away. The first band is a lowpass and the
last a highpass, which makes `low` and `high` crossovers rather than centres:
rumble under `low` and hiss over `high` are exactly where noise lives, and a
bank of bandpasses alone would have covered neither.

The estimator is a sliding-window minimum in two buckets, `adapt` seconds
each, biased upward (the minimum of a fluctuating noise sits below its
average). Two guards keep it from eating the instrument, both of which matter
because a held pad looks exactly like a fan:"""
new = """Reconstruction is by **residual** — `y = x + sum (g-1) * band(x)` — so with
nothing to remove the output is the input, sample for sample, with no
filterbank colouration to explain away.

That form only pays off if the bands sum back to `x`, because whatever the bank
fails to account for is never attenuated at all. **S39c replaced the bank for
exactly this reason.** The original was a Butterworth lowpass, constant-Q
unity-peak bandpasses and a Butterworth highpass, on the reasoning that skirts
crossing at -3 dB sum flat. They do not: measured, that bank summed to -9 dB at
100 Hz, +3 dB midrange and -5.5 dB at 8 kHz, leaving 18% of the input outside
it. The unit could not cut by more than ~8 dB however `floor` was set, and past
-10 dB setting it *deeper made the output louder*.

The bank now telescopes: `bands-1` Butterworth lowpasses, geometrically
spaced, with `band_k = LP_k - LP_k-1` and the top band `x - LP_last`. The sum
is `x` as an algebraic identity — true at every frequency for any spacing or
band count — so `floor` means what it says. `low` and `high` are still the
first and last crossover, naming the frequencies they always named. It is also
cheaper: `bands-1` filters per channel instead of `bands`. What it costs is
selectivity — neighbours overlap by ~2.5 dB at twelve bands — which is why
`bands` now barely changes the result, and why this unit manages 4.5 dB under
a voice where `fx.mnr` manages more.

The estimator is a sliding-window minimum in four buckets covering `adapt`
between them, biased upward (the minimum of a fluctuating noise sits below its
average). The level it takes the minimum *of* is smoothed by a 150 ms one-pole
first, and **that smoother is what makes the unit work at all**: without it the
minimum ran over raw per-block magnitudes, and a block is 1.33 ms — a sixth of
a cycle at the bottom crossover. What that measured was not the band's level
but where in its waveform the band happened to be, running 5 dB low at the top
of the bank and 28 dB low at the bottom, so the unit believed the floor was far
quieter than it was and subtracted almost nothing. Smoothed, the spread is
1-3 dB across the bank, which is what the single `bias` constant was always
documented as correcting. Only the estimator reads the smoothed level; the gain
reads the raw one, because that half has to move at the speed of a syllable.

Two guards keep it from eating the instrument, both of which matter
because a held pad looks exactly like a fan:"""
assert s.count(old) == 1, 'anr bank/estimator'
s = s.replace(old, new)

old = """* if two whole windows pass with nothing offered, the estimate may climb
  toward the raw minimum but by no more than 6 dB per window. Without an
  escape the unit would lock out on any input that starts loud; unbounded, a
  long pad would be learned and would fade under your hands."""
new = """* if a whole window passes with nothing offered in any bucket, the estimate
  may climb toward the raw minimum but by no more than 6 dB **per 8 seconds**.
  Without an escape the unit would lock out on any input that starts loud;
  unbounded, a long pad would be learned and would fade under your hands. At
  the default a held pad 40 dB up is untouched at 30 s, 1.3 dB down at 45 s
  and gone by 60 s. The rate is per *second* and not per bucket deliberately:
  how fast a held note is mistaken for a fan is a promise to the player, and
  it must not change because `adapt` was moved."""
assert s.count(old) == 1, 'anr creep'
s = s.replace(old, new)

old = """band is not primed at all until something turns up in it: switch the unit
on mid-sentence and the sentence survives, switch it on over a silent bus
— the usual case, since that is when anyone sets this up — and it waits
for the input rather than priming every estimate to zero and then
rejecting every real signal for being too far above it. `fx.anr.learn` drops the window to
80 ms and waives the offer test, so a second of held button in a quiet room is
a complete profile."""
new = """band is not primed at all until something turns up in it: switch the unit
on mid-sentence and the sentence survives, switch it on over a silent bus
— the usual case, since that is when anyone sets this up — and it waits
for the input rather than priming every estimate to zero and then
rejecting every real signal for being too far above it. The first usable
profile lands one *bucket* after switch-on rather than one whole window, i.e.
0.75 s at the default rather than the 8 s of a lit unit doing nothing.
`fx.anr.learn` drops the window to 80 ms and waives the offer test, so a second
of held button in a quiet room is a complete profile."""
assert s.count(old) == 1, 'anr priming'
s = s.replace(old, new)

# ------------------------------------------------------- changed defaults --
for old, new in [
    ("| `0x03EA` | `fx.anr.src`     | enum  | 0..1           | bus     |",
     "| `0x03EA` | `fx.anr.src`     | enum  | 0..1           | input   |"),
    ("| `0x03E6` | `fx.anr.adapt`   | float | 0.5..60 s (exp)| 8 s     | the estimator's window, and so how long the unit listens before it starts |",
     "| `0x03E6` | `fx.anr.adapt`   | float | 0.5..60 s (exp)| 3 s     | the estimator's window; the first profile lands after a quarter of it |"),
    ("| `0x03E3` | `fx.anr.bands`   | int   | 4..16 / 4..10  | 12 / 8  | ceiling is per target; the default is below it on purpose — past a dozen bands the profile is finer than the noise it describes |",
     "| `0x03E3` | `fx.anr.bands`   | int   | 4..16 / 4..10  | 12 / 8  | ceiling is per target. Since S39c this barely changes the result — 8, 12 and 16 land within 0.1 dB — because the difference bank's neighbours overlap by design |"),
    ("| `0x03F9` | `fx.nr.src`     | enum  | 0..1            | bus     |",
     "| `0x03F9` | `fx.nr.src`     | enum  | 0..1            | input   |"),
]:
    assert s.count(old) == 1, old[:40]
    s = s.replace(old, new)

old = ("| `0x03F3` | `fx.nr.thresh`  | float | -80..0 dB       | -45     | peak, "
       "measured after the filters and on whatever `src` names — at `input` that is "
       "the input *as it arrives on the bus*, i.e. after `in.gain` |")
new = ("| `0x03F3` | `fx.nr.thresh`  | float | -80..0 dB       | -24     | peak, "
       "measured after the filters and on whatever `src` names — at `input` that is "
       "the input *as it arrives on the bus*, i.e. after `in.gain`. **The one number "
       "here that cannot be guessed**: it is absolute, and what it must sit above is "
       "the room and the gain. It shipped at -45, which is right for a quiet line "
       "source and useless for a microphone — osynth's own at `in.micgain` 1.33 puts "
       "this detector at about -29 dBFS on ambience alone, so the threshold was never "
       "crossed from below, `hold` re-armed every block and the unit did 0.4 dB of "
       "nothing. Halve the gain and this wants moving with it |")
assert s.count(old) == 1, 'nr thresh row'
s = s.replace(old, new)

old = """Neither unit is an FX LFO destination, for the same reason the vocoder is not:"""
new = """None of the three is an FX LFO destination, for the same reason the vocoder is
not:"""
assert s.count(old) == 1, 'lfo note'
s = s.replace(old, new)

old = """Cost is two SVFs per band per sample (one per channel) plus a block-rate
follower; everything else"""
new = """Cost is two SVFs per crossover per sample (one per channel, and there are
`bands-1` crossovers) plus a block-rate follower; everything else"""
assert s.count(old) == 1, 'anr cost'
s = s.replace(old, new)

p.write_text(s, encoding='utf-8', newline='')
print('PARAM_MAP.md: per-unit sections patched')
