#!/usr/bin/env python3
"""S39 — add the two noise-reduction units to the FX bus.

Why a script rather than a patch: the same edit lands in six files that have
nothing else in common (firmware header, DSP, presets, the parameter-store
capacity note, and two app files), every one of them anchored on a line that
already exists, and getting one of the six wrong is silent. Running this
twice is a no-op — every insertion checks for its own marker first — so it
doubles as the record of exactly what S39 touched.

    python tools/s39_patch_noise_reduction.py [--check]

--check reports what is still missing and changes nothing.

Not the whole of S39: a handful of comment reflows that followed the first run
(the stale "first in the chain" claims around the vocoder and the drive, and
HoldSampleCard.qml's own header now that the card takes both directions) were
made directly in the files. Everything structural is here.
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

EDITS = []  # (path, marker, anchor, text, where) filled in below


def edit(path, marker, anchor, text, where="before"):
    """Insert `text` before/after `anchor`; skip if `marker` is already there."""
    EDITS.append((path, marker, anchor, text, where))


def replace(path, marker, old, new):
    """Swap `old` for `new`; skip if `marker` is already there."""
    EDITS.append((path, marker, old, new, "replace"))


# ---------------------------------------------------------------- fx.h ----

FXH = "components/fx/include/fx.h"

replace(
    FXH, "adaptive NR -> NR -> vocoder",
    """ * osynth — master FX bus: drive -> chorus -> flanger -> phaser -> delay ->
 * granular delay -> reverb -> bitcrush -> filter -> EQ -> compressor ->
 * stereo/output (Sessions 10 + 11; bitcrush S17; filter S33; drive, flanger,
 * phaser, EQ, compressor, stereo and the FX LFOs S34).""",
    """ * osynth — master FX bus: adaptive NR -> NR -> vocoder -> drive -> chorus ->
 * flanger -> phaser -> delay -> granular delay -> reverb -> bitcrush ->
 * filter -> EQ -> compressor -> stereo/output (Sessions 10 + 11; bitcrush
 * S17; filter S33; drive, flanger, phaser, EQ, compressor, stereo and the FX
 * LFOs S34; vocoder S38; the two noise-reduction units S39).""")

edit(FXH, "FX_PID_ANR_ON", "#define FX_PID_LFO1_DEST  0x03C0", """\
/* Noise reduction (S39) — two units at the head of the chain, and the reason
 * they exist is a use for this instrument that is not musical at all: a P4
 * build already enumerates on a computer as a UAC2 capture device (the USB
 * tap, S29) and already has a microphone on it (S37), so it is one cleanup
 * stage short of being a usable USB microphone. These are that stage.
 *
 * The path is the one that was already there and needs no new plumbing: an
 * input device chosen by `in.source`, mixed into the bus by `in.route` = fx,
 * through this bus, out over USB. What was missing is everything between "the
 * microphone works" and "the microphone is worth listening to" — a room's air
 * conditioning, a desk's rumble, mains hum off an unbalanced lead, and the
 * hiss a MEMS capsule has at the gain a conversational voice needs.
 *
 * Two units rather than one mode switch, because they answer different halves
 * of that question and fail in different ways:
 *
 *   fx.anr  adaptive. A filterbank that learns the *steady* part of whatever
 *           is arriving and subtracts it, continuously, with nothing to set
 *           up. It is what takes out fan noise and hiss. It cannot take out a
 *           slammed door, because a slammed door is not steady.
 *   fx.nr   fixed. High-pass, mains-hum notches, and a downward expander with
 *           a hold. Nothing is learned and every number is one the player
 *           chose — which is exactly why it is the one to reach for when the
 *           adaptive unit has guessed wrong.
 *
 * Their order is anr -> nr and it does not commute. The expander's whole job
 * is to duck the gaps between phrases, and a ducked gap is a gap with no noise
 * floor left in it; run the other way round, the estimator would learn that
 * ducked floor — up to `fx.nr.floor` too low — and then under-subtract by
 * exactly that much for the whole of the next phrase. An estimator has to see
 * the floor it is estimating.
 *
 * Both sit ahead of the vocoder, which is where a source cleanup belongs but
 * changes nothing *for* the vocoder: its modulator comes from
 * audio_io_in_mono(), not from this bus, so it is deaf to everything here.
 * Cleaning that path too would mean a second instance of the analysis and is
 * not what these are for.
 *
 * Neither is microphone-specific. They are ordinary bus units, so a noisy
 * sampled loop or a hissy line input gets the same treatment; the microphone
 * is only the case that made them worth building.
 *
 * These two blocks fill 0x03xx. A tenth unit needs a page of its own. */

