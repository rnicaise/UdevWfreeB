package com.qorvo.uwbreceiver.data

import android.net.Uri
import android.os.SystemClock
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlin.math.abs
import kotlin.math.sqrt

object RuntimeStore {
    private const val SPEED_SPIKE_THRESHOLD_MPS = 10f
    private const val JUMP_DISTANCE_THRESHOLD_M = 0.75f
    private const val QUALITY_WINDOW_SAMPLES = 300
    private const val FREQUENCY_ANOMALY_THRESHOLD_HZ = 250f
    private const val MAX_REASONABLE_SAMPLE_DELTA = 10L
    private const val MAX_SPEED_DT_SEC = 1.0f

    private val _state = MutableStateFlow(RuntimeState())
    val state: StateFlow<RuntimeState> = _state.asStateFlow()

    private var hzWindowStartElapsed = 0L
    private var hzWindowStartSample = 0L
    private val distanceHistory = ArrayDeque<Pair<Long, Float>>()

    private var qualityPrevSample: CsvSample? = null
    private var qualityPrevRawDistance: Float? = null
    private var qualitySampleCount = 0L
    private var qualityMean = 0.0
    private var qualityM2 = 0.0
    private var qualitySpeedSpikeCount = 0L
    private var qualityFrequencyAnomalyCount = 0L
    private var qualityLastRelativeSpeedMps: Float? = null
    private var qualityLastInstantHz: Float? = null
    private var qualitySignalCount = 0L
    private var qualitySignalMean = 0.0
    private var livePrevSample: CsvSample? = null
    private var livePrevDisplayDistance: Float? = null
    private val liveTimingAnomalyWindow = ArrayDeque<Boolean>()
    private val liveJumpWindow = ArrayDeque<Boolean>()
    private val liveBadWindow = ArrayDeque<Boolean>()
    private val liveValidRateWindow = ArrayDeque<Float>()

    @Synchronized
    fun setLinkState(linkState: LinkState, status: String) {
        val current = _state.value
        _state.value = current.copy(
            linkState = linkState,
            status = status,
            connectedRole = if (linkState == LinkState.CONNECTED) current.connectedRole else ConnectedUwbRole.UNKNOWN,
        )
        if (linkState != LinkState.CONNECTED) {
            hzWindowStartElapsed = 0L
            distanceHistory.clear()
            resetLiveTransmissionQuality()
        }
    }

    @Synchronized
    fun onConnected(status: String = "Connected") {
        val now = SystemClock.elapsedRealtime()
        val current = _state.value
        hzWindowStartElapsed = now
        hzWindowStartSample = current.samples
        resetLiveTransmissionQuality()
        _state.value = current.copy(
            linkState = LinkState.CONNECTED,
            status = status,
            connectedRole = ConnectedUwbRole.UNKNOWN,
            sessionStartElapsedMs = current.sessionStartElapsedMs ?: now,
        )
    }

    @Synchronized
    fun setConnectedRole(role: ConnectedUwbRole) {
        val current = _state.value
        _state.value = current.copy(connectedRole = role)
    }

    @Synchronized
    fun onSample(sample: CsvSample, displayDist: Float, phoneTelemetry: PhoneTelemetry) {
        val now = SystemClock.elapsedRealtime()
        val current = _state.value
        var hz = current.hz

        if (hzWindowStartElapsed == 0L) {
            hzWindowStartElapsed = now
            hzWindowStartSample = sample.sample
        } else {
            val dt = now - hzWindowStartElapsed
            if (dt >= 1_000L) {
                val ds = sample.sample - hzWindowStartSample
                hz = if (dt > 0) ds * 1000f / dt else 0f
                hzWindowStartElapsed = now
                hzWindowStartSample = sample.sample
            }
        }

        distanceHistory.addLast(now to displayDist)
        while (distanceHistory.isNotEmpty() && (now - distanceHistory.first().first > 30_000L)) {
            distanceHistory.removeFirst()
        }

        val std5s = computeStd(now, 5_000L)
        val std30s = computeStd(now, 30_000L)
        val transmissionQuality = updateLiveTransmissionQuality(sample, displayDist, std5s)
        val sessionQuality = if (current.recording) {
            updateSessionQuality(sample, displayDist)
        } else {
            current.sessionQuality
        }

        _state.value = current.copy(
            latest = sample,
            displayDist = displayDist,
            std5s = std5s,
            std30s = std30s,
            phoneTelemetry = phoneTelemetry,
            samples = sample.sample,
            hz = hz,
            sessionQuality = sessionQuality,
            transmissionQuality = transmissionQuality,
        )
    }

