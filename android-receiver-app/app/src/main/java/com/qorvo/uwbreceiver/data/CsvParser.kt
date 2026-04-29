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
            CsvSample(
                ms = parts[0].toLong(),
                sample = parts[1].toLong(),
                dist = parts[2].toFloat(),
                iax = parts.getOrNull(3)?.toIntOrNull() ?: 0,
                iay = parts.getOrNull(4)?.toIntOrNull() ?: 0,
                iaz = parts.getOrNull(5)?.toIntOrNull() ?: 0,
                rax = parts.getOrNull(6)?.toIntOrNull() ?: 0,
                ray = parts.getOrNull(7)?.toIntOrNull() ?: 0,
                raz = parts.getOrNull(8)?.toIntOrNull() ?: 0,
                responderAcquisitionPeriodMs = parts.getOrNull(9)?.toIntOrNull(),
                initiatorAcquisitionPeriodMs = parts.getOrNull(10)?.toIntOrNull(),
                responderProfileOpt = parts.getOrNull(11)?.toIntOrNull(),
                initiatorProfileOpt = parts.getOrNull(12)?.toIntOrNull(),
            )
        } catch (_: NumberFormatException) {
            null
        }
    }
}
