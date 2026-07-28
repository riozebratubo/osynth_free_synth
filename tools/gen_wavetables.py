#!/usr/bin/env python3
"""osynth -- factory wavetable generator (Session 7).

Generates `factory_wavetables.h`: 4 table sets x 8 frames x 8 mip levels,
int16 in flash (.rodata). Each frame is a single-cycle waveform; the engine
morphs between adjacent frames (table position) and picks the mip whose
harmonic cap fits the current pitch, so high notes never gross-alias.

Mip scheme (48 kHz): mip m is band-limited to `harm[m]` harmonics and is
valid while f0 <= 24000 / harm[m]:

    mip     0     1     2     3     4     5     6     7
    len   1024   512   256   128    64    64    64    64
    harm   256   128    64    32    16     8     4     2
    f0 <=  93.75 187.5 375   750   1500  3000  6000  12000 Hz

Table length is always >= 4x the harmonic cap, keeping linear-interpolation
image noise low. Each (frame, mip) run gets one wraparound guard sample so
the engine interpolates without a modulo.

Table sets:
    0 basic  -- analog morph: sine, triangle, saw, square, narrowing pulses
    1 sync   -- PPG-style sweep: hard-synced saw, ratio 1 -> 7.5
    2 vocal  -- formant morph A -> E -> I -> O -> U (3 gaussian formants)
    3 fm     -- 2-op FM (1:3), index 0.15 -> 7.5 (bright metallic sweep)

Pipeline: every frame becomes a 256-harmonic complex spectrum (time-domain
recipes are sampled at 2048 points and FFT-analyzed; the vocal set is built
directly in the spectral domain), then each mip is resynthesized by inverse
FFT of the truncated spectrum. Frames are RMS-normalized (equal loudness
across a morph), then each set is uniformly scaled so no mip clips int16.

Pure standard library (the ESP-IDF python venv has no numpy). Invoked
automatically by components/engines/CMakeLists.txt at build time; can also
be run by hand: python tools/gen_wavetables.py -o factory_wavetables.h
"""

import argparse
import cmath
import math
import os

MIP_LEN = [1024, 512, 256, 128, 64, 64, 64, 64]
MIP_HARM = [256, 128, 64, 32, 16, 8, 4, 2]
FRAMES = 8
H_MAX = 256
ANALYSIS_N = 2048  # sampling grid for time-domain recipes (>= 2 * 2 * H_MAX)
TARGET_RMS = 0.45  # per-frame loudness target (sub engine's saw is RMS 0.577)
PEAK_CAP = 0.98    # per-set uniform rescale so int16 never clips

# ---------------------------------------------------------------- FFT (pure)


