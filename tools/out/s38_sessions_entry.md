
## S38 — 2026-08-21 — Granular engine: a grain cloud, and the input inside it

A fifth fixed engine (`components/engines/engine_granular.cpp`), 42
parameters at 0x02xx, `caps` = filter | env2 | lfo1 | lfo2 | modmatrix. Per
voice: a cloud of windowed grains -> the S33 filter family -> amp env. Two
grain sources behind `grn.src`:

- **`synth`** — each grain is an oscillator burst at its own frequency. In
  `sync` mode one grain is emitted per cycle of the note, so the *train*
  carries the pitch and `grn.form` (grain frequency / key frequency) is a
  formant that moves independently of it. That is FOF / pulsar synthesis, and
  it is the reason this earns an engine slot rather than being a preset of
  something else: a fixed oscillator chain has no way to move a spectral peak
  against a fundamental. `free` mode unhooks the onsets from the key and the
  cloud goes asynchronous.
- **`in`** — a window onto a capture ring filled from the audio input, 2 s on
  PSRAM and 0.35 s otherwise, mono int16. Transposed against `buf.root`,
  scattered by `buf.pos` / `buf.spray`, `buf.rev` for backwards grains, and
  `buf.freeze` to stop the write head and turn the ring into a fixed sample.

Both sources register on every build. `in` renders silence where there is no
audio input, which is the modular graph's LineIn contract (S31f) applied to
an engine: a parameter that exists on one build and not the next is a preset
that loads differently depending on who reads it.

### The engine numbering had to be frozen first

Adding a sixth entry is what retired the "shorter enum" idea from S28. The
modular engine was compiled *out of* `synth_engine_type_t` when its Kconfig
option was off, on the reasoning that a shorter enum means no stored value
can name an engine that is not there. That works right up until something
comes after it: an entry following a conditional one has no fixed number, and
the engine index is not private — it is in the preset filename
(`p<engine>_<slot>.osp`), in that file's header, in `OP_SELECT_ENGINE` and in
program change.

Both ways out were worse than the disease:

- granular at 4, modular at 5 — renumbering modular. Existing `p4_*.osp`
  files are version-2/4 with a graph blob and header `engine == 4`; after the
  renumber `do_load` still matches on engine id and scatters a graph patch's
  values across granular's parameters. Silently, and only for people who had
  saved modular patches.
- granular after a conditional modular — its index becomes 5 or 4 depending
  on a Kconfig option, so a rebuild orphans a user's granular presets.

So the numbers are frozen and *availability* is what varies: every value is
unconditional, `engines_get()` returns NULL for an engine this build lacks,
and `s_engines[4]` is a literal `nullptr` when the graph is not compiled in.
That path was always reachable — `engines_init()` falls back to subtractive at
boot, the switch task warns and reverts `engine.type`, PARAM_INFO serves caps
0 — it is now simply also how an absent modular engine reports itself. The
factory bank's modular row loses its `#if` for the same reason: the row has to
exist to keep granular at 5.

### What the DSP does differently from the S11 granular delay

The FX bus has had a grain pool since S11 and a lot of it ported over — the
window, the immutable per-grain rate, the equal-power random pan, the
`1/sqrt(dens*size)` density normalisation, the "read the parameters at spawn
so a flying grain's bounds cannot move" rule. Four things could not be
copied:

- **Onsets are sample-accurate, not block-accurate.** The effect spawns on
  block boundaries because 1.33 ms of onset jitter is inaudible in a delay.
  Here, in `sync` mode, the onset grid *is* the pitch — quantising it to
  750 Hz would detune every note. The accumulator keeps its fraction and each
  grain is pre-advanced by the sub-sample remainder.
- **The normalisation exponent is not fixed.** A `sync` train with no jitter
  and no scatter is phase-coherent: every grain traces the same trajectory
  and they add in *amplitude*, n rather than sqrt(n) — that coherence is the
  formant. Jitter, pitch scatter or free-running density break the relation
  and they add in power. The two differ by the whole of sqrt(n): 11 dB at an
  overlap of 12. `grain_norm()` interpolates the exponent on how decorrelated
  the cloud actually is, taking six semitones of scatter as fully
  decorrelated.
