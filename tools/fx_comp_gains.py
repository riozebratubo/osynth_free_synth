#!/usr/bin/env python3
"""Derive and print the FX level-compensation constants (S35).

The three `comp` switches in components/fx/fx.cpp carry make-up gains whose
values are *calculated*, not tuned by ear, so this reproduces the arithmetic
next to the code that uses it. If someone changes kRevInGain / kRevPreAp /
kRevWet, or the granular window, the constants in fx.cpp have to move with
them — run this to get the new ones.

The reverb figure assumes a mono-correlated input (bl == br), which is the
loud case and therefore the safe one to size the make-up against; uncorrelated
stereo sums 3 dB lower into the combs and will land a little under unity.
Damping is not modelled: it removes real energy from the tail on purpose, and
fx.rev.comp deliberately does not compensate it.

Run from anywhere:  python tools/fx_comp_gains.py
"""
import math

# ---- reverb: components/fx/fx.cpp gain staging ----
K_REV_IN_GAIN = 0.06
K_REV_PRE_AP = 0.25
K_REV_WET = 3.0
N_COMBS = 8

# fb = 0.70 + 0.28 * fx.rev.size
FB_MIN, FB_MAX = 0.70, 0.98
SIZE_DEFAULT = 0.55

# ---- granular ----
# Window 4p(1-p) over p in [0,1]: mean 2/3, mean square 8/15.
GRN_WIN_RMS = math.sqrt(8.0 / 15.0)
# Equal-power pan with theta uniform on [0, pi/2]: E[cos^2 theta] = 1/2.
GRN_PAN_RMS = math.sqrt(0.5)
GRN_COMP_MAX = 4.0

DENS_DEFAULT, SIZE_S_DEFAULT = 12.0, 0.09


def rev_ref_gain() -> float:
    """Everything fixed in the wet path, incl. the x2 for bl+br (mono)."""
    return K_REV_IN_GAIN * K_REV_PRE_AP * K_REV_WET * 2.0


def rev_wet_ratio(fb: float) -> float:
    """Wet/dry amplitude. Each comb has power gain 1/(1-fb^2); the eight are
    tuned to mutually prime lengths, so their outputs sum in power."""
    return rev_ref_gain() * math.sqrt(N_COMBS) / math.sqrt(1.0 - fb * fb)


def rev_makeup(fb: float) -> float:
    return math.sqrt(1.0 - fb * fb) / (rev_ref_gain() * math.sqrt(N_COMBS))


def grn_makeup(dens: float, size_s: float) -> float:
    duty = min(1.0, dens * size_s)
    return min(GRN_COMP_MAX,
               1.0 / (GRN_WIN_RMS * GRN_PAN_RMS * math.sqrt(max(duty, 1e-3))))


def db(x: float) -> float:
    return 20.0 * math.log10(x)


def main() -> int:
    print("reverb (fx.rev.comp)")
    print(f"  kRevRefGain = {rev_ref_gain():.4f}   kRevCombSum = "
          f"{math.sqrt(N_COMBS):.7f}")
    print(f"  product     = {rev_ref_gain() * math.sqrt(N_COMBS):.5f}"
          "   (the divisor in fx.cpp)")
    print(f"  {'size':>6} {'fb':>6} {'wet/dry':>9} {'dB':>7} {'make-up':>9}")
    for size in (0.0, 0.25, SIZE_DEFAULT, 0.75, 1.0):
        fb = FB_MIN + (FB_MAX - FB_MIN) * size
        r = rev_wet_ratio(fb)
        tag = "  <- default" if size == SIZE_DEFAULT else ""
        print(f"  {size:6.2f} {fb:6.3f} {r:9.4f} {db(r):7.2f} "
              f"{rev_makeup(fb):9.3f}{tag}")
    print(f"  clamped to [0.25, 4.0] in fx.cpp: slack over the whole range")

    print("\ngranular (fx.grn.comp)")
    print(f"  kGrnWinRms = {GRN_WIN_RMS:.5f}  ({db(GRN_WIN_RMS):+.2f} dB, "
          "parabolic window)")
    print(f"  kGrnPanRms = {GRN_PAN_RMS:.5f}  ({db(GRN_PAN_RMS):+.2f} dB, "
          "random equal-power pan)")
    fixed = 1.0 / (GRN_WIN_RMS * GRN_PAN_RMS)
    print(f"  fixed make-up at full duty = {fixed:.4f}  ({db(fixed):+.2f} dB)")
    cap_duty = (1.0 / (GRN_COMP_MAX * GRN_WIN_RMS * GRN_PAN_RMS)) ** 2
    print(f"  cap {GRN_COMP_MAX} (+{db(GRN_COMP_MAX):.1f} dB) binds below duty "
          f"{cap_duty:.3f}")
    print(f"  {'dens':>6} {'size':>6} {'duty':>6} {'make-up':>9} {'dB':>7}")
    for dens, size_s in ((DENS_DEFAULT, SIZE_S_DEFAULT), (12.0, 0.02),
                         (1.0, 0.09), (32.0, 0.2), (64.0, 0.5)):
        duty = min(1.0, dens * size_s)
        g = grn_makeup(dens, size_s)
        tag = ("  <- default" if (dens, size_s) == (DENS_DEFAULT,
                                                    SIZE_S_DEFAULT) else "")
        print(f"  {dens:6.1f} {size_s:6.2f} {duty:6.3f} {g:9.3f} "
              f"{db(g):7.2f}{tag}")

    print("\ndelay (fx.dly.comp): crossfade law only, make-up 1.0")
    print("  equal-gain vs equal-power over the mix knob:")
    print(f"  {'mix':>5} {'gain law':>10} {'power law':>10} {'delta dB':>9}")
    for m in (0.0, 0.25, 0.5, 0.75, 1.0):
        lin = math.sqrt((1.0 - m) ** 2 + m * m)   # decorrelated wet, unity RMS
        pw = math.sqrt((1.0 - m) + m)             # == 1 by construction
        print(f"  {m:5.2f} {lin:10.4f} {pw:10.4f} {db(pw / lin):9.2f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
