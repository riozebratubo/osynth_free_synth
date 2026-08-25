"""Default changes for the two noise-reduction units (S39c)."""
import io
p = 'components/fx/fx.cpp'
s = io.open(p, encoding='utf-8').read()

edits = [
    # ANR source: the unit exists to clean a microphone, so point it at one.
    ("""    {FX_PID_ANR_SRC, "fx.anr.src", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kNrSrcCount - 1), 0.0f /* bus */, kNrSrcs, kNrSrcCount},""",
     """    {FX_PID_ANR_SRC, "fx.anr.src", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kNrSrcCount - 1), 1.0f /* input */, kNrSrcs, kNrSrcCount},"""),
    # ANR window.
    ("""    {FX_PID_ANR_ADAPT, "fx.anr.adapt", ParamType::Float, ParamCurve::Exp,
     0.5f, 60.0f, 8.0f, nullptr, 0},   /* s — how fast the floor may rise */""",
     """    {FX_PID_ANR_ADAPT, "fx.anr.adapt", ParamType::Float, ParamCurve::Exp,
     0.5f, 60.0f, 3.0f, nullptr, 0},   /* s — how fast the floor may rise */"""),
    # NR source.
    ("""    {FX_PID_NR_SRC, "fx.nr.src", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kNrSrcCount - 1), 0.0f /* bus */, kNrSrcs, kNrSrcCount},""",
     """    {FX_PID_NR_SRC, "fx.nr.src", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kNrSrcCount - 1), 1.0f /* input */, kNrSrcs, kNrSrcCount},"""),
    # NR threshold: see the note above FX_PID_NR_THRESH in fx.h.
    ("""    {FX_PID_NR_THRESH, "fx.nr.thresh", ParamType::Float, ParamCurve::Linear,
     -80.0f, 0.0f, -45.0f, nullptr, 0}, /* dB, peak */""",
     """    {FX_PID_NR_THRESH, "fx.nr.thresh", ParamType::Float, ParamCurve::Linear,
     -80.0f, 0.0f, -24.0f, nullptr, 0}, /* dB, peak — see nr_process() */"""),
]
for old, new in edits:
    assert s.count(old) == 1, old[:60]
    s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('defaults patched')
