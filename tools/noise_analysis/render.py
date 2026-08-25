"""Renders noise.wav through the shipped and the fixed units, at exactly the
defaults now in fx.cpp, and writes the results next to the source for listening."""
import sys, wave, numpy as np
sys.path.insert(0, 'tools/noise_analysis')
from sim_fx import *

a = np.load('tools/noise_analysis/data.npy')
x = a[:, 0]
n = len(x) // BLK * BLK
x = x[:n]


def w16(path, y):
    y = np.clip(y, -1.0, 1.0)
    s = (y * 32767.0).astype('<i2')
    st = np.repeat(s[:, None], 2, axis=1).tobytes()
    w = wave.open(path, 'wb')
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
    w.writeframes(st); w.close()


# defaults as they now stand in fx.cpp
ANR = dict(bands=12, low=120.0, high=9000.0, amount=0.6, floor_db=-20.0,
           adapt_s=3.0, attack_ms=5.0, release_ms=150.0,
           est_tau=0.150, subwins=4, order=2)
NR = dict(hpf=80.0, hum=0, thresh=-24.0, ratio=4.0, floor_db=-24.0,
          attack_ms=3.0, hold_ms=150.0, release_ms=200.0)

runs = [
    ('noise_anr_before.wav', 'ANR, as shipped',        anr_run_fast(x)),
    ('noise_anr_after.wav',  'ANR, fixed',             anr_run_v5(x, **ANR)),
    ('noise_nr_before.wav',  'NR, thresh -45 (old)',   nr_run_fast(x, **{**NR, 'thresh': -45.0})),
    ('noise_nr_after.wav',   'NR, thresh -24 (new)',   nr_run_fast(x, **NR)),
    ('noise_both_after.wav', 'ANR then NR, both fixed',
     nr_run_fast(anr_run_v5(x, **ANR), **NR)),
    ('noise_both_hum.wav',   '...plus hum = 50 Hz',
     nr_run_fast(anr_run_v5(x, **ANR), **{**NR, 'hum': 1})),
]
base = rms_db(x)
print(f"source: {base:.1f} dBFS rms, {20*np.log10(np.max(np.abs(x))):.1f} dBFS peak\n")
late = slice(int(5.0 * SR), n)
for path, tag, y in runs:
    w16('tools/noise_analysis/' + path, y)
    print(f"  {tag:26s} {rms_db(y):7.1f} dBFS   -{base-rms_db(y):5.1f} dB overall"
          f"   -{rms_db(x[late])-rms_db(y[late]):5.1f} dB after 5 s   -> {path}")
