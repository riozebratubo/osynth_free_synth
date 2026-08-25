"""S42 — mic noise reduction: the fx.h half (PIDs + the note above them)."""
import io
p = 'components/fx/include/fx.h'
s = io.open(p, encoding='utf-8').read()

old = """/* Fixed: high-pass, hum notch, downward expander. `fx.nr.floor` is the most"""
new = """/* Mic noise reduction (S42): per-bin suppression in the STFT domain — the
 * family a USB headset chip, WebRTC's NS and the Ephraim-Malah literature all
 * belong to, and the only one of the three units here that removes anything
 * *while you are talking*.
 *
 * The other two cannot, and the reason is structural rather than a matter of
 * tuning. `fx.nr` computes one gain for the whole spectrum from one level
 * detector, so the instant a voice crosses its threshold the gain goes to 1
 * and the hiss comes back with it at full level: measured on a real recording
 * it improves signal-to-noise by 0.0 dB, and only ever cleans the gaps
 * *between* words. `fx.anr` is the right family but coarse — a dozen heavily
 * overlapping bands, so a voice in one region opens the gain across a wide
 * swathe. This unit runs 129 bins, and the whole trick is that speech is
 * sparse in frequency: at any instant it occupies a fraction of them, and
 * every bin it is not in can still be pushed down.
 *
 * `fx.mnr.learn` is momentary and works exactly as the adaptive unit's does —
 * held, the noise profile is taken straight from what is arriving instead of
 * from a minimum over `fx.mnr.adapt`, so a second of a quiet room is a
 * complete profile. Not stored in presets, for the reason fx.anr.learn is not.
 *
 * MONO. It folds its source down, cleans that, and writes the result to both
 * channels — the same choice the vocoder makes, for the same reason: it exists
 * to clean a microphone and osynth's is mono. In `input` mode that is exact.
 * In `bus` mode it will fold a stereo patch, which is why `input` is the
 * default and the other setting is for mono material.
 *
 * It costs 5.3 ms of latency (one 256-sample frame) on whatever it is
 * cleaning. In `input` mode that delays the input against the synth beside it
 * and nothing else.
 *
 * The ids are borrowed from the bitcrush block's unused tail rather than given
 * a block of their own, because there is no block left: every 0x03x0 in the FX
 * namespace is assigned, and fx_init() unregisters this component with
 * removeRange(PID_FX_BASE, PID_SEQARP_BASE), so an id outside 0x03xx would
 * register and then never be cleaned up. Bitcrush has four parameters and has
 * had four since it was written. */
#define FX_PID_MNR_ON     0x0344
#define FX_PID_MNR_SRC    0x0345
#define FX_PID_MNR_AMOUNT 0x0346
#define FX_PID_MNR_FLOOR  0x0347
#define FX_PID_MNR_ADAPT  0x0348
#define FX_PID_MNR_LEARN  0x0349
/* 0x034A..0x034F stay free for this unit, and only this unit. */

/* Fixed: high-pass, hum notch, downward expander. `fx.nr.floor` is the most"""
assert s.count(old) == 1
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('fx.h patched')