- **The pool is a budget, not a wall.** S11's rule is "pool full: skip, never
  steal", which is right for an effect and wrong here — a skipped spawn drops
  a grain out of the train and takes the pitch with it. Instead the budget is
  divided by the voices that sounded last block and grain *length* is clamped
  so the expected overlap fits. Shortening a grain widens its formant, which
  is the trade FOF makes at high pitch anyway: audible as timbre, not as gaps.
- **Read positions are re-anchored per block, not accumulated.** A float
  delay near 1e5 accumulated across a 9600-sample grain drifts by tens of
  samples on a 24-bit mantissa — an audible detune *and* a lost bound. The
  exact state is `{d0, e, wbase}` and the running `d` is rebuilt from it at
  the top of every block, so at most one block of steps ever accumulates.

The `buf.freeze` bound is the one that needed care. A grain's read position is
a distance behind the write head, so the head stopping mid-grain changes the
per-sample travel from (1 - rate) to (-rate) — a live switch that inverts the
sign of the term the bounds were proven against. `grain_spawn()` therefore
sizes against `max(rate,0) + max(1-rate,0)`, which covers both regimes, plus a
block in each direction because the write head moves in block-sized jumps
while the read head moves per sample. The ring is also filled every block even
when the input is absent, so a head that quietly stopped can never repitch
what is flying.

Two filter states per voice rather than one, which is the other break from the
house pattern: the other engines pan a mono voice with `f->gain_l/r`, but here
every grain lands at its own place in the field, so filtering the mono sum
would mean rebuilding a stereo image out of a sum — which cannot be done.

### Everything else

- Registry, CMake (`PRIV_REQUIRES audio_io` for the ring), `kEngineNames` with
  a `static_assert` against `SYNTH_ENGINE_COUNT`, and a `k_cc_granular` row.
  CC 74 stays the filter cutoff and deliberately does *not* land on
  `grn.form`: that sounds like a brightness control and is not one — sweeping
  it is a vowel, and one CC has to mean one thing across the engines.
- Factory bank: 16 written, 32 left at "init". A granular patch lives or dies
  on the interaction of grain length, onset rate and window shape, and two
  settings a semitone apart on paper can be a vowel and a rattle. Sixteen that
  were listened to beats forty-eight that were reasoned about, and the bank is
  append-only. Three of them set `grn.src = in` and are named "in: ..." so it
  is obvious before loading that they need something plugged in.
- App: `engineList` replaces HomeScreen's hardcoded engine array. It counts
  `engine.type`'s enum labels from PARAM_INFO — so one app build talking to
  pre-S38 firmware sees four names and never draws a Granular button that
  would come back `ST_BAD_ARG` — and still gates Modular on GRAPH_INFO, since
  a reserved index is now in the enum whether or not the engine is there.
  `PatchLibraryScreen.engineNames` is *not* the same list and keeps all six:
  it names the engine that wrote a saved row, including ones the connected
  synth does not have.

### To verify on hardware

- Default patch, a held note: a hollow vocal tone, no clicking at the grain
  rate. Sweep `grn.form` — the formant should move while the pitch stays put.
- `grn.shape` from 0.05 to 0.95 on a long grain: struck -> bowed, and no click
  at either extreme (the warped Hann is what buys that; a two-piece parabola
  would tick).
- Level check across the range the normalisation exists for: `grn.dens`
  1 -> 400 in `free` mode, then `grn.jit` 0 -> 1 in `sync` mode. Neither should
  move the perceived level much; the second is the exponent doing its job.
- Polyphony: hold eight notes high on the keyboard. Grains should get shorter
  (timbre thins, formant widens) with no dropouts and no underruns in the
  heartbeat.
- `grn.src = in` with a source plugged in: scrub `buf.pos`, then throw
  `buf.freeze` *while notes are held* — the frozen ring should keep playing at
  the same pitch, which is the bound that had to cover both regimes.
- Save a granular preset, switch to another engine and back, reload it. Then
  confirm existing modular presets still load correctly — the numbering change
  is what that is checking.
