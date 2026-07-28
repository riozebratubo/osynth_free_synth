import QtQuick
import QtQuick.Controls.Material

import org.osynth.osyntho

// Oscillators / tone shaping for the active engine (the 0x02xx range minus the
// filter/env/LFO groups, which live on their own pages). Empty groups hide, so
// this one page adapts across subtractive / additive / FM / wavetable.
Item {
    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: panels.implicitHeight + 24
        clip: true

        PanelFlow {
            id: panels

            ParamGroup { title: "Oscillator 1"; prefix: "osc1" }
            ParamGroup { title: "Oscillator 2"; prefix: "osc2" }
            // No "Noise" card: no parameter is named noise.* on any engine —
            // the noise level is mix.noise, which the Mixer card below already
            // shows. The card was permanently empty, and an empty ParamGroup
            // hides itself, so nothing ever pointed it out.
            ParamGroup { title: "Mixer"; prefix: "mix"; capBit: 16 }
            ParamGroup { title: "Partials / Spectrum"; prefix: "add" }
            ParamGroup { title: "Velocity"; prefix: "vel." }
            ParamGroup { title: "Operator A"; prefix: "fm.a" }
            ParamGroup { title: "Operator B"; prefix: "fm.b" }
            ParamGroup { title: "FM velocity"; prefix: "fm.vel" }
            ParamGroup { title: "Wavetable 1"; prefix: "wt1" }
            ParamGroup { title: "Wavetable 2"; prefix: "wt2" }
            ParamGroup { title: "Table motion"; prefix: "env.pos" }
            ParamGroup { title: "Brightness env"; prefix: "env.bright" }
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
