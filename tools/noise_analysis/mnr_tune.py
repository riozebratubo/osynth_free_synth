"""Settle the mnr defaults before they are cast into fx.cpp."""
import sys, numpy as np, wave
sys.path.insert(0, 'tools/noise_analysis')
from sim_fx import rms_db, svf_fast, svf_coef_k, BLK
import mnr_sim
from mnr_sim import mnr_run, replay, SR

a = np.load('tools/noise_analysis/data.npy')
noise = np.tile(a[:, 0], 3)
n = len(noise) // BLK * BLK
noise = noise[:n]
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
for k in range(20):
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
snr_in = rms_db(speech[voiced]) - rms_db(noise[voiced])
print(f"input SNR {snr_in:.1f} dB, noise {rms_db(noise):.1f} dBFS\n")
print(f"  {'setting':34s} {'gaps':>8s} {'under voice':>12s} {'SNR+':>6s} {'voice':>7s}")


def ev(tag, **kw):
    g = []
    mnr_run(mix, gains_out=g, **kw)
    nu = replay(noise, g); su = replay(speech, g)
    m = min(len(nu), n); v = voiced[:m]; p = pause[:m]
    print(f"  {tag:34s} {rms_db(nu[p])-rms_db(noise[:m][p]):5.1f} dB "
          f"{rms_db(nu[v])-rms_db(noise[:m][v]):9.1f} dB "
          f"{rms_db(su[v])-rms_db(nu[v])-snr_in:+5.1f} "
          f"{rms_db(su[v])-rms_db(speech[:m][v]):+6.1f}")


for amt in [0.3, 0.6, 0.8, 1.0]:
    ev(f"amount={amt}", amount=amt)
print()
for fl in [-12.0, -18.0, -24.0, -36.0]:
    ev(f"floor={fl}", floor_db=fl)
print()
for ad in [0.8, 1.5, 3.0, 6.0]:
    ev(f"adapt={ad}s", adapt_s=ad)
print()
for b in [2, 4, 8]:
    mnr_sim.BUCKETS = b
    ev(f"buckets={b} (RAM: {b*mnr_sim.BINS*4} B)")
mnr_sim.BUCKETS = 4
print()
for tau in [0.030, 0.060, 0.120]:
    mnr_sim.DD_TAU = tau
    ev(f"dd_tau={tau*1000:.0f}ms")
mnr_sim.DD_TAU = 0.060
print()
ev("learn held", learn=True)

# chosen defaults -> audio
g = []
y = mnr_run(mix, gains_out=g)
for path, sig in [('mnr_demo.wav', y), ('mnr_noise.wav', mnr_run(a[:, 0]))]:
    s16 = (np.clip(sig, -1, 1) * 32767).astype('<i2')
    w = wave.open('tools/noise_analysis/' + path, 'wb')
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000)
    w.writeframes(s16.tobytes()); w.close()
x1 = a[:, 0]
print(f"\n  noise.wav alone at defaults: {rms_db(x1)-rms_db(mnr_run(x1)):.1f} dB down")
print("  wrote mnr_demo.wav, mnr_noise.wav")
