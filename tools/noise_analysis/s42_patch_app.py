"""S42 — mic noise reduction: the app half (panel, learn button, translations)."""
import io
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

# ------------------------------------------------------------ FxScreen.qml --
q = ROOT / 'app_osyntho/qml/FxScreen.qml'
s = q.read_text(encoding='utf-8')

old = """            // Both cards carry a `src` selector (S39b). It is drawn beside the
            // bypass rather than among the knobs because it is not a setting
            // of the effect, it is what the effect is pointed at: `input`
            // cleans the microphone and leaves the instrument alone, and needs
            // in.route set to fx — on the Input page — to have anything to
            // work on.
            ParamGroup { title: "Adaptive NR"; prefix: "fx.anr" }"""
new = """            // All three cards carry a `src` selector (S39b, S42). It is drawn
            // beside the bypass rather than among the knobs because it is not
            // a setting of the effect, it is what the effect is pointed at:
            // `input` cleans the microphone and leaves the instrument alone,
            // and needs in.route set to fx — on the Input page — to have
            // anything to work on.
            //
            // Mic NR (S42) leads, matching the firmware. It is the one to
            // reach for when the noise is still audible *under* the voice:
            // the other two only clean the gaps between words. Running more
            // than one of the three at `src` = input double-corrects, so they
            // are alternatives rather than a stack.
            ParamGroup { title: "Mic NR"; prefix: "fx.mnr" }
            HoldSampleCard {
                title: "Mic noise profile"
                paramName: "fx.mnr.learn"
                downValue: 1
                upValue: 0
                idleText: "Hold to learn"
                downText: "Learning…"
                activeText: "Learning"
                hint: "Hold during a silent moment; the room is sampled."
            }
            ParamGroup { title: "Adaptive NR"; prefix: "fx.anr" }"""
assert s.count(old) == 1, 'qml panel'
s = s.replace(old, new)

old = """// The panels are in signal-chain order — adaptive NR -> NR -> vocoder ->"""
new = """// The panels are in signal-chain order — mic NR -> adaptive NR -> NR -> vocoder ->"""
if s.count(old) == 1:
    s = s.replace(old, new)
q.write_text(s, encoding='utf-8', newline='')
print('FxScreen.qml patched')

# ---------------------------------------------------------- translator.cpp --
t = ROOT / 'app_osyntho/src/translator.cpp'
u = t.read_text(encoding='utf-8')
old = '  pt["Adaptive NR"] = "Redução adaptativa";'
new = ('  pt["Mic NR"] = "Redução de ruído do mic";\n'
       '  pt["Mic noise profile"] = "Perfil de ruído do mic";\n'
       '  pt["Adaptive NR"] = "Redução adaptativa";')
assert u.count(old) == 1, 'translator'
u = u.replace(old, new)
t.write_text(u, encoding='utf-8', newline='')
print('translator.cpp patched')
