"""Transliteration of components/fx/fx_fft.cpp BACK to Python, line for line,
so the C++ can be checked against numpy without building it. If this matches,
the only thing left between here and correct firmware is the compiler.

Mirrors the C++ exactly, including its explicit ka/kb branches (the reference
in fft_ref.py uses `% h` instead; the two are shown equivalent by the test)."""
import math
import numpy as np

N = 256
H = N // 2

s_tw_r = [0.0] * (H // 2)
s_tw_i = [0.0] * (H // 2)
s_htw_r = [0.0] * H
s_htw_i = [0.0] * H


def fft_init():
    for j in range(H // 2):
        a = -2.0 * math.pi * j / H
        s_tw_r[j] = math.cos(a)
        s_tw_i[j] = math.sin(a)
    for k in range(H):
        a = -2.0 * math.pi * k / N
        s_htw_r[k] = math.cos(a)
        s_htw_i[k] = math.sin(a)


def fft_cplx(re, im, inverse):
    j = 0
    for i in range(1, H):
        bit = H >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            re[i], re[j] = re[j], re[i]
            im[i], im[j] = im[j], im[i]
    length = 2
    while length <= H:
        half = length >> 1
        step = H // length
        for i in range(0, H, length):
            k = 0
            for m in range(half):
                wr = s_tw_r[k]
                wi = -s_tw_i[k] if inverse else s_tw_i[k]
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


def fft_real(inp):
    zr = [inp[2 * i] for i in range(H)]
    zi = [inp[2 * i + 1] for i in range(H)]
    fft_cplx(zr, zi, False)
    re = [0.0] * (H + 1)
    im = [0.0] * (H + 1)
    for k in range(H + 1):
        ka = 0 if k == H else k
        kb = 0 if (k == 0 or k == H) else (H - k)
        er = 0.5 * (zr[ka] + zr[kb])
        ei = 0.5 * (zi[ka] - zi[kb])
        orr = 0.5 * (zi[ka] + zi[kb])
        oi = -0.5 * (zr[ka] - zr[kb])
        wr = -1.0 if k == H else s_htw_r[k]
        wi = 0.0 if k == H else s_htw_i[k]
        re[k] = er + (orr * wr - oi * wi)
        im[k] = ei + (orr * wi + oi * wr)
    return re, im


def fft_real_inv(re, im):
    zr = [0.0] * H
    zi = [0.0] * H
    for k in range(H):
        if k == 0:
            er = 0.5 * (re[0] + re[H])
            ei = 0.0
            dr = 0.5 * (re[0] - re[H])
            di = 0.0
        else:
            kb = H - k
            er = 0.5 * (re[k] + re[kb])
            ei = 0.5 * (im[k] - im[kb])
            dr = 0.5 * (re[k] - re[kb])
            di = 0.5 * (im[k] + im[kb])
        wr = s_htw_r[k]
        wi = -s_htw_i[k]
        orr = dr * wr - di * wi
        oi = dr * wi + di * wr
        zr[k] = er - oi
        zi[k] = ei + orr
    fft_cplx(zr, zi, True)
    out = [0.0] * N
    inv = 1.0 / H
    for i in range(H):
        out[2 * i] = zr[i] * inv
        out[2 * i + 1] = zi[i] * inv
    return out


if __name__ == '__main__':
    fft_init()
    rng = np.random.default_rng(11)
    ok = True
    for trial in range(4):
        x = list(rng.standard_normal(N))
        R, I = fft_real(x)
        ref = np.fft.rfft(np.array(x))
        e = np.max(np.abs((np.array(R) + 1j * np.array(I)) - ref))
        y = fft_real_inv(R, I)
        e2 = np.max(np.abs(np.array(y) - np.array(x)))
        ok &= (e < 1e-9 and e2 < 1e-12)
        print(f"  trial {trial}: fwd {e:.3e}  round-trip {e2:.3e}")
    # a pure tone must land in exactly one bin
    k0 = 17
    x = [math.cos(2 * math.pi * k0 * i / N) for i in range(N)]
    R, I = fft_real(x)
    mag = np.hypot(np.array(R), np.array(I))
    peak = int(np.argmax(mag))
    leak = 20 * math.log10(max(np.max(np.delete(mag, peak)), 1e-15) / mag[peak])
    print(f"  tone at bin {k0}: peak bin {peak}, next-highest {leak:.1f} dB down")
    ok &= (peak == k0 and leak < -100)
    # imaginary parts at DC and Nyquist must be zero for real input
    print(f"  im[0] {I[0]:.2e}   im[Nyquist] {I[H]:.2e}")
    ok &= abs(I[0]) < 1e-12 and abs(I[H]) < 1e-12
    print("  fx_fft.cpp mirror:", "OK" if ok else "FAILED")
