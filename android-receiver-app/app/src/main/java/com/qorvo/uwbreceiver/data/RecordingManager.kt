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

class RecordingManager(private val context: Context) {
    private var currentUri: Uri? = null
    private var writer: BufferedWriter? = null

    fun start(): Pair<Uri, String> {
        if (writer != null && currentUri != null) {
            return currentUri!! to fileNameFromUri(currentUri!!)
        }

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

        writer = BufferedWriter(OutputStreamWriter(output)).apply {
            appendLine("ms,sample,dist,iax,iay,iaz,rax,ray,raz")
            flush()
        }
        currentUri = uri

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            values.clear()
            values.put(MediaStore.Downloads.IS_PENDING, 0)
            context.contentResolver.update(uri, values, null, null)
        }

        return uri to fileName
    }

    fun appendRawLine(line: String) {
        val w = writer ?: return
        w.appendLine(line)
        w.flush()
    }

    fun stop(): Uri? {
        writer?.flush()
        writer?.close()
        writer = null
        val uri = currentUri
        currentUri = null
        return uri
    }

    private fun fileNameFromUri(uri: Uri): String {
        return uri.lastPathSegment?.substringAfterLast('/') ?: "recording.csv"
    }
}
