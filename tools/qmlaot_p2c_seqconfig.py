#!/usr/bin/env python3
"""Phase 2c: TrackConfig / PatternConfig value types.

trackConfig() and patternConfig() returned QVariantMap, which made `cfg.length`
and `pat.swing` untyped hash lookups -- 25 AOT fallbacks across
SequencerScreen.qml and TrackSheet.qml, the two worst files left after the
Material ones (which cannot be fixed from here).

They return the gadgets in src/seqtypes.h now, and the eighteen
`root.cfg.X !== undefined ? root.cfg.X : <default>` guards go with them. Those
guards were not defensive programming -- they existed because the property
started as an empty `({})` literal, so every field really was undefined until
the first reload(). A default-constructed TrackConfig already carries the value
each guard was substituting, which is why seqtypes.h mirrors SeqTrackCfg's
defaults exactly rather than zero-initialising.

One behaviour change, deliberate and worth knowing: the Swing slider in
TrackSheet reads

    value: root.cfg.followsPatternSwing ? 0 : root.cfg.swing

A default TrackConfig has swing == 0xFF, so followsPatternSwing is true and the
slider shows 0. The old empty-object default made followsPatternSwing undefined
(falsy), so it showed 50. This is visible only before the first reload() lands,
and "follows the pattern" is the honest reading of "no data yet".

Idempotent. --check reports without writing.
"""
import argparse
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "app_osyntho")
QML = os.path.join(APP, "qml")

ARGS = None

CPP_H = [
    ('#include "src/paramtypes.h"',
     '#include "src/paramtypes.h"\n#include "src/seqtypes.h"'),
    ("  Q_INVOKABLE QVariantMap trackConfig() const;",
     "  Q_INVOKABLE TrackConfig trackConfig() const;"),
    ("  Q_INVOKABLE QVariantMap patternConfig() const;",
     "  Q_INVOKABLE PatternConfig patternConfig() const;"),
]

CPP_IMPL = [
    ("QVariantMap SynthController::trackConfig() const {\n  QVariantMap m;\n"
     '  m["target"] = m_trackCfg.target;\n'
     '  m["slot"] = m_trackCfg.slot;\n'
     '  m["length"] = m_trackCfg.length;\n'
     '  m["div"] = m_trackCfg.div;\n'
     '  m["dir"] = m_trackCfg.dir;\n'
     '  m["transpose"] = m_trackCfg.transpose;\n'
     '  m["swing"] = m_trackCfg.swing;\n'
     '  m["gateScale"] = m_trackCfg.gateScale;\n'
     '  m["velScale"] = m_trackCfg.velScale;\n'
     '  m["probScale"] = m_trackCfg.probScale;\n'
     '  m["humanize"] = m_trackCfg.humanize;\n'
     '  m["scale"] = m_trackCfg.scale;\n'
     '  m["root"] = m_trackCfg.root;\n'
     '  m["followsPatternSwing"] = m_trackCfg.swing == 0xFF;\n'
     '  m["followsPatternScale"] = m_trackCfg.scale == 0xFF;\n'
     '  m["noteToSlot"] = m_trackCfg.slot == SEQ_SLOT_FROM_NOTE;\n'
     "  return m;\n}",
     "TrackConfig SynthController::trackConfig() const {\n"
     "  TrackConfig c;\n"
     "  c.target = m_trackCfg.target;\n"
     "  c.slot = m_trackCfg.slot;\n"
     "  c.length = m_trackCfg.length;\n"
     "  c.div = m_trackCfg.div;\n"
     "  c.dir = m_trackCfg.dir;\n"
     "  c.transpose = m_trackCfg.transpose;\n"
     "  c.swing = m_trackCfg.swing;\n"
     "  c.gateScale = m_trackCfg.gateScale;\n"
     "  c.velScale = m_trackCfg.velScale;\n"
     "  c.probScale = m_trackCfg.probScale;\n"
     "  c.humanize = m_trackCfg.humanize;\n"
     "  c.scale = m_trackCfg.scale;\n"
     "  c.root = m_trackCfg.root;\n"
     "  c.followsPatternSwing = m_trackCfg.swing == 0xFF;\n"
     "  c.followsPatternScale = m_trackCfg.scale == 0xFF;\n"
     "  c.noteToSlot = m_trackCfg.slot == SEQ_SLOT_FROM_NOTE;\n"
     "  return c;\n}"),
    ("QVariantMap SynthController::patternConfig() const {\n  QVariantMap m;\n"
     '  m["length"] = m_patternLength;\n'
     '  m["scale"] = m_patternScale;\n'
     '  m["root"] = m_patternRoot;\n'
     '  m["swing"] = m_patternSwing;\n'
     '  m["name"] = m_patternName;\n'
     "  return m;\n}",
     "PatternConfig SynthController::patternConfig() const {\n"
     "  PatternConfig p;\n"
     "  p.length = m_patternLength;\n"
     "  p.scale = m_patternScale;\n"
     "  p.root = m_patternRoot;\n"
     "  p.swing = m_patternSwing;\n"
     "  p.name = m_patternName;\n"
     "  return p;\n}"),
]

