"""Reference for the radix-2 real FFT that goes into components/fx/fx_fft.cpp.

Written as a transliteration target: no numpy vectorisation, table-driven
twiddles (not a recurrence, which drifts in float32), so the C++ can be checked
line for line against it. Verified against numpy.fft below.
"""
import math
import numpy as np


def make_tw(n):
    """w[j] = exp(-2*pi*i*j/n), j = 0..n/2-1. One table serves every stage:
    a stage with half-length h steps the table by n/(2h)."""
    wr = [0.0] * (n // 2)
    wi = [0.0] * (n // 2)
    for j in range(n // 2):
        a = -2.0 * math.pi * j / n
        wr[j] = math.cos(a)
        wi[j] = math.sin(a)
    return wr, wi


def fft_cplx(re, im, n, tw_r, tw_i, inverse):
    """In-place iterative radix-2 DIT. `inverse` conjugates the twiddles only;
    the 1/n scaling is the caller's business."""
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            re[i], re[j] = re[j], re[i]
            im[i], im[j] = im[j], im[i]
    length = 2
    while length <= n:
        half = length >> 1
        step = n // length          # table stride for this stage
        for i in range(0, n, length):
            k = 0
            for m in range(half):
                wr = tw_r[k]
                wi = tw_i[k] if not inverse else -tw_i[k]
                k += step
                a = i + m
                b = a + half
                vr = re[b] * wr - im[b] * wi
                vi = re[b] * wi + im[b] * wr
                re[b] = re[a] - vr
                im[b] = im[a] - vi
                re[a] = re[a] + vr
                im[a] = im[a] + vi
        length <<= 1


def rfft(x, n, tw_r, tw_i, htw_r, htw_i):
    """N real samples -> N/2+1 bins, via one N/2-point complex FFT.
    tw_* is the N/2-point table; htw_* is exp(-2*pi*i*k/N) for k=0..N/2-1."""
    h = n // 2
    re = [x[2 * i] for i in range(h)]
    im = [x[2 * i + 1] for i in range(h)]
    fft_cplx(re, im, h, tw_r, tw_i, False)
    out_r = [0.0] * (h + 1)
    out_i = [0.0] * (h + 1)
    for k in range(h + 1):
        ka = k % h
        kb = (h - k) % h
        # even/odd split of the packed transform
        er = 0.5 * (re[ka] + re[kb])
        ei = 0.5 * (im[ka] - im[kb])
        orr = 0.5 * (im[ka] + im[kb])
        oi = -0.5 * (re[ka] - re[kb])
        wr = htw_r[k] if k < h else -1.0
        wi = htw_i[k] if k < h else 0.0
        out_r[k] = er + (orr * wr - oi * wi)
        out_i[k] = ei + (orr * wi + oi * wr)
    return out_r, out_i


def irfft(sr, si, n, tw_r, tw_i, htw_r, htw_i):
    """N/2+1 bins -> N real samples. Undoes rfft(), including the 1/n."""
    h = n // 2
    re = [0.0] * h
    im = [0.0] * h
    for k in range(h):
        if k == 0:
            # X[0] pairs with the NYQUIST bin, not with itself: the identity
            # is X[k+h] = conj(X[h-k]) for k>0, but at k=0 that partner is
            # X[h], which the `% h` wrap below cannot name. Both are real.
            er = 0.5 * (sr[0] + sr[h])
            ei = 0.0
            dr = 0.5 * (sr[0] - sr[h])
            di = 0.0
        else:
            kb = h - k
            er = 0.5 * (sr[k] + sr[kb])
            ei = 0.5 * (si[k] - si[kb])
            dr = 0.5 * (sr[k] - sr[kb])
            di = 0.5 * (si[k] + si[kb])
        # undo the twiddle on the odd half: conj(w) because this is the inverse
        wr = htw_r[k]
        wi = -htw_i[k]
        orr = dr * wr - di * wi
        oi = dr * wi + di * wr
        re[k] = er - oi
        im[k] = ei + orr
    fft_cplx(re, im, h, tw_r, tw_i, True)
    out = [0.0] * n
    inv = 1.0 / h
    for i in range(h):
        out[2 * i] = re[i] * inv
        out[2 * i + 1] = im[i] * inv
    return out


def make_half_tw(n):
    hr = [0.0] * (n // 2)
    hi = [0.0] * (n // 2)
    for k in range(n // 2):
        a = -2.0 * math.pi * k / n
        hr[k] = math.cos(a)
        hi[k] = math.sin(a)
    return hr, hi


if __name__ == '__main__':
    rng = np.random.default_rng(3)
    ok = True
    for N in [64, 128, 256, 512]:
        tw_r, tw_i = make_tw(N // 2)
        htw_r, htw_i = make_half_tw(N)
        x = list(rng.standard_normal(N))
        R, I = rfft(x, N, tw_r, tw_i, htw_r, htw_i)
        ref = np.fft.rfft(np.array(x))
        e = np.max(np.abs((np.array(R) + 1j * np.array(I)) - ref))
        y = irfft(R, I, N, tw_r, tw_i, htw_r, htw_i)
        e2 = np.max(np.abs(np.array(y) - np.array(x)))
        print(f"  N={N:4d}  fwd err {e:.3e}   round-trip err {e2:.3e}")
        ok &= (e < 1e-9 and e2 < 1e-12)
    print("  fft reference:", "OK" if ok else "FAILED")
