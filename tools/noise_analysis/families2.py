"""Gate vs per-band subtraction vs per-bin STFT, scored the standard way.

Every one of these units applies a gain to its input, so the output splits
exactly into a speech component and a noise component: compute the gains from
the mix, then replay them on speech and on noise separately. That gives the two
numbers that actually matter - output SNR, and how much of the voice survived -
instead of a spectral-error figure that punishes a denoiser for correctly
suppressing a bin where the voice was already buried.
"""
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
snr_in = rms_db(speech[voiced]) - rms_db(noise[voiced])

print(f"30 s, speech -20 dBFS over your real noise, input SNR {snr_in:.1f} dB\n")
print(f"  {'family':41s} {'gaps':>7s} {'under voice':>12s} {'out SNR':>8s} {'voice':>7s}")


def report(tag, nu, su, m):
    v = voiced[:m]; p = pause[:m]
    gaps = rms_db(nu[p]) - rms_db(noise[:m][p])
    under = rms_db(nu[v]) - rms_db(noise[:m][v])
    snr_out = rms_db(su[v]) - rms_db(nu[v])
    keep = rms_db(su[v]) - rms_db(speech[:m][v])
    print(f"  {tag:41s} {gaps:4.1f} dB {under:9.1f} dB {snr_out:5.1f} dB {keep:+5.1f} dB")


# --- the gate --------------------------------------------------------------
_, db, grs = nr_run_fast(mix, trace=True, **NR)
fn_ = svf_fast(svf_coef_k(NR['hpf'], NR_HP_K), 'Hp', noise)
fs_ = svf_fast(svf_coef_k(NR['hpf'], NR_HP_K), 'Hp', speech)
nb = len(grs); nu = np.empty(nb * BLK); su = np.empty(nb * BLK)
gain = 1.0; ramp = np.arange(1, BLK + 1)
for bi in range(nb):
    t = 10.0 ** (grs[bi] / 20.0); g = gain + (t - gain) / BLK * ramp
    nu[bi * BLK:(bi + 1) * BLK] = fn_[bi * BLK:(bi + 1) * BLK] * g
    su[bi * BLK:(bi + 1) * BLK] = fs_[bi * BLK:(bi + 1) * BLK] * g
    gain = t
report("NR: expander, one gain for all frequencies", nu, su, nb * BLK)

# --- per-band --------------------------------------------------------------
_, gt = anr_run_v5(mix, trace=True, **ANR)
bands = ANR['bands']; nb = gt.shape[1]; m = nb * BLK


def replay_bank(sig):
    B = bank_diff_run(sig, bands, ANR['low'], ANR['high'], ANR['order'])
    Bb = B[:, :m].reshape(bands, nb, BLK)
    o = sig[:m].copy(); d = np.zeros(bands); rp = np.arange(BLK)
    for bi in range(nb):
        ds = (gt[:, bi] - 1.0 - d) / BLK
        o[bi * BLK:(bi + 1) * BLK] += np.sum(
            (d[:, None] + ds[:, None] * rp[None, :]) * Bb[:, bi, :], axis=0)
        d += ds * BLK
    return o


report(f"ANR: {bands} overlapping bands (as fixed)", replay_bank(noise),
       replay_bank(speech), m)

# --- per-bin ---------------------------------------------------------------
best = None
for nfft, hop in [(256, 64), (512, 128), (1024, 256)]:
    g = []
    y = stft_denoise(mix, nfft=nfft, hop=hop, gains_out=g)
    nu = apply_gains(noise, g, nfft, hop)
    su = apply_gains(speech, g, nfft, hop)
    m2 = min(len(nu), n)
    report(f"STFT per-bin Wiener+DD {nfft}/{hop} ({nfft/SR*1000:4.1f} ms)",
           nu, su, m2)
    if nfft == 512:
        best = y
print()
print(f"  {'':41s} {'(gaps/under voice are noise levels vs untouched;':>7s}")
print(f"  {'':41s}  voice = how much of the speech survived)")

# --- listening files -------------------------------------------------------
def w16(path, y, ch=1):
    s16 = (np.clip(y, -1, 1) * 32767).astype('<i2')
    if ch == 2:
        s16 = np.repeat(s16[:, None], 2, axis=1)
    w = wave.open(path, 'wb'); w.setnchannels(ch); w.setsampwidth(2)
    w.setframerate(48000); w.writeframes(s16.tobytes()); w.close()


w16('tools/noise_analysis/demo_raw.wav', mix)
w16('tools/noise_analysis/demo_expander.wav', nr_run_fast(mix, **NR))
w16('tools/noise_analysis/demo_anr.wav', anr_run_v5(mix, **ANR))
w16('tools/noise_analysis/demo_stft.wav', best)
print("\n  wrote demo_raw / demo_expander / demo_anr / demo_stft .wav")

x1 = a[:, 0]
for nfft, hop in [(256, 64), (512, 128)]:
    y = stft_denoise(x1, nfft=nfft, hop=hop)
    print(f"  noise.wav alone, STFT {nfft}/{hop}: {rms_db(x1)-rms_db(y):5.1f} dB down")
    if nfft == 512:
        w16('tools/noise_analysis/noise_stft.wav', y, ch=2)
