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
            // Granular (S38). Two cards because the two halves are not always
            // both live: buf.* only does anything at grn.src = in, and giving
            // it its own panel is the honest way to say which controls the
            // current source ignores.
            ParamGroup { title: "Grain cloud"; prefix: "grn." }
            ParamGroup { title: "Capture buffer"; prefix: "buf" }
            // Alongside the buf.freeze switch that group already draws, not
            // instead of it: same parameter, one gesture instead of three.
            HoldSampleCard {
                title: "Buffer capture"
                paramName: "buf.freeze"
                hint: "Hold to record into the ring; frozen on release."
            }
            ParamGroup { title: "Formant env"; prefix: "env.form" }
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
