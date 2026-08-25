#!/usr/bin/env python3
"""Check the vocoder's staging invariants against fx.cpp.

Two things went wrong in S43 and both are checkable without a build, so they
are checked here instead of by ear.

1. kVocQRef must equal Q_eff at the *registered defaults*. It stands where
   voc_rebuild()'s derived Q_eff used to, and SvfMode::BpN is exactly
   SvfMode::Bp divided by that Q -- so if the two drift apart, every magnitude
   in the unit moves, and `fx.voc.gate` (an ABSOLUTE threshold, not a ratio)
   silently stops meaning what its default was chosen for. That is what made
   the unit quiet and unintelligible rather than merely mis-levelled: at 4.67x
   too high, the gate put most bands under `if (e <= 0) continue`.

2. The output must no longer track fx.voc.q, fx.voc.bands or the low/high
   span. Before the BpN change the wet path carried a Q^2 term, which made
   four tone controls into level controls across a ~28 dB range. The report
   below is what that swing looked like and what it is now.

Reads the constants and defaults straight out of the source, so it fails if
someone edits one without the other.

Usage:  python tools/voc_stage_check.py
Exit status 1 if an invariant is broken.
"""
import math
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FX = os.path.join(ROOT, "components", "fx", "fx.cpp")


def q_eff(bands, low, high, q01):
    """voc_rebuild()'s Q: q_nat sized so adjacent -3 dB skirts meet, times the
    0.4..2.0 the fx.voc.q knob spans."""
    if high < low * 2.0:
        high = low * 2.0
    ratio = (high / low) ** (1.0 / (bands - 1))
    q_nat = math.sqrt(ratio) / (ratio - 1.0)
    return q_nat * (0.4 + 1.6 * q01)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def const(src, name):
    m = re.search(r"constexpr float %s\s*=\s*([0-9.]+)f" % name, src)
    if not m:
        raise SystemExit("could not find constexpr float %s in fx.cpp" % name)
    return float(m.group(1))


def param_default(src, pid):
    """The `def` field of a kParams row: {PID, "name", type, curve, min, max, def, ...}"""
    m = re.search(r"\{%s,\s*\"[\w.]+\",[^}]*?"
                  r"([-\d.]+)f,\s*([-\d.]+)f,\s*([-\d.]+)f," % pid, src, re.S)
    if not m:
        raise SystemExit("could not read the kParams row for %s" % pid)
    return float(m.group(1)), float(m.group(2)), float(m.group(3))


