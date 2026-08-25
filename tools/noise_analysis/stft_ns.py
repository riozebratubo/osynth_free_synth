"""What a real microphone denoiser does: per-BIN suppression in the STFT domain.

This is the family every USB headset chip, Speex, WebRTC NS and the classic
literature belong to - Wiener/MMSE gains driven by a decision-directed a-priori
SNR (Ephraim & Malah 1984) over a minimum-statistics noise floor (Martin 2001).

The one idea that makes it work under speech: a voice is SPARSE in frequency.
At any instant it occupies a fraction of the bins, so every other bin can still
be pushed down. A gate makes one decision for the whole spectrum and therefore
cannot do this, no matter how it is tuned.

Note the smoothing constants are given in SECONDS and converted, not as raw
per-frame alphas: the minimum tracker has to see a level, not a waveform, and
the hop is short enough that a hand-picked alpha is the same trap the per-band
unit fell into.
"""
import numpy as np

SR = 48000.0


def stft_denoise(x, nfft=512, hop=128, dd_tau=0.060, floor_db=-20.0,
                 sm_tau=0.120, min_win_s=1.5, buckets=8, bias=1.6,
                 oversub=1.0, gains_out=None):
    x = np.asarray(x, dtype=np.float64)
    win = np.sqrt(np.hanning(nfft + 1)[:nfft])   # sqrt-Hann: WOLA at 75% overlap
    fr_s = hop / SR
    a_sm = np.exp(-fr_s / sm_tau)
    a_dd = np.exp(-fr_s / dd_tau)
    nfr = 1 + (len(x) - nfft) // hop
    out = np.zeros(len(x)); wsum = np.zeros(len(x))
    nbin = nfft // 2 + 1
    lam = np.zeros(nbin); sm = np.zeros(nbin)
    buck = np.full((buckets, nbin), np.inf)
    fr_per_bucket = max(int(min_win_s / fr_s / buckets), 1)
    gmin = 10.0 ** (floor_db / 20.0)
    G_prev = np.ones(nbin); gam_prev = np.ones(nbin)
    cnt = 0; primed = False
    if gains_out is not None:
        gains_out.clear()
    for i in range(nfr):
        s = i * hop
        Y = np.fft.rfft(x[s:s + nfft] * win)
        P = np.abs(Y) ** 2
        if not primed:
            sm = P.copy(); lam = P * bias; buck[:] = P; primed = True
        else:
            sm = a_sm * sm + (1.0 - a_sm) * P
        buck[0] = np.minimum(buck[0], sm)
        cnt += 1
        if cnt >= fr_per_bucket:
            cnt = 0
            lam = np.minimum.reduce(buck) * bias
            buck[1:] = buck[:-1]; buck[0] = np.inf
        lam_s = np.maximum(lam, 1e-20)
        gam = np.minimum(P / lam_s, 1e6)
        xi = a_dd * (G_prev ** 2) * gam_prev + \
            (1.0 - a_dd) * np.maximum(gam - oversub, 0.0)
        xi = np.maximum(xi, 1e-10)
        G = np.maximum(xi / (1.0 + xi), gmin)
        G_prev = G; gam_prev = gam
        if gains_out is not None:
            gains_out.append(G.copy())
        out[s:s + nfft] += np.fft.irfft(Y * G, nfft) * win
        wsum[s:s + nfft] += win ** 2
    nz = wsum > 1e-8
    out[nz] /= wsum[nz]
    return out


def apply_gains(x, gains, nfft=512, hop=128):
    """Replay a stored gain sequence on a different signal, so the speech and
    noise components of the output can be recovered separately."""
    win = np.sqrt(np.hanning(nfft + 1)[:nfft])
    out = np.zeros(len(x)); wsum = np.zeros(len(x))
    for i, G in enumerate(gains):
        s = i * hop
        if s + nfft > len(x):
            break
        Y = np.fft.rfft(x[s:s + nfft] * win)
        out[s:s + nfft] += np.fft.irfft(Y * G, nfft) * win
        wsum[s:s + nfft] += win ** 2
    nz = wsum > 1e-8
    out[nz] /= wsum[nz]
    return out
