#!/usr/bin/env python3
"""Offline model of vocoder_process(), to measure each preset instead of guessing.

A faithful port of components/fx/fx.cpp: the same svf_coef_k / svf_next
difference equations, the same band layout from voc_rebuild(), the same
follower, gate, normalisation and staging, and the same soft_clip() backstop.
It is not a substitute for listening -- it says nothing about how a preset
sounds -- but it answers the questions that were being answered by estimate:

  * how hot is the wet path, and does soft_clip() engage?
  * how many bands survive the gate?
  * how does the sibilance tap sit against the bank?

The modulator is synthetic speech-ish: a pitched buzz with three formants,
amplitude-contoured into syllables, plus an HF burst for the consonants and a
room-noise floor. The carrier is either a saw (the "hold a chord" case) or
white noise, matching fx.voc.carrier.

Usage:  python tools/voc_sim.py            # every preset in VocoderPresets.qml
        python tools/voc_sim.py Robot      # just one
"""
import json
import math
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FX = os.path.join(ROOT, "components", "fx", "fx.cpp")
QML = os.path.join(ROOT, "app_osyntho", "qml", "VocoderPresets.qml")

SR = 48000.0
BLOCK = 64          # SYNTH_BLOCK_SIZE-ish; only the normaliser cares
SOFT_KNEE = 0.80


# ---- ports of synth_dsp.h ------------------------------------------------

def clamp_cutoff(hz):
    return min(max(hz, 20.0), 0.45 * SR)


def svf_coef_k(cutoff, k):
    cutoff = clamp_cutoff(cutoff)
    k = max(k, 0.04)                      # kSvfMinK
    g = math.tan(math.pi * cutoff / SR)
    a1 = 1.0 / (1.0 + g * (g + k))
    return (k, a1, g * a1, g * g * a1)


class Svf(object):
    __slots__ = ("ic1", "ic2")

    def __init__(self):
        self.ic1 = 0.0
        self.ic2 = 0.0


def svf_bpn(f, c, x):
    """SvfMode::BpN -> k * v1."""
    k, a1, a2, a3 = c
    v3 = x - f.ic2
    v1 = a1 * f.ic1 + a2 * v3
    v2 = f.ic2 + a2 * f.ic1 + a3 * v3
    f.ic1 = 2.0 * v1 - f.ic1
    f.ic2 = 2.0 * v2 - f.ic2
    return k * v1


def svf_hp(f, c, x):
    k, a1, a2, a3 = c
    v3 = x - f.ic2
    v1 = a1 * f.ic1 + a2 * v3
    v2 = f.ic2 + a2 * f.ic1 + a3 * v3
    f.ic1 = 2.0 * v1 - f.ic1
    f.ic2 = 2.0 * v2 - f.ic2
    return x - k * v1 - v2


def soft_clip(x):
    a = abs(x)
    if a <= SOFT_KNEE:
        return x, False
    d = 1.0 - SOFT_KNEE
    over = a - SOFT_KNEE
    y = SOFT_KNEE + d * over / (over + d)
    return (-y if x < 0.0 else y), True


# ---- constants read out of fx.cpp ---------------------------------------

def fx_const(src, name):
    m = re.search(r"constexpr float %s\s*=\s*([0-9.]+)f" % name, src)
    return float(m.group(1)) if m else None


def fx_param_default(src, pid):
    m = re.search(r"\{%s,\s*\"[\w.]+\",[^}]*?([-\d.]+)f,\s*([-\d.]+)f,\s*([-\d.]+)f,"
                  % pid, src, re.S)
    return float(m.group(3))


def band_layout(bands, low, high, q01):
    if high < low * 2.0:
        high = low * 2.0
    ratio = (high / low) ** (1.0 / (bands - 1))
    q_nat = math.sqrt(ratio) / (ratio - 1.0)
    k = 1.0 / max(q_nat * (0.4 + 1.6 * q01), 0.05)
    return ratio, k


