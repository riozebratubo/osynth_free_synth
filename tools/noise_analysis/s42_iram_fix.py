"""S42 — take the mic NR's code out of IRAM.

IRAM was already at ~149 KB with the FX bus alone holding 41 KB of it, and
S42's 2.6 KB pushed the link over by 2150 bytes. On this target that presents
as `--enable-non-contiguous-regions discards section ...` rather than the
"region iram0 overflowed" the note in synth_config.h names, but it is the same
wall.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

NOTE = """/* Deliberately NOT SYNTH_RENDER_IRAM, unlike every other unit on this bus
 * (S42). IRAM is full: the FX bus alone holds 41 KB of it, and this unit's
 * 2.6 KB was 2150 bytes more than the image had left. Something had to give,
 * and this is the right thing to give for three reasons — it is the newest
 * code, so nothing else was working before it and stopped; it is opt-in and
 * off by default, where the units it would have displaced run on every patch;
 * and it is the only unit here that already carries 5.3 ms of latency, so it
 * is the one with somewhere to absorb a cache miss.
 *
 * What that costs is the jitter immunity the note above SYNTH_RENDER_IRAM in
 * synth_config.h describes: with `fx.mnr` on, BLE or LittleFS traffic can put
 * a flash-cache miss in the middle of a frame. The frame is one per two
 * blocks rather than per block, and 2.7 ms of budget is a lot of misses, so
 * this should be inaudible — but it is a real difference from the rest of the
 * bus and the first thing to suspect if this unit alone crackles under app
 * traffic. Marking it back up means finding the IRAM somewhere else; turning
 * OSYNTH_RENDER_IN_IRAM off entirely is the blunt instrument that frees all
 * of it, at the cost of every other unit's immunity. */
"""

# ---------------------------------------------------------------- fx.cpp ---
p = ROOT / 'components/fx/fx.cpp'
s = p.read_text(encoding='utf-8')

old = """/* One frame: window, transform, scale every bin, transform back, overlap-add.
 * Everything that is not per-bin has been hoisted into the caller. */
void SYNTH_RENDER_IRAM mnr_frame("""
new = ("""/* One frame: window, transform, scale every bin, transform back, overlap-add.
 * Everything that is not per-bin has been hoisted into the caller. */
""" + NOTE + """void mnr_frame(""")
assert s.count(old) == 1, 'mnr_frame'
s = s.replace(old, new)

old = """void SYNTH_RENDER_IRAM mnr_process(float* __restrict__ bl,
                                   float* __restrict__ br, size_t frames) {"""
new = """/* Out of IRAM with mnr_frame() above — the note there covers why. */
void mnr_process(float* __restrict__ bl, float* __restrict__ br,
                 size_t frames) {"""
assert s.count(old) == 1, 'mnr_process'
s = s.replace(old, new)
p.write_text(s, encoding='utf-8', newline='')
print('fx.cpp: mic NR out of IRAM')

# ------------------------------------------------------------ fx_fft.cpp ---
p = ROOT / 'components/fx/fx_fft.cpp'
s = p.read_text(encoding='utf-8')
for old, new in [
    ("""void SYNTH_RENDER_IRAM fft_cplx(float* __restrict__ re, float* __restrict__ im,
                                bool inverse) {""",
     """void fft_cplx(float* __restrict__ re, float* __restrict__ im, bool inverse) {"""),
    ("""void SYNTH_RENDER_IRAM fft_real(const float* __restrict__ in,
                                float* __restrict__ re,
                                float* __restrict__ im) {""",
     """void fft_real(const float* __restrict__ in, float* __restrict__ re,
              float* __restrict__ im) {"""),
    ("""void SYNTH_RENDER_IRAM fft_real_inv(const float* __restrict__ re,
                                    const float* __restrict__ im,
                                    float* __restrict__ out) {""",
     """void fft_real_inv(const float* __restrict__ re, const float* __restrict__ im,
                  float* __restrict__ out) {"""),
]:
    assert s.count(old) == 1, old[:40]
    s = s.replace(old, new)

old = """#include <cmath>

#include "synth_config.h"
"""
new = """#include <cmath>
"""
assert s.count(old) == 1, 'fx_fft include'
s = s.replace(old, new)

old = """ * Verified against numpy.fft to 1e-14 forward and 1e-15 round trip by"""
new = """ * Not IRAM-resident, unlike the rest of the render path — see the note above
 * mnr_frame() in fx.cpp, which is this file's only caller.
 *
 * Verified against numpy.fft to 1e-14 forward and 1e-15 round trip by"""
assert s.count(old) == 1, 'fx_fft header note'
s = s.replace(old, new)
p.write_text(s, encoding='utf-8', newline='')
print('fx_fft.cpp: out of IRAM, synth_config.h no longer needed')