    private fun updateLiveTransmissionQuality(sample: CsvSample, displayDist: Float, std5s: Float?): TransmissionQuality {
        val previous = livePrevSample
        val previousDisplayDistance = livePrevDisplayDistance
        var deltaM: Float? = null
        var relativeSpeed: Float? = null
        var instantHz: Float? = null
        var instantValidRate: Float? = null
        var timingAnomaly = false
        var jump = false

        if (previous != null && previousDisplayDistance != null) {
            val sampleDelta = sample.sample - previous.sample
            val msDelta = sample.ms - previous.ms
            deltaM = displayDist - previousDisplayDistance

            if (msDelta <= 0L || sampleDelta <= 0L || sampleDelta > MAX_REASONABLE_SAMPLE_DELTA) {
                timingAnomaly = true
            } else {
                val dtSec = msDelta / 1000f
                instantHz = sampleDelta / dtSec
                if (!instantHz.isFinite() || instantHz > FREQUENCY_ANOMALY_THRESHOLD_HZ || instantHz < 10f) {
                    timingAnomaly = true
                }
                if (instantHz.isFinite()) {
                    instantValidRate = (instantHz / expectedRoleHz()).coerceIn(0f, 1f)
                }

                if (dtSec > 0f && dtSec <= MAX_SPEED_DT_SEC) {
                    relativeSpeed = deltaM / dtSec
                }
            }

            jump = (relativeSpeed?.let { abs(it) > SPEED_SPIKE_THRESHOLD_MPS } == true) ||
                    (deltaM?.let { abs(it) > JUMP_DISTANCE_THRESHOLD_M } == true)

            liveTimingAnomalyWindow.addLast(timingAnomaly)
            liveJumpWindow.addLast(jump)
            liveBadWindow.addLast(timingAnomaly || jump)
            liveValidRateWindow.addLast(instantValidRate ?: 0f)
            while (liveTimingAnomalyWindow.size > QUALITY_WINDOW_SAMPLES) {
                liveTimingAnomalyWindow.removeFirst()
            }
            while (liveJumpWindow.size > QUALITY_WINDOW_SAMPLES) {
                liveJumpWindow.removeFirst()
            }
            while (liveBadWindow.size > QUALITY_WINDOW_SAMPLES) {
                liveBadWindow.removeFirst()
            }
            while (liveValidRateWindow.size > QUALITY_WINDOW_SAMPLES) {
                liveValidRateWindow.removeFirst()
            }
        }

        livePrevSample = sample
        livePrevDisplayDistance = displayDist

        val anomalyRate = if (liveTimingAnomalyWindow.isNotEmpty()) {
            liveTimingAnomalyWindow.count { it }.toFloat() / liveTimingAnomalyWindow.size.toFloat()
        } else {
            null
        }
        val jumpRate = if (liveJumpWindow.isNotEmpty()) {
            liveJumpWindow.count { it }.toFloat() / liveJumpWindow.size.toFloat()
        } else {
            null
        }
        val validRate = if (liveValidRateWindow.isNotEmpty()) {
            liveValidRateWindow.sum() / liveValidRateWindow.size.toFloat()
        } else {
            null
        }
        val jumpScore = jumpRate?.let { (10f - (it * 9f)).coerceIn(1f, 10f) }
        val burstMax = maxBurstLength(liveBadWindow)
        val burstScore = scoreLowIsGood(burstMax.toFloat(), goodValue = 0f, badValue = 10f)
        val validRateScore = validRate?.let { (it * 10f).coerceIn(1f, 10f) }
        val nlosScore = sample.nlosQuality10
        val stabilityScore = scoreLowIsGood(std5s, goodValue = 0.05f, badValue = 0.50f)
        val smoothnessScore = scoreLowIsGood(relativeSpeed?.let { abs(it) }, goodValue = 0.5f, badValue = 8.0f)
        val timingScore = scoreTimingHz(instantHz)
        val dropoutScore = anomalyRate?.let { (10f - (it * 9f)).coerceIn(1f, 10f) }

        return TransmissionQuality(
            stabilityScore10 = stabilityScore,
            smoothnessScore10 = smoothnessScore,
            timingScore10 = timingScore,
            dropoutScore10 = dropoutScore,
            rollingStd5sM = std5s,
            lastDeltaM = deltaM,
            lastRelativeSpeedMps = relativeSpeed,
            lastInstantHz = instantHz,
            timingAnomalyRate = anomalyRate,
            validRate5s = validRate,
            jumpRate5s = jumpRate,
            jumpScore10 = jumpScore,
            badBurstMax = burstMax,
            nlosScore10 = nlosScore,
            linkReliabilityScore10 = weightedAverageScore(
                listOf(
                    stabilityScore to 0.25f,
                    smoothnessScore to 0.15f,
                    validRateScore to 0.15f,
                    jumpScore to 0.20f,
                    burstScore to 0.10f,
                    nlosScore to 0.15f,
                )
            ),
        )
    }

