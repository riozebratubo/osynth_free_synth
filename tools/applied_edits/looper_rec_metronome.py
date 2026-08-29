#!/usr/bin/env python3
"""Looper: a second, record-only metronome (loop.recclick).

loop.click (looper_metronome.py) is the free-running one: it ticks on every
beat of the seq/arp clock whatever the transport is doing. It is unchanged by
this, button and behaviour both.

Added beside it: loop.recclick, the same click but only while the looper is in
record. Also off by default.

"Only while recording" is read as the published loop.mode being rec, not as a
take being open, and that choice is the whole design:

  * The count-in and the sync wait are covered for free. Pressing rec writes
    loop.mode = rec immediately; the arm only delays the *take* (ctl_handle_mode
    moves the shadow to rec before any take exists -- see ctl_arm_cancel's
    comment on putting it back). So the record metronome is already ticking
    through the four counts, and it continues into the take rather than
    starting after it. That is what "synced with the count-in" means here.
  * The beat the take opens on clicks. loop.countin alone deliberately leaves
    that one silent -- four counts must not become five, and the tick landed
    at sample zero of the loop -- but for a metronome that goes on running
    through the take it is beat 1, and dropping it would put a hole at the top
    of every take. The suppression in the countdown branch is left exactly as
    it was; this simply ticks before reaching it.
  * A punch-in waiting for the loop to wrap is covered. The rec button is lit,
    no take is open yet, and a click is precisely what is wanted. An internal
    "is a take open" flag (s_rec_trk_live) would have gone quiet there.

It stops on its own: the firmware drives loop.mode back to play when a punch-in
completes, and stop/play clear it any other time.

Both metronomes on at once is one tick, not two -- drums_click() is a single
atomic the audio task consumes once per block -- so the pair compose rather
than beat against each other. Neither can reach a take: drums_render_click()
mixes past the looper's record tap and past the FX bus.

Tempo is the sequencer's, as before: the beat grid free-runs at seq.tempo, or
at the external MIDI clock when seq.clock says so.

  1. LOOP_PID_RECCLICK (0x0619, "loop.recclick", bool, default 0).
  2. beat_cb() gains the second condition, and two bound pointers -- the new
     parameter and loop.mode, which the clock task has no other safe way to
     read (s_ctl_mode belongs to loop_ctl).
  3. A "Rec Metronome" toggle on the Looper page beside Metronome, hidden on
     firmware that does not register the param.

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


def read(path):
    with io.open(path, encoding="utf-8", newline="") as f:
        text = f.read()
    eol = "\r\n" if "\r\n" in text else "\n"
    return text.replace("\r\n", "\n"), eol


def write(path, text, eol):
    with io.open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text.replace("\n", eol) if eol != "\n" else text)


def sub(text, old, new, label):
    if new in text:
        print("  = %s (already applied)" % label)
        return text
    if text.count(old) != 1:
        sys.exit("!! %s: anchor found %d times, expected 1" % (label, text.count(old)))
    print("  + %s" % label)
    return text.replace(old, new)


# ------------------------------------------------------------------ looper.h
h, h_eol = read(HDR)

h = sub(h, '''#define LOOP_PID_CLICK  0x0618 /* loop.click  bool, metronome on             */
''', '''#define LOOP_PID_CLICK  0x0618 /* loop.click  bool, metronome on             */

/* The same click, but only while the looper is in record — and "in record"
 * means loop.mode, not an open take, which is what makes this continue the
 * count-in instead of starting after it. Pressing rec sets the mode at once;
 * loop.sync and loop.countin only delay the take. So the four counts, the
 * beat the take opens on (which loop.countin alone leaves silent) and the
 * take itself are one unbroken tick, and a punch-in waiting for the loop to
 * wrap is ticked through too.
 *
 * Independent of loop.click rather than a mode of it: wanting a click only
 * while tracking and wanting one always are different habits, and a player
 * who has both on hears one click, not two — drums_click() is a single atomic
 * the audio task consumes once per block. */
#define LOOP_PID_RECCLICK 0x0619 /* loop.recclick bool, metronome while rec  */
''', "LOOP_PID_RECCLICK id")

write(HDR, h, h_eol)

# ---------------------------------------------------------------- looper.cpp
s, s_eol = read(SRC)

s = sub(s, '''    {LOOP_PID_CLICK, "loop.click", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* metronome; off — a synth that ticked
                                     * on its own at power-on would be a bug
                                     * report. The level is drums.click. */
};
''', '''    {LOOP_PID_CLICK, "loop.click", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* metronome; off — a synth that ticked
                                     * on its own at power-on would be a bug
                                     * report. The level is drums.click. */
    {LOOP_PID_RECCLICK, "loop.recclick", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0}, /* metronome while recording; same level,
                                     * same reason to default off */
};
''', "loop.recclick parameter")

s = sub(s, '''const std::atomic<float>* s_p_metro = nullptr;
''', '''const std::atomic<float>* s_p_metro = nullptr;
/* loop.recclick, and the transport it is conditioned on. loop.mode is read
 * from the store rather than through s_ctl_mode because that shadow belongs to
 * loop_ctl and this runs on the clock task; the store is where the two agree. */
const std::atomic<float>* s_p_metro_rec = nullptr;
const std::atomic<float>* s_p_mode = nullptr;
''', "s_p_metro_rec / s_p_mode pointers")

s = sub(s, '''    if (s_p_metro != nullptr &&
        s_p_metro->load(std::memory_order_relaxed) >= 0.5f) {
        drums_click(beat_in_bar == 0);
    }
''', '''    bool tick = s_p_metro != nullptr &&
                s_p_metro->load(std::memory_order_relaxed) >= 0.5f;

    /* The record metronome (loop.recclick): the same click, gated on the
     * looper being in record.
     *
     * On loop.mode and not on an open take, deliberately. Pressing rec sets
     * the mode immediately and the arm only delays the take, so this is
     * already ticking through the count-in and carries straight on into the
     * recording — which is the point of it, and why the beat the take opens
     * on is *not* silent here the way loop.countin alone leaves it. It also
     * keeps ticking through a punch-in waiting for the loop to wrap, where
     * no take is open yet but the rec button is lit.
     *
     * Both metronomes on is one tick, not two: drums_click() is a single
     * atomic the audio task consumes once per block. `tick` short-circuits
     * anyway, so the common case does not even load the second pair. */
    if (!tick && s_p_metro_rec != nullptr && s_p_mode != nullptr &&
        s_p_metro_rec->load(std::memory_order_relaxed) >= 0.5f) {
        const int mode =
            (int)(s_p_mode->load(std::memory_order_relaxed) + 0.5f);
        tick = mode == MODE_REC;
    }

    if (tick) drums_click(beat_in_bar == 0);
''', "record metronome in beat_cb")

# The countdown branch's silence is still right for the count-in alone; say so.
s = sub(s, '''        /* Deliberately no click on this beat. It is the downbeat the take
         * opens on, and the four counts have already been given on the four
         * beats before it — clicking here would be counting to five, and it
         * put the last tick at sample zero of the loop. */
''', '''        /* Deliberately no click on this beat. It is the downbeat the take
         * opens on, and the four counts have already been given on the four
         * beats before it — clicking here would be counting to five, and it
         * put the last tick at sample zero of the loop.
         *
         * Only the count-in's own click is meant: either metronome above has
         * already ticked this beat, and should have. For one that goes on
         * running through the take this is beat 1, and skipping it would put
         * a hole at the top of every take. */
''', "count-in silence comment")

s = sub(s, '''    s_p_metro = ps.valuePtr(LOOP_PID_CLICK);
''', '''    s_p_metro = ps.valuePtr(LOOP_PID_CLICK);
    s_p_metro_rec = ps.valuePtr(LOOP_PID_RECCLICK);
    s_p_mode = ps.valuePtr(LOOP_PID_MODE);
''', "bind s_p_metro_rec / s_p_mode")

write(SRC, s, s_eol)

# ----------------------------------------------------------- LooperScreen.qml
q, q_eol = read(QML)

q = sub(q, '''    property int idClick: -1
    property bool clickOn: false
''', '''    property int idClick: -1
    property bool clickOn: false
    // The same click, but only while the looper is in record — so it runs on
    // from the count-in into the take instead of starting after it.
    property int idRecClick: -1
    property bool recClickOn: false
''', "idRecClick / recClickOn properties")

q = sub(q, '''        idClick = Synth.paramIdForName("loop.click")
        if (idClick >= 0 && Synth.paramValueKnown(idClick))
            clickOn = Synth.paramValue(idClick) > 0.5
''', '''        idClick = Synth.paramIdForName("loop.click")
        if (idClick >= 0 && Synth.paramValueKnown(idClick))
            clickOn = Synth.paramValue(idClick) > 0.5
        idRecClick = Synth.paramIdForName("loop.recclick")
        if (idRecClick >= 0 && Synth.paramValueKnown(idRecClick))
            recClickOn = Synth.paramValue(idRecClick) > 0.5
''', "resolve loop.recclick in rebind")

q = sub(q, '''            else if (id === root.idClick) root.clickOn = value > 0.5
''', '''            else if (id === root.idClick) root.clickOn = value > 0.5
            else if (id === root.idRecClick) root.recClickOn = value > 0.5
''', "follow loop.recclick in paramChanged")

q = sub(q, '''                            ToolTip.text: Tr.t("A click on every beat at the "
                                              + "sequencer tempo. Never "
                                              + "recorded into a take.")
                        }
''', '''                            ToolTip.text: Tr.t("A click on every beat at the "
                                              + "sequencer tempo. Never "
                                              + "recorded into a take.")
                        }
                        // Next to the free-running one because they are the
                        // same click and the choice between them is one
                        // decision: always, or only while tracking.
                        SyncedButton {
                            text: Tr.t("Rec Metronome")
                            visible: root.idRecClick >= 0
                            modelChecked: root.recClickOn
                            onToggled: Synth.setParam(root.idRecClick, checked ? 1 : 0)
                            ToolTip.visible: hovered
                            ToolTip.text: Tr.t("The same click, but only while "
                                              + "recording — it runs on from "
                                              + "the count-in into the take.")
                        }
''', "Rec Metronome toggle")

q = sub(q, '''                        visible: root.idSync >= 0 || root.idCountIn >= 0
                                 || root.idClick >= 0
''', '''                        visible: root.idSync >= 0 || root.idCountIn >= 0
                                 || root.idClick >= 0 || root.idRecClick >= 0
''', "row visibility includes loop.recclick")

write(QML, q, q_eol)

print("done")
