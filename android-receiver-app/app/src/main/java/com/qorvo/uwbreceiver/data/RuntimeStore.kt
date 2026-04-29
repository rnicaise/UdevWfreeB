package com.qorvo.uwbreceiver.data

import android.net.Uri
import android.os.SystemClock
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

object RuntimeStore {
    private val _state = MutableStateFlow(RuntimeState())
    val state: StateFlow<RuntimeState> = _state.asStateFlow()

    private var hzWindowStartElapsed = 0L
    private var hzWindowStartSample = 0L
    private val distanceHistory = ArrayDeque<Pair<Long, Float>>()

    @Synchronized
    fun setLinkState(linkState: LinkState, status: String) {
        val current = _state.value
        _state.value = current.copy(linkState = linkState, status = status)
        if (linkState != LinkState.CONNECTED) {
            hzWindowStartElapsed = 0L
            distanceHistory.clear()
        }
    }

    @Synchronized
    fun onConnected(status: String = "Connected") {
        val now = SystemClock.elapsedRealtime()
        val current = _state.value
        hzWindowStartElapsed = now
        hzWindowStartSample = current.samples
        _state.value = current.copy(
            linkState = LinkState.CONNECTED,
            status = status,
            sessionStartElapsedMs = current.sessionStartElapsedMs ?: now,
        )
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

        _state.value = current.copy(
            latest = sample,
            displayDist = displayDist,
            std5s = std5s,
            std30s = std30s,
            phoneTelemetry = phoneTelemetry,
            samples = sample.sample,
            hz = hz,
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
        return kotlin.math.sqrt(variance).toFloat()
    }

    @Synchronized
    fun onInvalidLine() {
        val current = _state.value
        _state.value = current.copy(invalidLines = current.invalidLines + 1)
    }

    @Synchronized
    fun setRecording(active: Boolean, name: String? = null) {
        val current = _state.value
        _state.value = current.copy(recording = active, recordingName = name)
    }

    @Synchronized
    fun setLastSavedUri(uri: Uri?) {
        val current = _state.value
        _state.value = current.copy(lastSavedUri = uri)
    }
}
