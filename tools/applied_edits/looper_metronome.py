#!/usr/bin/env python3
"""Looper: an optional metronome that is never recorded (loop.click).

A free-running click on the beat, switched from the Looper page, default off.

Almost all of it already existed and none of it is new DSP:

  * drums_render_click() (drums.cpp) synthesises the click -- a decaying sine,
    not a sample, so it works before a kit is loaded -- and is mixed *after*
    the looper's record tap and past the FX bus. Its own comment says why: a
    count-in tick that overlapped the start of a take used to be printed into
    the loop. So "not recorded" is a property of where that mix happens, and
    this change inherits it rather than arranging it.
  * drums.click (0x0706, default 0.35) is the click level, already registered.
  * The looper already subscribes to the seq/arp beat callback for loop.sync
    and loop.countin, and that beat grid free-runs independently of the
    transport (seqarp.cpp: "The beat grid free-runs with the clock ... that is
    what lets the looper bar-lock while the sequencer is stopped").

So the tempo is the sequencer tempo by construction, not by a second setting
that could disagree with it: the clock period comes from seq.tempo, or from
the external MIDI clock when seq.clock says so. Changing the BPM anywhere --
the Sequencer page, the Arp page, MIDI clock -- moves the metronome with it.

What is added is the switch and one call:

  1. LOOP_PID_CLICK (0x0618, "loop.click", bool, default 0) -- the first free
     id after the loop.lvl1..8 block at 0x0610..0x0617.
  2. beat_cb() clicks when it is on, ahead of the arm countdown because the
     metronome is not part of the arm: it ticks whether a take is armed,
     recording, playing or stopped.
  3. A "Metronome" toggle on the Looper page beside Count-in, hidden on
     firmware that does not register the param.

Two interactions worth naming, both benign:

  * A count-in beat and a metronome beat coincide harmlessly. drums_click() is
    a single atomic that the audio task consumes once per block, so two arms
    on one beat are one tick, not two.
  * The count-in deliberately gives no click on the downbeat the take opens on
    (counting to five, and the tick landed at sample zero of the loop). That
    silence is not extended to the metronome: a timekeeper that skipped the
    beat at the top of the take would just be wrong, and the click cannot
    reach the take anyway.

Without PSRAM the whole looper is absent (looper_init logs "unavailable on
this target"), so the metronome is absent with it and the page never shows.

Idempotent: every replacement is anchored on text the edit removes, so a
second run is a no-op that reports "already applied".

Kept per the project's artifact policy; not part of the build.
"""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HDR = os.path.join(ROOT, "components", "looper", "include", "looper.h")
SRC = os.path.join(ROOT, "components", "looper", "looper.cpp")
QML = os.path.join(ROOT, "app_osyntho", "qml", "LooperScreen.qml")


# Matching is done on LF text so the anchors below can be written normally, and
# whatever ending the file already had is put back on the way out -- rewriting
# a whole file with the other ending would turn a small edit into a whole-file
# diff.
def read(path):
    with io.open(path, encoding="utf-8", newline="") as f:
        text = f.read()
    eol = "\r\n" if "\r\n" in text else "\n"
    return text.replace("\r\n", "\n"), eol


def write(path, text, eol):
    with io.open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text.replace("\n", eol) if eol != "\n" else text)


# `marker` exists because looper_rec_metronome.py, which runs after this, edits
# inside four of the blocks below -- so "is NEW still present verbatim" stops
# being the right question once it has run. A short distinctive substring of
# NEW answers "has this edit been made" without caring what was inserted into
# it afterwards.
def sub(text, old, new, label, marker=None):
    if (marker if marker is not None else new) in text:
        print("  = %s (already applied)" % label)
        return text
    if text.count(old) != 1:
        sys.exit("!! %s: anchor found %d times, expected 1" % (label, text.count(old)))
    print("  + %s" % label)
    return text.replace(old, new)


# ------------------------------------------------------------------ looper.h
h, h_eol = read(HDR)

h = sub(h, '''#define LOOP_PID_LEVEL(t) (0x0610 + (t)) /* loop.lvl1..8, t = 0..7           */
''', '''#define LOOP_PID_LEVEL(t) (0x0610 + (t)) /* loop.lvl1..8, t = 0..7           */

/* Metronome (S45). A click on every beat of the seq/arp clock, for playing
 * along to while recording — so it must not end up *in* the recording, and
 * does not: drums_render_click() mixes past the looper's record tap and past
 * the FX bus, which is the same placement loop.countin's clicks already rely
 * on. The level is drums.click, shared with the count-in.
 *
 * No tempo of its own. The beat grid it rides free-runs at seq.tempo (or the
 * external MIDI clock when seq.clock says so) whether or not the sequencer is
 * playing, so the metronome is the sequencer's tempo by construction rather
 * than a second setting that could drift out of agreement with it.
 *
 * Off by default: it is a monitoring aid you ask for, and a synth that
 * started ticking on its own at power-on would be a bug report. */
#define LOOP_PID_CLICK  0x0618 /* loop.click  bool, metronome on             */
''', "LOOP_PID_CLICK id")

write(HDR, h, h_eol)

# ---------------------------------------------------------------- looper.cpp
s, s_eol = read(SRC)

# 1. The parameter itself, after the last per-track level.
s = sub(s, '''    {LOOP_PID_LEVEL(7), "loop.lvl8", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0},
};
''', '''    {LOOP_PID_LEVEL(7), "loop.lvl8", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 1.0f, nullptr, 0},
    {LOOP_PID_CLICK, "loop.click", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* metronome; off — a synth that ticked
                                     * on its own at power-on would be a bug
                                     * report. The level is drums.click. */
};
''', "loop.click parameter",
         marker='{LOOP_PID_CLICK, "loop.click", ParamType::Bool')

