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
    val igx: Int = 0,
    val igy: Int = 0,
    val igz: Int = 0,
    val rgx: Int = 0,
    val rgy: Int = 0,
    val rgz: Int = 0,
    val rxPowerDbm: Float? = null,
    val firstPathPowerDbm: Float? = null,
    val clockOffsetPpm: Float? = null,
    val signalQuality10: Float? = null,
    val nlosQuality10: Float? = null,
    val peakToFirstPathSamples: Float? = null,
    val firstPathConfidence: Int? = null,
    val stsQuality: Int? = null,
    val responderAcquisitionPeriodMs: Int? = null,
    val initiatorAcquisitionPeriodMs: Int? = null,
    val responderProfileOpt: Int? = null,
    val initiatorProfileOpt: Int? = null,
    val firmwareValid: Boolean? = null,
    val firmwareDistFilt: Float? = null,
    val firmwareDistSmooth: Float? = null,
    val receiverLoadMv: Int? = null,
    val receiverLoadConnected: Boolean? = null,
)

enum class LinkState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
}

enum class LinkSource {
    NONE,
    USB,
    BLE_ADV,
}

enum class ConnectedUwbRole {
    UNKNOWN,
    INITIATOR,
    RESPONDER,
}

enum class SafetyArmMode {
    DISARMED,
    DISTANCE_2M,
    TILT_50_DEG,
}

data class SessionQuality(
    val speedSpikeCount: Long = 0,
    val frequencyAnomalyCount: Long = 0,
    val sessionStd: Float? = null,
    val sessionSamples: Long = 0,
    val lastRelativeSpeedMps: Float? = null,
    val lastInstantHz: Float? = null,
    val lastSignalQuality10: Float? = null,
    val meanSignalQuality10: Float? = null,
)

data class TransmissionQuality(
    val stabilityScore10: Float? = null,
    val smoothnessScore10: Float? = null,
    val timingScore10: Float? = null,
    val dropoutScore10: Float? = null,
    val rollingStd5sM: Float? = null,
    val lastDeltaM: Float? = null,
    val lastRelativeSpeedMps: Float? = null,
    val lastInstantHz: Float? = null,
    val timingAnomalyRate: Float? = null,
    val validRate5s: Float? = null,
    val jumpRate5s: Float? = null,
    val jumpScore10: Float? = null,
    val badBurstMax: Int = 0,
    val nlosScore10: Float? = null,
    val linkReliabilityScore10: Float? = null,
)

data class RuntimeState(
    val linkState: LinkState = LinkState.DISCONNECTED,
    val linkSource: LinkSource = LinkSource.NONE,
    val status: String = "Idle",
    val connectedRole: ConnectedUwbRole = ConnectedUwbRole.UNKNOWN,
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
    val safetyArmMode: SafetyArmMode = SafetyArmMode.DISARMED,
    val safetyArmStatus: String = "Disarmed",
    val sessionQuality: SessionQuality = SessionQuality(),
    val transmissionQuality: TransmissionQuality = TransmissionQuality(),
)

data class DistanceThresholds(
    val greenMax: Float = 1.0f,
    val orangeMax: Float = 2.0f,
)

data class UwbControlSettings(
    val medianWindow: Int = 1,
    val uwbDataRateKbps: Int = 6800,
    val rfChannel: Int = 5,
    val acquisitionPeriodMs: Int = 1,
    val rangingMode: RangingMode = RangingMode.SS_TWR,
    val testProfile: TestProfile = TestProfile.TURBO_DISTANCE_ONLY,
)

data class ExperimentSettings(
    val bikeBoxPosition: Int = 1,
    val vestBoxPosition: Int = 1,
)

enum class RangingMode {
    SS_TWR,
}

enum class TestProfile {
    TURBO_DISTANCE_ONLY,
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
    val experiment: ExperimentSettings = ExperimentSettings(),
    val elapsedSec: Long = 0,
)
