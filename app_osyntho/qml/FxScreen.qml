import QtQuick
import QtQuick.Controls.Material

import org.osynth.osyntho

// Master FX bus: chorus -> delay -> granular delay -> reverb -> bitcrush,
// plus the line input that feeds into it (S31). Engine-independent
// (registered at boot), so these persist across engine switches.
Item {
    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: panels.implicitHeight + 24
        clip: true

        PanelFlow {
            id: panels

            // Line input (S31). Self-hides on firmware without it, since
            // PARAM_INFO then reports no `in.` ids at all.
            ParamGroup { title: "Line in"; prefix: "in." }

            ParamGroup { title: "Chorus"; prefix: "fx.cho" }
            ParamGroup { title: "Delay"; prefix: "fx.dly" }
            ParamGroup { title: "Granular delay"; prefix: "fx.grn" }
            ParamGroup { title: "Reverb"; prefix: "fx.rev" }
            ParamGroup { title: "Bitcrush"; prefix: "fx.crush" }
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
