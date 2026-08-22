#!/usr/bin/env python3
"""S39 follow-up: per-band priming for the adaptive NR's estimator.

The first draft primed every band's estimate on the unit's first block, from
whatever magnitude was there. Switching `fx.anr.on` while the bus is *digitally*
silent — nothing playing, no input routed yet, which is exactly when someone
sets this up — primed every estimate to 0, and 0 is not a floor:

    offer test   mag < 0 * 4 + seed   rejects every real signal that follows
    escape       noise * kCreep       is 0 * 2, so the estimate never climbs

leaving the unit permanently transparent. Benign, silent, and wrong.

The fix is to prime per band and only from something: a band with nothing in it
stays unprimed and does nothing, and an estimate that closes a window at or
below the seed is thrown away rather than kept as a floor. Applied to both the
source and the tools/s39 copy the patch script inserts.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

OLD_FLAGS = """    uint32_t win_cnt = 0;
    bool primed = false;  /* the estimates hold something */
    bool settled = false; /* ...and a full window has closed over them */
"""
NEW_FLAGS = """    uint32_t win_cnt = 0;
"""

OLD_BANDFLAGS = """    float d = 0.0f;        /* (g - 1), ramped across the block */
    float dstep = 0.0f;
};
"""
NEW_BANDFLAGS = """    float d = 0.0f;        /* (g - 1), ramped across the block */
    float dstep = 0.0f;
    /* Per band, not per unit. A band with nothing in it has nothing to
     * estimate from, and priming it anyway from a silent bus is what used to
     * wedge the whole unit — see the estimator note above. */
    bool primed = false;  /* the estimate holds something */
    bool settled = false; /* ...and a window has closed over it */
};
"""

OLD_RESET = """        for (int k = 0; k < kAnrBandsMax; ++k) {
            a.b[k].noise = 0.0f;
            a.b[k].cur = a.b[k].prev = a.b[k].raw = kAnrHuge;
        }
        a.primed = false;
        a.settled = false;
        a.win_cnt = 0;
"""
NEW_RESET = """        for (int k = 0; k < kAnrBandsMax; ++k) {
            a.b[k].noise = 0.0f;
            a.b[k].cur = a.b[k].prev = a.b[k].raw = kAnrHuge;
            a.b[k].primed = false;
            a.b[k].settled = false;
        }
        a.win_cnt = 0;
"""

OLD_OFF = """            for (int k = 0; k < kAnrBandsMax; ++k) a.b[k] = AnrBand{};
            a.c_bands = -1;
            a.win_cnt = 0;
            a.primed = false;
            a.settled = false;
"""
NEW_OFF = """            for (int k = 0; k < kAnrBandsMax; ++k) a.b[k] = AnrBand{};
            a.c_bands = -1;
            a.win_cnt = 0;
"""

OLD_EPS = "constexpr float kAnrEps = 1e-9f;"
NEW_EPS = ("constexpr float kAnrEps = 1e-7f;      /* under this a band is "
           "empty, not quiet */")

OLD_BODY = """        if (!a.primed) {
            /* Seeded from whatever is there, loud or not: the offer test
             * below needs a non-zero estimate to be a test at all, and the
             * first window closing replaces this with a real minimum. */
            b.noise = mag * kAnrBias;
            b.cur = b.prev = b.raw = mag;
        } else {
            if (mag < b.raw) b.raw = mag;
            if (learn || mag < b.noise * kAnrSignalRatio + kAnrSeed) {
                if (mag < b.cur) b.cur = mag;
            }
        }

        if (boundary && a.primed) {
            if (b.cur < kAnrHuge || b.prev < kAnrHuge) {
                b.noise = fminf(b.cur, b.prev) * kAnrBias;
            } else {
                /* Two windows with nothing plausible in either. Climb, but by
                 * no more than kAnrCreep — see the estimator note above. */
                b.noise = fminf(b.raw * kAnrBias, b.noise * kAnrCreep);
            }
            b.prev = b.cur;
            b.cur = kAnrHuge;
            b.raw = kAnrHuge;
        }
