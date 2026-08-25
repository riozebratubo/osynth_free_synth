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
// The panels are in signal-chain order — mic NR -> adaptive NR -> NR -> vocoder ->
// drive -> chorus -> flanger -> phaser -> delay -> granular -> reverb ->
// bitcrush -> filter -> EQ -> compressor -> stereo -> limiter — so the page
// reads the way the audio flows. The two LFO panels come last because they are
// not a stage of the chain; they modulate the panels above them.
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
            // All three cards carry a `src` selector (S39b, S42). It is drawn
            // beside the bypass rather than among the knobs because it is not
            // a setting of the effect, it is what the effect is pointed at:
            // `input` cleans the microphone and leaves the instrument alone,
            // and needs in.route set to fx — on the Input page — to have
            // anything to work on.
            //
            // Mic NR (S42) leads, matching the firmware. It is the one to
            // reach for when the noise is still audible *under* the voice:
            // the other two only clean the gaps between words. Running more
            // than one of the three at `src` = input double-corrects, so they
            // are alternatives rather than a stack.
            ParamGroup { title: "Mic NR"; prefix: "fx.mnr" }
            HoldSampleCard {
                title: "Mic noise profile"
                paramName: "fx.mnr.learn"
                downValue: 1
                upValue: 0
                idleText: "Hold to learn"
                downText: "Learning…"
                activeText: "Learning"
                hint: "Hold during a silent moment; the room is sampled."
            }
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
            // S43: this records audio now, not a spectral frame. Held, the
            // mic is live and being recorded; released, the recording becomes
            // the modulator and every note-on replays it from the start.
            HoldSampleCard {
                title: "Vocoder capture"
                paramName: "fx.voc.freeze"
                idleText: "Hold to record"
                downText: "Recording…"
                activeText: "Live"
                hint: "Hold and speak a phrase; each note then says it back."
            }
            // Directly under the knobs it writes, so pressing a button and
            // watching the card above it move is the same glance. It is a
            // full-width card and the two above are tiles, so in tiled mode
            // it also ends the vocoder's row — which is the right break, the
            // next panel being a different effect.
            VocoderPresets {}
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
            // Last, matching the firmware: the limiter runs after the stereo
            // stage, so it is the only unit that sees the mix as the output
            // will. Its ids live in the stereo block for want of a free one of
            // their own, which changes nothing here — a card is built from a
            // name prefix, not from an id range.
            ParamGroup { title: "Limiter"; prefix: "fx.lim" }
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
