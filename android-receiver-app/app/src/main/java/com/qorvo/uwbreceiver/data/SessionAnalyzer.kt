package com.qorvo.uwbreceiver.data

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import java.io.BufferedReader
import java.io.InputStreamReader
import java.util.Locale
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.floor
import kotlin.math.sqrt

/**
 * Post-acquisition quality analysis of a recorded pure-CSV file.
 * Kotlin port of tools/uwb_quality_capture.py metrics.
 */
object SessionAnalyzer {

    data class Result(val text: String, val reportUri: Uri?)

    private class Series {
        val ms = ArrayList<Long>(200_000)
        val sample = ArrayList<Long>(200_000)
        val dist = ArrayList<Double>(200_000)
        val cppm = ArrayList<Double>(200_000)
        val valid = ArrayList<Boolean>(200_000)
        val distFilt = ArrayList<Double>(200_000)
        val distSmooth = ArrayList<Double>(200_000)
        val rxFail = ArrayList<Long>(200_000)
    }

    private data class StatsBlock(
        val count: Int,
        val mean: Double,
        val median: Double,
        val std: Double,
        val mad: Double,
        val min: Double,
        val max: Double,
        val p05: Double,
        val p95: Double,
        val iqr: Double,
        val peakToPeak: Double,
    ) {
        companion object {
            val EMPTY = StatsBlock(0, Double.NaN, Double.NaN, Double.NaN, Double.NaN, Double.NaN, Double.NaN, Double.NaN, Double.NaN, Double.NaN, Double.NaN)
        }
    }

    /** Reads the recorded CSV back from MediaStore, computes metrics, writes the report next to it. */
    fun analyze(context: Context, csvUri: Uri, csvName: String?): Result {
        val series = readCsv(context, csvUri)
        if (series.dist.isEmpty()) {
            return Result("Aucun echantillon dans l'enregistrement.", null)
        }

        val text = summarize(series, csvName ?: "recording.csv")
        val reportUri = writeReport(context, csvName, text)
        return Result(text, reportUri)
    }

    private fun readCsv(context: Context, uri: Uri): Series {
        val series = Series()
        context.contentResolver.openInputStream(uri)?.use { input ->
            BufferedReader(InputStreamReader(input), 64 * 1024).forEachLine { line ->
                if (line.isEmpty() || !line[0].isDigit()) {
                    return@forEachLine
                }
                val s = CsvParser.parse(line) ?: return@forEachLine
                series.ms.add(s.ms)
                series.sample.add(s.sample)
                series.dist.add(s.dist.toDouble())
                s.cppm?.let { series.cppm.add(it.toDouble()) }
                series.valid.add(s.valid)
                s.distFilt?.let { series.distFilt.add(it.toDouble()) }
                s.distSmooth?.let { series.distSmooth.add(it.toDouble()) }
                s.rxFail?.let { series.rxFail.add(it) }
            }
        }
        return series
    }

