pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Material

import org.osynth.osyntho

// Starting points for the vocoder (S43), as a row of buttons.
//
// Why this card exists at all. The vocoder has thirteen controls and no two of
// them are independent — the band count sets the natural Q, the Q sets how much
// the bands overlap, the span sets both, and the follower times decide whether
// any of it survives a consonant. There is a small set of combinations that
// sound like something, and finding one from the knobs is a research project
// rather than a musical decision. These are those combinations.
//
// They are *starting points*, not modes. Each button writes a plain set of
// parameter values and then has nothing further to do with them: the knobs
// above move exactly as they did, the card does not light up to say which one
// you are "in", and a preset save stores whatever the knobs read afterwards.
// That is deliberate — a button that owned its settings would need an "is this
// still mine" test on every knob move, and the honest answer is nearly always
// no.
//
// Two things every one of these depends on, which is why the footer says so:
// the Input page's `in.source` has to name the microphone, and the synth has
// to be making a sound for the voice to land on. A vocoder with no carrier is
// silent by construction, not by fault.
//
// setParam(), not setParamNow(). These are a dozen ids written at once, which
// is exactly what the ~40 ms coalescing window is for: they arrive as one or
// two write-without-response frames in ascending id order instead of a dozen
// separate ones. (setParamNow is for a press/release pair of the *same* id,
// which coalescing would collapse — see HoldSampleCard.)
Rectangle {
    id: card

    property string title: "Vocoder starting points"

    // The hint belonging to whichever button was pressed last, shown in the
    // footer. Touch has no hover, so the ToolTip on each button is the desktop
    // affordance and this is the one that works everywhere.
    property string activeHint: ""

    // Every set is written against the same key list, so a set that leaves a
    // control out is a control that keeps whatever it had — and reading two
    // rows against each other tells you what actually differs between them.
    // `bands` is deliberately absent: the ceiling is 16 on PSRAM parts and 10
    // on the rest, and since S43 the bank sums to the same level either way,
    // so the right value is "whatever this board can do" — which is what the
    // firmware default already is.
    readonly property var sets: [
        {
            name: "Robot",
            hint: "The classic. Hold a saw chord and speak evenly.",
            values: {
                "fx.voc.on": 1, "fx.voc.mix": 1.0,
                "fx.voc.low": 150, "fx.voc.high": 6500, "fx.voc.q": 0.68,
                "fx.voc.attack": 2, "fx.voc.release": 25,
                "fx.voc.shift": 0, "fx.voc.sib": 0.35, "fx.voc.gate": 0.02,
                "fx.voc.norm": 1, "fx.voc.clarity": 1, "fx.voc.level": 2.0, "fx.voc.carrier": 0
            }
        },
        {
            name: "Talkbox",
            hint: "Narrower and more forward. Sawtooth or square.",
            values: {
                "fx.voc.on": 1, "fx.voc.mix": 1.0,
                "fx.voc.low": 250, "fx.voc.high": 4500, "fx.voc.q": 0.82,
                "fx.voc.attack": 1.5, "fx.voc.release": 18,
                "fx.voc.shift": 2, "fx.voc.sib": 0.25, "fx.voc.gate": 0.02,
                "fx.voc.norm": 1, "fx.voc.clarity": 0, "fx.voc.level": 2.5, "fx.voc.carrier": 0
            }
        },
        {
            name: "Choir",
            hint: "Slow and smeared. Hold a pad and sing vowels.",
            values: {
                "fx.voc.on": 1, "fx.voc.mix": 1.0,
                "fx.voc.low": 200, "fx.voc.high": 5000, "fx.voc.q": 0.38,
                "fx.voc.attack": 10, "fx.voc.release": 140,
                "fx.voc.shift": 0, "fx.voc.sib": 0.12, "fx.voc.gate": 0.02,
                "fx.voc.norm": 1, "fx.voc.clarity": 0, "fx.voc.level": 2.0, "fx.voc.carrier": 0
            }
        },
        {
            name: "Whisper",
            hint: "Noise carrier — this one works with no note held.",
            values: {
                "fx.voc.on": 1, "fx.voc.mix": 1.0,
                "fx.voc.low": 200, "fx.voc.high": 6500, "fx.voc.q": 0.5,
                "fx.voc.attack": 1, "fx.voc.release": 15,
                "fx.voc.shift": 0, "fx.voc.sib": 0.7, "fx.voc.gate": 0.015,
                "fx.voc.norm": 1, "fx.voc.clarity": 1, "fx.voc.level": 4.0, "fx.voc.carrier": 1
            }
        },
        {
            name: "Giant",
            hint: "Formants down seven semitones. Play low.",
            values: {
                "fx.voc.on": 1, "fx.voc.mix": 1.0,
                "fx.voc.low": 100, "fx.voc.high": 5000, "fx.voc.q": 0.6,
                "fx.voc.attack": 3, "fx.voc.release": 60,
                "fx.voc.shift": -7, "fx.voc.sib": 0.2, "fx.voc.gate": 0.02,
                "fx.voc.norm": 1, "fx.voc.clarity": 0, "fx.voc.level": 2.0, "fx.voc.carrier": 0
            }
        },
        {
            name: "Alien",
            hint: "Formants up, with noise mixed into the synth.",
            values: {
                "fx.voc.on": 1, "fx.voc.mix": 1.0,
                "fx.voc.low": 180, "fx.voc.high": 7000, "fx.voc.q": 0.75,
                "fx.voc.attack": 1.5, "fx.voc.release": 20,
                "fx.voc.shift": 7, "fx.voc.sib": 0.5, "fx.voc.gate": 0.02,
                "fx.voc.norm": 1, "fx.voc.clarity": 1, "fx.voc.level": 2.5, "fx.voc.carrier": 2
            }
        },
        {
            name: "Speak",
            hint: "Tuned for words rather than character.",
            values: {
                "fx.voc.on": 1, "fx.voc.mix": 1.0,
                "fx.voc.low": 160, "fx.voc.high": 6000, "fx.voc.q": 0.55,
                "fx.voc.attack": 1.5, "fx.voc.release": 20,
                "fx.voc.shift": 0, "fx.voc.sib": 0.65, "fx.voc.gate": 0.02,
                "fx.voc.norm": 1, "fx.voc.clarity": 1, "fx.voc.level": 1.5, "fx.voc.carrier": 2
            }
        }
    ]

    // Resolved by refresh(), never by a binding — paramIdForName() is a plain
    // call and discovery has not run when this is created. Same trap and same
    // fix as ParamControl and HoldSampleCard document.
    //
    // fx.voc.norm is the probe rather than fx.voc.on, and it is the right one
    // for a reason that outlived a wrong one. These `level` values are the
    // firmware's own default staging, so a pre-S43 synth would not be wildly
    // mis-levelled by them — but on that firmware the output still tracks
    // fx.voc.q and the band count, so each button would land somewhere
    // different, and nothing here could switch the normalisation on because
    // there is none. Hiding the card is the honest outcome; the app still
    // drives that synth, just without this.
    property bool available: false

    function refresh() {
        available = Synth.paramIdForName("fx.voc.norm") >= 0
    }

    Component.onCompleted: refresh()

    Connections {
        target: Synth
        function onParamsDiscovered() { card.refresh() }
    }

    function apply(values: var): void {
        for (const name in values) {
            const id = Synth.paramIdForName(name)
            // A synth that does not register one of these is not an error
            // here: skip it and write the rest, exactly as a preset load does.
            if (id >= 0)
                Synth.setParam(id, values[name])
        }
    }

    property real maxWidth: {
        if (!parent)
            return 320
        const cw = parent.contentWidth
        return cw !== undefined && cw > 0 ? cw : parent.width
    }

    // Unlike ParamGroup and HoldSampleCard this does not ask for a narrow tile:
    // seven buttons plus a line of prose is a full-width card in either layout,
    // and claiming otherwise would just make the Flow inside it wrap to one
    // button per row.
    width: maxWidth
    implicitHeight: available ? (col.implicitHeight + 16) : 0
    height: implicitHeight
    visible: available
    radius: 8
    color: Material.theme === Material.Dark ? "#1AFFFFFF" : "#0D000000"

    Column {
        id: col
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 8
        spacing: 6

        Label {
            text: Tr.t(card.title)
            font.bold: true
            font.pointSize: UI.fontSize * 0.95
            color: Material.foreground
        }

        Flow {
            width: col.width
            spacing: 6

            Repeater {
                model: card.sets

                Button {
                    required property var modelData
                    enabled: Synth.connected
                    text: Tr.t(modelData.name)
                    ToolTip.visible: hovered && modelData.hint !== ""
                    ToolTip.text: Tr.t(modelData.hint)
                    onClicked: {
                        card.apply(modelData.values)
                        card.activeHint = modelData.hint
                    }
                }
            }
        }

        Label {
            visible: card.activeHint !== ""
            text: card.activeHint !== "" ? Tr.t(card.activeHint) : ""
            font.pointSize: UI.fontSize * 0.8
            color: Material.foreground
            width: col.width
            wrapMode: Text.WordWrap
        }

        Label {
            text: Tr.t("Sets the Vocoder knobs above and switches it on. Pick the mic under Input, and hold a note — a vocoder with no synth under it is silent.")
            font.pointSize: UI.fontSize * 0.8
            color: Material.foreground
            opacity: 0.7
            width: col.width
            wrapMode: Text.WordWrap
        }
    }
}