/* Adaptive: a learned noise profile, subtracted per band. `fx.anr.learn` is
 * momentary: held, the estimator's window drops to 80 ms and its "that is
 * signal, not noise" test is waived, so a second of it in a quiet room is a
 * complete profile. That is what the app's Hold-to-learn button drives, and
 * it is not stored in presets, for the reason fx.voc.freeze is not. */
#define FX_PID_ANR_ON      0x03E0
#define FX_PID_ANR_AMOUNT  0x03E1
#define FX_PID_ANR_FLOOR   0x03E2
#define FX_PID_ANR_BANDS   0x03E3
#define FX_PID_ANR_LOW     0x03E4
#define FX_PID_ANR_HIGH    0x03E5
#define FX_PID_ANR_ADAPT   0x03E6
#define FX_PID_ANR_ATTACK  0x03E7
#define FX_PID_ANR_RELEASE 0x03E8
#define FX_PID_ANR_LEARN   0x03E9

/* Fixed: high-pass, hum notch, downward expander. `fx.nr.floor` is the most
 * important control here and the one a gate usually does not have — it caps
 * the attenuation, so the unit *ducks* the gaps instead of chopping them, and
 * a room that goes absolutely silent between words is the thing that makes a
 * cheap gate audible as a gate. */
#define FX_PID_NR_ON      0x03F0
#define FX_PID_NR_HPF     0x03F1
#define FX_PID_NR_HUM     0x03F2
#define FX_PID_NR_THRESH  0x03F3
#define FX_PID_NR_RATIO   0x03F4
#define FX_PID_NR_FLOOR   0x03F5
#define FX_PID_NR_ATTACK  0x03F6
#define FX_PID_NR_HOLD    0x03F7
#define FX_PID_NR_RELEASE 0x03F8

""")


# -------------------------------------------------------------- fx.cpp ----

FXC = "components/fx/fx.cpp"

replace(
    FXC, "anr -> nr -> vocoder -> drive",
    """ * osynth — master FX bus (Sessions 10 + 11; bitcrush S17; filter S33;
 * drive / flanger / phaser / EQ / compressor / stereo / LFOs S34):
 *
 *   drive -> chorus -> flanger -> phaser -> delay -> granular -> reverb
 *         -> bitcrush -> filter -> EQ -> compressor -> stereo/output
 *
 * That order is the whole design of the bus, so it is worth stating why:
 *
 *  - Drive is first. Saturation belongs on the source, not on the tails;""",
    """ * osynth — master FX bus (Sessions 10 + 11; bitcrush S17; filter S33;
 * drive / flanger / phaser / EQ / compressor / stereo / LFOs S34; vocoder
 * S38; the two noise-reduction units S39):
 *
 *   anr -> nr -> vocoder -> drive -> chorus -> flanger -> phaser -> delay
 *       -> granular -> reverb -> bitcrush -> filter -> EQ -> compressor
 *       -> stereo/output
 *
 * That order is the whole design of the bus, so it is worth stating why:
 *
 *  - The noise reduction is first, and anr before nr, because it is a
 *    *source* cleanup rather than an effect: nothing below it should be
 *    asked to work on top of a room's air conditioning, and an estimator
 *    cannot learn a noise floor that a gate downstream has already ducked
 *    away. The full argument is above FX_PID_ANR_ON in fx.h.
 *  - The vocoder is next, because it decides what the sound *is*, so
 *    everything after it colours the spoken result rather than the carrier.
 *  - Drive leads the effects proper. Saturation belongs on the source, not
 *    on the tails;""")

edit(FXC, "kAnrBandsMax", "constexpr int kVocBandsMin = 4;", """\