def fft(x):
    n = len(x)
    if n == 1:
        return x
    even = fft(x[0::2])
    odd = fft(x[1::2])
    out = [0j] * n
    for k in range(n // 2):
        t = cmath.exp(-2j * math.pi * k / n) * odd[k]
        out[k] = even[k] + t
        out[k + n // 2] = even[k] - t
    return out


def ifft(x):
    n = len(x)
    y = fft([v.conjugate() for v in x])
    return [v.conjugate() / n for v in y]


# ------------------------------------------------------- spectra and resynth
# A frame spectrum is c[1..H_MAX]: complex peak amplitude per harmonic, i.e.
# signal(t) = sum_k Re(c[k] * e^(2*pi*i*k*t)).


def analyze(fn):
    """Sample a 1-periodic time-domain recipe and return its spectrum.
    Content above ANALYSIS_N/2 harmonics aliases into the analysis at
    <= 1/1024 of the fundamental (< -60 dB) -- ignored."""
    x = [complex(fn(n / ANALYSIS_N), 0.0) for n in range(ANALYSIS_N)]
    X = fft(x)
    return [0j] + [2.0 * X[k] / ANALYSIS_N for k in range(1, H_MAX + 1)]


def synth_mip(c, length, harm):
    """Inverse-FFT the first `harm` harmonics into a `length`-sample cycle."""
    S = [0j] * length
    for k in range(1, harm + 1):
        S[k] = c[k] * (length / 2.0)
        S[length - k] = S[k].conjugate()
    return [v.real for v in ifft(S)]


# ------------------------------------------------------------- frame recipes


def basic_frames():
    def tri(t):  # 0-crossing at t=0 rising, like the sine
        if t < 0.25:
            return 4.0 * t
        if t < 0.75:
            return 2.0 - 4.0 * t
        return 4.0 * t - 4.0

    def pulse(d):
        return lambda t: 1.0 if t < d else -1.0

    shapes = [
        lambda t: math.sin(2.0 * math.pi * t),
        tri,
        lambda t: 2.0 * t - 1.0,
        pulse(0.5),
        pulse(0.35),
        pulse(0.25),
        pulse(0.15),
        pulse(0.08),
    ]
    return [analyze(f) for f in shapes]


def sync_frames():
    # Hard-synced saw; ratio 1.0 is a plain saw, the sweep's resting frame.
    ratios = [1.0, 1.33, 1.78, 2.37, 3.16, 4.22, 5.62, 7.5]
    return [analyze(lambda t, s=s: 2.0 * ((t * s) % 1.0) - 1.0)
            for s in ratios]


VOWELS = [  # (freq Hz, level, bandwidth Hz) x 3 formants, male-ish
    [(730, 1.00, 90), (1090, 0.50, 110), (2440, 0.35, 160)],  # A
    [(530, 1.00, 80), (1840, 0.45, 120), (2480, 0.30, 160)],  # E
    [(270, 1.00, 60), (2290, 0.35, 130), (3010, 0.30, 180)],  # I
    [(570, 1.00, 80), (840, 0.60, 90), (2410, 0.25, 150)],    # O
    [(300, 1.00, 60), (870, 0.50, 90), (2240, 0.20, 150)],    # U
]
VOCAL_F0 = 110.0  # nominal fundamental the formants are laid out for


def vocal_frames():
    def lerp_vowel(a, b, t):
        return [tuple(x + (y - x) * t for x, y in zip(fa, fb))
                for fa, fb in zip(a, b)]

    def spectrum(formants):
        c = [0j] * (H_MAX + 1)
        for k in range(1, H_MAX + 1):
            f = k * VOCAL_F0
            a = sum(A * math.exp(-0.5 * ((f - F) / BW) ** 2)
                    for (F, A, BW) in formants)
            # zero phase: glottal-pulse-like; 1/sqrt(k) source tilt
            c[k] = complex(a / math.sqrt(k), 0.0)
        return c

    out = []
    for i in range(FRAMES):
        t = i * (len(VOWELS) - 1) / (FRAMES - 1)
        j = min(int(t), len(VOWELS) - 2)
        out.append(spectrum(lerp_vowel(VOWELS[j], VOWELS[j + 1], t - j)))
    return out


def fm_frames():
    # 2-op FM, carrier:modulator = 1:3 (integer ratio keeps the cycle
    # 1-periodic for the analysis). Produces every harmonic except
    # multiples of 3 -- a DX-ish metallic sweep as the index grows.
    indices = [0.15, 0.5, 1.0, 1.7, 2.6, 3.8, 5.4, 7.5]
    return [analyze(lambda t, I=I: math.sin(2.0 * math.pi * t +
                                            I * math.sin(6.0 * math.pi * t)))
            for I in indices]


TABLE_SETS = [
    ("basic", basic_frames),
    ("sync", sync_frames),
    ("vocal", vocal_frames),
    ("fm", fm_frames),
]

# ----------------------------------------------------------------- generator


def build_set(make_frames):
    """Returns (frames_mips, set_gain): frames_mips[frame][mip] = float list."""
    specs = make_frames()
    frames_mips = []
    peak = 0.0
    for c in specs:
        mip0 = synth_mip(c, MIP_LEN[0], MIP_HARM[0])
        rms = math.sqrt(sum(v * v for v in mip0) / len(mip0))
        g = TARGET_RMS / rms if rms > 1e-9 else 1.0
        cg = [v * g for v in c]
        mips = [synth_mip(cg, MIP_LEN[m], MIP_HARM[m])
                for m in range(len(MIP_LEN))]
        peak = max(peak, max(abs(v) for mip in mips for v in mip))
        frames_mips.append(mips)
    scale = min(1.0, PEAK_CAP / peak) if peak > 0.0 else 1.0
    if scale < 1.0:
        frames_mips = [[[v * scale for v in mip] for mip in mips]
                       for mips in frames_mips]
    return frames_mips, scale


def quantize(v):
    return max(-32768, min(32767, int(round(v * 32767.0))))


def emit(out_path):
    mip_off = []
    off = 0
    for length in MIP_LEN:
        mip_off.append(off)
        off += length + 1  # +1 wraparound guard sample
    frame_samples = off

    lines = []
    w = lines.append
    w("/* Generated by tools/gen_wavetables.py -- DO NOT EDIT.")
    w(" * 4 table sets x 8 frames x 8 band-limited mips, int16, ~%d KB"
      % (4 * FRAMES * frame_samples * 2 // 1024))
    w(" * (const -> flash/.rodata). Mip m holds wt_mip_harm[m] harmonics in")
    w(" * wt_mip_len[m] samples (+1 guard for interpolation); use it while")
    w(" * phase_step * wt_mip_harm[m] <= 0.5. */")
    w("#pragma once")
    w("")
    w("#include <stdint.h>")
    w("")
    w("#define WT_TABLE_COUNT %d" % len(TABLE_SETS))
    w("#define WT_FRAME_COUNT %d" % FRAMES)
    w("#define WT_MIP_COUNT %d" % len(MIP_LEN))
    w("#define WT_FRAME_SAMPLES %d" % frame_samples)
    w("")
    w("static const uint16_t wt_mip_len[WT_MIP_COUNT] = {%s};"
      % ", ".join(str(v) for v in MIP_LEN))
    w("static const uint16_t wt_mip_harm[WT_MIP_COUNT] = {%s};"
      % ", ".join(str(v) for v in MIP_HARM))
    w("static const uint16_t wt_mip_off[WT_MIP_COUNT] = {%s};"
      % ", ".join(str(v) for v in mip_off))
    w("")
    w("static const char* const wt_table_names[WT_TABLE_COUNT] = {%s};"
      % ", ".join('"%s"' % name for name, _ in TABLE_SETS))
    w("")
    w("static const int16_t")
    w("wt_tables[WT_TABLE_COUNT][WT_FRAME_COUNT][WT_FRAME_SAMPLES] = {")

    report = []
    for name, make_frames in TABLE_SETS:
        frames_mips, scale = build_set(make_frames)
        report.append((name, scale))
        w("{ /* %s */" % name)
        for fi, mips in enumerate(frames_mips):
            w("{ /* %s frame %d */" % (name, fi))
            samples = []
            for mip in mips:
                samples.extend(quantize(v) for v in mip)
                samples.append(quantize(mip[0]))  # guard
            assert len(samples) == frame_samples
            for i in range(0, len(samples), 12):
                w("    " + ", ".join(str(s) for s in samples[i:i + 12]) + ",")
            w("},")
        w("},")
    w("};")
    w("")

    with open(out_path, "w", newline="\n") as fh:
        fh.write("\n".join(lines))

    total = len(TABLE_SETS) * FRAMES * frame_samples * 2
    print("gen_wavetables: wrote %s (%d bytes of table data)"
          % (out_path, total))
    for name, scale in report:
        print("  set %-6s rms %.2f, set gain %.3f" % (name, TARGET_RMS, scale))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-o", "--output", default="factory_wavetables.h",
                    help="output header path")
    args = ap.parse_args()
    out_dir = os.path.dirname(os.path.abspath(args.output))
    os.makedirs(out_dir, exist_ok=True)
    emit(args.output)


if __name__ == "__main__":
    main()