    private fun summarize(series: Series, csvName: String): String {
        val total = series.dist.size

        // Rebuild a monotonic ms timeline. Firmware ms is monotonic since the
        // RTC wrap fix, so any backward jump is a reboot and any implausible
        // jump (forward or backward) is a corrupted serial line that still
        // parsed. Plausibility is checked against the sample-counter delta
        // (~1 ms/sample, generous 100x margin).
        val msU = ArrayList<Long>(total)
        var msOffset = 0L
        var msGlitches = 0
        var reboots = 0
        var lastDt = 1L
        for (i in 0 until total) {
            val raw = series.ms[i]
            if (i > 0) {
                val prevRaw = series.ms[i - 1]
                val dt = raw - prevRaw
                val sampleDelta = series.sample[i] - series.sample[i - 1]
                val rebooted = sampleDelta < 0
                val maxPlausibleDt = 100L * (if (sampleDelta > 0) sampleDelta else 1L) + 1000L
                if (rebooted || dt < 0 || dt > maxPlausibleDt) {
                    if (rebooted) reboots++ else msGlitches++
                    msOffset = msU[i - 1] + lastDt - raw
                } else if (dt > 0) {
                    lastDt = dt
                }
            }
            msU.add(raw + msOffset)
        }

        val validDist = ArrayList<Double>(total)
        val validMs = ArrayList<Long>(total)
        for (i in 0 until total) {
            if (series.valid[i]) {
                validDist.add(series.dist[i])
                validMs.add(msU[i])
            }
        }

        val fwSpanMs = if (total >= 2) msU.last() - msU.first() else 0L
        var fwSpanSamples = 0L
        for (i in 1 until total) {
            val d = series.sample[i] - series.sample[i - 1]
            if (d > 0) fwSpanSamples += d
        }
        val firmwareHz = if (fwSpanMs > 0) fwSpanSamples * 1000.0 / fwSpanMs else Double.NaN
        val rxFailDelta = if (series.rxFail.size >= 2) series.rxFail.last() - series.rxFail.first() else null

        val dist = statsBlock(validDist)
        val filt = statsBlock(series.distFilt)
        val smooth = statsBlock(series.distSmooth)
        val cppm = statsBlock(series.cppm)
        val jumps = jumpStats(validDist)
        val corr = if (series.cppm.size == total) pearson(series.dist, series.cppm) else Double.NaN

        val sb = StringBuilder()
        sb.appendLine("UWB quality report")
        sb.appendLine("==================")
        sb.appendLine("csv: $csvName")
        sb.appendLine()
        sb.appendLine("Acquisition")
        sb.appendLine("-----------")
        sb.appendLine("samples: $total")
        sb.appendLine("valid: ${validDist.size} (${fmt(100.0 * validDist.size / total, 2)}%)")
        sb.appendLine("firmware_hz: ${fmt(firmwareHz, 1)}")
        sb.appendLine("firmware_span_ms: $fwSpanMs")
        sb.appendLine("ms_glitches: $msGlitches")
        sb.appendLine("reboots: $reboots")
        sb.appendLine("rx_fail_delta: ${rxFailDelta ?: "n/a"}")
        sb.appendLine()
        sb.appendLine("Distance brute valide")
        sb.appendLine("---------------------")
        sb.appendLine("mean: ${fmtM(dist.mean)}")
        sb.appendLine("median: ${fmtM(dist.median)}")
        sb.appendLine("std_sigma: ${fmtCm(dist.std)}")
        sb.appendLine("mad: ${fmtCm(dist.mad)}")
        sb.appendLine("p05/p50/p95: ${fmtM(dist.p05)} / ${fmtM(dist.median)} / ${fmtM(dist.p95)}")
        sb.appendLine("iqr: ${fmtCm(dist.iqr)}")
        sb.appendLine("min/max: ${fmtM(dist.min)} / ${fmtM(dist.max)}")
        sb.appendLine("peak_to_peak: ${fmtCm(dist.peakToPeak)}")
        sb.appendLine()
        sb.appendLine("Filtres")
        sb.appendLine("-------")
        sb.appendLine("dist_filt sigma: ${fmtCm(filt.std)}, p05/p95: ${fmtM(filt.p05)} / ${fmtM(filt.p95)}")
        sb.appendLine("dist_smooth sigma: ${fmtCm(smooth.std)}, p05/p95: ${fmtM(smooth.p05)} / ${fmtM(smooth.p95)}")
        sb.appendLine()
        sb.appendLine("Jumps sample-a-sample")
        sb.appendLine("---------------------")
        sb.appendLine("mean_abs: ${fmtCm(jumps.meanAbs)}")
        sb.appendLine("p95_abs: ${fmtCm(jumps.p95Abs)}")
        sb.appendLine("max_abs: ${fmtCm(jumps.maxAbs)}")
        sb.appendLine("jumps_gt_10cm: ${jumps.gt10cm}")
        sb.appendLine("jumps_gt_20cm: ${jumps.gt20cm}")
        sb.appendLine()
        sb.appendLine("Clock offset")
        sb.appendLine("------------")
        sb.appendLine("cppm mean: ${fmt(cppm.mean, 3)}")
        sb.appendLine("cppm sigma: ${fmt(cppm.std, 3)}")
        sb.appendLine("corr_dist_cppm: ${fmt(corr, 4)}")
        sb.appendLine()
        sb.appendLine("Derive par fenetre")
        sb.appendLine("------------------")
        for (windowS in doubleArrayOf(1.0, 5.0, 10.0)) {
            val w = windowStats(validMs, validDist, windowS)
            sb.appendLine(
                "${windowS.toInt()}s: windows=${w.windows} mean_window_std=${fmtCm(w.meanWindowStd)} " +
                    "std_of_means=${fmtCm(w.stdOfMeans)} drift_pp=${fmtCm(w.driftPeakToPeak)}"
            )
        }
        sb.appendLine()
        sb.appendLine("Allan deviation distance")
        sb.appendLine("------------------------")
        for (tau in doubleArrayOf(0.01, 0.1, 1.0, 5.0)) {
            sb.appendLine("tau=${tau}s: ${fmtCm(allanDeviation(validMs, validDist, tau))}")
        }
        return sb.toString()
    }

