package com.qorvo.uwbreceiver.data

import android.net.Uri

data class CsvSample(
    val ms: Long,
    val sample: Long,
    val dist: Float,
    val iax: Int,
    val iay: Int,
    val iaz: Int,
    val rax: Int,
    val ray: Int,
    val raz: Int,
    val responderAcquisitionPeriodMs: Int? = null,
    val initiatorAcquisitionPeriodMs: Int? = null,
    val responderProfileOpt: Int? = null,
    val initiatorProfileOpt: Int? = null,
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
    val displayDist: Float? = null,
    val std5s: Float? = null,
    val std30s: Float? = null,
    val phoneTelemetry: PhoneTelemetry = PhoneTelemetry(),
    val samples: Long = 0,
    val hz: Float = 0f,
    val sessionStartElapsedMs: Long? = null,
    val recording: Boolean = false,
    val recordingName: String? = null,
    val lastSavedUri: Uri? = null,
    val invalidLines: Long = 0,
)

data class DistanceThresholds(
    val greenMax: Float = 1.0f,
    val orangeMax: Float = 2.0f,
)

data class UwbControlSettings(
    val medianWindow: Int = 5,
    val uwbDataRateKbps: Int = 6800,
    val acquisitionPeriodMs: Int = 20,
    val rangingMode: RangingMode = RangingMode.DS_TWR,
    val testProfile: TestProfile = TestProfile.STABLE_FULL,
)

enum class RangingMode {
    DS_TWR,
    SS_TWR,
}

enum class TestProfile {
    FAST_DISTANCE_ONLY,
    FAST_ACCEL_DECIMATED,
    STABLE_FULL,
    ROBUST_DETECTION,
    DIAGNOSTICS_FULL,
}

data class PhoneTelemetry(
    val gyroX: Float? = null,
    val gyroY: Float? = null,
    val gyroZ: Float? = null,
    val latitude: Double? = null,
    val longitude: Double? = null,
    val altitudeM: Double? = null,
    val speedMps: Float? = null,
    val fixElapsedMs: Long? = null,
)

data class UwbUiState(
    val runtime: RuntimeState = RuntimeState(),
    val thresholds: DistanceThresholds = DistanceThresholds(),
    val controls: UwbControlSettings = UwbControlSettings(),
    val elapsedSec: Long = 0,
)