/* Adaptive noise reduction (S39). Two SVFs per band per sample — one per
 * channel — plus a block-rate follower, so the ceiling is a CPU budget like
 * the vocoder's above and gated on the same PSRAM proxy for "the bigger
 * chip". The default is deliberately below the ceiling: past about a dozen
 * bands the profile gets *finer* than the noise it is describing, and a
 * finer profile mostly buys the artefact this class of algorithm is known
 * for — isolated bands opening and closing on their own, which sounds like
 * wind chimes in the background. */
#if CONFIG_SPIRAM
constexpr int kAnrBandsMax = 16;
constexpr int kAnrBandsDef = 12;
#else
constexpr int kAnrBandsMax = 10;
constexpr int kAnrBandsDef = 8;
#endif
constexpr int kAnrBandsMin = 4;
""", where="after")

edit(FXC, 'kNrHum[]', "/* Vocoder carrier (S38). Append-only.", """\
/* Mains hum (S39). Append-only, and the two entries are regions of the world
 * rather than a frequency knob: 50 and 60 Hz is the whole list, nobody is
 * hunting for 53, and a notch that can be mistuned is a notch that will be. */
const char* const kNrHum[] = {"off", "50 Hz", "60 Hz"};

""")


edit(FXC, "kNrHumCount", "constexpr int kCompKeyCount = count_of(kCompKeys);", """
constexpr int kNrHumCount = count_of(kNrHum);""", where="after")

replace(FXC, "    ANR_ON, ANR_AMOUNT",
        """enum PIdx {
    VOC_ON, VOC_MIX,""",
        """enum PIdx {
    ANR_ON, ANR_AMOUNT, ANR_FLOOR, ANR_BANDS, ANR_LOW, ANR_HIGH, ANR_ADAPT,
    ANR_ATTACK, ANR_RELEASE, ANR_LEARN,
    NR_ON, NR_HPF, NR_HUM, NR_THRESH, NR_RATIO, NR_FLOOR, NR_ATTACK, NR_HOLD,
    NR_RELEASE,
    VOC_ON, VOC_MIX,""")

# A replace(), not an edit(): the block below ends with the anchor line, so
# inserting it *before* the anchor would leave the array's declaration and its
# first row duplicated — which is a one-row overrun of P_COUNT, i.e. exactly
# the silent failure tools/check_param_tables.py exists to catch.
replace(FXC, '"fx.anr.on"', """const ParamDesc kParams[P_COUNT] = {
    {FX_PID_VOC_ON,""", """\
const ParamDesc kParams[P_COUNT] = {
    {FX_PID_ANR_ON, "fx.anr.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_ANR_AMOUNT, "fx.anr.amount", ParamType::Float, ParamCurve::Linear,
     0.0f, 1.0f, 0.6f, nullptr, 0}, /* 1 = subtract 3x the estimated floor */
    {FX_PID_ANR_FLOOR, "fx.anr.floor", ParamType::Float, ParamCurve::Linear,
     -48.0f, 0.0f, -20.0f, nullptr, 0}, /* dB, the deepest a band may be cut */
    {FX_PID_ANR_BANDS, "fx.anr.bands", ParamType::Int, ParamCurve::Linear,
     (float)kAnrBandsMin, (float)kAnrBandsMax, (float)kAnrBandsDef, nullptr, 0},
    {FX_PID_ANR_LOW, "fx.anr.low", ParamType::Float, ParamCurve::Exp,
     40.0f, 400.0f, 120.0f, nullptr, 0},      /* first crossover, Hz */
    {FX_PID_ANR_HIGH, "fx.anr.high", ParamType::Float, ParamCurve::Exp,
     2000.0f, 16000.0f, 9000.0f, nullptr, 0}, /* last crossover, Hz */
    {FX_PID_ANR_ADAPT, "fx.anr.adapt", ParamType::Float, ParamCurve::Exp,
     0.5f, 60.0f, 8.0f, nullptr, 0},   /* s — how fast the floor may rise */
    {FX_PID_ANR_ATTACK, "fx.anr.attack", ParamType::Float, ParamCurve::Exp,
     1.0f, 100.0f, 5.0f, nullptr, 0},    /* ms — a band reopening */
    {FX_PID_ANR_RELEASE, "fx.anr.release", ParamType::Float, ParamCurve::Exp,
     5.0f, 1000.0f, 150.0f, nullptr, 0}, /* ms — a band closing */
    {FX_PID_ANR_LEARN, "fx.anr.learn", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_NR_ON, "fx.nr.on", ParamType::Bool, ParamCurve::Linear,
     0.0f, 1.0f, 0.0f, nullptr, 0},
    {FX_PID_NR_HPF, "fx.nr.hpf", ParamType::Float, ParamCurve::Exp,
     20.0f, 400.0f, 80.0f, nullptr, 0}, /* the registered minimum is the bypass */
    {FX_PID_NR_HUM, "fx.nr.hum", ParamType::Enum, ParamCurve::Linear,
     0.0f, (float)(kNrHumCount - 1), 0.0f, kNrHum, kNrHumCount},
    {FX_PID_NR_THRESH, "fx.nr.thresh", ParamType::Float, ParamCurve::Linear,
     -80.0f, 0.0f, -45.0f, nullptr, 0}, /* dB, peak */
    {FX_PID_NR_RATIO, "fx.nr.ratio", ParamType::Float, ParamCurve::Exp,
     1.0f, 20.0f, 4.0f, nullptr, 0},    /* downward expansion below thresh */
    {FX_PID_NR_FLOOR, "fx.nr.floor", ParamType::Float, ParamCurve::Linear,
     -60.0f, 0.0f, -24.0f, nullptr, 0}, /* dB: duck this far and no further */
    {FX_PID_NR_ATTACK, "fx.nr.attack", ParamType::Float, ParamCurve::Exp,
     1.0f, 100.0f, 3.0f, nullptr, 0},   /* ms — opening */
    {FX_PID_NR_HOLD, "fx.nr.hold", ParamType::Float, ParamCurve::Linear,
     0.0f, 1000.0f, 150.0f, nullptr, 0}, /* ms; 0 is a real setting, so linear */
    {FX_PID_NR_RELEASE, "fx.nr.release", ParamType::Float, ParamCurve::Exp,
     5.0f, 1000.0f, 200.0f, nullptr, 0}, /* ms — closing */
    {FX_PID_VOC_ON,""")


# The two DSP units live beside this script as plain text so the C++ stays
# readable as C++ (and so tools/check_comment_blocks.py can be run on them
# directly). Both are inserted ahead of the vocoder, which is the head of the
# chain they now sit in front of.
S39 = ROOT / "tools" / "s39"
VOC_ANCHOR = ("/* ---- vocoder (S38): the input's spectrum imposed on the "
              "synth bus ----")

edit(FXC, "anr_process", VOC_ANCHOR,
     (S39 / "anr_dsp.txt").read_text(encoding="utf-8") + "\n" +
     (S39 / "nr_dsp.txt").read_text(encoding="utf-8") + "\n")

replace(FXC, "    anr_process(l, r, frames);",
        "    vocoder_process(l, r, frames);",
        """    anr_process(l, r, frames);
    nr_process(l, r, frames);
    vocoder_process(l, r, frames);""")

replace(
    FXC, '"fx bus up: anr -> nr -> vocoder',
    '''             "fx bus up: vocoder -> drive -> chorus -> flanger -> phaser -> delay -> "
             "granular -> reverb -> crush -> filter -> eq -> comp -> stereo, "''',
    '''             "fx bus up: anr -> nr -> vocoder -> drive -> chorus -> flanger -> "
             "phaser -> delay -> granular -> reverb -> crush -> filter -> eq "
             "-> comp -> stereo, "''')


# ---------------------------------------------------------- presets.cpp ----

edit("components/presets/presets.cpp", "FX_PID_ANR_LEARN",
     "        case FX_PID_VOC_FREEZE:\n", """\
        /* The adaptive NR's learn (S39): momentary, and the same class of
         * control as the freeze above. The Hold-to-learn button leaves it
         * *off*, so storing it would store nothing useful even when it worked
         * — but a preset saved with a finger on the button would come back
         * permanently sampling, which is a unit that never settles. */
        case FX_PID_ANR_LEARN:
""", where="after")


# ------------------------------------------------------- synth_params.h ----

replace(
    "components/synth_core/include/synth_params.h", "S39 added the nineteen",
    """     * and its five shared stages, and 354 since S38 added the vocoder's
     * fourteen. Raised from 384 to 448 in""",
    """     * and its five shared stages, 354 since S38 added the vocoder's
     * fourteen, and 373 since S39 added the nineteen belonging to the two
     * noise-reduction units. Raised from 384 to 448 in""")


# --------------------------------------------------------- FxScreen.qml ----

QML = "app_osyntho/qml/FxScreen.qml"

replace(QML, "adaptive NR -> NR -> vocoder ->",
        """// The panels are in signal-chain order — drive -> chorus -> flanger ->
// phaser -> delay -> granular -> reverb -> bitcrush -> filter -> EQ ->
// compressor -> stereo — so the page reads the way the audio flows. The two
// LFO panels come last because they are not a stage of the chain; they
// modulate the panels above them.""",
        """// The panels are in signal-chain order — adaptive NR -> NR -> vocoder ->
// drive -> chorus -> flanger -> phaser -> delay -> granular -> reverb ->
// bitcrush -> filter -> EQ -> compressor -> stereo — so the page reads the
// way the audio flows. The two LFO panels come last because they are not a
// stage of the chain; they modulate the panels above them.""")

edit(QML, 'prefix: "fx.anr"',
     '            ParamGroup { title: "Vocoder"; prefix: "fx.voc" }', """\
            //
            // Noise reduction (S39) is the head of the firmware's chain and
            // so the head of the page. Hold-to-learn writes the same id the
            // switch beside it does, inverted against the vocoder's freeze
            // below: held means *doing* something here, where held means
            // "not freezing" there.
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
""")


# ---------------------------------------------------- HoldSampleCard.qml ----

CARD = "app_osyntho/qml/HoldSampleCard.qml"

replace(CARD, "property int downValue",
        """    // Registered name of the bool freeze parameter, e.g. "buf.freeze".
    property string paramName: ""
    property string title: \"\"""",
        """    // Registered name of the bool parameter, e.g. "buf.freeze".
    property string paramName: ""
    property string title: ""

    // What the gesture writes. The defaults are the freeze idiom this card
    // was built for — press releases the buffer, release holds it — and S39's
    // adaptive-NR `learn` swaps them, because there the held state is the one
    // that is doing something. Everything below is written in terms of these
    // two, so neither direction is the special case.
    property int downValue: 0
    property int upValue: 1
    // Button captions, in the three states below. Defaults describe a freeze.
    property string idleText: "Hold to sample"
    property string downText: "Recording…"
    property string activeText: "Live\"""")

replace(CARD, "=== card.upValue",
        """        if (paramId >= 0)
            frozen = Math.round(Synth.paramValue(paramId)) === 1""",
        """        if (paramId >= 0)
            frozen = Math.round(Synth.paramValue(paramId)) === card.upValue""")

replace(CARD, "Math.round(value) === card.upValue",
        "                card.frozen = Math.round(value) === 1",
        "                card.frozen = Math.round(value) === card.upValue")

replace(CARD, "t.t(card.downText)",
        """            text: down ? t.t("Recording…")
                       : (card.frozen ? t.t("Hold to sample") : t.t("Live"))

            onPressed: Synth.setParamNow(card.paramId, 0)
            onReleased: Synth.setParamNow(card.paramId, 1)""",
        """            text: down ? t.t(card.downText)
                       : (card.frozen ? t.t(card.idleText)
                                      : t.t(card.activeText))

            onPressed: Synth.setParamNow(card.paramId, card.downValue)
            onReleased: Synth.setParamNow(card.paramId, card.upValue)""")

replace(CARD, "onCanceled: Synth.setParamNow(card.paramId, card.upValue)",
        "            onCanceled: Synth.setParamNow(card.paramId, 1)",
        "            onCanceled: Synth.setParamNow(card.paramId, card.upValue)")


# -------------------------------------------------------- translator.cpp ----

TR = "app_osyntho/src/translator.cpp"

edit(TR, '"Hold to learn"',
     '''  pt["Hold to record into the ring; frozen on release."] =
      "Segure para gravar no buffer; congela ao soltar.";
''', '''  pt["Hold to learn"] = "Segure para aprender";
  pt["Learning…"] = "Aprendendo…";
  pt["Learning"] = "Aprendendo";
  pt["Hold during a silent moment; the room is sampled."] =
      "Segure em um momento de silêncio; a sala é amostrada.";
''', where="after")

edit(TR, '"Adaptive NR"',
     '  pt["Buffer capture"] = "Captura do buffer";\n',
     '''  pt["Adaptive NR"] = "Redução adaptativa";
  pt["Noise reduction"] = "Redução de ruído";
  pt["Noise profile"] = "Perfil de ruído";
''', where="after")


# -------------------------------------------------------- PARAM_MAP.md ----

PMAP = "private_docs/PARAM_MAP.md"

replace(PMAP, "**Ahead of every effect**, before drive",
        "**First in the chain**, before drive — it decides what the sound *is*, so",
        "**Ahead of every effect**, before drive — only the two noise-reduction\n"
        "units (S39) run earlier — it decides what the sound *is*, so")

edit(PMAP, "### Noise reduction (0x03Ex",
     "### Sequencer + arpeggiator (0x04xx",
     (S39 / "param_map.md").read_text(encoding="utf-8") + "\n")


# ------------------------------------------------------------------ run ----

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="report what is missing; change nothing")
    args = ap.parse_args()

    rc = 0
    pending = 0
    for rel, marker, anchor, text, where in EDITS:
        path = ROOT / rel
        src = path.read_text(encoding="utf-8")
        if marker in src:
            print(f"  ok   {rel}: {marker!r} already present")
            continue
        pending += 1
        if where == "replace":
            if src.count(anchor) != 1:
                print(f"  FAIL {rel}: anchor for {marker!r} found "
                      f"{src.count(anchor)} times")
                rc = 1
                continue
            out = src.replace(anchor, text)
        else:
            if src.count(anchor) != 1:
                print(f"  FAIL {rel}: anchor for {marker!r} found "
                      f"{src.count(anchor)} times")
                rc = 1
                continue
            out = (src.replace(anchor, text + anchor) if where == "before"
                   else src.replace(anchor, anchor + text))
        if args.check:
            print(f"  todo {rel}: would insert {marker!r}")
            continue
        path.write_text(out, encoding="utf-8")
        print(f"  +    {rel}: {marker!r}")

    if rc == 0 and pending == 0:
        print("nothing to do — S39 is already applied")
    return rc


if __name__ == "__main__":
    sys.exit(main())


