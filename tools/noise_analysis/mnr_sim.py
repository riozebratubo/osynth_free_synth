"""Mirror of the planned mnr_process() in fx.cpp, at block granularity.

Same frame size, hop, bucket count, parameter meanings and update order as the
C++ will use, so the numbers below are the numbers the firmware should produce.
"""
import numpy as np

SR = 48000.0
N = 256
HOP = 128
BINS = N // 2 + 1
BUCKETS = 4
OVERSUB = 3.0          # kMnrOversub: what `amount` = 1 subtracts
BIAS = 1.6             # kMnrBias: minimum -> mean, per bin
DD_TAU = 0.060         # kMnrDdTauS: decision-directed memory
SM_TAU = 0.120         # kMnrSmTauS: periodogram smoothing before the minimum
LEARN_S = 0.20         # kMnrLearnS: window while `learn` is held


def mnr_run(x, amount=0.6, floor_db=-24.0, adapt_s=1.5, learn=False,
            gains_out=None):
    x = np.asarray(x, dtype=np.float64)
    win = np.sqrt(np.hanning(N + 1)[:N])      # sqrt-Hann: WOLA at 50% overlap
    fr_s = HOP / SR
    a_sm = np.exp(-fr_s / SM_TAU)
    a_dd = np.exp(-fr_s / DD_TAU)
    win_s = LEARN_S if learn else adapt_s
    fr_per_bucket = max(int(win_s / fr_s / BUCKETS), 1)
    gmin = 10.0 ** (floor_db / 20.0)
    over = max(amount * OVERSUB, 1e-3)
    sm = np.zeros(BINS); lam = np.zeros(BINS)
    buck = np.full((BUCKETS, BINS), np.inf)
    g_prev = np.ones(BINS); gam_prev = np.ones(BINS)
    cnt = 0; primed = False
    nfr = 1 + (len(x) - N) // HOP
    out = np.zeros(len(x))
    if gains_out is not None:
        gains_out.clear()
    for i in range(nfr):
        s = i * HOP
        fr = x[s:s + N] * win
        sp = np.fft.rfft(fr)
        P = np.abs(sp) ** 2
        if not primed:
            sm = P.copy(); lam = P * BIAS; buck[:] = P; primed = True
        else:
            sm = a_sm * sm + (1.0 - a_sm) * P
        if learn:
            # Held: the profile is whatever is here now, no minimum needed.
            lam = sm * BIAS
            buck[:] = sm
        else:
            buck[0] = np.minimum(buck[0], sm)
            cnt += 1
            if cnt >= fr_per_bucket:
                cnt = 0
                lam = np.minimum.reduce(buck) * BIAS
                buck[1:] = buck[:-1]
                buck[0] = np.inf
        lam_e = np.maximum(lam * over, 1e-20)
        gam = np.minimum(P / lam_e, 1e6)
        xi = a_dd * (g_prev ** 2) * gam_prev + \
            (1.0 - a_dd) * np.maximum(gam - 1.0, 0.0)
        xi = np.maximum(xi, 1e-10)
        G = np.maximum(xi / (1.0 + xi), gmin)
        g_prev = G; gam_prev = gam
        if gains_out is not None:
            gains_out.append(G.copy())
        out[s:s + N] += np.fft.irfft(sp * G, N) * win
    return out


def replay(x, gains):
    """The same gains on a different signal - separates the speech and noise
    components of the output."""
    win = np.sqrt(np.hanning(N + 1)[:N])
    out = np.zeros(len(x))
    for i, G in enumerate(gains):
        s = i * HOP
        if s + N > len(x):
            break
        out[s:s + N] += np.fft.irfft(np.fft.rfft(x[s:s + N] * win) * G, N) * win
    return out
