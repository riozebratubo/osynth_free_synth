#!/usr/bin/env python3
"""Static sanity pass over the files touched by the S31e/S31f/S23c fixes.

Not a compiler. It checks the things a quick eyeball misses and a build would
only tell you about after several minutes of ESP-IDF:

  * braces/parens/brackets balance per file (ignoring strings and comments)
  * every symbol introduced by these changes is both declared and used
  * enum/table pairs that must stay the same length actually do

Kept in tools/ rather than run once and deleted: the same checks are what you
want after the next edit to any of these tables.

Usage:  python tools/check_edits.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def strip_code(src, qml=False):
    """Remove comments and string/char literals so bracket counting is honest."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        two = src[i:i + 2]
        if two == "/*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if two == "//":
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if c in "\"'":
            q = c
            i += 1
            while i < n:
                if src[i] == "\\":
                    i += 2
                    continue
                if src[i] == q:
                    i += 1
                    break
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def balance(rel, qml=False):
    code = strip_code(read(rel), qml)
    pairs = {")": "(", "]": "[", "}": "{"}
    stack = []
    for ch in code:
        if ch in "([{":
            stack.append(ch)
        elif ch in pairs:
            if not stack or stack[-1] != pairs[ch]:
                return "unbalanced '%s'" % ch
            stack.pop()
    return None if not stack else "unclosed '%s'" % stack[-1]


FILES = [
    "components/audio_io/audio_io.cpp",
    "components/audio_io/include/audio_io.h",
    "components/ble_ctrl/ble_ctrl.cpp",
    "components/engines/engine_additive.cpp",
    "components/codec/codec_es8388.cpp",
    "components/codec/include/codec.h",
    "components/graph/graph_model.cpp",
    "components/graph/graph_render.cpp",
    "components/graph/include/graph_model.h",
    "components/graph/include/graph_render.h",
    "components/midi/midi.c",
    "components/midi/include/midi.h",
    "components/presets/presets.cpp",
    "components/seqarp/seq_model.cpp",
    "components/seqarp/seq_play.cpp",
    "components/seqarp/seqarp.cpp",
    "components/seqarp/include/seq_model.h",
    "components/seqarp/include/seq_play.h",
    "components/seqarp/include/seqarp.h",
    "main/main.cpp",
    "app_osyntho/src/synthcontroller.cpp",
    "app_osyntho/src/synthcontroller.h",
    "app_osyntho/qml/ParamControl.qml",
    "app_osyntho/qml/Main.qml",
]

# symbol -> (file that must declare it, files that must reference it)
SYMBOLS = {
    "codec_early_mute": ("components/codec/include/codec.h",
                         ["components/codec/codec_es8388.cpp", "main/main.cpp"]),
    "audio_io_line_in_block": ("components/audio_io/include/audio_io.h",
                               ["components/audio_io/audio_io.cpp",
                                "components/graph/graph_render.cpp"]),
    "seq_model_revision": ("components/seqarp/include/seq_model.h",
                           ["components/seqarp/seq_model.cpp",
                            "components/seqarp/seqarp.cpp"]),
    "seq_play_record_drum": ("components/seqarp/include/seq_play.h",
                             ["components/seqarp/seq_play.cpp",
                              "components/seqarp/seqarp.cpp"]),
    "seqarp_record_drum": ("components/seqarp/include/seqarp.h",
                           ["components/seqarp/seqarp.cpp",
                            "components/ble_ctrl/ble_ctrl.cpp"]),
    "midi_set_drum_tap": ("components/midi/include/midi.h",
                          ["components/midi/midi.c",
                           "components/seqarp/seqarp.cpp"]),
    "SEQ_PID_REV": ("components/seqarp/include/seqarp.h",
                    ["components/seqarp/seqarp.cpp",
                     "components/presets/presets.cpp"]),
    "kRenderRows": ("components/graph/include/graph_render.h",
                    ["components/graph/graph_render.cpp"]),
    "voice_manager_active_voices": ("components/synth_core/include/synth_voice.h",
                                    ["components/engines/engine_additive.cpp"]),
    "s_len_prev": ("components/seqarp/seqarp.cpp",
                   ["components/seqarp/seqarp.cpp"]),
    "requestInfoTopUp": ("app_osyntho/src/synthcontroller.h",
                         ["app_osyntho/src/synthcontroller.cpp"]),
    "syncGraphParamInfo": ("app_osyntho/src/synthcontroller.h",
                           ["app_osyntho/src/synthcontroller.cpp"]),
    "forgetNodeParamInfo": ("app_osyntho/src/synthcontroller.h",
                            ["app_osyntho/src/synthcontroller.cpp"]),
}

