"""S42 — mic noise reduction: the fx.cpp half.

enum + kParams entries (which is also the app's panel order), the DSP block
itself, the chain call and the two init calls.
"""
import io
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
p = ROOT / 'components/fx/fx.cpp'
s = p.read_text(encoding='utf-8')
dsp = (Path(__file__).resolve().parent / 's42_mnr_dsp.txt').read_text(encoding='utf-8')

# ---------------------------------------------------------------- include --
old = '#include "fx_reverb_wet.h"'
if old in s:
    s = s.replace(old, '#include "fx_fft.h"\n' + old, 1)
else:
    raise SystemExit('include anchor missing')

# ------------------------------------------------------------------- enum --
old = """enum PIdx {
    ANR_ON, ANR_SRC, ANR_AMOUNT, ANR_FLOOR, ANR_BANDS, ANR_LOW, ANR_HIGH,"""
new = """enum PIdx {
    MNR_ON, MNR_SRC, MNR_AMOUNT, MNR_FLOOR, MNR_ADAPT, MNR_LEARN,
    ANR_ON, ANR_SRC, ANR_AMOUNT, ANR_FLOOR, ANR_BANDS, ANR_LOW, ANR_HIGH,"""
assert s.count(old) == 1, 'enum'
s = s.replace(old, new)

# ---------------------------------------------------------------- kParams --
old = """const ParamDesc kParams[P_COUNT] = {
    {FX_PID_ANR_ON, "fx.anr.on", ParamType::Bool, ParamCurve::Linear,"""
new = """const ParamDesc kParams[P_COUNT] = {
    /* ---- mic noise reduction (S42) ---- */
    {FX_PID_MNR_ON, "fx.mnr.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_MNR_SRC, "fx.mnr.src", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kNrSrcCount - 1), 1.0f /* input */, kNrSrcs, kNrSrcCount},
    {FX_PID_MNR_AMOUNT, "fx.mnr.amount", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.6f, nullptr, 0}, /* 1 = subtract 3x the estimated floor */
    {FX_PID_MNR_FLOOR, "fx.mnr.floor", ParamType::Float, ParamCurve::Linear,
     -48.0f, 0.0f, -24.0f, nullptr, 0}, /* dB, the deepest a bin may be cut */
    {FX_PID_MNR_ADAPT, "fx.mnr.adapt", ParamType::Float, ParamCurve::Exp,
     0.2f, 20.0f, 1.5f, nullptr, 0},    /* s — the minimum-tracking window */
    {FX_PID_MNR_LEARN, "fx.mnr.learn", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},

    {FX_PID_ANR_ON, "fx.anr.on", ParamType::Bool, ParamCurve::Linear,"""
assert s.count(old) == 1, 'kParams'
s = s.replace(old, new)

# -------------------------------------------------------------- the unit ---
# After nr_process(), so s_nr_src_l/r and unit_gate are already in scope.
anchor = """/* ---- vocoder (S38): the input's spectrum imposed on the synth bus ----"""
assert s.count(anchor) == 1, 'dsp anchor'
s = s.replace(anchor, dsp + '\n' + anchor)

# ------------------------------------------------------------- the chain ---
old = """    anr_process(l, r, frames);
    nr_process(l, r, frames);"""
new = """    /* Mic NR first: it is the one that exists to hand the rest of the bus a
     * clean input. Note that running more than one of the three with
     * `src` = input does not cascade — each computes its correction against
     * the *same* untouched input block and adds it, so two of them remove the
     * input's contribution twice. One at a time is the intended use. */
    mnr_process(l, r, frames);
    anr_process(l, r, frames);
    nr_process(l, r, frames);"""
assert s.count(old) == 1, 'chain'
s = s.replace(old, new)

# --------------------------------------------------------------- fx_init ---
old = """    s_cho.ok = line_alloc(s_cho.l, kChoLen) && line_alloc(s_cho.r, kChoLen);"""
new = """    /* Twiddles and the analysis window, both once (S42). Neither can fail and
     * neither allocates — the buffers behind them are static — so this needs
     * none of the `ok` flags the delay lines below carry. */
    osynth::fx::fft_init();
    mnr_build();

    s_cho.ok = line_alloc(s_cho.l, kChoLen) && line_alloc(s_cho.r, kChoLen);"""
assert s.count(old) == 1, 'fx_init'
s = s.replace(old, new)

p.write_text(s, encoding='utf-8', newline='')
print('fx.cpp patched')

# --------------------------------------------------------- CMakeLists.txt --
cm = ROOT / 'components/fx/CMakeLists.txt'
t = cm.read_text(encoding='utf-8')
old = '    SRCS "fx.cpp" "fx_reverb_wet.cpp"'
new = '    SRCS "fx.cpp" "fx_reverb_wet.cpp" "fx_fft.cpp"'
assert t.count(old) == 1, 'cmake'
cm.write_text(t.replace(old, new), encoding='utf-8', newline='')
print('CMakeLists.txt patched')