"""

NEW_BODY = """        if (!b.primed) {
            /* Seeded from whatever is there, loud or not — the offer test
             * below needs a non-zero estimate to be a test at all, and the
             * first window to close replaces this with a real minimum. But
             * only from *something*: an empty band is not a quiet one, and
             * an estimate of zero rejects every signal that follows it. */
            if (mag > kAnrEps) {
                b.noise = mag * kAnrBias;
                b.cur = b.prev = b.raw = mag;
                b.primed = true;
            }
        } else {
            if (mag < b.raw) b.raw = mag;
            if (learn || mag < b.noise * kAnrSignalRatio + kAnrSeed) {
                if (mag < b.cur) b.cur = mag;
            }
            if (boundary) {
                if (b.cur < kAnrHuge || b.prev < kAnrHuge) {
                    b.noise = fminf(b.cur, b.prev) * kAnrBias;
                } else {
                    /* Two windows with nothing plausible in either. Climb,
                     * but by no more than kAnrCreep — see the estimator note
                     * above. */
                    b.noise = fminf(b.raw * kAnrBias, b.noise * kAnrCreep);
                }
                b.prev = b.cur;
                b.cur = kAnrHuge;
                b.raw = kAnrHuge;
                /* A window that closed on silence did not measure a floor,
                 * it measured the absence of one. Throwing it away and
                 * re-priming is the difference between an input that arrives
                 * late being cleaned up and being ignored forever. */
                if (b.noise <= kAnrEps) {
                    b.primed = false;
                    b.settled = false;
                } else {
                    b.settled = true;
                }
            }
        }
"""

OLD_GAIN = """        if (a.settled && mag > kAnrEps) {"""
NEW_GAIN = """        if (b.settled && mag > kAnrEps) {"""

OLD_TAIL = """        b.dstep = (b.g - 1.0f - b.d) / (float)frames;
    }
    a.primed = true;
    if (boundary) a.settled = true;
}
"""
NEW_TAIL = """        b.dstep = (b.g - 1.0f - b.d) / (float)frames;
    }
}
"""

OLD_DOC = """ * Nothing is subtracted until the first window has closed. The unit listens
 * for `adapt` seconds after being switched on and only then starts working,
 * which is the difference between switching it on mid-sentence and having the
 * sentence disappear."""
NEW_DOC = """ * Nothing is subtracted from a band until a window has closed over it, and a
 * band is not primed at all until something turns up in it. The unit listens
 * for `adapt` seconds after being switched on and only then starts working,
 * which is the difference between switching it on mid-sentence and having the
 * sentence disappear; the per-band half of that is what makes switching it on
 * over a *silent* bus — the usual case, since that is when anyone sets this up
 * — behave the same way instead of priming every estimate to zero and then
 * rejecting every real signal for being too far above it."""

SUBS = [OLD_FLAGS, OLD_BANDFLAGS, OLD_RESET, OLD_OFF, OLD_EPS, OLD_BODY,
        OLD_GAIN, OLD_TAIL, OLD_DOC]
REPL = [NEW_FLAGS, NEW_BANDFLAGS, NEW_RESET, NEW_OFF, NEW_EPS, NEW_BODY,
        NEW_GAIN, NEW_TAIL, NEW_DOC]

TARGETS = ["components/fx/fx.cpp", "tools/s39/anr_dsp.txt"]


def main() -> int:
    rc = 0
    for rel in TARGETS:
        p = ROOT / rel
        s = p.read_text(encoding="utf-8")
        if "bool primed = false;  /* the estimate holds something */" in s:
            print(f"  ok   {rel}: already applied")
            continue
        for old, new in zip(SUBS, REPL):
            if s.count(old) != 1:
                print(f"  FAIL {rel}: {s.count(old)} matches for {old[:44]!r}")
                rc = 1
                break
            s = s.replace(old, new)
        else:
            p.write_text(s, encoding="utf-8")
            print(f"  +    {rel}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
