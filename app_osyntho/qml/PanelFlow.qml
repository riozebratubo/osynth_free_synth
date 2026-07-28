import QtQuick

// Container for a screen's panels (the titled cards of ParamGroup, plus the
// hand-built ones). In tiled mode — Settings ▸ General ▸ "Panel layout",
// the default — panels pack left to right / top to bottom, each taking only
// the width its controls need; in "one per line" mode every panel is given
// the full width and the flow degenerates into a plain column.
//
// The mode lives entirely in the children's width: ParamGroup reads
// `contentWidth` off this parent and decides. Anything else dropped in here
// (envelope curves, transport rows, the mod matrix) should bind
// `width: parent.contentWidth` so it keeps a line of its own in both modes.
Flow {
    // Width available to a child: the flow minus its own padding. Children
    // wider than this would be clipped by the screen's Flickable.
    readonly property real contentWidth: width - leftPadding - rightPadding

    width: parent ? parent.width : 0
    padding: 12
    spacing: 10
}