    private fun maxBurstLength(values: Iterable<Boolean>): Int {
        var current = 0
        var max = 0
        for (bad in values) {
            if (bad) {
                current++
                if (current > max) {
                    max = current
                }
            } else {
                current = 0
            }
        }
        return max
    }

    private fun weightedAverageScore(values: List<Pair<Float?, Float>>): Float? {
        var weighted = 0f
        var weights = 0f
        for ((value, weight) in values) {
            if (value != null && value.isFinite()) {
                weighted += value * weight
                weights += weight
            }
        }
        return if (weights > 0f) (weighted / weights).coerceIn(1f, 10f) else null
    }

    private fun scoreLowIsGood(value: Float?, goodValue: Float, badValue: Float): Float? {
        if (value == null || !value.isFinite()) {
            return null
        }
        return when {
            value <= goodValue -> 10f
            value >= badValue -> 1f
            else -> 10f - ((value - goodValue) * 9f / (badValue - goodValue))
        }.coerceIn(1f, 10f)
    }

    private fun scoreTimingHz(value: Float?): Float? {
        if (value == null || !value.isFinite()) {
            return null
        }
        return when {
            value in 40f..180f -> 10f
            value <= 10f || value >= 260f -> 1f
            value < 40f -> 1f + ((value - 10f) * 9f / 30f)
            else -> 10f - ((value - 180f) * 9f / 80f)
        }.coerceIn(1f, 10f)
    }

    private fun expectedRoleHz(): Float {
        return when (_state.value.connectedRole) {
            ConnectedUwbRole.INITIATOR -> 120f
            ConnectedUwbRole.RESPONDER -> 55f
            ConnectedUwbRole.UNKNOWN -> 120f
        }
    }

    private fun resetLiveTransmissionQuality() {
        livePrevSample = null
        livePrevDisplayDistance = null
        liveTimingAnomalyWindow.clear()
        liveJumpWindow.clear()
        liveBadWindow.clear()
        liveValidRateWindow.clear()
    }

