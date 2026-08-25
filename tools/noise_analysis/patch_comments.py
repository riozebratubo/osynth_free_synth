"""Brings the S39 design notes back in line with what the code now does."""
import io
p = 'components/fx/fx.cpp'
s = io.open(p, encoding='utf-8').read()

old = """ * Reconstruction is by *residual*, not by summing the bank back up:
 *
 *     y = x + sum_k (g_k - 1) * band_k(x)
 *
 * which buys the one property that matters for a unit that spends most of its
 * life doing nothing: with every g_k at 1 the output is the input, sample for
 * sample — no filterbank colouration, no phase smear to explain away. A bank
 * summed the ordinary way is only approximately flat, and "approximately
 * flat" is an audible dulling that would sit on the patch whether or not
 * there was any noise to remove. The price is at the other end — the deepest
 * achievable cut is bounded by how well the bank sums back — and that end is
 * capped by `fx.anr.floor` anyway, which no useful setting takes past -30 dB.
 *
 * Band shapes: the first is a lowpass and the last a highpass, both
 * Butterworth, so `low` and `high` are crossovers and not centres. That is
 * what makes the bank cover the whole spectrum, and it matters more here than
 * anywhere else in this file: rumble under `low` and hiss over `high` are
 * precisely the two places noise lives, and a bank of bandpasses alone would
 * have left both of them untouched. Everything between is a *unity-peak*
 * bandpass — BpN, not Bp, because the residual form adds each band back at
 * its own gain and a plain Bp peaks at Q, which would subtract two and a half
 * times what it measured.
 *
 * ---- the estimator ----
 *
 * A sliding-window minimum, in two buckets held one window each. The floor of
 * a band is its minimum over a few seconds, not its average: an average
 * includes the speech, the minimum does not. `fx.anr.adapt` is the window.
 *"""

