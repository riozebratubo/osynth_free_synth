"""src default + NR threshold rationale (S39c)."""
import io
p = 'components/fx/fx.cpp'
s = io.open(p, encoding='utf-8').read()

old = """ * `input` is the answer to "clean my microphone without putting a denoiser
 * across my synth": the unit runs on the block audio_io mixed in at the fx
 * position and adds only the difference back — so nothing reaching the bus is
 * a function of anything but the input. It needs `in.route` = fx, and
 * is inert otherwise — the reasoning is above FX_PID_ANR_SRC in fx.h. */"""
new = """ * `input` is the answer to "clean my microphone without putting a denoiser
 * across my synth": the unit runs on the block audio_io mixed in at the fx
 * position and adds only the difference back — so nothing reaching the bus is
 * a function of anything but the input. It needs `in.route` = fx, and
 * is inert otherwise — the reasoning is above FX_PID_ANR_SRC in fx.h.
 *
 * It is also the *default* for both units (S39c), which is why entry 0 being
 * the pre-control behaviour is now only an enum-ordering rule and no longer
 * describes what a fresh patch does. Cleaning a microphone is what anyone who
 * switches one of these on is trying to do; denoising the synth's own bus is
 * the specialist case, and it is the one that has a held pad to lose. */"""
assert s.count(old) == 1, 'kNrSrcs note'
s = s.replace(old, new)

old2 = """ *   the expander  everything below fx.nr.thresh is pushed down at
 *                 fx.nr.ratio, no further than fx.nr.floor, and not until
 *                 fx.nr.hold has run out.
 *"""
new2 = """ *   the expander  everything below fx.nr.thresh is pushed down at
 *                 fx.nr.ratio, no further than fx.nr.floor, and not until
 *                 fx.nr.hold has run out.
 *
 * ---- where the threshold has to sit (S39c) ----
 *
 * `thresh` is the one number in this unit that cannot be guessed from first
 * principles, because it is an absolute level and the thing it has to sit
 * above is a property of the room and the gain, not of the algorithm. It
 * shipped at -45 dBFS, which is a sensible figure for a quiet line source and
 * a useless one for the microphone this unit exists to clean: osynth's own
 * mic at `in.micgain` 1.33 in an ordinary room puts this detector at about
 * -29 dBFS on the ambience alone. A threshold 16 dB underneath that is never
 * crossed from below, so `hold` re-armed on every block, the expander sat
 * fully open for the entire recording and the unit did — measurably, on a
 * 10 s capture — 0.4 dB of nothing.
 *
 * -24 dBFS is the default now: above that mic's floor with a few dB to spare,
 * below anything anyone would call speech. It is still a number the player
 * has to own — halve the gain and it wants moving with it — but it is now
 * wrong in the direction that is audible and adjustable rather than the
 * direction that looks like a broken feature. Note that it is a *peak*
 * detector reading after the high-pass, so it sits a few dB above the RMS
 * level the master meter shows.
 *"""
assert s.count(old2) == 1, 'nr threshold note'
s = s.replace(old2, new2)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('comments2 patched')
