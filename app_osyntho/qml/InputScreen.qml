import QtQuick
import QtQuick.Controls.Material

import org.osynth.osyntho

// What is feeding the synth from outside: the input trio (in.route / in.gain /
// in.pga), plus in.source on a build with two capture devices — it picks line,
// mic or both.
//
// in.micgain appears on more builds than in.source does, and the difference is
// worth knowing when reading this page. It is the trim applied *at the
// capture*, ahead of every reader; in.gain is the monitor trim and scales only
// the three positions in.route selects between. The two consumers that read the
// capture directly — the vocoder's modulator and the granular engine's ring —
// see in.micgain and never in.gain, so on the standalone build, whose one
// device is whatever the OS calls its default input, this is the only control
// that can lift a quiet microphone before the vocoder's gate gets to it.
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
