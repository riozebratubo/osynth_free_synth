"""Quality per FFT: which (nfft, hop) earns its CPU on an embedded target.
`cost` is FFTs per second, which is what the audio task actually pays."""
import sys, numpy as np
sys.path.insert(0, 'tools/noise_analysis')
from sim_fx import *
from stft_ns import stft_denoise, apply_gains

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
print(f"input SNR {snr_in:.1f} dB\n")
print(f"  {'nfft/hop':12s} {'ovl':>4s} {'lat':>7s} {'FFT/s':>7s} {'blocks':>7s} "
      f"{'gaps':>7s} {'under':>7s} {'SNR+':>6s} {'voice':>6s}")
for nfft in [128, 256, 512]:
    for div in [2, 4]:
        hop = nfft // div
        if hop < BLK or hop % BLK:
            continue
        g = []
        stft_denoise(mix, nfft=nfft, hop=hop, gains_out=g)
        nu = apply_gains(noise, g, nfft, hop)
        su = apply_gains(speech, g, nfft, hop)
        m = min(len(nu), n)
        v = voiced[:m]; p = pause[:m]
        gaps = rms_db(nu[p]) - rms_db(noise[:m][p])
        und = rms_db(nu[v]) - rms_db(noise[:m][v])
        snro = rms_db(su[v]) - rms_db(nu[v])
        keep = rms_db(su[v]) - rms_db(speech[:m][v])
        print(f"  {nfft:4d}/{hop:<7d} {100-100//div:3d}% {nfft/SR*1000:5.1f}ms "
              f"{SR/hop:7.0f} {hop//BLK:6d}  {gaps:5.1f} dB {und:5.1f} dB "
              f"{snro-snr_in:+5.1f} {keep:+5.1f}")
print("\n  'blocks' = audio blocks per FFT; 1 means one transform every render call.")
