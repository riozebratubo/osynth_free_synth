### Noise reduction (0x03Ex + 0x03Fx, S39 — part of the FX bus)

Two units at the head of the bus, added so a P4 build can be used as a plain
**USB microphone** for a computer. The path was already there — an input
device chosen by `in.source`, mixed in by `in.route` = `fx`, and out over the
UAC2 capture interface the USB tap already streams (S29) — and this is the
cleanup stage it was missing. Neither unit is microphone-specific; both are
ordinary bus units and will do the same to a hissy line input or a noisy
sampled loop.

They run **before the vocoder**, `fx.anr` first and `fx.nr` second, and that
order does not commute: the expander in `fx.nr` ducks the gaps between
phrases, and a ducked gap has no noise floor left in it to measure. Run the
other way round, `fx.anr` would learn the *gated* floor — up to `fx.nr.floor`
too low — and then under-subtract by exactly that much for the whole of the
next phrase. An estimator has to see the floor it is estimating.

What they do **not** reach is the vocoder's modulator: that comes from
`audio_io_in_mono()` rather than from this bus (see the vocoder section
above), so it is deaf to everything here.

Both are off by default and both are ordinary preset values, with one
exception: `fx.anr.learn` is on the preset skip list, like `fx.voc.freeze`.

Setting the instrument up as a USB microphone:

| step | what |
| --- | --- |
| 1 | `in.source` = the capsule (`mic`, or `both`), `in.route` = `fx` |
| 2 | `in.gain` / `in.micgain` until the heartbeat's `in mic` peaks read around 0.25 on speech |
| 3 | `fx.anr.on` = 1, then hold **Hold to learn** for a second in a quiet room |
| 4 | `fx.nr.on` = 1, `fx.nr.thresh` just under the quietest thing worth keeping |
| 5 | on the computer, pick the **osynth audio** capture device |

Step 3's button is optional: left alone, `fx.anr` listens for `fx.anr.adapt`
seconds after being switched on and starts working by itself. The button is
the short cut, and the answer when the room has changed since.

#### Adaptive — ambient noise (0x03Ex)

A per-band spectral subtractor. The bus is split into `bands` sections, each
keeps an estimate of its own *steady* level, and each is attenuated by however
much of what is in it looks like that estimate. Speech, notes and transients
move; a fan, a hiss floor and a spinning disk do not, and that difference is
the whole algorithm.

Reconstruction is by **residual** — `y = x + sum (g-1) * band(x)` — so with
nothing to remove the output is the input, sample for sample, with no
filterbank colouration to explain away. The first band is a lowpass and the
last a highpass, which makes `low` and `high` crossovers rather than centres:
rumble under `low` and hiss over `high` are exactly where noise lives, and a
bank of bandpasses alone would have covered neither.

The estimator is a sliding-window minimum in two buckets, `adapt` seconds
each, biased upward (the minimum of a fluctuating noise sits below its
average). Two guards keep it from eating the instrument, both of which matter
because a held pad looks exactly like a fan:

* a band is offered to the bucket only while it is within 12 dB of the current
  estimate — above that it is signal, and signal has no business in a noise
  profile;
* if two whole windows pass with nothing offered, the estimate may climb
  toward the raw minimum but by no more than 6 dB per window. Without an
  escape the unit would lock out on any input that starts loud; unbounded, a
  long pad would be learned and would fade under your hands.

Nothing is subtracted from a band until a window has closed over it, and a
band is not primed at all until something turns up in it: switch the unit
on mid-sentence and the sentence survives, switch it on over a silent bus
— the usual case, since that is when anyone sets this up — and it waits
for the input rather than priming every estimate to zero and then
rejecting every real signal for being too far above it. `fx.anr.learn` drops the window to
80 ms and waives the offer test, so a second of held button in a quiet room is
a complete profile.