QML_DECLS = {
    "SequencerScreen.qml": [("    property var cfg: ({})", "    property trackConfig cfg")],
    "TrackSheet.qml": [("    property var cfg: ({})", "    property trackConfig cfg"),
                       ("    property var pat: ({})", "    property patternConfig pat")],
}

# Every `X !== undefined ? X : default` guard collapses to X, because the gadget
# default already IS that default (see seqtypes.h).
GUARD = re.compile(
    r"(root\.)?(cfg|pat)\.(\w+) !== undefined \? (?:root\.)?(?:cfg|pat)\.\w+ : [^\s)]+")


def apply(path, pairs, label):
    text = io.open(path, encoding="utf-8").read()
    before = text
    applied = 0
    for old, new in pairs:
        if len(new) > len(old) and new in text:
            continue
        if text.count(old) == 1:
            text = text.replace(old, new, 1)
            applied += 1
        elif new in text:
            continue
        else:
            sys.exit("{}: could not match\n---\n{}\n---".format(label, old[:200]))
    if text != before:
        print("  {:<26} {} edits".format(label, applied))
        if not ARGS.check:
            io.open(path, "w", encoding="utf-8", newline="").write(text)
        return 1
    return 0


def strip_guards(name):
    path = os.path.join(QML, name)
    text = io.open(path, encoding="utf-8").read()
    new, n = GUARD.subn(lambda m: "{}{}.{}".format(m.group(1) or "", m.group(2), m.group(3)), text)
    if n:
        print("  {:<26} {} undefined-guards removed".format(name, n))
        if not ARGS.check:
            io.open(path, "w", encoding="utf-8", newline="").write(new)
    return n


def main():
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    ARGS = parser.parse_args()

    apply(os.path.join(APP, "src", "synthcontroller.h"), CPP_H, "src/synthcontroller.h")
    apply(os.path.join(APP, "src", "synthcontroller.cpp"), CPP_IMPL, "src/synthcontroller.cpp")
    for name, pairs in sorted(QML_DECLS.items()):
        apply(os.path.join(QML, name), pairs, name)
    for name in sorted(QML_DECLS):
        strip_guards(name)

    cmake = os.path.join(APP, "CMakeLists.txt")
    text = io.open(cmake, encoding="utf-8").read()
    if "src/seqtypes.h" not in text:
        anchor = "    SOURCES src/paramtypes.h\n"
        assert text.count(anchor) == 1
        text = text.replace(anchor, anchor + "    SOURCES src/seqtypes.h\n", 1)
        print("  CMakeLists.txt             seqtypes.h added")
        if not ARGS.check:
            io.open(cmake, "w", encoding="utf-8", newline="").write(text)
    print("(dry run)" if ARGS.check else "done")


if __name__ == "__main__":
    main()