def main():
    src = read(FX)
    fails = []

    # Band ceiling is per-target; the registered default is the max.
    m = re.search(r"#if CONFIG_SPIRAM\s*\nconstexpr int kVocBandsMax = (\d+);"
                  r"\s*\n#else\s*\nconstexpr int kVocBandsMax = (\d+);", src)
    if not m:
        raise SystemExit("could not read kVocBandsMax")
    bands_psram, bands_plain = int(m.group(1)), int(m.group(2))

    _, _, low = param_default(src, "FX_PID_VOC_LOW")
    _, _, high = param_default(src, "FX_PID_VOC_HIGH")
    _, _, q01 = param_default(src, "FX_PID_VOC_Q")
    gmin, gmax, gate01 = param_default(src, "FX_PID_VOC_GATE")
    lmin, lmax, level = param_default(src, "FX_PID_VOC_LEVEL")
    qref = const(src, "kVocQRef")

    want = q_eff(bands_psram, low, high, q01)
    print("registered defaults: bands=%d  low=%.0f  high=%.0f  q=%.2f"
          % (bands_psram, low, high, q01))
    print("  Q_eff there      = %.4f" % want)
    print("  kVocQRef         = %.4f" % qref)
    if abs(want - qref) > 0.01:
        fails.append("kVocQRef (%.4f) != Q_eff at the registered defaults"
                     " (%.4f). The gate and level defaults were chosen in the"
                     " Bp domain; if this drifts they stop matching it."
                     % (qref, want))
    else:
        print("  -> matches, so fx.voc.gate/level keep their pre-S43 meaning")

    # fx.voc.gate is absolute: gate = fx.voc.gate * 0.2, compared against
    # b.env * kVocQRef. Sanity-check the default lands where it used to.
    print("\ngate: default %.3f -> %.4f absolute, vs envelopes scaled by %.2f"
          % (gate01, gate01 * 0.2, qref))
    if not (gmin == 0.0 and gmax == 1.0):
        fails.append("fx.voc.gate range moved from 0..1 (%g..%g)" % (gmin, gmax))
    # fx.voc.gate and fx.voc.level are coupled: the gate decides how much of
    # the modulator's envelope reaches the bank, so opening it makes the unit
    # louder and `level` has to come back. The pair below was measured with
    # tools/voc_sim.py (every preset under soft_clip's knee, no clipping).
    # Changing one without the other, or without re-running voc_sim, is the
    # mistake this guards.
    PAIRS = {0.02: 2.5, 0.06: 4.0}
    if lmax != 16.0 or lmin != 0.0:
        fails.append("fx.voc.level range moved from 0..16 (%g..%g)" % (lmin, lmax))
    want_level = PAIRS.get(round(gate01, 3))
    if want_level is None:
        fails.append("fx.voc.gate default %g is not a measured pairing %s --"
                     " re-run tools/voc_sim.py and record the level that keeps"
                     " every preset under the 0.80 soft-clip knee"
                     % (gate01, sorted(PAIRS)))
    elif abs(level - want_level) > 1e-6:
        fails.append("fx.voc.gate %g pairs with fx.voc.level %g, but level is"
                     " %g -- opening the gate roughly doubles what reaches the"
                     " bank, so the two move together"
                     % (gate01, want_level, level))

    # --- fx.voc.norm: the three faults that made it "much clipping" --------
    nref = const(src, "kVocNormRef")
    nfloor = const(src, "kVocNormFloor")
    nrise = const(src, "kVocNormRiseMs")
    boost = nref / nfloor
    print("")
    print("normalisation: ref=%.3f floor=%.3f -> max boost %.1fx, rise %.0f ms"
          % (nref, nfloor, boost, nrise))
    # env_bb for an ordinary mic signal measures 0.02-0.04; a reference above
    # that makes nrm a boost always and never a cut, which is what turned this
    # switch into a +10 dB button.
    if not (0.02 <= nref <= 0.06):
        fails.append("kVocNormRef %.3f is outside the 0.02-0.06 that env_bb"
                     " actually reaches -- above it the switch only ever adds"
                     " gain, which is the bug it had" % nref)
    if boost > 4.0:
        fails.append("max normalisation boost is %.1fx (ref/floor). It was 20x"
                     " and reached it, doubling the output peak. Keep it <= 4x"
                     % boost)
    if nrise < 100.0:
        fails.append("kVocNormRiseMs %.0f is faster than a band envelope's"
                     " decay; the gain then amplifies the ring-out after every"
                     " word, which is what was heard as clipping" % nrise)
    if "if (target < v.nrm)" not in src:
        fails.append("the normaliser's instant-down/slow-up branch is gone;"
                     " without it the rise time constant does nothing")

    # --- fx.voc.freeze records audio, not a spectral frame -----------------
    # Two earlier designs froze the band envelopes instead, and both failed:
    # frozen live they captured the silence after the word (the envelopes
    # follow fx.voc.release, 25-40 ms, and nobody releases a button while
    # still speaking); given a per-band peak-hold to reach back through that
    # gap, each band took its maximum from a different moment, which is a
    # flat spectrum, and a flat bank passes the carrier through untouched.
    print("")
    if "constexpr uint32_t kVocSampLen" not in src:
        fails.append("the vocoder capture buffer is gone; fx.voc.freeze has"
                     " no audio to replay")
    else:
        secs = [ln.split("=")[1].strip().rstrip("f;")
                for ln in src.splitlines()
                if "constexpr float kVocSampS" in ln]
        print("capture: %s s with PSRAM, %s s without" % tuple(secs[:2]))
    if "voice_manager_block_note()" not in src:
        fails.append("the note-on retrigger is gone; a captured phrase would"
                     " play once and never restart")
    for dead, why in (("kVocHoldMs", "the peak-hold constant"),
                      ("v.hold_bb", "the snapshot reference"),
                      ("b.hold", "the per-band snapshot")):
        if dead in src:
            fails.append("%s (%s) is back -- fx.voc.freeze stores audio now,"
                         " and freezing spectrum instead is the bug that was"
                         " reported twice" % (dead, why))

    # The whole point: level must not track q / bands / span any more.
    print("\nold Bp path carried Q^2; the frozen Q removes it:")
    print("  %-34s %10s %10s" % ("setting", "old (dB)", "now (dB)"))
    base = q_eff(bands_psram, low, high, q01)
    cases = [
        ("fx.voc.q = 0.0", (bands_psram, low, high, 0.0)),
        ("fx.voc.q = 1.0", (bands_psram, low, high, 1.0)),
        ("bands = %d (no PSRAM)" % bands_plain, (bands_plain, low, high, q01)),
        ("span 250-4500", (bands_psram, 250.0, 4500.0, q01)),
    ]
    for label, args in cases:
        q = q_eff(*args)
        old_db = 20.0 * math.log10((q * q) / (base * base))
        print("  %-34s %+10.1f %+10.1f" % (label, old_db, 0.0))

    if src.count("SvfMode::BpN") < 3:
        fails.append("expected the analysis and both synthesis calls to use"
                     " SvfMode::BpN; found %d" % src.count("SvfMode::BpN"))
    if re.search(r"SvfMode::Bp\b(?!N)", src.split("vocoder_process")[-1][:4000]):
        fails.append("a plain SvfMode::Bp survives inside vocoder_process;"
                     " that reintroduces the Q^2 term")

    print()
    if fails:
        for f in fails:
            print("FAIL: %s" % f)
        return 1
    print("staging invariants hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
