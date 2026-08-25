/*
 * osynth — a small radix-2 real FFT for the mic noise reduction unit (S42).
 *
 * Private to the `fx` component; fx.cpp is the only caller. Deliberately not
 * esp-dsp: that would be a new managed-component dependency fetched at build
 * time for two transforms per 128 samples, and this is 120 lines that build
 * everywhere the rest of the project does. If the transform ever shows up in a
 * profile, esp-dsp's assembly version is a drop-in for fft_real/fft_real_inv
 * and nothing above this header needs to know.
 *
 * The size is fixed at compile time because the twiddle tables are static and
 * the caller's frame buffers are too. `fft_init()` fills the tables and must
 * run once before either transform; fx_init() calls it.
 *
 * Not IRAM-resident, unlike the rest of the render path — see the note above
 * mnr_frame() in fx.cpp, this file's only caller.
 *
 * Verified against numpy.fft to 1e-14 forward and 1e-15 round trip by
 * tools/noise_analysis/fft_ref.py, which is the line-for-line reference this
 * file was transliterated from — including the one case that is easy to get
 * wrong, noted at the top of fft_real_inv().
 */
#pragma once

namespace osynth {
namespace fx {

constexpr int kFftN = 256;                 /* 5.3 ms at 48 kHz */
constexpr int kFftBins = kFftN / 2 + 1;    /* 129, DC..Nyquist inclusive */

/* Builds the twiddle tables. Idempotent, not thread-safe; call it from init. */
void fft_init();

/* kFftN real samples -> kFftBins complex bins. `re`/`im` are kFftBins long. */
void fft_real(const float* in, float* re, float* im);

/* kFftBins complex bins -> kFftN real samples, scaling included, so
 * fft_real_inv(fft_real(x)) == x. `im[0]` and `im[kFftBins-1]` are ignored:
 * DC and Nyquist are real for any real input, and a caller that has only
 * scaled the magnitudes cannot have made them anything else. */
void fft_real_inv(const float* re, const float* im, float* out);

}  // namespace fx
}  // namespace osynth
