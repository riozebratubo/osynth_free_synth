/*
 * osynth — radix-2 real FFT (S42). See fx_fft.h for why this is here rather
 * than esp-dsp, and for the reference it was transliterated from.
 */
#include "fx_fft.h"

#include <cmath>

namespace osynth {
namespace fx {

namespace {

constexpr int kH = kFftN / 2;   /* the complex transform's length: 128 */

/* w[j] = exp(-2*pi*i*j/kH), j = 0..kH/2-1. One table serves every stage of
 * the complex FFT: a stage of half-length `half` steps it by kH/(2*half).
 * A table rather than the usual per-stage recurrence, because the recurrence
 * accumulates phase error across a stage and this runs in float. */
float s_tw_r[kH / 2];
float s_tw_i[kH / 2];

/* htw[k] = exp(-2*pi*i*k/kFftN), k = 0..kH-1 — the half-length twiddle that
 * recombines the even and odd halves after the packed transform. */
float s_htw_r[kH];
float s_htw_i[kH];

/* Spelled out rather than the M_PI macro: that one is a POSIX extension and
 * not standard C++, so it is absent under a strict -std=c++NN. The rest of
 * this project writes the literal for the same reason. */
constexpr double kPi = 3.14159265358979323846;

bool s_ready = false;

/* The packed half-length transform, shared by both directions. On the stack
 * this is 1 KB inside a 6 KB audio task that also runs a reverb, so it lives
 * here instead — the same call-scoped-static idiom fx.cpp uses for the
 * noise-reduction source block, and safe for the same reason: one audio task,
 * and neither function is re-entered or nested inside the other. */
float s_zr[kH];
float s_zi[kH];

/* In-place iterative radix-2 decimation-in-time. `inverse` conjugates the
 * twiddles and nothing else — the 1/n is applied by the caller. */
void fft_cplx(float* __restrict__ re, float* __restrict__ im, bool inverse) {
    /* Bit-reversal permutation. */
    int j = 0;
    for (int i = 1; i < kH; ++i) {
        int bit = kH >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) {
            const float tr = re[i]; re[i] = re[j]; re[j] = tr;
            const float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }
    for (int length = 2; length <= kH; length <<= 1) {
        const int half = length >> 1;
        const int step = kH / length;
        for (int i = 0; i < kH; i += length) {
            int k = 0;
            for (int m = 0; m < half; ++m) {
                const float wr = s_tw_r[k];
                const float wi = inverse ? -s_tw_i[k] : s_tw_i[k];
                k += step;
                const int a = i + m;
                const int b = a + half;
                const float vr = re[b] * wr - im[b] * wi;
                const float vi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - vr;
                im[b] = im[a] - vi;
                re[a] = re[a] + vr;
                im[a] = im[a] + vi;
            }
        }
    }
}

}  // namespace

void fft_init() {
    if (s_ready) return;
    for (int j = 0; j < kH / 2; ++j) {
        const double a = -2.0 * kPi * (double)j / (double)kH;
        s_tw_r[j] = (float)cos(a);
        s_tw_i[j] = (float)sin(a);
    }
    for (int k = 0; k < kH; ++k) {
        const double a = -2.0 * kPi * (double)k / (double)kFftN;
        s_htw_r[k] = (float)cos(a);
        s_htw_i[k] = (float)sin(a);
    }
    s_ready = true;
}

void fft_real(const float* __restrict__ in, float* __restrict__ re,
              float* __restrict__ im) {
    /* Pack the real input as kH complex samples — evens real, odds imaginary —
     * so one half-length transform does the work of a full-length one. */
    float* const zr = s_zr;
    float* const zi = s_zi;
    for (int i = 0; i < kH; ++i) {
        zr[i] = in[2 * i];
        zi[i] = in[2 * i + 1];
    }
    fft_cplx(zr, zi, false);

    /* Untangle: the packed transform holds the even-sample and odd-sample
     * spectra summed with a quarter turn between them, and they separate by
     * conjugate symmetry. */
    for (int k = 0; k <= kH; ++k) {
        const int ka = (k == kH) ? 0 : k;
        const int kb = (k == 0 || k == kH) ? 0 : (kH - k);
        const float er = 0.5f * (zr[ka] + zr[kb]);
        const float ei = 0.5f * (zi[ka] - zi[kb]);
        const float orr = 0.5f * (zi[ka] + zi[kb]);
        const float oi = -0.5f * (zr[ka] - zr[kb]);
        /* At Nyquist the twiddle is exp(-i*pi) = -1, which is off the end of
         * the table rather than in it. */
        const float wr = (k == kH) ? -1.0f : s_htw_r[k];
        const float wi = (k == kH) ? 0.0f : s_htw_i[k];
        re[k] = er + (orr * wr - oi * wi);
        im[k] = ei + (orr * wi + oi * wr);
    }
}

void fft_real_inv(const float* __restrict__ re, const float* __restrict__ im,
                  float* __restrict__ out) {
    float* const zr = s_zr;
    float* const zi = s_zi;
    for (int k = 0; k < kH; ++k) {
        float er, ei, dr, di;
        if (k == 0) {
            /* The one case worth spelling out. For k > 0 the mirror of bin
             * k+kH is conj(bin kH-k), which is inside the array. At k = 0 that
             * partner is bin kH — Nyquist — and a `(kH - k) % kH` wrap names
             * bin 0 instead, which silently loses the top of the spectrum and
             * costs a few percent of amplitude everywhere. Both bins are real
             * for real input, so the imaginary halves are zero here. */
            er = 0.5f * (re[0] + re[kH]);
            ei = 0.0f;
            dr = 0.5f * (re[0] - re[kH]);
            di = 0.0f;
        } else {
            const int kb = kH - k;
            er = 0.5f * (re[k] + re[kb]);
            ei = 0.5f * (im[k] - im[kb]);
            dr = 0.5f * (re[k] - re[kb]);
            di = 0.5f * (im[k] + im[kb]);
        }
        /* Conjugate twiddle: this undoes what fft_real() applied. */
        const float wr = s_htw_r[k];
        const float wi = -s_htw_i[k];
        const float orr = dr * wr - di * wi;
        const float oi = dr * wi + di * wr;
        zr[k] = er - oi;
        zi[k] = ei + orr;
    }
    fft_cplx(zr, zi, true);
    const float inv = 1.0f / (float)kH;
    for (int i = 0; i < kH; ++i) {
        out[2 * i] = zr[i] * inv;
        out[2 * i + 1] = zi[i] * inv;
    }
}

}  // namespace fx
}  // namespace osynth
