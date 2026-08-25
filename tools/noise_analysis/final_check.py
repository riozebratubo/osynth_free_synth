"""Final validation at the exact defaults now in fx.cpp: how much noise goes,
how much speech survives, and whether the held-pad guard still holds."""
import sys, numpy as np
sys.path.insert(0, 'tools/noise_analysis')
from sim_fx import *

ANR = dict(bands=12, low=120.0, high=9000.0, amount=0.6, floor_db=-20.0,
           adapt_s=3.0, attack_ms=5.0, release_ms=150.0,
           est_tau=0.150, subwins=4, order=2)
NR = dict(hpf=80.0, hum=0, thresh=-24.0, ratio=4.0, floor_db=-24.0,
          attack_ms=3.0, hold_ms=150.0, release_ms=200.0)

a = np.load('tools/noise_analysis/data.npy')
noise = np.tile(a[:, 0], 3)
n = len(noise) // BLK * BLK
noise = noise[:n]
t = np.arange(n) / SR

# a voiced, formant-shaped signal with realistic phrase lengths
f0 = 110.0
ph = np.cumsum(np.full(n, f0 / SR))
src = np.zeros(n)
for h in range(1, 64):
    if h * f0 > 7000:
        break
    src += np.sin(2 * np.pi * h * ph) / h
for fc, q, g in [(700, 8, 1.0), (1200, 10, 0.6), (2600, 12, 0.35)]:
    src = src + g * svf_fast(svf_coef_k(fc, 1.0 / q), 'BpN', src)
env = np.zeros(n)
for k in range(20):                      # 2.0 s phrase, 1.0 s gap
    s0 = int(k * 3.0 * SR); L = int(2.0 * SR)
    if s0 >= n:
        break
    L = min(L, n - s0)
    env[s0:s0 + L] = np.hanning(max(L, 2))[:L] ** 0.25
speech = src * env
speech *= 10 ** (-20 / 20) / np.sqrt(np.mean(speech[env > 0.7] ** 2))
mix = speech + noise
voiced = env > 0.7
pause = env < 1e-9

print("30 s of speech (-20 dBFS, 2 s phrases) over the real noise floor")
print(f"  input SNR {rms_db(speech[voiced])-rms_db(noise):.1f} dB\n")
print(f"  {'chain':34s} {'noise in gaps':>14s} {'speech kept':>12s} {'SNR gain':>9s}")


def ev(tag, fn):
    y = fn(mix)[:n]
    ys = fn(speech)[:n]
    cut = rms_db(noise[pause]) - rms_db(y[pause])
    dmg, bias = spectral_damage(speech, ys, voiced)
    print(f"  {tag:34s} {-cut:11.1f} dB {dmg:9.1f} dB* {cut+bias:7.1f} dB")


ev("nothing", lambda s: s)
ev("ANR only (new defaults)", lambda s: anr_run_v5(s, **ANR))
ev("NR only (new defaults)", lambda s: nr_run_fast(s, **NR))
ev("ANR -> NR (both on)", lambda s: nr_run_fast(anr_run_v5(s, **ANR), **NR))
print("  * mean absolute spectral error on the voiced parts; under ~1 dB is")
print("    below what anyone hears as a change in the voice.\n")

# the guard: a held note must not be learned as noise
n2 = int(60.0 * SR) // BLK * BLK
t2 = np.arange(n2) / SR
pad = np.zeros(n2)
for f in [110., 138.6, 164.8, 220., 277.2, 329.6]:
    for h in [1, 2, 3]:
        pad += np.sin(2 * np.pi * f * h * t2 + f) / (h * h)
pad *= 10 ** (-18 / 20) / np.sqrt(np.mean(pad ** 2))
pad[:int(0.05 * SR)] *= np.linspace(0, 1, int(0.05 * SR))
seg = lambda s, e: slice(int(s * SR), int(e * SR))
y = anr_run_v5(pad, **ANR)
print("held pad, ANR at the new defaults (the 'a pad looks like a fan' guard):")
print("  " + "  ".join(f"{m}s {rms_db(y[seg(m-1,m)])-rms_db(pad[seg(m-1,m)]):+5.1f}dB"
                       for m in [5, 10, 20, 30, 45, 59]))
print("  (documented promise: 40 dB up survives 'the better part of a minute')")
