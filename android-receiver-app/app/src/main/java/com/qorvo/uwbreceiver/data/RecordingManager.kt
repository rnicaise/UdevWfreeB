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
            appendLine("ms,sample,dist_raw,dist_filt,iax,iay,iaz,rax,ray,raz,resp_acq_ms,init_acq_ms,resp_profile_opt,init_profile_opt,phone_gx,phone_gy,phone_gz,phone_lat,phone_lon,phone_alt_m,phone_speed_mps,phone_fix_elapsed_ms")
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

    fun appendEnrichedSample(sample: CsvSample, filteredDist: Float, phone: PhoneTelemetry) {
        val w = writer ?: return
        w.appendLine(buildString {
            append(sample.ms)
            append(',')
            append(sample.sample)
            append(',')
            append(sample.dist)
            append(',')
            append(filteredDist)
            append(',')
            append(sample.iax)
            append(',')
            append(sample.iay)
            append(',')
            append(sample.iaz)
            append(',')
            append(sample.rax)
            append(',')
            append(sample.ray)
            append(',')
            append(sample.raz)
            append(',')
            append(sample.responderAcquisitionPeriodMs?.toString() ?: "")
            append(',')
            append(sample.initiatorAcquisitionPeriodMs?.toString() ?: "")
            append(',')
            append(sample.responderProfileOpt?.toString() ?: "")
            append(',')
            append(sample.initiatorProfileOpt?.toString() ?: "")
            append(',')
            append(phone.gyroX?.toString() ?: "")
            append(',')
            append(phone.gyroY?.toString() ?: "")
            append(',')
            append(phone.gyroZ?.toString() ?: "")
            append(',')
            append(phone.latitude?.toString() ?: "")
            append(',')
            append(phone.longitude?.toString() ?: "")
            append(',')
            append(phone.altitudeM?.toString() ?: "")
            append(',')
            append(phone.speedMps?.toString() ?: "")
            append(',')
            append(phone.fixElapsedMs?.toString() ?: "")
        })
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
