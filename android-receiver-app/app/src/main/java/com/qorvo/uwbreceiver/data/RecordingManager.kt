package com.qorvo.uwbreceiver.data

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import java.io.BufferedWriter
import java.io.OutputStreamWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** Records raw pure-CSV lines to Downloads/UWBReceiver. */
class RecordingManager(private val context: Context) {
    private var currentUri: Uri? = null
    private var writer: BufferedWriter? = null
    private var rowsSinceFlush = 0

    var rowCount: Long = 0
        private set

    fun start(): Pair<Uri, String> {
        currentUri?.let { return it to fileNameFromUri(it) }

        val timestamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        val fileName = "uwb-$timestamp.csv"

        val values = ContentValues().apply {
            put(MediaStore.Downloads.DISPLAY_NAME, fileName)
            put(MediaStore.Downloads.MIME_TYPE, "text/csv")
            put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS + "/UWBReceiver")
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                put(MediaStore.Downloads.IS_PENDING, 1)
            }
        }

        val uri = context.contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
            ?: error("Unable to create MediaStore entry")

        val output = context.contentResolver.openOutputStream(uri)
            ?: error("Unable to open output stream")

        writer = BufferedWriter(OutputStreamWriter(output), 64 * 1024).apply {
            appendLine("ms,sample,dist,cppm,valid,dist_filt,dist_smooth,rx_fail")
        }
        currentUri = uri
        rowCount = 0
        rowsSinceFlush = 0

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            values.clear()
            values.put(MediaStore.Downloads.IS_PENDING, 0)
            context.contentResolver.update(uri, values, null, null)
        }

        return uri to fileName
    }

    /** Appends the raw CSV line as received from the firmware. */
    fun appendLine(line: String) {
        val w = writer ?: return
        w.appendLine(line)
        rowCount++
        if (++rowsSinceFlush >= FLUSH_EVERY_ROWS) {
            rowsSinceFlush = 0
            w.flush()
        }
    }

    fun stop(): Uri? {
        try {
            writer?.flush()
            writer?.close()
        } catch (_: Exception) {
        }
        writer = null
        val uri = currentUri
        currentUri = null
        return uri
    }

    private fun fileNameFromUri(uri: Uri): String {
        return uri.lastPathSegment?.substringAfterLast('/') ?: "recording.csv"
    }

    companion object {
        private const val FLUSH_EVERY_ROWS = 1000
    }
}
