"""Gate vs per-band subtraction vs per-bin STFT suppression, on the real noise."""
import sys, numpy as np, wave
sys.path.insert(0, 'tools/noise_analysis')
from sim_fx import *
from stft_ns import stft_denoise, apply_gains

ANR = dict(bands=12, low=120.0, high=9000.0, amount=0.6, floor_db=-20.0,
           adapt_s=3.0, attack_ms=5.0, release_ms=150.0,
           est_tau=0.150, subwins=4, order=2)
NR = dict(hpf=80.0, hum=0, thresh=-24.0, ratio=4.0, floor_db=-24.0,
          attack_ms=3.0, hold_ms=150.0, release_ms=200.0)

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

print("30 s, speech -20 dBFS over your real noise floor, input SNR 14.5 dB\n")
print(f"  {'family':44s} {'gaps':>8s} {'under voice':>12s} {'speech':>9s}")


def report(tag, noise_out, speech_out, m):
    v = voiced[:m]; p = pause[:m]
    dmg, _ = spectral_damage(speech[:m], speech_out[:m], v)
    print(f"  {tag:44s} {rms_db(noise_out[p])-rms_db(noise[:m][p]):5.1f} dB "
          f"{rms_db(noise_out[v])-rms_db(noise[:m][v]):9.1f} dB {dmg:6.1f} dB")


# --- the gate --------------------------------------------------------------
_, db, grs = nr_run_fast(mix, trace=True, **NR)
f = svf_fast(svf_coef_k(NR['hpf'], NR_HP_K), 'Hp', noise)
fs = svf_fast(svf_coef_k(NR['hpf'], NR_HP_K), 'Hp', speech)
nb = len(grs); nu = np.empty(nb * BLK); su = np.empty(nb * BLK)
gain = 1.0; ramp = np.arange(1, BLK + 1)
for bi in range(nb):
    t = 10.0 ** (grs[bi] / 20.0); g = gain + (t - gain) / BLK * ramp
    nu[bi * BLK:(bi + 1) * BLK] = f[bi * BLK:(bi + 1) * BLK] * g
    su[bi * BLK:(bi + 1) * BLK] = fs[bi * BLK:(bi + 1) * BLK] * g
    gain = t
report("NR: downward expander (one gain, whole band)", nu, su, nb * BLK)

# --- per-band subtraction (the ANR) ----------------------------------------
_, gt = anr_run_v5(mix, trace=True, **ANR)
bands = ANR['bands']; nb = gt.shape[1]; m = nb * BLK


def replay_bank(sig):
    B = bank_diff_run(sig, bands, ANR['low'], ANR['high'], ANR['order'])
    Bb = B[:, :m].reshape(bands, nb, BLK)
    o = sig[:m].copy(); d = np.zeros(bands); ramp = np.arange(BLK)
    for bi in range(nb):
        ds = (gt[:, bi] - 1.0 - d) / BLK
        o[bi * BLK:(bi + 1) * BLK] += np.sum(
            (d[:, None] + ds[:, None] * ramp[None, :]) * Bb[:, bi, :], axis=0)
        d += ds * BLK
    return o


report(f"ANR: {bands} overlapping bands", replay_bank(noise), replay_bank(speech), m)

# --- per-bin STFT ----------------------------------------------------------
for nfft, hop in [(256, 64), (512, 128), (1024, 256)]:
    g = []
    y = stft_denoise(mix, nfft=nfft, hop=hop, gains_out=g)
    nu = apply_gains(noise, g, nfft, hop)
    su = apply_gains(speech, g, nfft, hop)
    m2 = min(len(nu), n)
    report(f"STFT Wiener + decision-directed, {nfft}/{hop} "
           f"({nfft/SR*1000:.1f} ms lat)", nu, su, m2)
    if nfft == 256:
        yy = np.clip(y, -1, 1)
        w = wave.open('tools/noise_analysis/speech_stft_256.wav', 'wb')
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000)
        w.writeframes((yy * 32767).astype('<i2').tobytes()); w.close()
        w = wave.open('tools/noise_analysis/speech_raw.wav', 'wb')
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000)
        w.writeframes((np.clip(mix, -1, 1) * 32767).astype('<i2').tobytes()); w.close()

# --- and on the bare noise file, for comparability with the earlier runs ---
print()
x1 = a[:, 0]
for nfft, hop in [(256, 64), (512, 128)]:
    y = stft_denoise(x1, nfft=nfft, hop=hop)
    print(f"  noise.wav alone, STFT {nfft}/{hop}: {rms_db(x1)-rms_db(y):.1f} dB down")
    if nfft == 256:
        w = wave.open('tools/noise_analysis/noise_stft_256.wav', 'wb')
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
        s16 = (np.clip(y, -1, 1) * 32767).astype('<i2')
        w.writeframes(np.repeat(s16[:, None], 2, axis=1).tobytes()); w.close()
