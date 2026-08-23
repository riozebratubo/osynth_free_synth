pragma Singleton

import QtQuick
import org.osynth.osyntho

// Session-state singleton shared by every screen/component. Main.qml assigns
// `window` once on startup; components depend on this explicit object instead of
// Main.qml internals. Upward requests (backup/firmware/settings/device flows)
// are signals the shell (Main.qml, which owns the dialogs and pickers) connects
// to.
QtObject {
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
        { label: "Drum", name: "Drums",              icon: "\uf569" },  // drum
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
