#!/usr/bin/env python3
"""Sequencer step inspector: a note Length measured in whole steps.

The firmware has always stored a per-step gate -- seq_step_t.gate, counted in
1/16 of a step, 1..255 so just under sixteen steps -- and seq_play.cpp holds
the note for it. The app exposed that byte raw: a "Gate" spinner over 1..255
with an "(1.00x)" suffix, which nobody read as "how long the note is", so the
feature was there and unreachable.

It is now "Length ... steps": a plain integer spinner over 1..16 whole steps,
the same unit as the grid's squares and as the track's own Length. The 1/16
encoding is converted at this one call site and nowhere else.

Whole steps, not fractions. The first cut of this made StepField able to show
a scaled value ("2.50 steps") so the sub-step gates the raw byte can express
stayed reachable, and snapped spinner presses back onto the step grid to stop
the bottom of the range (a clamped gate of 1/16 = 0.06 steps) from offsetting
every press after it. The snap turned out to be unreliable -- it keys off
SpinBox's up/down `pressed`, which is not set on every path that emits
valueModified -- so the field still walked 0.06, 1.06, 2.06. Integer steps
have no such bottom edge, and no caller has yet wanted a fraction of one;
if one does, the fix belongs in the conversion here, not in StepField.

The 16-step end maps to 255 rather than 256, which the byte cannot hold. That
is what the firmware's own comment calls "a tie across the bar": a hair under
sixteen steps, so the note-off lands just before the next bar rather than on
top of it. Math.round() carries it back to 16 on the way in, so the value
round-trips.

Idempotent: the replacement is anchored on text the edit removes, so a second
run is a no-op that reports "already applied".

Kept per the project's artifact policy; not part of the build.
"""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCREEN = os.path.join(ROOT, "app_osyntho", "qml", "SequencerScreen.qml")


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


def sub(text, old, new, label):
    if new in text:
        print("  = %s (already applied)" % label)
        return text
    if text.count(old) != 1:
        sys.exit("!! %s: anchor found %d times, expected 1" % (label, text.count(old)))
    print("  + %s" % label)
    return text.replace(old, new)


OLD = '''                    StepField {
                        label: Tr.t("Gate")
                        from: 1; to: 255
                        // 16 = exactly one step; show it in step-lengths.
                        suffix: "(" + ((root.sel.gate !== undefined ? root.sel.gate : 16) / 16.0).toFixed(2) + "x)"
                        value: root.sel.gate !== undefined ? root.sel.gate : 16
                        onEdited: (v) => Synth.setStepField(root.selectedStep, "gate", v)
                    }
'''

NEW = '''                    // How long the note is held, counted in steps — the same
                    // unit as the grid's squares and as the track's own
                    // Length, which is what you are thinking in when you want
                    // a note to ring on over the next square. The firmware
                    // stores the gate in 1/16 of a step; that encoding is
                    // converted here and is not visible anywhere else.
                    //
                    // Whole steps only. The byte can express a fraction of a
                    // step (a staccato gate), but a spinner that walks in
                    // sixteenths of a square reads as broken next to a grid
                    // whose unit is the square — and nothing has asked for
                    // one yet. If something does, it belongs in this
                    // conversion, not in StepField.
                    //
                    // 16 steps maps to 255, not 256, which the byte cannot
                    // hold: the firmware calls that "a tie across the bar",
                    // landing the note-off a hair before the next bar instead
                    // of on top of it. Math.round carries 255 back to 16, so
                    // the value round-trips. A gate that is not a multiple of
                    // 16 — written by an older build, or by the firmware
                    // itself — shows as the nearest whole step and is left
                    // alone until the field is actually edited.
                    //
                    // A long note only rings over the steps that follow it
                    // while they are empty: a lane is monophonic, so the next
                    // trig on the same track cuts it short.
                    StepField {
                        label: Tr.t("Length")
                        from: 1; to: 16
                        suffix: Tr.t("steps")
                        value: Math.max(1, Math.round(
                                   (root.sel.gate !== undefined ? root.sel.gate : 16) / 16))
                        onEdited: (v) => Synth.setStepField(root.selectedStep, "gate",
                                                            Math.min(255, v * 16))
                    }
'''

s, s_eol = read(SCREEN)
s = sub(s, OLD, NEW, "step Length field, in whole steps")
write(SCREEN, s, s_eol)

print("done")
