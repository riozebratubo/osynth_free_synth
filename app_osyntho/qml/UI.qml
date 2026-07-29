pragma Singleton

import QtQuick

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

    // The SwipeView pages of the main screen, in order — the one source for both
    // the navigation dock (short `label` + icon, all the room it has) and the
    // startup-screen picker in Settings (full `name`). A page's index in this
    // list *is* its SwipeView index, so the two must stay in the same order as
    // the pages declared in Main.qml. Strings are kept in English here and
    // translated where they are shown, as everywhere else.
    readonly property var screens: [
        { label: "Home", name: "Home",               icon: "\uf015" },  // house
        { label: "Osc",  name: "Oscillator",         icon: "\uf83e" },  // wave-square
        { label: "Flt",  name: "Filter & envelopes", icon: "\uf0b0" },  // filter
        { label: "Mod",  name: "Modulation",         icon: "\uf4e2" },  // circle-nodes
        { label: "FX",   name: "Effects",            icon: "\uf72b" },  // wand-magic-sparkles
        { label: "Seq",  name: "Sequencer",          icon: "\uf00a" },  // table-cells
        { label: "Drum", name: "Drums",              icon: "\uf569" },  // drum
        { label: "Arp",  name: "Arpeggiator",        icon: "\uf550" },  // bars-staggered
        { label: "Loop", name: "Looper",             icon: "\uf363" },  // repeat
        { label: "Patch", name: "Modular patch",     icon: "\uf542" },  // project-diagram
        { label: "Pre",  name: "Presets",            icon: "\uf0c7" },  // floppy-disk
        { label: "Lib",  name: "Patch library",      icon: "\uf02d" }   // book
    ]

    // Panel layout (PanelFlow / ParamGroup): true packs the cards left to right
    // at the width each needs, false gives every card its own full-width line.
    // Same deal as fontSize — SettingsScreen re-assigns it on change.
    property bool tiledPanels: App.setting("panel_layout") !== "rows"

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
