import QtQuick
import QtQuick.Controls.Material

import org.osynth.osyntho

// Non-interactive ADSR curve for an envelope, drawn from the live
// attack/decay/sustain/release params resolved by `prefix` (e.g. "env1" ->
// env1.attack/decay/sustain/release). Repaints as the knobs move. Hidden when
// the envelope isn't registered for the active engine.
//
// Times are seconds (param range 1ms..10s); a sqrt scale keeps a 5 ms attack and
// a 2 s release both visible. Sustain is a 0..1 level.
Item {
    id: root

    property string prefix: "env1"
    // Resolved imperatively (paramIdForName has no change signal) so they update
    // when discovery completes.
    property int aId: -1
    property int dId: -1
    property int sId: -1
    property int rId: -1
    readonly property bool available: aId >= 0 && dId >= 0 && sId >= 0 && rId >= 0

    visible: available
    implicitHeight: available ? 72 : 0
    height: implicitHeight

    onAvailableChanged: canvas.requestPaint()

    function refreshIds() {
        aId = Synth.paramIdForName(prefix + ".attack")
        dId = Synth.paramIdForName(prefix + ".decay")
        sId = Synth.paramIdForName(prefix + ".sustain")
        rId = Synth.paramIdForName(prefix + ".release")
        canvas.requestPaint()
    }
    Component.onCompleted: refreshIds()

    Connections {
        target: Synth
        function onParamChanged(id: int, value: real): void {
            if (id === root.aId || id === root.dId || id === root.sId || id === root.rId)
                canvas.requestPaint()
        }
        function onParamsDiscovered() { root.refreshIds() }
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            if (!root.available)
                return

            var a = Synth.paramValue(root.aId)
            var d = Synth.paramValue(root.dId)
            var s = Synth.paramValue(root.sId)
            var r = Synth.paramValue(root.rId)

            var sc = function(t) { return Math.sqrt(Math.max(0, t)) + 0.08 }
            var wa = sc(a), wd = sc(d), wr = sc(r)
            var ws = 0.55  // fixed sustain-hold segment
            var units = wa + wd + ws + wr
            var pad = 6
            var usableW = width - pad * 2
            var top = pad
            var bottom = height - pad
            var span = bottom - top

            var x0 = pad
            var xa = x0 + wa / units * usableW
            var xd = xa + wd / units * usableW
            var xs = xd + ws / units * usableW
            var xr = xs + wr / units * usableW
            var sustainY = bottom - s * span

            // baseline
            ctx.strokeStyle = Material.theme === Material.Dark ? "#33FFFFFF" : "#22000000"
            ctx.lineWidth = 1
            ctx.beginPath(); ctx.moveTo(pad, bottom); ctx.lineTo(width - pad, bottom); ctx.stroke()

            // envelope
            ctx.strokeStyle = Material.accent
            ctx.lineWidth = 2
            ctx.beginPath()
            ctx.moveTo(x0, bottom)     // note on
            ctx.lineTo(xa, top)        // attack -> peak
            ctx.lineTo(xd, sustainY)   // decay -> sustain
            ctx.lineTo(xs, sustainY)   // sustain hold
            ctx.lineTo(xr, bottom)     // release -> 0
            ctx.stroke()

            // fill under the curve
            ctx.lineTo(x0, bottom)
            ctx.closePath()
            ctx.fillStyle = Material.theme === Material.Dark ? "#22FFFFFF" : "#11000000"
            ctx.fill()
        }
    }
}