| id | name | type | range | default | notes |
| --- | --- | --- | --- | --- | --- |
| `0x03E0` | `fx.anr.on`      | bool  | 0..1           | 0       |                                     |
| `0x03E1` | `fx.anr.amount`  | float | 0..1           | 0.6     | 1 subtracts 3x the estimated floor  |
| `0x03E2` | `fx.anr.floor`   | float | -48..0 dB      | -20     | deepest cut per band. Taking a band to silence is what makes residual noise sound like wind chimes; leaving 20 dB of it is what a listener hears as "quiet" |
| `0x03E3` | `fx.anr.bands`   | int   | 4..16 / 4..10  | 12 / 8  | ceiling is per target; the default is below it on purpose — past a dozen bands the profile is finer than the noise it describes |
| `0x03E4` | `fx.anr.low`     | float | 40..400 (exp)  | 120 Hz  | first crossover: below it, one lowpass band |
| `0x03E5` | `fx.anr.high`    | float | 2k..16k (exp)  | 9000 Hz | last crossover: above it, one highpass band |
| `0x03E6` | `fx.anr.adapt`   | float | 0.5..60 s (exp)| 8 s     | the estimator's window, and so how long the unit listens before it starts |
| `0x03E7` | `fx.anr.attack`  | float | 1..100 ms (exp)| 5 ms    | a band reopening; 1 ms floor is the block period |
| `0x03E8` | `fx.anr.release` | float | 5..1000 ms(exp)| 150 ms  | a band closing                      |
| `0x03E9` | `fx.anr.learn`   | bool  | 0..1           | 0       | momentary: adopt the profile now. Not stored in presets |

Cost is two SVFs per band per sample (one per channel) plus a block-rate
follower; everything else — estimate, gain, attack/release — runs once per
block and is ramped across the next one, the same shape the compressor uses.

#### Fixed — the USB-microphone chain (0x03Fx)

Three stages, nothing learned, every number chosen by the player. It is what
the adaptive unit cannot be — predictable — and that is its job description.

`fx.nr.floor` is what separates this from a gate, and it is the control worth
understanding. A gate closes, and a room that goes absolutely silent between
words does not sound like a quiet room: it sounds like a dropped connection,
and the moment it reopens the noise arrives as an audible swell. Capping the
attenuation at -24 dB leaves the room present and merely distant, which is
what "quiet" sounds like to a listener. Take it to -60 dB and this is a gate
again, deliberately.

`fx.nr.hold` is the other half. Speech ends in unvoiced consonants at a
fraction of the energy of the vowel before them, and an expander with no hold
eats every one of them — the word-ending chop that gives cheap gates away.

The detector runs on the *filtered* signal, which is why the filters are
first: a footfall is almost all energy under 80 Hz, and a gate that opens on
one has opened for something nobody can hear. One detector for both channels,
taking the larger of the two — a microphone that ducks one side and not the
other is a microphone with a hole in the middle.

| id | name | type | range | default | notes |
| --- | --- | --- | --- | --- | --- |
| `0x03F0` | `fx.nr.on`      | bool  | 0..1            | 0       |                                    |
| `0x03F1` | `fx.nr.hpf`     | float | 20..400 Hz (exp)| 80 Hz   | Butterworth rumble filter. The registered minimum **is** the bypass — the stage is skipped, not run at unity |
| `0x03F2` | `fx.nr.hum`     | enum  | 0..2            | off     | off / 50 Hz / 60 Hz — notches at the mains frequency and its first two harmonics, Q 20 |
| `0x03F3` | `fx.nr.thresh`  | float | -80..0 dB       | -45     | peak, measured after the filters |
| `0x03F4` | `fx.nr.ratio`   | float | 1..20 (exp)     | 4       | downward expansion below the threshold, 6 dB soft knee |
| `0x03F5` | `fx.nr.floor`   | float | -60..0 dB       | -24     | maximum attenuation — see above |
| `0x03F6` | `fx.nr.attack`  | float | 1..100 ms (exp) | 3 ms    | detector rise, i.e. how fast it opens |
| `0x03F7` | `fx.nr.hold`    | float | 0..1000 ms      | 150 ms  | linear, because 0 is a real setting |
| `0x03F8` | `fx.nr.release` | float | 5..1000 ms (exp)| 200 ms  | detector fall, i.e. how fast it closes |

Neither unit is an FX LFO destination, for the same reason the vocoder is not:
the destination enum is close to overflowing one `PARAM_INFO` frame. Nothing
here would want a modulator anyway — a noise floor that moves in time with the
tempo is not a noise floor.

These two blocks **fill the 0x03xx namespace**. A tenth FX unit needs a page
of its own.