    // --- metric helpers -------------------------------------------------

    private fun statsBlock(values: List<Double>): StatsBlock {
        val finite = values.filter { it.isFinite() }
        if (finite.isEmpty()) {
            return StatsBlock.EMPTY
        }
        val sorted = finite.sorted()
        val median = percentile(sorted, 0.50)
        val deviations = finite.map { abs(it - median) }.sorted()
        val p25 = percentile(sorted, 0.25)
        val p75 = percentile(sorted, 0.75)
        val mean = finite.average()
        val variance = finite.sumOf { (it - mean) * (it - mean) } / finite.size
        return StatsBlock(
            count = finite.size,
            mean = mean,
            median = median,
            std = sqrt(variance),
            mad = percentile(deviations, 0.50),
            min = sorted.first(),
            max = sorted.last(),
            p05 = percentile(sorted, 0.05),
            p95 = percentile(sorted, 0.95),
            iqr = p75 - p25,
            peakToPeak = sorted.last() - sorted.first(),
        )
    }

    private fun percentile(sorted: List<Double>, pct: Double): Double {
        if (sorted.isEmpty()) return Double.NaN
        if (sorted.size == 1) return sorted[0]
        val pos = (sorted.size - 1) * pct
        val lo = floor(pos).toInt()
        val hi = ceil(pos).toInt()
        if (lo == hi) return sorted[lo]
        val weight = pos - lo
        return sorted[lo] * (1.0 - weight) + sorted[hi] * weight
    }

    private data class JumpStats(
        val meanAbs: Double,
        val p95Abs: Double,
        val maxAbs: Double,
        val gt10cm: Int,
        val gt20cm: Int,
    )

    private fun jumpStats(values: List<Double>): JumpStats {
        if (values.size < 2) {
            return JumpStats(Double.NaN, Double.NaN, Double.NaN, 0, 0)
        }
        val jumps = ArrayList<Double>(values.size - 1)
        for (i in 1 until values.size) {
            jumps.add(abs(values[i] - values[i - 1]))
        }
        val sorted = jumps.sorted()
        return JumpStats(
            meanAbs = jumps.average(),
            p95Abs = percentile(sorted, 0.95),
            maxAbs = sorted.last(),
            gt10cm = jumps.count { it > 0.10 },
            gt20cm = jumps.count { it > 0.20 },
        )
    }

    private data class WindowStats(
        val windows: Int,
        val meanWindowStd: Double,
        val stdOfMeans: Double,
        val driftPeakToPeak: Double,
    )

