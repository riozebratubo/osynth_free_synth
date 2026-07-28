import QtQuick
import QtQuick.Controls.Material

SpinBox {
    property int decimals: 2
    property real realValue: 0.0
    property real realFrom: 0.0
    property real realTo: 100.0
    property real realStepSize: 1.0

    property real factor: Math.pow(10, decimals)

    id: spinbox
    stepSize: realStepSize * factor
    value: realValue * factor
    to : realTo * factor
    from : realFrom * factor

    validator: DoubleValidator {
        // spinbox.from / spinbox.to are already scaled by factor (realFrom*factor,
        // realTo*factor), so they must not be multiplied by factor again here.
        bottom: Math.min(spinbox.from, spinbox.to)
        top:  Math.max(spinbox.from, spinbox.to)
    }

    onValueChanged: {
        realValue = parseFloat(value * 1.0 / factor).toFixed(decimals)
    }

    textFromValue: function(value, locale) {
        // console.log(">>> textFromValue real value: ", realValue)
        return parseFloat(value * 1.0 / factor).toFixed(decimals)
    }

    valueFromText: function(text, locale) {
        // The text shows the REAL value; the SpinBox's internal value is scaled
        // by factor, so typed input must be scaled back up before returning.
        return Math.round(Number.fromLocaleString(locale, text) * factor)
    }
}