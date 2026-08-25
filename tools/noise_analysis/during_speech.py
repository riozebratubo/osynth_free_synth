"""How much noise survives *underneath* the voice?

Both units apply a gain trajectory to their input, so the noise component of the
output can be recovered exactly: compute the gains from the mix, then apply those
same gains to the noise alone. That separates "noise removed in the gaps" (which
a gate does well) from "noise removed under the voice" (which is the thing the
user is actually complaining about)."""
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


def nr_noise_under_voice(mix, noise):
    """Re-run nr with the gain trajectory it computes from the mix, but applied
    to the noise alone."""
    _, db, grs = nr_run_fast(mix, trace=True, **NR)
    f = svf_fast(svf_coef_k(NR['hpf'], NR_HP_K), 'Hp', noise)
    nb = len(grs); out = np.empty(nb * BLK); gain = 1.0
    ramp = np.arange(1, BLK + 1)
    for bi in range(nb):
        t = 10.0 ** (grs[bi] / 20.0)
        out[bi * BLK:(bi + 1) * BLK] = f[bi * BLK:(bi + 1) * BLK] * (gain + (t - gain) / BLK * ramp)
        gain = t
    return out


def anr_noise_under_voice(mix, noise):
    _, gt = anr_run_v5(mix, trace=True, **ANR)
    bands = ANR['bands']
    B = bank_diff_run(noise, bands, ANR['low'], ANR['high'], ANR['order'])
    nb = gt.shape[1]
    out = noise[:nb * BLK].copy()
    Bb = B[:, :nb * BLK].reshape(bands, nb, BLK)
    d = np.zeros(bands); ramp = np.arange(BLK)
    for bi in range(nb):
        dstep = (gt[:, bi] - 1.0 - d) / BLK
        out[bi * BLK:(bi + 1) * BLK] += np.sum(
            (d[:, None] + dstep[:, None] * ramp[None, :]) * Bb[:, bi, :], axis=0)
        d += dstep * BLK
    return out


print("noise level under the voice vs in the gaps (0 dB = untouched)\n")
print(f"  {'unit':30s} {'in the gaps':>13s} {'under the voice':>17s}")
for tag, fn in [("NR (downward expander)", nr_noise_under_voice),
                ("ANR (per-band subtraction)", anr_noise_under_voice)]:
    nu = fn(mix, noise)
    m = min(len(nu), n)
    v = voiced[:m]; p = pause[:m]
    print(f"  {tag:30s} {rms_db(nu[p])-rms_db(noise[:m][p]):10.1f} dB "
          f"{rms_db(nu[v])-rms_db(noise[:m][v]):14.1f} dB")
print("\n  A gate makes ONE decision for the whole spectrum, so the moment it")
print("  opens for the voice it opens for the noise too. That is not a bug in")
print("  the implementation - it is what a downward expander is.")