fails = []

for rel in FILES:
    path = os.path.join(ROOT, rel)
    if not os.path.exists(path):
        fails.append("missing file: %s" % rel)
        continue
    err = balance(rel, qml=rel.endswith(".qml"))
    if err:
        fails.append("%s: %s" % (rel, err))

for sym, (decl, uses) in SYMBOLS.items():
    if sym not in read(decl):
        fails.append("%s not declared in %s" % (sym, decl))
    for u in uses:
        if sym not in read(u):
            fails.append("%s not referenced in %s" % (sym, u))

# --- table/enum length agreement -----------------------------------------
gm = read("components/graph/graph_model.cpp")
gh = read("components/graph/include/graph_model.h")

kinds = re.search(r"const KindDesc kKinds\[\(int\)Kind::Count\] = \{(.*?)\n\};",
                  gm, re.S)
if not kinds:
    fails.append("could not find kKinds table")
else:
    rows = re.findall(r'^\s*\{"', kinds.group(1), re.M)
    enum_body = re.search(r"enum class Kind : uint8_t \{(.*?)\n\};", gh, re.S)
    names = []
    for line in enum_body.group(1).splitlines():
        line = re.sub(r"/\*.*?\*/", "", line).strip()
        for part in line.split(","):
            part = part.strip()
            if part and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", part):
                names.append(part)
    names = [x for x in names if x != "Count"]
    if len(rows) != len(names):
        fails.append("kKinds has %d rows, Kind enum has %d values %s"
                     % (len(rows), len(names), names))

# LineIn param spec count must equal pidx::LIN_N
spec = re.search(r"const ParamSpec kPLineIn\[\] = \{(.*?)\n\};", gm, re.S)
lin_n = re.search(r"enum LineP \{(.*?)\};", gh, re.S)
if spec and lin_n:
    got = len(re.findall(r"^\s*\{\"", spec.group(1), re.M))
    want = len([x for x in lin_n.group(1).replace("\n", " ").split(",")
                if x.strip() and not x.strip().startswith("LIN_N")])
    if got != want:
        fails.append("kPLineIn has %d specs, LineP declares %d" % (got, want))

# LineMode values must match the kLineModes name list
modes = re.search(r"const char\* const kLineModes\[\] = \{(.*?)\};", gm, re.S)
if modes:
    got = len(re.findall(r'"', modes.group(1))) // 2
    if got != 3:
        fails.append("kLineModes has %d names, LineMode has 3" % got)

# --- every Q_INVOKABLE in app.h has a definition -------------------------
#
# The class of bug a name check cannot see. moc generates a call into every
# Q_INVOKABLE whether or not a body exists, so a declaration with no definition
# builds cleanly and fails at *link* time — and only for the target being
# linked, which on this project is the Android one, long after the desktop
# build looked fine. Structural rather than by name, so it covers the next one
# too.
app_h = read("app_osyntho/src/app.h")
app_cpp = read("app_osyntho/src/app.cpp")
for m in re.finditer(r"^\s*Q_INVOKABLE\s+(.+?)\b(\w+)\s*\(", app_h, re.M):
    decl_line = m.group(0)
    name = m.group(2)
    # Defined inline in the header? Its declaration runs on to a brace.
    tail = app_h[m.end():m.end() + 400]
    if "{" in tail.split(";")[0]:
        continue
    if ("App::" + name) not in app_cpp:
        fails.append("Q_INVOKABLE %s declared in app.h but not defined in "
                     "app.cpp (moc will call it -> link error)"
                     % (name + "()"))

if fails:
    print("FAIL")
    for f in fails:
        print("  -", f)
    sys.exit(1)
print("OK — %d files balanced, %d symbols wired, tables agree"
      % (len(FILES), len(SYMBOLS)))
