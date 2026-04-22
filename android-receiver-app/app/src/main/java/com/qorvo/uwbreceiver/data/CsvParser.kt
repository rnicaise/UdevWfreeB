package com.qorvo.uwbreceiver.data

object CsvParser {
    fun parse(line: String): CsvSample? {
        val trimmed = line.trim()
        if (trimmed.isEmpty() || trimmed.startsWith("#")) {
            return null
        }

        val parts = trimmed.split(',')
        if (parts.size < 9) {
            return null
        }

        return try {
            CsvSample(
                ms = parts[0].toLong(),
                sample = parts[1].toLong(),
                dist = parts[2].toFloat(),
                iax = parts[3].toInt(),
                iay = parts[4].toInt(),
                iaz = parts[5].toInt(),
                rax = parts[6].toInt(),
                ray = parts[7].toInt(),
                raz = parts[8].toInt(),
            )
        } catch (_: NumberFormatException) {
            null
        }
    }
}
