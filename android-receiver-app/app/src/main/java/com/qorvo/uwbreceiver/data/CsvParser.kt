package com.qorvo.uwbreceiver.data

/** Parses the pure UWB firmware CSV line (8 columns). */
object CsvParser {
    fun parse(line: String): CsvSample? {
        val parts = line.split(',')
        if (parts.size != 8) {
            return null
        }

        val ms = parts[0].toLongOrNull() ?: return null
        val sample = parts[1].toLongOrNull() ?: return null
        val dist = parts[2].toFloatOrNull() ?: return null
        if (ms < 0 || !dist.isFinite()) {
            return null
        }

        return CsvSample(
            ms = ms,
            sample = sample,
            dist = dist,
            cppm = parts[3].toFloatOrNull()?.takeIf { it.isFinite() },
            valid = (parts[4].toIntOrNull() ?: return null) != 0,
            distFilt = parts[5].toFloatOrNull()?.takeIf { it.isFinite() },
            distSmooth = parts[6].toFloatOrNull()?.takeIf { it.isFinite() },
            rxFail = parts[7].toLongOrNull(),
        )
    }
}
