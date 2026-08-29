#!/usr/bin/env python3
"""Sequencer track selector: the mute buttons move under the track numbers.

Mute was already there -- a red "M" SyncedButton wired to the firmware's
`trkN.mute` parameter -- but it sat *beside* each track number inside a Row, so
eight tracks read as sixteen interleaved buttons ("1 M 2 M 3 M ...") and the
mute state could not be scanned as a row. Asked for as its own row underneath
the track switches.

The delegate becomes a Column instead of a Row: one vertical pair per track,
so the mutes line up under the numbers they belong to and the pair wraps as a
unit when the Flow runs out of width. Two separate Repeaters in two Flows
would have come apart on the first wrap, and on any width difference between
a number and an M.

Both buttons take the wider of the two implicit widths so the column is square
-- `width` reads `implicitWidth`, which is content-driven and never reads
`width` back, so there is no loop.

It also fixes why the M never appeared in the first place. `pidMute` was a
binding onto paramIdForName(), which has no change signal, so it answered once
when the delegate was built and kept that answer. Delegates are built the
moment SEQ_INFO lands, which finishDiscovery() asks for -- including on the
path where the discovery budget expires with metadata still missing
(kDiscoveryBudgetMs, synthcontroller.cpp). trk*.mute sits at the top of the id
space (0x0430+), so it is among the last the metadata pump reaches and the
first to be left unresolved: the name answered -1, `visible` latched false, and
the top-up that filled it in seconds later changed nothing. It is now resolved
imperatively and re-asked on paramsDiscovered, which every newly known
PARAM_INFO schedules. The long-press-to-mute shortcut on the track number was
dead for the same reason and comes back with it.

Nothing about what mute *does* changes: seq_play.cpp:430 gates the whole of
fire_step's trigger branch on track_audible(), so a muted lane emits no
note-ons and no drum hits. It was never a volume mute -- every lane feeds one
engine and one voice bus, so there is no per-track audio path to attenuate.

Idempotent: the replacement is anchored on text the edit removes, so a second
run is a no-op that reports "already applied".

Kept per the project's artifact policy; not part of the build.
"""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCREEN = os.path.join(ROOT, "app_osyntho", "qml", "SequencerScreen.qml")


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


OLD = '''                delegate: Row {
                    id: trackRow
                    required property int index
                    spacing: 2

                    // Safe as bindings, unlike the page-level ids above: a
                    // delegate is only created once its model has content, and
                    // both models (Synth.kitSlots, Synth.seqTracks) are filled
                    // from responses requested at finishDiscovery() — i.e.
                    // after every parameter's metadata is in. Any later model
                    // change recreates the delegates and re-resolves these.
                    readonly property int pidMute: Synth.paramIdForName("trk" + (index + 1) + ".mute")
                    ParamValue { id: muteVal; paramId: trackRow.pidMute }
                    readonly property bool muted: muteVal.on

                    Button {
                        text: trackRow.index + 1
                        highlighted: Synth.editTrack === trackRow.index
                        // Dimmed rather than marked: the M beside it now says
                        // *which* state this is, so a second glyph in here only
                        // competed with it for a very small button.
                        opacity: trackRow.muted ? 0.45 : 1.0
                        padding: 8
                        onClicked: Synth.editTrack = trackRow.index
                        // The long-press survives the button beside it: it is
                        // the fastest gesture on a phone, where the M is a
                        // small target, and it costs nothing to keep.
                        // >= 0, matching the button beside it: paramIdForName
                        // answers -1 for "no such name", and 0 is a real id.
                        onPressAndHold: if (trackRow.pidMute >= 0)
                                            Synth.setParam(trackRow.pidMute,
                                                           trackRow.muted ? 0 : 1)
                    }

                    // Mute, in the open. It was a long-press and nothing else
                    // before — undiscoverable, and impossible to read back at a
                    // glance across eight tracks while playing.
                    //
                    // Synced, not plain-checkable: the firmware owns this
                    // parameter and moves it on its own (a loaded sequence or
                    // set republishes every track's mute from the pattern), so
                    // a `checked` the button assigned itself would go stale.
                    // See SyncedButton.qml.
                    SyncedButton {
                        text: "M"
                        visible: trackRow.pidMute >= 0
                        padding: 8
                        font.bold: true
                        // Red rather than the accent: mute is the one state
                        // here that means "you are not hearing this", and it
                        // has to read as different from the blue selection
                        // highlight on the button it sits against.
                        Material.accent: "#FF5252"
                        // Filled when muted. The default checked Button is a
                        // very quiet change, which is not enough for a state
                        // you need to read across eight of these mid-take.
                        highlighted: checked
                        modelChecked: trackRow.muted
                        onToggled: Synth.setParam(trackRow.pidMute, checked ? 1 : 0)
                    }
                }
'''

