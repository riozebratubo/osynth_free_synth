import QtQuick
import QtQuick.Controls.Material

import org.osynth.osyntho

// Master FX bus. Engine-independent (registered at boot), so these persist
// across engine switches.
//
// The input that feeds the bus is the Input page next door, not a card here:
// the same `in.` group was drawn on this page and on the old osynth page at
// once, so one control appeared twice with nothing to say the two were one.
//
// The panels are in signal-chain order — adaptive NR -> NR -> vocoder ->
// drive -> chorus -> flanger -> phaser -> delay -> granular -> reverb ->
// bitcrush -> filter -> EQ -> compressor -> stereo — so the page reads the
// way the audio flows. The two LFO panels come last because they are not a
// stage of the chain; they modulate the panels above them.
Item {
    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: panels.implicitHeight + 24
        clip: true

        PanelFlow {
            id: panels

            // Every card here self-hides on firmware that does not register
            // its prefix — so one app build still drives a pre-S33 or pre-S34
            // synth.
            //
            // Noise reduction (S39) is the head of the firmware's chain and
            // so the head of the page. Hold-to-learn writes the same id the
            // switch beside it does, inverted against the vocoder's freeze
            // below: held means *doing* something here, where held means
            // "not freezing" there.
            //
            // Both cards carry a `src` selector (S39b). It is drawn beside the
            // bypass rather than among the knobs because it is not a setting
            // of the effect, it is what the effect is pointed at: `input`
            // cleans the microphone and leaves the instrument alone, and needs
            // in.route set to fx — on the Input page — to have anything to
            // work on.
            ParamGroup { title: "Adaptive NR"; prefix: "fx.anr" }
            HoldSampleCard {
                title: "Noise profile"
                paramName: "fx.anr.learn"
                downValue: 1
                upValue: 0
                idleText: "Hold to learn"
                downText: "Learning…"
                activeText: "Learning"
                hint: "Hold during a silent moment; the room is sampled."
            }
            ParamGroup { title: "Noise reduction"; prefix: "fx.nr" }
            ParamGroup { title: "Vocoder"; prefix: "fx.voc" }
            HoldSampleCard {
                title: "Vocoder capture"
                paramName: "fx.voc.freeze"
                hint: "Hold and speak; the vowel is held on release."
            }
            ParamGroup { title: "Drive"; prefix: "fx.drv" }
            ParamGroup { title: "Chorus"; prefix: "fx.cho" }
            ParamGroup { title: "Flanger"; prefix: "fx.flg" }
            ParamGroup { title: "Phaser"; prefix: "fx.phs" }
            ParamGroup { title: "Delay"; prefix: "fx.dly" }
            ParamGroup { title: "Granular delay"; prefix: "fx.grn" }
            ParamGroup { title: "Reverb"; prefix: "fx.rev" }
            ParamGroup { title: "Bitcrush"; prefix: "fx.crush" }
            // Master filter (S33).
            ParamGroup { title: "Filter"; prefix: "fx.flt" }
            ParamGroup { title: "EQ"; prefix: "fx.eq" }
            ParamGroup { title: "Compressor"; prefix: "fx.comp" }
            ParamGroup { title: "Stereo & output"; prefix: "fx.st" }
            ParamGroup { title: "FX LFO 1"; prefix: "fx.lfo1" }
            ParamGroup { title: "FX LFO 2"; prefix: "fx.lfo2" }
        }
    }

    Label {
        anchors.centerIn: parent
        visible: !Synth.ready
        text: Synth.connected ? Tr.t("Discovering parameters…") : Tr.t("Not connected")
        opacity: 0.5
        color: Material.foreground
    }
}
