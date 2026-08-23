import QtQuick
import QtQuick.Controls.Material

import org.osynth.osyntho

// What is feeding the synth from outside: the input trio (in.route / in.gain /
// in.pga) and, on a build with a microphone, in.source and in.micgain as well
// — the source picks line, mic or both, and the micgain is the mic's own trim
// so "both" is usable.
//
// A page of its own because the same `in.` group used to be drawn twice, on
// the FX page and on the old osynth page. They were one set of controls with
// two titles ("Line in" and "Audio input"), and nothing on either page said
// so: moving a knob on one silently moved it on the other.
//
// These are *persisted* firmware parameters — the ones main.cpp lists in
// kPersisted. They describe the source plugged into the box rather than the
// sound, which is why presets deliberately skip them and why they sit apart
// from the FX chain they feed.
Item {
    id: screen

    // Whether this build has an input at all. paramIdsByPrefix() is a plain
    // call with no change signal, so it is re-asked when discovery says the
    // map moved rather than bound — the same shape ParamGroup uses for its own
    // id list, and for the same reason.
    property bool hasInput: false

    function refresh() { hasInput = Synth.paramIdsByPrefix("in.").length > 0 }
    Component.onCompleted: refresh()

    Connections {
        target: Synth
        function onParamsDiscovered() { screen.refresh() }
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: panels.implicitHeight + 24
        clip: true

        PanelFlow {
            id: panels
            // Titled for the general case: the firmware decides how many
            // sockets there are and registers only what it has, so the card
            // renders whatever came back and self-hides whole on a build with
            // no input.
            ParamGroup { title: "Audio input"; prefix: "in." }
        }
    }

    Label {
        anchors.centerIn: parent
        visible: !Synth.connected || !Synth.ready || !screen.hasInput
        // Three different silences, and the difference matters: a build
        // genuinely without an input looks exactly like a failed discovery
        // unless the page says which one it is.
        text: !Synth.connected ? Tr.t("Not connected")
            : !Synth.ready ? Tr.t("Discovering parameters…")
            : Tr.t("This synth has no audio input.")
        opacity: 0.5
        color: Material.foreground
    }
}
