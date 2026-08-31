package com.qorvo.uwbreceiver.data

import android.net.Uri

/** One row of the pure UWB firmware CSV: ms,sample,dist,cppm,valid,dist_filt,dist_smooth,rx_fail */
data class CsvSample(
    val ms: Long,
    val sample: Long,
    val dist: Float,
    val cppm: Float?,
    val valid: Boolean,
    val distFilt: Float?,
    val distSmooth: Float?,
    val rxFail: Long?,
)

enum class LinkState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
}

data class RuntimeState(
    val linkState: LinkState = LinkState.DISCONNECTED,
    val status: String = "Idle",
    val latest: CsvSample? = null,
    val samples: Long = 0,
    val hz: Float = 0f,
    val invalidLines: Long = 0,
    val recording: Boolean = false,
    val recordingName: String? = null,
    val recordedRows: Long = 0,
    val lastCsvUri: Uri? = null,
    val analyzing: Boolean = false,
    val analysisText: String? = null,
    val lastReportUri: Uri? = null,
    val sessionStartElapsedMs: Long? = null,
)

data class UwbUiState(
    val runtime: RuntimeState = RuntimeState(),
    val elapsedSec: Long = 0,
)
