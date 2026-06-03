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
            appendLine("ms,sample,dist_raw,dist_filt,bike_box_position,vest_box_position,app_preset,app_test_profile,app_ranging_mode,app_rf_channel,app_uwb_data_rate_kbps,app_acq_period_ms,app_median_window,connected_role,link_reliability_10,stability_quality_10,smoothness_quality_10,timing_quality_10,dropout_quality_10,valid_rate_5s,jump_rate_5s,bad_burst_max,nlos_quality_10,rolling_std_5s_m,relative_speed_mps,instant_hz,timing_anomaly_rate,rx_power_dbm,fp_power_dbm,clock_offset_ppm,rx_quality_10,path_quality_10,multipath_quality_10,clock_quality_10,signal_quality_10,peak_to_fp_samples,fp_conf_level,sts_quality,iax,iay,iaz,rax,ray,raz,resp_acq_ms,init_acq_ms,resp_profile_opt,init_profile_opt,phone_gx,phone_gy,phone_gz,phone_lat,phone_lon,phone_alt_m,phone_speed_mps,phone_fix_elapsed_ms")
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

    fun appendEnrichedSample(
        sample: CsvSample,
        filteredDist: Float,
        phone: PhoneTelemetry,
        transmission: TransmissionQuality,
        controls: UwbControlSettings,
        experiment: ExperimentSettings,
        connectedRole: ConnectedUwbRole,
    ) {
        val w = writer ?: return
        val signal = SignalQualityCalculator.fromSample(sample)
        w.appendLine(buildString {
            append(sample.ms)
            append(',')
            append(sample.sample)
            append(',')
            append(sample.dist)
            append(',')
            append(filteredDist)
            append(',')
            append(experiment.bikeBoxPosition)
            append(',')
            append(experiment.vestBoxPosition)
            append(',')
            append(presetLabel(controls))
            append(',')
            append(controls.testProfile.name)
            append(',')
            append(controls.rangingMode.name)
            append(',')
            append(controls.rfChannel)
            append(',')
            append(controls.uwbDataRateKbps)
            append(',')
            append(controls.acquisitionPeriodMs)
            append(',')
            append(controls.medianWindow)
            append(',')
            append(connectedRole.name)
            append(',')
            append(transmission.linkReliabilityScore10?.toString() ?: "")
            append(',')
            append(transmission.stabilityScore10?.toString() ?: "")
            append(',')
            append(transmission.smoothnessScore10?.toString() ?: "")
            append(',')
            append(transmission.timingScore10?.toString() ?: "")
            append(',')
            append(transmission.dropoutScore10?.toString() ?: "")
            append(',')
            append(transmission.validRate5s?.toString() ?: "")
            append(',')
            append(transmission.jumpRate5s?.toString() ?: "")
            append(',')
            append(transmission.badBurstMax)
            append(',')
            append(transmission.nlosScore10?.toString() ?: "")
            append(',')
            append(transmission.rollingStd5sM?.toString() ?: "")
            append(',')
            append(transmission.lastRelativeSpeedMps?.toString() ?: "")
            append(',')
            append(transmission.lastInstantHz?.toString() ?: "")
            append(',')
            append(transmission.timingAnomalyRate?.toString() ?: "")
            append(',')
            append(sample.rxPowerDbm?.toString() ?: "")
            append(',')
            append(sample.firstPathPowerDbm?.toString() ?: "")
            append(',')
            append(sample.clockOffsetPpm?.toString() ?: "")
            append(',')
            append(signal.rxScore10?.toString() ?: "")
            append(',')
            append(signal.directPathScore10?.toString() ?: "")
            append(',')
            append(signal.multipathScore10?.toString() ?: "")
            append(',')
            append(signal.clockScore10?.toString() ?: "")
            append(',')
            append(sample.signalQuality10?.toString() ?: "")
            append(',')
            append(sample.peakToFirstPathSamples?.toString() ?: "")
            append(',')
            append(sample.firstPathConfidence?.toString() ?: "")
            append(',')
            append(sample.stsQuality?.toString() ?: "")
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

    private fun presetLabel(controls: UwbControlSettings): String {
        return when {
            controls.uwbDataRateKbps == 6800 && controls.acquisitionPeriodMs == 1 && controls.medianWindow == 1 && controls.testProfile == TestProfile.TURBO_DISTANCE_ONLY -> "turbo_experimental"
            controls.uwbDataRateKbps == 6800 && controls.acquisitionPeriodMs == 1 && controls.medianWindow == 1 && controls.testProfile == TestProfile.FAST_DISTANCE_ONLY -> "max_speed_safe"
            else -> "custom"
        }
    }
}
