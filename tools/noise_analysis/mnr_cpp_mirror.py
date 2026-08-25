"""Transliteration of mnr_process()/mnr_frame() from fx.cpp back to Python,
driven one SYNTH_BLOCK_SIZE block at a time exactly as fx_process() drives it.

Two things this proves that the tuning script cannot: that the block-by-block
plumbing (sliding analysis buffer, overlap-add, output FIFO) reconstructs the
signal, and that the unit as written still lands on the numbers the defaults
were chosen against.
"""
import math
import numpy as np

SR = 48000.0
BLK = 64
N = 256
HOP = N // 2
BINS = N // 2 + 1
BUCKETS = 4
OVERSUB = 3.0
BIAS = 1.6
DD_TAU = 0.06
SM_TAU = 0.12
HUGE = 1e30
EPS = 1e-20
GAM_MAX = 1e6


class Mnr:
    def __init__(self):
        self.win = [math.sqrt(0.5 * (1.0 - math.cos(2.0 * math.pi * i / N)))
                    for i in range(N)]
        self.inb = [0.0] * N
        self.ola = [0.0] * N
        self.out = [0.0] * HOP
        self.in_fill = 0
        self.out_rd = 0
        self.out_have = 0
        self.sm = [0.0] * BINS
        self.lam = [0.0] * BINS
        self.buck = [[HUGE] * BUCKETS for _ in range(BINS)]
        self.gp = [1.0] * BINS
        self.gamp = [1.0] * BINS
        self.cnt = 0
        self.primed = False
        self.capture = False
        self.gains = []


def mnr_frame(c, over, gmin, k_sm, a_dd, learn, boundary):
    """`c.gains` collects each frame's 129 gains when c.capture is set, so the
    same trajectory can be replayed on the speech and noise components
    separately — the only way to measure what a unit did *under* a voice."""
    buf = [c.inb[i] * c.win[i] for i in range(N)]
    sp = np.fft.rfft(np.array(buf))
    re = list(sp.real)
    im = list(sp.imag)
    for k in range(BINS):
        p = re[k] * re[k] + im[k] * im[k]
        if not c.primed:
            c.sm[k] = p
            c.lam[k] = p
            for w in range(BUCKETS):
                c.buck[k][w] = p
        else:
            c.sm[k] += k_sm * (p - c.sm[k])
            if learn:
                c.lam[k] = c.sm[k]
                for w in range(BUCKETS):
                    c.buck[k][w] = c.sm[k]
            else:
                if c.sm[k] < c.buck[k][0]:
                    c.buck[k][0] = c.sm[k]
                if boundary:
                    best = HUGE
                    for w in range(BUCKETS):
                        if c.buck[k][w] < best:
                            best = c.buck[k][w]
                    if best < HUGE:
                        c.lam[k] = best
                    for w in range(BUCKETS - 1, 0, -1):
                        c.buck[k][w] = c.buck[k][w - 1]
                    c.buck[k][0] = HUGE
        lam_e = c.lam[k] * BIAS * over
        if lam_e < EPS:
            lam_e = EPS
        gam = p / lam_e
        if gam > GAM_MAX:
            gam = GAM_MAX
        ex = (gam - 1.0) if gam > 1.0 else 0.0
        xi = a_dd * c.gp[k] * c.gp[k] * c.gamp[k] + (1.0 - a_dd) * ex
        if xi < 1e-10:
            xi = 1e-10
        g = xi / (1.0 + xi)
        if g < gmin:
            g = gmin
        c.gp[k] = g
        c.gamp[k] = gam
        re[k] *= g
        im[k] *= g
        if c.capture:
            c.gains[-1][k] = g
    back = np.fft.irfft(np.array(re) + 1j * np.array(im), N)
    for i in range(N):
        c.ola[i] += back[i] * c.win[i]