    private fun updateSessionQuality(sample: CsvSample, displayDist: Float): SessionQuality {
        qualitySampleCount++

        sample.signalQuality10?.let { score ->
            qualitySignalCount++
            qualitySignalMean += (score.toDouble() - qualitySignalMean) / qualitySignalCount.toDouble()
        }

        val value = displayDist.toDouble()
        val delta = value - qualityMean
        qualityMean += delta / qualitySampleCount.toDouble()
        val delta2 = value - qualityMean
        qualityM2 += delta * delta2

        val previous = qualityPrevSample
        val previousRawDistance = qualityPrevRawDistance
        if (previous != null && previousRawDistance != null) {
            val sampleDelta = sample.sample - previous.sample
            val msDelta = sample.ms - previous.ms
            var frequencyAnomaly = false

            if (msDelta <= 0L || sampleDelta <= 0L || sampleDelta > MAX_REASONABLE_SAMPLE_DELTA) {
                frequencyAnomaly = true
            } else {
                val dtSec = msDelta / 1000f
                val instantHz = sampleDelta / dtSec
                if (instantHz.isFinite()) {
                    qualityLastInstantHz = instantHz
                    if (instantHz > FREQUENCY_ANOMALY_THRESHOLD_HZ) {
                        frequencyAnomaly = true
                    }
                } else {
                    frequencyAnomaly = true
                }
            }

            if (frequencyAnomaly) {
                qualityFrequencyAnomalyCount++
            }

            if (msDelta > 0L && sampleDelta > 0L && sampleDelta <= MAX_REASONABLE_SAMPLE_DELTA) {
                val dtSec = msDelta / 1000f
                if (dtSec > 0f && dtSec <= MAX_SPEED_DT_SEC) {
                    val relativeSpeed = (sample.dist - previousRawDistance) / dtSec
                    if (relativeSpeed.isFinite()) {
                        qualityLastRelativeSpeedMps = relativeSpeed
                        if (abs(relativeSpeed) > SPEED_SPIKE_THRESHOLD_MPS) {
                            qualitySpeedSpikeCount++
                        }
                    }
                }
            }
        }

        qualityPrevSample = sample
        qualityPrevRawDistance = sample.dist

        val sessionStd = if (qualitySampleCount > 1L) {
            sqrt((qualityM2 / qualitySampleCount.toDouble()).coerceAtLeast(0.0)).toFloat()
        } else {
            null
        }

        return SessionQuality(
            speedSpikeCount = qualitySpeedSpikeCount,
            frequencyAnomalyCount = qualityFrequencyAnomalyCount,
            sessionStd = sessionStd,
            sessionSamples = qualitySampleCount,
            lastRelativeSpeedMps = qualityLastRelativeSpeedMps,
            lastInstantHz = qualityLastInstantHz,
            lastSignalQuality10 = sample.signalQuality10,
            meanSignalQuality10 = if (qualitySignalCount > 0L) qualitySignalMean.toFloat() else null,
        )
    }

    private fun computeStd(now: Long, windowMs: Long): Float? {
        var n = 0
        var sum = 0.0
        var sumSq = 0.0

        for ((ts, value) in distanceHistory) {
            if (now - ts <= windowMs) {
                val v = value.toDouble()
                n++
                sum += v
                sumSq += v * v
            }
        }

        if (n < 2) {
            return null
        }

        val mean = sum / n
        var variance = (sumSq / n) - (mean * mean)
        if (variance < 0.0) {
            variance = 0.0
        }
        return sqrt(variance).toFloat()
    }

    @Synchronized
    fun onInvalidLine() {
        val current = _state.value
        _state.value = current.copy(invalidLines = current.invalidLines + 1)
    }

    @Synchronized
    fun setRecording(active: Boolean, name: String? = null) {
        val current = _state.value
        _state.value = current.copy(
            recording = active,
            recordingName = name,
            sessionQuality = resetSessionQuality(),
        )
    }

    private fun resetSessionQuality(): SessionQuality {
        qualityPrevSample = null
        qualityPrevRawDistance = null
        qualitySampleCount = 0L
        qualityMean = 0.0
        qualityM2 = 0.0
        qualitySpeedSpikeCount = 0L
        qualityFrequencyAnomalyCount = 0L
        qualityLastRelativeSpeedMps = null
        qualityLastInstantHz = null
        qualitySignalCount = 0L
        qualitySignalMean = 0.0
        return SessionQuality()
    }

    @Synchronized
    fun setLastSavedUri(uri: Uri?) {
        val current = _state.value
        _state.value = current.copy(lastSavedUri = uri)
    }
}
