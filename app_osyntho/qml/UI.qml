pragma Singleton

import QtQuick
import org.osynth.osyntho

// Session-state singleton shared by every screen/component. Main.qml assigns
// `window` once on startup; components depend on this explicit object instead of
// Main.qml internals. Upward requests (backup/firmware/settings/device flows)
// are signals the shell (Main.qml, which owns the dialogs and pickers) connects
// to.
QtObject {
    id: root

    // The root ApplicationWindow. Set by Main.qml on startup.
    property Window window

    readonly property bool portrait: window ? window.width < window.height : false

    // App.setting() reads are not tracked by the binding engine, so SettingsScreen
    // re-assigns fontSize when the underlying setting changes.
    property int fontSize: App.setting("app_font_size")
    property bool desktopLayout: !portrait

    // Display forms for names the firmware owns. The registered spelling is
    // never touched: a parameter's name is the firmware's identifier and every
    // lookup — paramIdForName, the p-lock picker, a stored patch's rows, an
    // exported JSON file — still spells it exactly as registered. Only what is
    // drawn passes through here.
    function capitalized(text: string): string {
        return text.length > 0 ? text.charAt(0).toUpperCase() + text.slice(1) : text
    }

    // The leaf of a parameter's dotted name, as a control's label:
    // "common.glide" -> "Glide", "fx.lfo1.rate" -> "Rate". Every control that
    // labels itself from a parameter goes through this, so the pages cannot
    // drift apart on capitalisation the way three separate `.split('.').pop()`
    // expressions were free to.
    function paramLabel(name: string): string {
        const leaf = name.split('.').pop()
        return leaf.length > 0 ? leaf.charAt(0).toUpperCase() + leaf.slice(1) : leaf
    }

    // The slot the synth is on, in the one form every surface names it in:
    // "5 - grain choir", carrying the same 0-47 / 48-111 number the Presets
    // page tiles are labelled with. Empty — not "-1", not a placeholder —
    // until the synth has reported a slot and that engine's listing has
    // supplied its name, so a caller can drop the whole clause rather than
    // print a stand-in.
    readonly property string presetLabel:
        Synth.presetSlot >= 0
            ? (Synth.presetSlot
               + (Synth.presetName.length > 0
                  ? (" - " + capitalized(Synth.presetName)) : ""))
            : ""

    // The SwipeView pages of the main screen, in order — the one source for both
    // the navigation dock (short `label` + icon, all the room it has) and the
    // startup-screen picker in Settings (full `name`). A page's index in this
    // list *is* its SwipeView index, so the two must stay in the same order as
    // the pages declared in Main.qml. Strings are kept in English here and
    // translated where they are shown, as everywhere else.
    //
    // Inserting or removing an entry moves every index after it, and two
    // settings store one — the startup screen and the page the last run was
    // left on. Bump screen_order_rev and extend Settings::migrateScreenOrder()
    // when you change this list, or people's stored choices quietly slide one
    // page over. (That is what the last edit did: an Input page went in before
    // FX and the osynth page came off the end. It held three things and
    // repeated two of them — master volume was already on the toolbar and its
    // `in.` group was already on FX — so the analogue output level and the USB
    // card moved to Home and the input took a page of its own.)
    //
    // rev 2 (S41) inserted Chord after Arp; Settings::migrateScreenOrder()
    // carries the two stored indices across that in one step.
    readonly property var screens: [
        { label: "Home", name: "Home",               icon: "\uf015" },  // house
        { label: "Osc",  name: "Oscillator",         icon: "\uf83e" },  // wave-square
        { label: "Flt",  name: "Filter & envelopes", icon: "\uf0b0" },  // filter
        { label: "Mod",  name: "Modulation",         icon: "\uf4e2" },  // circle-nodes
        // Before FX because that is the order the signal takes, and its own
        // page because the `in.` group used to be drawn on both the FX page
        // and the old osynth page — one set of controls under two titles.
        { label: "In",   name: "Input",              icon: "\uf130" },  // microphone
        { label: "FX",   name: "Effects",            icon: "\uf72b" },  // wand-magic-sparkles
        { label: "Seq",  name: "Sequencer",          icon: "\uf00a" },  // table-cells
        // S44 turned the drum bus into a rack of recordable sample kits,
        // so the page is "Sample kits" and the tab is "Kit". `name` is what
        // the toolbar title and the screen picker show; the stored screen
        // order is by *index*, so renaming in place costs no migration.
        { label: "Kit",  name: "Sample kits",              icon: "\uf569" },  // drum
        { label: "Arp",  name: "Arpeggiator",        icon: "\uf550" },  // bars-staggered
        // Next to the arpeggiator because the two are the same kind of thing:
        // a transform between the key you press and the notes that sound. And
        // because chord.route decides which of them runs first.
        { label: "Chord", name: "Chord mode",        icon: "\uf5fd" },  // layer-group
        { label: "Loop", name: "Looper",             icon: "\uf363" },  // repeat
        { label: "Patch", name: "Modular patch",     icon: "\uf542" },  // project-diagram
        { label: "Pre",  name: "Presets",            icon: "\uf0c7" },  // floppy-disk
        { label: "Loc. Pre", name: "Local presets",  icon: "\uf02d" }  // book
    ]

    // Panel layout (PanelFlow / ParamGroup): true packs the cards left to right
    // at the width each needs, false gives every card its own full-width line.
    // Same deal as fontSize — SettingsScreen re-assigns it on change.
    property bool tiledPanels: App.setting("panel_layout") !== "rows"

    // Whether a horizontal drag anywhere on a page changes page (SwipeView's
    // `interactive`). Off by default, and that is the point: nearly every page
    // here is covered in things you drag — knobs, the sequencer grid, the
    // graph canvas, the keyboard — and a drag that starts on one of those and
    // is read as a page swipe both misses the control and moves the page. The
    // nav dock and the toolbar arrows are always there either way. Same deal
    // as fontSize: App.setting() is not a tracked read, so SettingsScreen
    // re-assigns this on change.
    property bool swipeNavigation: App.settingIsTrue("swipe_change_screens")

    // Note written into a step when you tap an empty cell in the sequencer
    // grid. These live here rather than on SequencerScreen because the
    // surfaces that *pick* them — the on-screen keyboard and the drum pads —
    // are siblings of the SwipeView in Main.qml and cannot reach into a page.
    // Right-click (desktop) or press-and-hold (touch) a key or pad to set one.
    //
    // Melodic and percussive picks are kept apart on purpose: choosing a kick
    // on the pads should not throw away the C4 you lined up for a bass lane,
    // and one shared value cannot have a sensible default for both — 60 is
    // middle C on a keyboard and answers to no drum slot at all.
    property int paintNote: 60

    // -1 = nothing picked yet, so fall back to the kit's first slot (the kick
    // in the factory kit). Resolved rather than stored, because the kit — and
    // therefore the right default — can change at runtime.
    // ---- sample-kit recording (S44) ----------------------------------------
    //
    // Lives here rather than on either surface that uses it, because both do:
    // the Sample kits page has Record / Erase buttons and so does the pad strip
    // at the bottom of every other page, and arming on one has to light up the
    // other. There is one recorder in the firmware, so there is one armed
    // state here.
    //
    // The gesture is deliberately two-step -- press Record, *then* press the
    // pad -- because a pad is a destination that gets overwritten, and a
    // one-touch record would make an accidental brush against the strip
    // destroy a sample. The second press is the confirmation, and it is also
    // where the recording actually happens: the pad is held for as long as you
    // want to capture.
    //
    // "" = nothing armed, "record" and "erase" = waiting for a pad.
    property string padAction: ""

    // Which pad the firmware last reported as armed, so both surfaces can
    // outline it. -1 for none.
    property int armedPad: -1

    // smp.state, mirrored from the firmware: 0 idle, 1 armed, 2 waiting for the
    // threshold, 3 recording, 4 committing. Taken from the firmware rather than
    // inferred from what we last sent, because the firmware is what decides
    // when a threshold-armed take actually starts and when the ceiling stops
    // it -- a surface that trusted its own writes would show "recording"
    // through a take that never triggered.
    //
    // A plain property fed by the signal below, not a binding onto
    // Synth.paramValue(): that is an invokable, so a binding on it captures no
    // property and evaluates exactly once. Same trap ParamValue.qml exists to
    // avoid; this is the singleton-scoped version of it.
    property int samplerState: 0
    property real samplerPos: 0
    property real samplerPeak: 0
    property real samplerFreeKb: 0
    readonly property bool samplerRecording: samplerState === 2 || samplerState === 3

    // True when this firmware has the recorder at all. Everything sampler-ish
    // in the UI hangs off this, so a build with OSYNTH_SAMPLE_KITS=0 -- or a
    // firmware older than S44 -- simply does not draw it.
    property bool samplerAvailable: false

    property Connections _smpConn: Connections {
        target: Synth
        function onParamsDiscovered(): void {
            root.samplerAvailable = Synth.paramIdForName("smp.arm") > 0
            root.refreshSampler()
        }
        function onReadyChanged(): void {
            if (!Synth.ready) {
                root.samplerAvailable = false
                root.padAction = ""
                root.armedPad = -1
                root.samplerState = 0
            }
        }
        function onParamChanged(id: int, v: real): void {
            if (!root.samplerAvailable) return
            if (id === Synth.paramIdForName("smp.state")) root.samplerState = Math.round(v)
            else if (id === Synth.paramIdForName("smp.pos")) root.samplerPos = v
            else if (id === Synth.paramIdForName("smp.peak")) root.samplerPeak = v
            else if (id === Synth.paramIdForName("smp.free")) root.samplerFreeKb = v
            else if (id === Synth.paramIdForName("smp.arm")) root.armedPad = Math.round(v)
        }
    }

    function refreshSampler(): void {
        if (!samplerAvailable) return
        samplerState = Math.round(smpValue("smp.state"))
        samplerPos = smpValue("smp.pos")
        samplerPeak = smpValue("smp.peak")
        samplerFreeKb = smpValue("smp.free")
        armedPad = Math.round(smpValue("smp.arm"))
    }

    function setSmp(name: string, value: real): void {
        const pid = Synth.paramIdForName(name)
        // setParamNow, not setParam: these are transport gestures, and the
        // 40 ms coalescing batch would merge a press and a release that
        // happened inside one window into nothing at all.
        if (pid > 0) Synth.setParamNow(pid, value)
    }

    function smpValue(name: string): real {
        const pid = Synth.paramIdForName(name)
        return pid > 0 ? Synth.paramValue(pid) : 0
    }

    // Arm a pad and open the gate. Called by whichever surface the player
    // pressed; the firmware's pre-roll ring is what covers the round trip.
    function startRecordInto(slot: int): void {
        setSmp("smp.arm", slot)
        setSmp("smp.rec", 1)
        armedPad = slot
    }

    function stopRecord(): void {
        setSmp("smp.rec", 0)
        padAction = ""
    }

    function erasePad(slot: int): void {
        setSmp("smp.erase", slot)
        padAction = ""
    }

    property int paintDrumNote: -1
    readonly property int drumNote: paintDrumNote >= 0 ? paintDrumNote
                                                       : Synth.defaultDrumNote

    // Patch interchange. The pages build and consume the JSON; the shell
    // (Main.qml) owns the pickers and the per-platform delivery, exactly as it
    // does for the database backup. `suggestedName` is a bare base name — the
    // shell makes it safe for a file system and adds the extension.
    signal exportJsonRequested(string text, string suggestedName)
    // `page` names who is asking, and comes back on jsonImported below. Both
    // importing pages sit in the same SwipeView and are alive at once, so a
    // bare "a file was picked" would be acted on by both — the file would be
    // pushed to the synth *and* stored, whichever button was pressed. (Not
    // called `target`: inside a Connections handler that would shadow the
    // element's own `target` property.)
    signal importJsonRequested(string page)
    // A picked file's contents, answering importJsonRequested(): "preset" for
    // PresetsScreen (applies it), "library" for PatchLibraryScreen (stores it).
    signal jsonImported(string page, string text)

    signal restoreBackupRequested()
    signal shareBackupRequested()
    signal updateFirmwareRequested(string extension)
    signal settingsRequested()
    signal selectDeviceRequested()
}
