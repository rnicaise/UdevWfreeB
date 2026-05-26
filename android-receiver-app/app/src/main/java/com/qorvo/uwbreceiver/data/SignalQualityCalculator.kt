package com.qorvo.uwbreceiver.data

import kotlin.math.abs

data class SignalBreakdown(
    val rxScore10: Float? = null,
    val directPathScore10: Float? = null,
    val multipathScore10: Float? = null,
    val clockScore10: Float? = null,
    val multipathGapDb: Float? = null,
)

object SignalQualityCalculator {
    fun fromSample(sample: CsvSample?): SignalBreakdown {
        if (sample == null) {
            return SignalBreakdown()
        }

        val rx = sample.rxPowerDbm
        val fp = sample.firstPathPowerDbm
        val clock = sample.clockOffsetPpm
        val gap = if (rx != null && fp != null) rx - fp else null

        return SignalBreakdown(
            // Close body/bike placement tests often report very strong RX values
            // (-30..0 dBm). Use a tighter scale so orientation changes remain visible.
            rxScore10 = rx?.let { scoreHighIsGood(it, badValue = -35f, goodValue = -5f) },
            directPathScore10 = fp?.let { scoreHighIsGood(it, badValue = -45f, goodValue = -10f) },
            multipathScore10 = gap?.let { scoreLowIsGood(it, goodValue = 6f, badValue = 25f) },
            clockScore10 = clock?.let { scoreLowIsGood(abs(it), goodValue = 5f, badValue = 40f) },
            multipathGapDb = gap,
        )
    }

    private fun scoreHighIsGood(value: Float, badValue: Float, goodValue: Float): Float {
        return when {
            value <= badValue -> 1f
            value >= goodValue -> 10f
            else -> 1f + ((value - badValue) * 9f / (goodValue - badValue))
        }.coerceIn(1f, 10f)
    }

    private fun scoreLowIsGood(value: Float, goodValue: Float, badValue: Float): Float {
        return when {
            value <= goodValue -> 10f
            value >= badValue -> 1f
            else -> 10f - ((value - goodValue) * 9f / (badValue - goodValue))
        }.coerceIn(1f, 10f)
    }
}