# ---- test signals --------------------------------------------------------

def modulator(n, level=0.10):
    """Speech-ish: buzz through three formants, syllable contour, HF bursts."""
    out = [0.0] * n
    f0 = 110.0
    formants = ((700.0, 0.9), (1220.0, 0.5), (2600.0, 0.25))
    rng = 12345
    for i in range(n):
        t = i / SR
        # syllables at ~4 Hz, with gaps
        env = max(0.0, math.sin(2.0 * math.pi * 4.0 * t))
        env = env * env
        s = 0.0
        for h in range(1, 30):
            fh = f0 * h
            if fh > 5000.0:
                break
            a = 0.0
            for fc, fa in formants:
                a += fa / (1.0 + ((fh - fc) / 300.0) ** 2)
            s += a * math.sin(2.0 * math.pi * fh * t) / h
        # consonant bursts on the syllable edges
        rng = (1103515245 * rng + 12345) & 0x7FFFFFFF
        hiss = (rng / 1073741824.0 - 1.0)
        burst = 1.0 if (t % 0.25) < 0.03 else 0.0
        out[i] = level * (env * s * 0.35 + burst * hiss * 0.5 + hiss * 0.004)
    return out


def carrier_saw(n, amp=0.40):
    out = [0.0] * n
    ph = 0.0
    for i in range(n):
        ph += 130.81 / SR
        if ph >= 1.0:
            ph -= 1.0
        out[i] = amp * (2.0 * ph - 1.0)
    return out


def carrier_noise(n, gain=None):
    """gain defaults to kVocNoiseGain, read from fx.cpp so the two cannot drift."""
    if gain is None:
        gain = fx_const(open(FX, encoding="utf-8").read(), "kVocNoiseGain") or 0.5
    out = [0.0] * n
    s = 0x9e3779b9
    for i in range(n):
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        v = s if s < 0x80000000 else s - 0x100000000
        out[i] = gain * (v / 2147483648.0)
    return out


# ---- the unit ------------------------------------------------------------