new = """ * Reconstruction is by *residual*, not by summing the bank back up:
 *
 *     y = x + sum_k (g_k - 1) * band_k(x)
 *
 * which buys the one property that matters for a unit that spends most of its
 * life doing nothing: with every g_k at 1 the output is the input, sample for
 * sample — no filterbank colouration, no phase smear to explain away.
 *
 * ---- the bank (S39c) ----
 *
 * That residual form only pays off if the bands sum back to x, because with
 * every g_k at `fx.anr.floor` the output is
 *
 *     x + (gmin - 1) * sum_k band_k(x)
 *
 * and whatever the bank fails to account for is *not attenuated at all*. It
 * is the floor of the whole unit, not a detail. The first version of this
 * bank was a Butterworth lowpass, a run of constant-Q unity-peak bandpasses
 * and a Butterworth highpass, on the reasoning that skirts crossing at -3 dB
 * sum back to something flat. They do not: adjacent bandpasses cross at
 * different phases, and measured on a real recording that bank summed to
 * -9 dB at 100 Hz, +3 dB through the midrange and -5 dB at 8 kHz, leaving
 * 18% of the input outside the bank entirely. The unit could not cut by more
 * than about 8 dB however `fx.anr.floor` was set — and past -10 dB, setting
 * it *deeper* made the output louder, because the bands that summed hot were
 * being over-subtracted while the residue sat there untouched.
 *
 * So the bank telescopes instead. bands-1 Butterworth lowpasses, geometrically
 * spaced, and each band is the difference of two neighbouring ones:
 *
 *     band_0     = LP_0(x)
 *     band_k     = LP_k(x) - LP_k-1(x)
 *     band_n-1   = x - LP_n-2(x)
 *
 * The sum is x identically — an algebraic identity, not a filter design, so
 * it holds sample for sample at every frequency, for any spacing, any band
 * count, any Q. All g_k at 1 gives x; all g_k at gmin gives gmin*x exactly;
 * `fx.anr.floor` means what it says. `low` and `high` are still the first and
 * last crossover, naming the same two frequencies they named before, and the
 * bank still covers the whole spectrum — rumble under `low` and hiss over
 * `high` are precisely where noise lives. It is also *cheaper*: bands-1
 * filters per channel where the old bank ran bands.
 *
 * What it costs is selectivity. Neighbouring bands overlap heavily — at
 * twelve bands a band's neighbour is only ~2.5 dB down at its centre — so the
 * profile is a blurred picture of the spectrum rather than a sharp one. That
 * is a good trade twice over: noise floors are smooth in frequency, so there
 * is little detail to lose, and a gain curve that cannot vary sharply between
 * neighbours is exactly the curve that does not produce the isolated
 * opening-and-closing bands this class of algorithm is known for. It is also
 * why `fx.anr.bands` now barely changes the result — 8, 12 and 16 land within
 * 0.1 dB of each other on stationary noise.
 *
 * ---- the estimator ----
 *
 * A sliding-window minimum over the band's level, held in kAnrSubWins buckets
 * that between them cover `fx.anr.adapt`. The floor of a band is its minimum
 * over a few seconds, not its average: an average includes the speech, the
 * minimum does not.
 *
 * The level it takes the minimum *of* is smoothed first (kAnrEstTauS), and
 * that one-pole is what makes the whole unit work. Without it the minimum is
 * taken over raw per-block magnitudes, and a block is 1.33 ms — a sixth of a
 * cycle at the bottom crossover. What such a block measures is not the level
 * of the band, it is where in its waveform the band happened to be, and the
 * minimum of *that* over a few thousand blocks is a number far below anything
 * present in the signal. Measured on a stationary recording the gap between a
 * band's median block and its minimum block ran from 5 dB at the top of the
 * bank to 28 dB at the bottom, so the unit believed the noise floor was up to
 * 28 dB quieter than it was and subtracted next to nothing — while kAnrBias,
 * one constant, tried to correct a bias that was not one number but twelve.
 * With 150 ms of smoothing ahead of it that spread is 1-3 dB across the whole
 * bank, which is a bias small enough and uniform enough for kAnrBias to be
 * the fixed factor it was always documented as.
 *
 * Only the *estimator* reads the smoothed level. The gain below reads the raw
 * block magnitude, because that half has to move at the speed of a syllable:
 * running the gain off the smoothed level too costs another 5 dB of noise cut
 * and, worse, holds each band shut for 150 ms into every word.
 *
 * Buckets rather than the two the first version held, so the first usable
 * profile lands one bucket after switch-on rather than one whole window — at
 * the default `adapt` that is 0.75 s instead of 8 s of a unit that is on,
 * lit, and audibly doing nothing.
 *"""
assert s.count(old) == 1, 'bank/estimator note'
s = s.replace(old, new)

# The creep paragraph now has to say what it is a rate *of*.
old2 = """ *   - if two whole windows go by with nothing offered — something loud has
 *     been sitting in that band the entire time — the estimate may climb
 *     toward the raw minimum, by no more than kAnrCreep per window. Without
 *     an escape the unit locks out completely on any input that starts loud;
 *     with an unbounded one, a long pad is learned and fades away under the
 *     player's hands. Bounded, a pad 40 dB up takes the better part of a
 *     minute to be mistaken for noise, while a floor that genuinely rises
 *     6 dB is tracked in one window."""
new2 = """ *   - if a whole window goes by with nothing offered in any bucket —
 *     something loud has been sitting in that band the entire time — the
 *     estimate may climb toward the raw minimum, by no more than kAnrCreep
 *     per kAnrCreepRefS. Without an escape the unit locks out completely on
 *     any input that starts loud; with an unbounded one, a long pad is
 *     learned and fades away under the player's hands. Bounded, a pad 40 dB
 *     up takes the better part of a minute to be mistaken for noise, while a
 *     floor that genuinely rises 6 dB is tracked in one window — that one is
 *     the offer test's job, not the creep's, which is why slowing the creep
 *     down does not slow down tracking a floor that really moved.
 *
 *     Per *second*, note, not per bucket: the rate at which a held note is
 *     mistaken for a fan is a promise to the player, and it must not change
 *     just because `fx.anr.adapt` was moved. Anchored to the bucket instead,
 *     shortening `adapt` from 8 s to 3 s would have made that creep ten times
 *     faster and eaten a held pad in nine seconds."""
assert s.count(old2) == 1, 'creep note'
s = s.replace(old2, new2)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('comments patched')
