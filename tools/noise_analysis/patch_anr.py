"""Applies the ANR fixes to components/fx/fx.cpp (S39c). Kept per the repo's
intermediary-artifacts policy so the edit is reproducible/reviewable."""
import io
p = 'components/fx/fx.cpp'
s = io.open(p, encoding='utf-8').read()

# --- reset paths: cur/prev -> buckets ---------------------------------------
old = """        for (int k = 0; k < kAnrBandsMax; ++k) {
            a.b[k].noise = 0.0f;
            a.b[k].cur = a.b[k].prev = a.b[k].raw = kAnrHuge;
            a.b[k].primed = false;
            a.b[k].settled = false;
        }"""
new = """        for (int k = 0; k < kAnrBandsMax; ++k) {
            a.b[k].noise = 0.0f;
            a.b[k].sm = 0.0f;
            for (int w = 0; w < kAnrSubWins; ++w) a.b[k].buck[w] = kAnrHuge;
            a.b[k].raw = kAnrHuge;
            a.b[k].primed = false;
            a.b[k].settled = false;
        }"""
assert s.count(old) == 1, 'reset path'
s = s.replace(old, new)

# --- window arithmetic: sub-windows + wall-clock creep ----------------------
old = """    const bool learn = pv(ANR_LEARN) >= 0.5f;
    const float win_s = learn ? (kAnrLearnMs * 0.001f) : pvm(ANR_ADAPT);
    uint32_t win = (uint32_t)(win_s / blk_s);
    if (win < 1) win = 1;"""
new = """    const bool learn = pv(ANR_LEARN) >= 0.5f;
    const float win_s = learn ? (kAnrLearnMs * 0.001f) : pvm(ANR_ADAPT);
    /* The window is held as kAnrSubWins buckets rather than two, so the first
     * profile lands one *bucket* after switch-on instead of one whole window.
     * The look-back is still `adapt`; only the granularity changed. */
    uint32_t win = (uint32_t)(win_s / (float)kAnrSubWins / blk_s);
    if (win < 1) win = 1;
    /* One-pole ahead of the minimum tracker. Without it the "minimum" is the
     * minimum of the *waveform's* wander inside 1.33 ms, which at the bottom
     * of the bank is 25 dB below the level it is supposed to be measuring. */
    const float k_est = 1.0f - expf(-blk_s / kAnrEstTauS);
    /* kAnrCreep is a rate and has to stay one: anchored to the bucket it would
     * speed up whenever `adapt` was shortened, and the promise in the note
     * above — a pad 40 dB up surviving the better part of a minute — is wall
     * clock. */
    const float creep = powf(kAnrCreep, (float)win * blk_s / kAnrCreepRefS);"""
assert s.count(old) == 1, 'window arithmetic'
s = s.replace(old, new)

# --- the sample loop: telescoping difference bank ---------------------------
old = """            const float xl = sl[i], xr = sr[i];
            float cl = 0.0f, cr = 0.0f;
            for (int k = 0; k < n; ++k) {
                AnrBand& b = a.b[k];
                const float yl = osynth::dsp::svf_next(b.l, b.c, b.mode, xl);
                const float yr = osynth::dsp::svf_next(b.r, b.c, b.mode, xr);
                b.acc += fabsf(yl) + fabsf(yr);
                cl += b.d * yl;
                cr += b.d * yr;
                b.d += b.dstep;
            }
            /* The band sum *is* the correction"""
new = """            const float xl = sl[i], xr = sr[i];
            float cl = 0.0f, cr = 0.0f;
            /* The running crossover output, which is also band k-1's upper
             * edge. Starting it at zero is what makes band 0 a plain
             * lowpass. */
            float pl = 0.0f, pr = 0.0f;
            for (int k = 0; k < n - 1; ++k) {
                AnrBand& b = a.b[k];
                const float ll = osynth::dsp::svf_next(
                    b.l, b.c, osynth::dsp::SvfMode::Lp, xl);
                const float lr = osynth::dsp::svf_next(
                    b.r, b.c, osynth::dsp::SvfMode::Lp, xr);
                const float yl = ll - pl;
                const float yr = lr - pr;
                pl = ll;
                pr = lr;
                b.acc += fabsf(yl) + fabsf(yr);
                cl += b.d * yl;
                cr += b.d * yr;
                b.d += b.dstep;
            }
            {
                /* Whatever the last crossover did not take. This is the term
                 * that closes the telescope: every band below has already
                 * cancelled against its neighbour, so what is left of the
                 * sum is exactly x. */
                AnrBand& b = a.b[n - 1];
                const float yl = xl - pl;
                const float yr = xr - pr;
                b.acc += fabsf(yl) + fabsf(yr);
                cl += b.d * yl;
                cr += b.d * yr;
                b.d += b.dstep;
            }
            /* The band sum *is* the correction"""
assert s.count(old) == 1, 'sample loop'
s = s.replace(old, new)

# --- the per-band estimator -------------------------------------------------
old = """            if (mag > kAnrEps) {
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
                b.raw = kAnrHuge;"""
new = """            if (mag > kAnrEps) {
                b.sm = mag;
                b.noise = mag * kAnrBias;
                for (int w = 0; w < kAnrSubWins; ++w) b.buck[w] = mag;
                b.raw = mag;
                b.primed = true;
            }
        } else {
            /* Everything the estimator looks at is the *smoothed* level; the
             * gain below still reads the raw one, because that is the half
             * that has to move at the speed of a syllable. */
            b.sm += k_est * (mag - b.sm);
            if (b.sm < b.raw) b.raw = b.sm;
            if (learn || b.sm < b.noise * kAnrSignalRatio + kAnrSeed) {
                if (b.sm < b.buck[0]) b.buck[0] = b.sm;
            }
            if (boundary) {
                float best = kAnrHuge;
                for (int w = 0; w < kAnrSubWins; ++w) {
                    if (b.buck[w] < best) best = b.buck[w];
                }
                if (best < kAnrHuge) {
                    b.noise = best * kAnrBias;
                } else {
                    /* A whole window with nothing plausible anywhere in it.
                     * Climb, but no faster than kAnrCreep — see the estimator
                     * note above. */
                    b.noise = fminf(b.raw * kAnrBias, b.noise * creep);
                }
                for (int w = kAnrSubWins - 1; w > 0; --w) {
                    b.buck[w] = b.buck[w - 1];
                }
                b.buck[0] = kAnrHuge;
                b.raw = kAnrHuge;"""
assert s.count(old) == 1, 'estimator'
s = s.replace(old, new)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('patched ok')