def run(p, src, seconds=1.5):
    qref = fx_const(src, "kVocQRef")
    nref = fx_const(src, "kVocNormRef")
    nfloor = fx_const(src, "kVocNormFloor")
    bands = 16

    n = int(SR * seconds)
    mod = modulator(n)
    use_bus = p["carrier"] != 1
    use_noise = p["carrier"] != 0
    bus = carrier_saw(n) if use_bus else [0.0] * n
    noi = carrier_noise(n) if use_noise else [0.0] * n

    ratio, k = band_layout(bands, p["low"], p["high"], p["q"])
    cmul = 2.0 ** (p["shift"] / 12.0)
    mc, cc, mf, cf, env = [], [], [], [], []
    f = p["low"]
    for _ in range(bands):
        mc.append(svf_coef_k(f, k))
        cc.append(svf_coef_k(f * cmul, k))
        mf.append(Svf())
        cf.append(Svf())
        env.append(0.0)
        f *= ratio
    sib_c = svf_coef_k(p["high"], 0.9)
    sib_f = Svf()

    ka = 1.0 - math.exp(-1.0 / max(p["attack"] * 0.001 * SR, 1.0))
    kr = 1.0 - math.exp(-1.0 / max(p["release"] * 0.001 * SR, 1.0))
    gate = p["gate"] * 0.2
    level = p["level"]
    sib = p["sib"]
    clarity = bool(p.get("clarity", 0))
    cgain = fx_const(src, "kVocClarityGain") or 1.0
    rise_ms = fx_const(src, "kVocNormRiseMs") or 0.0
    krise = (1.0 - math.exp(-(BLOCK / SR) / (rise_ms * 0.001))) if rise_ms else None

    env_bb = env_car = 0.0
    nrm = 1.0
    peak = 0.0
    sq = 0.0
    clips = 0
    open_bands = 0
    bank_abs = 0.0
    sib_abs = 0.0

    for i in range(n):
        if i % BLOCK == 0 and p["norm"]:
            blk = sum(abs(mod[j]) for j in range(i, min(i + BLOCK, n)))
            blk /= max(1, min(BLOCK, n - i))
            ref = max(blk, env_bb)
            target = nref / max(ref, nfloor)
            # instant down, slow up -- matches the firmware's limiter idiom
            if krise is None or target < nrm:
                nrm = target
            else:
                nrm += krise * (target - nrm)

        x = mod[i]
        a = abs(x)
        env_bb += (ka if a > env_bb else kr) * (a - env_bb)
        for b in range(bands):
            y = abs(svf_bpn(mf[b], mc[b], x))
            env[b] += (ka if y > env[b] else kr) * (y - env[b])

        c = bus[i] + noi[i]
        ac = abs(c)
        env_car += (ka if ac > env_car else kr) * (ac - env_car)

        wet = 0.0
        for b in range(bands):
            e = (env[b] * qref - gate) * nrm
            if e <= 0.0:
                svf_bpn(cf[b], cc[b], c)
                continue
            open_bands += 1
            wet += svf_bpn(cf[b], cc[b], c) * e
        wet *= qref
        bank_abs += abs(wet)

        hp = svf_hp(sib_f, sib_c, x)
        bb = env_bb - gate
        if sib > 0.0 and bb > 0.0:
            op = (min(bb * nrm / nref, 1.0) if clarity else min(bb * 8.0, 1.0))
            op *= min(env_car * 20.0, 1.0)
            s = hp * sib * op * nrm * (cgain if clarity else 1.0)
            sib_abs += abs(s)
            wet += s

        y, clipped = soft_clip(wet * level)
        if clipped:
            clips += 1
        peak = max(peak, abs(y))
        sq += y * y

    rms = math.sqrt(sq / n)
    return {
        "peak": peak,
        "rms": rms,
        "clip_pct": 100.0 * clips / n,
        "bands_open_pct": 100.0 * open_bands / (n * bands),
        "sib_vs_bank_db": (20.0 * math.log10(sib_abs / bank_abs)
                           if bank_abs > 0 and sib_abs > 0 else float("-inf")),
    }


def presets():
    src = open(QML, encoding="utf-8").read()
    out = []
    for m in re.finditer(r'name:\s*"(\w+)",[\s\S]*?values:\s*\{([\s\S]*?)\}', src):
        body = m.group(2)
        vals = {}
        for km, vv in re.findall(r'"fx\.voc\.(\w+)":\s*(-?[\d.]+)', body):
            vals[km] = float(vv)
        vals.setdefault("shift", 0.0)
        vals["carrier"] = int(vals.get("carrier", 0))
        vals["norm"] = int(vals.get("norm", 1))
        out.append((m.group(1), vals))
    return out


def main(argv):
    src = open(FX, encoding="utf-8").read()
    want = argv[1] if len(argv) > 1 else None
    print("modulator: speech-ish at mean-rectified ~0.10 (the kVocNormRef point)")
    print("carrier:   saw @0.40 for bus presets, white @0.5 for noise\n")
    print("  %-9s %7s %7s %8s %9s %11s" %
          ("preset", "peak", "rms", "clip%", "bands%", "sib-bank dB"))
    print("  " + "-" * 60)
    for name, p in presets():
        if want and name.lower() != want.lower():
            continue
        r = run(p, src)
        flag = ""
        if r["peak"] >= 0.999:
            flag = "  <-- pinned at full scale"
        elif r["clip_pct"] > 5.0:
            flag = "  <-- clipping"
        elif r["peak"] < 0.05:
            flag = "  <-- inaudible"
        print("  %-9s %7.3f %7.3f %7.1f%% %8.1f%% %11.1f%s" %
              (name, r["peak"], r["rms"], r["clip_pct"],
               r["bands_open_pct"], r["sib_vs_bank_db"], flag))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
