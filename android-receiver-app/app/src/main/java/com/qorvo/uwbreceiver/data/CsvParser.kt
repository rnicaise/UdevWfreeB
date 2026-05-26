package com.qorvo.uwbreceiver.data

object CsvParser {
    fun parse(line: String): CsvSample? {
        val trimmed = line.trim()
        if (trimmed.isEmpty() || trimmed.startsWith("#")) {
            return null
        }

        val parts = trimmed.split(',')
        if (parts.size < 3) {
            return null
        }

        return try {
            val radioFieldCount = when {
                parts.size == 11 || parts.size >= 21 -> 8
                parts.size == 7 || parts.size >= 17 -> 4
                else -> 0
            }
            val hasRadioMetrics = radioFieldCount > 0
            val hasNlosMetrics = radioFieldCount >= 8
            val telemetryOffset = radioFieldCount

            CsvSample(
                ms = parts[0].toLong(),
                sample = parts[1].toLong(),
                dist = parts[2].toFloat(),
                iax = parts.getOrNull(3 + telemetryOffset)?.toIntOrNull() ?: 0,
                iay = parts.getOrNull(4 + telemetryOffset)?.toIntOrNull() ?: 0,
                iaz = parts.getOrNull(5 + telemetryOffset)?.toIntOrNull() ?: 0,
                rax = parts.getOrNull(6 + telemetryOffset)?.toIntOrNull() ?: 0,
                ray = parts.getOrNull(7 + telemetryOffset)?.toIntOrNull() ?: 0,
                raz = parts.getOrNull(8 + telemetryOffset)?.toIntOrNull() ?: 0,
                rxPowerDbm = if (hasRadioMetrics) parts.getOrNull(3)?.toFloatOrNull()?.takeIf { it.isFinite() } else null,
                firstPathPowerDbm = if (hasRadioMetrics) parts.getOrNull(4)?.toFloatOrNull()?.takeIf { it.isFinite() } else null,
                clockOffsetPpm = if (hasRadioMetrics) parts.getOrNull(5)?.toFloatOrNull()?.takeIf { it.isFinite() } else null,
                signalQuality10 = if (hasRadioMetrics) parts.getOrNull(6)?.toFloatOrNull()?.takeIf { it.isFinite() }?.coerceIn(1f, 10f) else null,
                nlosQuality10 = if (hasNlosMetrics) parts.getOrNull(7)?.toFloatOrNull()?.takeIf { it.isFinite() }?.coerceIn(1f, 10f) else null,
                peakToFirstPathSamples = if (hasNlosMetrics) parts.getOrNull(8)?.toFloatOrNull()?.takeIf { it.isFinite() } else null,
                firstPathConfidence = if (hasNlosMetrics) parts.getOrNull(9)?.toIntOrNull() else null,
                stsQuality = if (hasNlosMetrics) parts.getOrNull(10)?.toIntOrNull() else null,
                responderAcquisitionPeriodMs = parts.getOrNull(9 + telemetryOffset)?.toIntOrNull(),
                initiatorAcquisitionPeriodMs = parts.getOrNull(10 + telemetryOffset)?.toIntOrNull(),
                responderProfileOpt = parts.getOrNull(11 + telemetryOffset)?.toIntOrNull(),
                initiatorProfileOpt = parts.getOrNull(12 + telemetryOffset)?.toIntOrNull(),
            )
        } catch (_: NumberFormatException) {
            null
        }
    }
}
