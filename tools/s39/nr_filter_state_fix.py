#!/usr/bin/env python3
"""S39 follow-up: reset the fixed NR's filter states when they go stale.

Two transitions left an SVF holding state that describes a filter that is no
longer the one being run:

  * `fx.nr.hpf` swept down to its minimum bypasses the high-pass, and the
    integrators keep whatever they held. Sweeping back up re-engages the
    filter on top of it.
  * `fx.nr.hum` moved between off / 50 / 60 rebuilds the notch coefficients
    around state charged at the old frequency, which rings out as a beat at
    the difference rather than as a notch.

Zeroing is the right answer for both, and not just the safe one: a notch whose
state is zero passes the input through and settles into the null over a few
cycles, where one carrying the wrong resonator's energy has to ring it out
first.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

OLD_HP = """    const bool use_hp = hpf > kNrHpfOff * 1.02f;
    if (use_hp) c.hp_c = osynth::dsp::svf_coef_k(hpf, kNrHpK, kSr);

    const int hum = (int)pv(NR_HUM);
    if (hum != c.c_hum) {
        c.c_hum = hum;
        if (hum > 0) {
"""
NEW_HP = """    const bool use_hp = hpf > kNrHpfOff * 1.02f;
    if (use_hp) {
        c.hp_c = osynth::dsp::svf_coef_k(hpf, kNrHpK, kSr);
    } else {
        /* Bypassed, and held clean rather than merely unread: a sweep back up
         * off the minimum would otherwise re-engage the filter on top of
         * whatever its integrators were holding when it went out. */
        c.hp_l = c.hp_r = osynth::dsp::Svf{};
    }

    const int hum = (int)pv(NR_HUM);
    if (hum != c.c_hum) {
        c.c_hum = hum;
        /* New coefficients describe a different filter, so the state charged
         * at the old frequency is not this one's — carried across, it rings
         * out as a beat instead of settling into a null. */
        for (int h = 0; h < kNrHumHarmonics; ++h) {
            c.hum_l[h] = osynth::dsp::Svf{};
            c.hum_r[h] = osynth::dsp::Svf{};
        }
        if (hum > 0) {
"""

TARGETS = ["components/fx/fx.cpp", "tools/s39/nr_dsp.txt"]


def main() -> int:
    rc = 0
    for rel in TARGETS:
        p = ROOT / rel
        s = p.read_text(encoding="utf-8")
        if "held clean rather than merely unread" in s:
            print(f"  ok   {rel}: already applied")
            continue
        if s.count(OLD_HP) != 1:
            print(f"  FAIL {rel}: {s.count(OLD_HP)} matches")
            rc = 1
            continue
        p.write_text(s.replace(OLD_HP, NEW_HP), encoding="utf-8")
        print(f"  +    {rel}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