NEW = '''                // A Column, so each track is a number with its mute directly
                // underneath: the mutes form their own row under the track
                // switches and can be read across in one glance, which is the
                // whole point of a mute row. Interleaved in a Row — "1 M 2 M
                // 3 M" — eight tracks read as sixteen unrelated buttons.
                //
                // One Column per track rather than two Repeaters in two Flows,
                // because the pair has to stay together: two independent rows
                // come apart at the first wrap on a narrow phone, and at any
                // width difference between a number and an M.
                delegate: Column {
                    id: trackCell
                    required property int index
                    spacing: 2

                    // Resolved imperatively, like the page-level ids above
                    // and for the same reason: paramIdForName() has no change
                    // signal, so as a binding it answers once, when the
                    // delegate is created, and keeps that answer forever.
                    //
                    // It used to be a binding, on the reasoning that delegates
                    // only exist once Synth.seqTracks has content — i.e. after
                    // SEQ_INFO, which finishDiscovery() asks for, by which
                    // point every parameter's metadata is in. That last step
                    // does not hold: finishDiscovery() is also reached when
                    // the discovery budget expires with ids still unresolved
                    // (kDiscoveryBudgetMs, synthcontroller.cpp), and
                    // trk*.mute sits at the top of the id space (0x0430+), so
                    // it is among the last the metadata pump reaches and the
                    // first to be left out. The name then resolved to -1, the
                    // M button's `visible` latched false, and the top-up that
                    // filled the metadata in seconds later changed nothing —
                    // mute was unreachable for the rest of the session, from
                    // the long-press too, since it shares this guard.
                    //
                    // paramsDiscovered is the signal to re-ask: every newly
                    // known PARAM_INFO schedules one, top-ups included.
                    property int pidMute: -1
                    function refreshPid(): void {
                        pidMute = Synth.paramIdForName(
                            "trk" + (trackCell.index + 1) + ".mute")
                    }
                    Component.onCompleted: refreshPid()
                    // Non-visual, so the Column does not position it.
                    property Connections _pidConn: Connections {
                        target: Synth
                        function onParamsDiscovered() { trackCell.refreshPid() }
                    }

                    ParamValue { id: muteVal; paramId: trackCell.pidMute }
                    readonly property bool muted: muteVal.on

                    // Both buttons take the wider of the two, so the stack is
                    // square and every mute sits exactly under its number.
                    // Reading implicitWidth to set width is not a loop: a
                    // Button's implicit size comes from its label, which never
                    // reads the width back.
                    readonly property real cellWidth:
                        Math.max(trackBtn.implicitWidth, muteBtn.implicitWidth)

                    Button {
                        id: trackBtn
                        width: trackCell.cellWidth
                        text: trackCell.index + 1
                        highlighted: Synth.editTrack === trackCell.index
                        // Dimmed rather than marked: the M under it now says
                        // *which* state this is, so a second glyph in here only
                        // competed with it for a very small button.
                        opacity: trackCell.muted ? 0.45 : 1.0
                        padding: 8
                        onClicked: Synth.editTrack = trackCell.index
                        // The long-press survives the button under it: it is
                        // the fastest gesture on a phone, where the M is a
                        // small target, and it costs nothing to keep.
                        // >= 0, matching the button under it: paramIdForName
                        // answers -1 for "no such name", and 0 is a real id.
                        onPressAndHold: if (trackCell.pidMute >= 0)
                                            Synth.setParam(trackCell.pidMute,
                                                           trackCell.muted ? 0 : 1)
                    }

                    // Mute. Not a volume control and never was: the firmware
                    // gates the whole trigger branch on this parameter
                    // (seq_play.cpp, track_audible in fire_step), so a muted
                    // lane generates no note-ons and no drum hits at all —
                    // there is no per-track audio path to attenuate, since
                    // every lane feeds one engine and one voice bus.
                    //
                    // Synced, not plain-checkable: the firmware owns this
                    // parameter and moves it on its own (a loaded sequence or
                    // set republishes every track's mute from the pattern), so
                    // a `checked` the button assigned itself would go stale.
                    // See SyncedButton.qml.
                    SyncedButton {
                        id: muteBtn
                        width: trackCell.cellWidth
                        text: "M"
                        visible: trackCell.pidMute >= 0
                        padding: 8
                        font.bold: true
                        // Red rather than the accent: mute is the one state
                        // here that means "this lane is not playing", and it
                        // has to read as different from the blue selection
                        // highlight on the button it sits under.
                        Material.accent: "#FF5252"
                        // Filled when muted. The default checked Button is a
                        // very quiet change, which is not enough for a state
                        // you need to read across eight of these mid-take.
                        highlighted: checked
                        modelChecked: trackCell.muted
                        onToggled: Synth.setParam(trackCell.pidMute, checked ? 1 : 0)
                    }
                }
'''

s, s_eol = read(SCREEN)
s = sub(s, OLD, NEW, "mute row under the track switches")
write(SCREEN, s, s_eol)

print("done")
