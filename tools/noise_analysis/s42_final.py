"""Final validation of the shipped fx.mnr, driven through the C++ mirror.

The mirror in mnr_cpp_mirror.py is a line-for-line transliteration of
mnr_process()/mnr_frame() as they now stand in fx.cpp, so these are the numbers
the firmware should produce.
"""
import sys, numpy as np, wave
sys.path.insert(0, 'tools/noise_analysis')
from sim_fx import rms_db, svf_fast, svf_coef_k, anr_run_v5, nr_run_fast, BLK, SR
from mnr_cpp_mirror import Mnr, mnr_process, replay_gains, N as FFT_N

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

# fx.mnr is a delay line, so the reference has to move with it for the
# voiced/pause masks to still line up.
D = FFT_N - 1
vd = np.roll(voiced, D); vd[:D] = False
pd = np.roll(pause, D); pd[:D] = False

print(f"30 s of speech over the real osynth noise floor, input SNR {snr_in:.1f} dB")
print(f"(all three units at their shipped defaults, src = input)\n")
print(f"  {'unit':34s} {'gaps':>8s} {'under voice':>12s} {'SNR gain':>9s} {'voice':>7s}")

# --- fx.nr -----------------------------------------------------------------
_, db, grs = nr_run_fast(mix, trace=True, **NR)
fn_ = svf_fast(svf_coef_k(NR['hpf'], 1.41421356), 'Hp', noise)
fs_ = svf_fast(svf_coef_k(NR['hpf'], 1.41421356), 'Hp', speech)
nb = len(grs); nu = np.empty(nb * BLK); su = np.empty(nb * BLK)
gain = 1.0; ramp = np.arange(1, BLK + 1)
for bi in range(nb):
    t = 10.0 ** (grs[bi] / 20.0); g = gain + (t - gain) / BLK * ramp
    nu[bi * BLK:(bi + 1) * BLK] = fn_[bi * BLK:(bi + 1) * BLK] * g
    su[bi * BLK:(bi + 1) * BLK] = fs_[bi * BLK:(bi + 1) * BLK] * g
    gain = t


def report(tag, nu, su, m, v, p, ref_n, ref_s):
    print(f"  {tag:34s} {rms_db(nu[p])-rms_db(ref_n[p]):5.1f} dB "
          f"{rms_db(nu[v])-rms_db(ref_n[v]):9.1f} dB "
          f"{rms_db(su[v])-rms_db(nu[v])-snr_in:+6.1f} dB "
          f"{rms_db(su[v])-rms_db(ref_s[v]):+6.1f}")


m0 = nb * BLK
report("fx.nr   expander (S39)", nu, su, m0, voiced[:m0], pause[:m0], noise, speech)

# --- fx.anr ----------------------------------------------------------------
_, gt = anr_run_v5(mix, trace=True, **ANR)
bands = ANR['bands']; nbf = gt.shape[1]; m1 = nbf * BLK
from sim_fx import bank_diff_run


def replay_bank(sig):
    B = bank_diff_run(sig, bands, ANR['low'], ANR['high'], ANR['order'])
    Bb = B[:, :m1].reshape(bands, nbf, BLK)
    o = sig[:m1].copy(); d = np.zeros(bands); rp = np.arange(BLK)
    for bi in range(nbf):
        ds = (gt[:, bi] - 1.0 - d) / BLK
        o[bi * BLK:(bi + 1) * BLK] += np.sum(
            (d[:, None] + ds[:, None] * rp[None, :]) * Bb[:, bi, :], axis=0)
        d += ds * BLK
    return o


report("fx.anr  12 bands (S39c)", replay_bank(noise), replay_bank(speech),
       m1, voiced[:m1], pause[:m1], noise, speech)

# --- fx.mnr: the shipped code path ----------------------------------------
# The gains must come from the MIX and then be replayed on each component:
# running the unit on noise alone lets it learn and kill that noise with no
# voice present to open the bins, which measures nothing about real use.
c = Mnr(); c.capture = True
y = mnr_process(c, mix)
ynz = replay_gains(noise, c.gains)
ysp = replay_gains(speech, c.gains)
m2 = len(y)
report("fx.mnr  129 bins (S42)", ynz, ysp, m2, vd[:m2], pd[:m2], noise, speech)

print()
x1 = a[:, 0]
c = Mnr()
print(f"  noise.wav alone through fx.mnr: {rms_db(x1):.1f} -> "
      f"{rms_db(mnr_process(c, x1)):.1f} dBFS")

# --- listening ------------------------------------------------------------
def w16(path, sig):
    s16 = (np.clip(sig, -1, 1) * 32767).astype('<i2')
    w = wave.open('tools/noise_analysis/' + path, 'wb')
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000)
    w.writeframes(s16.tobytes()); w.close()


w16('s42_raw.wav', mix)
w16('s42_mnr.wav', y)
c = Mnr()
w16('s42_mnr_learn.wav', mnr_process(c, mix, learn=False, adapt_s=1.5))
print("  wrote s42_raw.wav / s42_mnr.wav")
