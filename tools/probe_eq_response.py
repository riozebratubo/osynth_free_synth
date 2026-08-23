#!/usr/bin/env python3
"""Probe: magnitude response of fx.cpp's EQ biquads, transcribed literally.

bq_shelf() / bq_peak() / bq_next() from components/fx/fx.cpp are reproduced
here coefficient for coefficient so the response can be inspected without a
build. Prints |H(f)| in dB at a set of probe frequencies for each band at a
few gain settings, plus a pole-radius stability check.

Run:  python tools/probe_eq_response.py
"""
import cmath
import math

SR = 48000.0
TWO_PI = 2.0 * math.pi


def bq_shelf(f0, gain_db, shelf):
    A = 10.0 ** (gain_db / 40.0)
    w0 = TWO_PI * min(f0, SR * 0.45) / SR
    cw = math.cos(w0)
    alpha = math.sin(w0) * 0.5 * 1.41421356
    tsa = 2.0 * math.sqrt(A) * alpha
    ap, am = A + 1.0, A - 1.0
    if shelf < 0:
        b0 = A * (ap - am * cw + tsa)
        b1 = 2.0 * A * (am - ap * cw)
        b2 = A * (ap - am * cw - tsa)
        a0 = ap + am * cw + tsa
        a1 = -2.0 * (am + ap * cw)
        a2 = ap + am * cw - tsa
    else:
        b0 = A * (ap + am * cw + tsa)
        b1 = -2.0 * A * (am + ap * cw)
        b2 = A * (ap + am * cw - tsa)
        a0 = ap - am * cw + tsa
        a1 = 2.0 * (am - ap * cw)
        a2 = ap - am * cw - tsa
    ia = 1.0 / a0
    return (b0 * ia, b1 * ia, b2 * ia, a1 * ia, a2 * ia)


def bq_peak(f0, gain_db, q):
    A = 10.0 ** (gain_db / 40.0)
    w0 = TWO_PI * min(f0, SR * 0.45) / SR
    cw = math.cos(w0)
    alpha = math.sin(w0) / (2.0 * max(q, 0.05))
    a0 = 1.0 + alpha / A
    ia = 1.0 / a0
    return ((1.0 + alpha * A) * ia, (-2.0 * cw) * ia, (1.0 - alpha * A) * ia,
            (-2.0 * cw) * ia, (1.0 - alpha / A) * ia)


def mag_db(c, f):
    b0, b1, b2, a1, a2 = c
    z = cmath.exp(-1j * TWO_PI * f / SR)
    h = (b0 + b1 * z + b2 * z * z) / (1.0 + a1 * z + a2 * z * z)
    return 20.0 * math.log10(abs(h) + 1e-30)


def poles(c):
    _, _, _, a1, a2 = c
    d = complex(a1 * a1 - 4.0 * a2) ** 0.5
    return max(abs((-a1 + d) / 2.0), abs((-a1 - d) / 2.0))


PROBE = [30, 60, 120, 250, 500, 1000, 2000, 4000, 6000, 10000, 16000, 20000]


def show(label, c):
    print(f"  {label:28s} pole|r|={poles(c):.6f}  " +
          " ".join(f"{f/1000:g}k:{mag_db(c, f):+6.2f}" if f >= 1000
                   else f"{f}:{mag_db(c, f):+6.2f}" for f in PROBE))


print(f"sample rate {SR:.0f}")
print("low shelf (fx.eq.low / fx.eq.lofreq, default f0=120)")
for g in (-18, -6, 6, 18):
    show(f"lofreq=120 gain={g:+d}dB", bq_shelf(120.0, g, -1))
show("lofreq= 40 gain=+18dB", bq_shelf(40.0, 18.0, -1))
show("lofreq=500 gain=+18dB", bq_shelf(500.0, 18.0, -1))

print("mid bell (fx.eq.mid / midfreq / midq, defaults f0=1000 q=1)")
for g in (-18, -6, 6, 18):
    show(f"midfreq=1000 q=1 gain={g:+d}dB", bq_peak(1000.0, g, 1.0))
show("midfreq= 200 q=1   gain=+12dB", bq_peak(200.0, 12.0, 1.0))
show("midfreq=6000 q=1   gain=+12dB", bq_peak(6000.0, 12.0, 1.0))
show("midfreq=1000 q=0.3 gain=+12dB", bq_peak(1000.0, 12.0, 0.3))
show("midfreq=1000 q=6   gain=+12dB", bq_peak(1000.0, 12.0, 6.0))

print("high shelf (fx.eq.high / fx.eq.hifreq, default f0=6000)")
for g in (-18, -6, 6, 18):
    show(f"hifreq=6000 gain={g:+d}dB", bq_shelf(6000.0, g, 1))
show("hifreq= 1500 gain=+18dB", bq_shelf(1500.0, 18.0, 1))
show("hifreq=16000 gain=+18dB", bq_shelf(16000.0, 18.0, 1))
show("hifreq=16000 gain=-18dB", bq_shelf(16000.0, -18.0, 1))

print()
print("---- candidate re-voicing (S40) ----")
print("low shelf: what f0 puts the boost where a speaker can show it")
for f0 in (120, 200, 250, 320, 400):
    show(f"lofreq={f0:4d} gain=+12dB", bq_shelf(float(f0), 12.0, -1))
print("high shelf: what f0 keeps the lift out of the hiss octave")
for f0 in (2000, 3000, 4000, 6000, 8000):
    show(f"hifreq={f0:5d} gain=+12dB", bq_shelf(float(f0), 12.0, 1))

print()
print("---- why a LOW hifreq is the noisy end (S40 range 1200..12000) ----")
for g in (6, 12):
    for f0 in (1200, 2000, 3000, 6000, 9000, 12000):
        show(f"hifreq={f0:5d} high={g:+d}dB", bq_shelf(float(f0), float(g), 1))
    print()
print("bandwidth lifted by >3 dB, as a fraction of the audible band:")
for f0 in (1200, 3000, 6000, 12000):
    c = bq_shelf(float(f0), 12.0, 1)
    lo = next((f for f in range(100, 20000, 25) if mag_db(c, f) > 3.0), None)
    print(f"  hifreq={f0:5d}  +3 dB from {lo} Hz up  "
          f"({math.log2(20000/lo):.1f} octaves of the spectrum lifted)")