# 2. The pointer the clock task reads it through.
s = sub(s, '''const std::atomic<float>* s_lvl[LOOP_TRACKS];
''', '''const std::atomic<float>* s_lvl[LOOP_TRACKS];
/* loop.click, read by beat_cb on the clock task. Null until register_params
 * binds it, which beat_cb tolerates as "no metronome" — the subscription is
 * live from looper_init and there is no ordering guarantee worth asserting
 * for a control that is off by default anyway. */
const std::atomic<float>* s_p_metro = nullptr;
''', "s_p_metro pointer")

# 3. The click itself, ahead of the arm countdown.
s = sub(s, '''void beat_cb(int beat_in_bar, void*) {
    /* Compare-exchange rather than a plain decrement: if loop_ctl zeroed the
''', '''void beat_cb(int beat_in_bar, void*) {
    /* The metronome (loop.click), ahead of everything else here because it is
     * not part of the arm: it ticks on every beat whether a take is armed,
     * recording, playing or stopped, and the early return below is only about
     * the countdown.
     *
     * It cannot reach a take. drums_render_click() mixes past the looper's
     * record tap and past the FX bus, which is exactly what the count-in
     * click below already depends on.
     *
     * Overlapping a count-in beat costs nothing: drums_click() is a single
     * atomic the audio task consumes once per block, so two arms on the same
     * beat are one tick. The count-in's deliberate silence on the downbeat
     * the take opens on is not extended here either — that silence exists so
     * four counts do not become five, while a timekeeper that dropped the
     * beat at the top of the take would simply be wrong. */
    if (s_p_metro != nullptr &&
        s_p_metro->load(std::memory_order_relaxed) >= 0.5f) {
        drums_click(beat_in_bar == 0);
    }

    /* Compare-exchange rather than a plain decrement: if loop_ctl zeroed the
''', "metronome click in beat_cb",
         marker="s_p_metro->load(std::memory_order_relaxed) >= 0.5f")

# 4. Bind the pointer where the level pointers are bound.
s = sub(s, '''    static const std::atomic<float> s_lvl_unity{1.0f};
''', '''    /* Not checked the way the levels below are: a null here is a metronome
     * that never ticks, which is the same thing the parameter says when it is
     * off. There is no silent-wrong-behaviour case to guard against. */
    s_p_metro = ps.valuePtr(LOOP_PID_CLICK);

    static const std::atomic<float> s_lvl_unity{1.0f};
''', "bind s_p_metro",
         marker="s_p_metro = ps.valuePtr(LOOP_PID_CLICK);")

write(SRC, s, s_eol)

# ----------------------------------------------------------- LooperScreen.qml
q, q_eol = read(QML)

q = sub(q, '''    property int idArmed: -1
''', '''    property int idArmed: -1
    // Metronome (S45): a click on every beat of the sequencer clock, mixed
    // past the record tap so it never lands in a take.
    property int idClick: -1
    property bool clickOn: false
''', "idClick / clickOn properties")

q = sub(q, '''        idArmed = Synth.paramIdForName("loop.armed")
''', '''        idArmed = Synth.paramIdForName("loop.armed")
        idClick = Synth.paramIdForName("loop.click")
        if (idClick >= 0 && Synth.paramValueKnown(idClick))
            clickOn = Synth.paramValue(idClick) > 0.5
''', "resolve loop.click in rebind")

q = sub(q, '''            else if (id === root.idCountIn) root.countInOn = value > 0.5
''', '''            else if (id === root.idCountIn) root.countInOn = value > 0.5
            else if (id === root.idClick) root.clickOn = value > 0.5
''', "follow loop.click in paramChanged")

q = sub(q, '''                        SyncedButton {
                            text: Tr.t("Count-in")
                            visible: root.idCountIn >= 0
                            modelChecked: root.countInOn
                            onToggled: Synth.setParam(root.idCountIn, checked ? 1 : 0)
                            ToolTip.visible: hovered
                            ToolTip.text: Tr.t("Four clicked beats before "
                                              + "recording begins.")
                        }
''', '''                        SyncedButton {
                            text: Tr.t("Count-in")
                            visible: root.idCountIn >= 0
                            modelChecked: root.countInOn
                            onToggled: Synth.setParam(root.idCountIn, checked ? 1 : 0)
                            ToolTip.visible: hovered
                            ToolTip.text: Tr.t("Four clicked beats before "
                                              + "recording begins.")
                        }
                        // Beside the count-in because it is the same click at
                        // the same tempo, just running continuously — and it
                        // is what you want *while* a take is going down, which
                        // is where the eye already is.
                        SyncedButton {
                            text: Tr.t("Metronome")
                            visible: root.idClick >= 0
                            modelChecked: root.clickOn
                            onToggled: Synth.setParam(root.idClick, checked ? 1 : 0)
                            ToolTip.visible: hovered
                            ToolTip.text: Tr.t("A click on every beat at the "
                                              + "sequencer tempo. Never "
                                              + "recorded into a take.")
                        }
''', "Metronome toggle")

# The row is gated on the two alignment params; without this a firmware that
# registers loop.click but neither of those would hide the metronome with them.
q = sub(q, '''                        visible: root.idSync >= 0 || root.idCountIn >= 0
''', '''                        visible: root.idSync >= 0 || root.idCountIn >= 0
                                 || root.idClick >= 0
''', "row visibility includes loop.click",
         marker="|| root.idClick >= 0")

write(QML, q, q_eol)

print("done")