    private fun windowStats(ms: List<Long>, values: List<Double>, windowS: Double): WindowStats {
        val buckets = HashMap<Long, ArrayList<Double>>()
        for (i in values.indices) {
            val bucket = ((ms[i] / 1000.0) / windowS).toLong()
            buckets.getOrPut(bucket) { ArrayList() }.add(values[i])
        }
        val means = buckets.values.filter { it.isNotEmpty() }.map { it.average() }
        val stds = buckets.values.filter { it.size > 1 }.map { list ->
            val m = list.average()
            sqrt(list.sumOf { (it - m) * (it - m) } / list.size)
        }
        val stdOfMeans = if (means.size > 1) {
            val m = means.average()
            sqrt(means.sumOf { (it - m) * (it - m) } / means.size)
        } else if (means.isNotEmpty()) 0.0 else Double.NaN
        return WindowStats(
            windows = means.size,
            meanWindowStd = if (stds.isNotEmpty()) stds.average() else Double.NaN,
            stdOfMeans = stdOfMeans,
            driftPeakToPeak = if (means.isNotEmpty()) (means.max() - means.min()) else Double.NaN,
        )
    }

    private fun allanDeviation(ms: List<Long>, values: List<Double>, tauS: Double): Double {
        if (values.isEmpty()) return Double.NaN
        val startS = ms.first() / 1000.0
        val buckets = sortedMapOf<Long, ArrayList<Double>>()
        for (i in values.indices) {
            val bucket = ((ms[i] / 1000.0 - startS) / tauS).toLong()
            buckets.getOrPut(bucket) { ArrayList() }.add(values[i])
        }
        val means = buckets.values.filter { it.isNotEmpty() }.map { it.average() }
        if (means.size < 2) return Double.NaN
        var sum = 0.0
        for (i in 1 until means.size) {
            val d = means[i] - means[i - 1]
            sum += d * d
        }
        return sqrt(0.5 * sum / (means.size - 1))
    }

    private fun pearson(xs: List<Double>, ys: List<Double>): Double {
        if (xs.size != ys.size || xs.size < 2) return Double.NaN
        val meanX = xs.average()
        val meanY = ys.average()
        var num = 0.0
        var denX = 0.0
        var denY = 0.0
        for (i in xs.indices) {
            val dx = xs[i] - meanX
            val dy = ys[i] - meanY
            num += dx * dy
            denX += dx * dx
            denY += dy * dy
        }
        if (denX == 0.0 || denY == 0.0) return Double.NaN
        return num / (sqrt(denX) * sqrt(denY))
    }

    // --- formatting + report file ----------------------------------------

    private fun fmt(value: Double, digits: Int): String {
        return if (value.isFinite()) String.format(Locale.US, "%.${digits}f", value) else "nan"
    }

    private fun fmtM(value: Double): String {
        return if (value.isFinite()) String.format(Locale.US, "%.4f m", value) else "nan"
    }

    private fun fmtCm(value: Double): String {
        return if (value.isFinite()) String.format(Locale.US, "%.2f cm", value * 100.0) else "nan"
    }

    private fun writeReport(context: Context, csvName: String?, text: String): Uri? {
        val base = csvName?.removeSuffix(".csv") ?: "recording"
        val fileName = "$base-summary.txt"

        return try {
            val values = ContentValues().apply {
                put(MediaStore.Downloads.DISPLAY_NAME, fileName)
                put(MediaStore.Downloads.MIME_TYPE, "text/plain")
                put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS + "/UWBReceiver")
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    put(MediaStore.Downloads.IS_PENDING, 1)
                }
            }
            val uri = context.contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
                ?: return null
            context.contentResolver.openOutputStream(uri)?.use { out ->
                out.write(text.toByteArray(Charsets.UTF_8))
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                values.clear()
                values.put(MediaStore.Downloads.IS_PENDING, 0)
                context.contentResolver.update(uri, values, null, null)
            }
            uri
        } catch (_: Exception) {
            null
        }
    }
}
