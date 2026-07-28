import QtQuick
import QtQuick.Controls.Material

import org.osynth.osyntho

// Filter + amplitude/mod envelopes. Filter gated on the FILTER cap, env2 on the
// ENV2 cap. (FM's per-operator envelopes live on the Tone page.)
Item {
    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: panels.implicitHeight + 24
        clip: true

        PanelFlow {
            id: panels

            ParamGroup { title: "Filter"; prefix: "flt"; capBit: 1 }

            // full width: each curve keeps its own line, above its knobs
            AdsrEnvelope { prefix: "env1"; width: panels.contentWidth }
            ParamGroup { title: "Envelope 1 (amp)"; prefix: "env1" }

            AdsrEnvelope { prefix: "env2"; width: panels.contentWidth }
            ParamGroup { title: "Envelope 2 (mod)"; prefix: "env2"; capBit: 2 }
        }
    }

    Label {
        anchors.centerIn: parent
        visible: !Synth.ready
        text: Synth.connected ? t.t("Discovering parameters…") : t.t("Not connected")
        opacity: 0.5
        color: Material.foreground
    }
}