def mnr_process(c, src, amount=0.6, floor_db=-24.0, adapt_s=1.5, learn=False):
    """`src` is the mono source; returns the unit's mono output y, which the
    bus receives as m*(y - x)."""
    over = max(amount * OVERSUB, 1e-3)
    gmin = 10.0 ** (floor_db / 20.0)
    fr_s = HOP / SR
    k_sm = 1.0 - math.exp(-fr_s / SM_TAU)
    a_dd = math.exp(-fr_s / DD_TAU)
    bucket = int(adapt_s / BUCKETS / fr_s)
    if bucket < 1:
        bucket = 1
    y = np.zeros(len(src))
    for i in range(len(src)):
        c.inb[HOP + c.in_fill] = src[i]
        c.in_fill += 1
        if c.in_fill >= HOP:
            c.in_fill = 0
            c.cnt += 1
            boundary = c.cnt >= bucket
            if boundary:
                c.cnt = 0
            if c.capture:
                c.gains.append([0.0] * BINS)
            mnr_frame(c, over, gmin, k_sm, a_dd, learn, boundary)
            c.primed = True
            for j in range(HOP):
                c.out[j] = c.ola[j]
                c.ola[j] = c.ola[j + HOP]
                c.ola[j + HOP] = 0.0
                c.inb[j] = c.inb[j + HOP]
            c.out_rd = 0
            c.out_have = HOP
        if c.out_have > 0:
            y[i] = c.out[c.out_rd]
            c.out_rd += 1
            c.out_have -= 1
    return y


if __name__ == '__main__':
    import sys
    sys.path.insert(0, 'tools/noise_analysis')
    from sim_fx import rms_db

    # 1) WOLA identity: at floor 0 dB and amount 0 the unit must hand back its
    #    input, delayed. Any window/hop/OLA slip shows up here as leftover.
    rng = np.random.default_rng(5)
    x = rng.standard_normal(48000) * 0.1
    c = Mnr()
    y = mnr_process(c, x, amount=0.0, floor_db=0.0)
    best, lag = None, None
    for d in range(N + 8):
        if d == 0:
            continue
        e = rms_db(y[d:40000] - x[:40000 - d]) - rms_db(x[:40000])
        if best is None or e < best:
            best, lag = e, d
    print(f"  passthrough: residual {best:.1f} dB at a lag of {lag} samples "
          f"({lag/SR*1000:.2f} ms)")

    # 2) the real file, at the shipped defaults
    a = np.load('tools/noise_analysis/data.npy')
    n1 = a[:, 0]
    c = Mnr()
    y = mnr_process(c, n1)
    print(f"  noise.wav: {rms_db(n1):.1f} -> {rms_db(y):.1f} dBFS  "
          f"({rms_db(n1)-rms_db(y):.1f} dB down)")
    print("  (mnr_tune.py's frame-driven model said 16.1 dB)")


def replay_gains(x, gains):
    """Run the same overlap-add path with a fixed gain sequence, so the speech
    and noise components of the unit's output can be recovered separately."""
    win = [math.sqrt(0.5 * (1.0 - math.cos(2.0 * math.pi * i / N)))
           for i in range(N)]
    inb = [0.0] * N
    ola = [0.0] * N
    out = [0.0] * HOP
    in_fill = 0
    out_rd = 0
    out_have = 0
    fi = 0
    y = np.zeros(len(x))
    for i in range(len(x)):
        inb[HOP + in_fill] = x[i]
        in_fill += 1
        if in_fill >= HOP:
            in_fill = 0
            if fi < len(gains):
                G = np.array(gains[fi])
                buf = np.array([inb[j] * win[j] for j in range(N)])
                sp = np.fft.rfft(buf) * G
                back = np.fft.irfft(sp, N)
                for j in range(N):
                    ola[j] += back[j] * win[j]
            fi += 1
            for j in range(HOP):
                out[j] = ola[j]
                ola[j] = ola[j + HOP]
                ola[j + HOP] = 0.0
                inb[j] = inb[j + HOP]
            out_rd = 0
            out_have = HOP
        if out_have > 0:
            y[i] = out[out_rd]
            out_rd += 1
            out_have -= 1
    return y
