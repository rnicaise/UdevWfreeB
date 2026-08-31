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

    @Synchronized
    fun setLinkState(linkState: LinkState, status: String) {
        _state.value = _state.value.copy(linkState = linkState, status = status)
        if (linkState != LinkState.CONNECTED) {
            hzWindowStartElapsed = 0L
        }
    }

    @Synchronized
    fun onConnected(status: String = "Connected") {
        val now = SystemClock.elapsedRealtime()
        hzWindowStartElapsed = 0L
        _state.value = _state.value.copy(
            linkState = LinkState.CONNECTED,
            status = status,
            sessionStartElapsedMs = _state.value.sessionStartElapsedMs ?: now,
        )
    }

    /** Called at throttled rate (~10 Hz) by the service; hz is computed from firmware sample counter. */
    @Synchronized
    fun onSample(sample: CsvSample, recordedRows: Long) {
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
                hz = if (ds > 0) ds * 1000f / dt else 0f
                hzWindowStartElapsed = now
                hzWindowStartSample = sample.sample
            }
        }

        _state.value = current.copy(
            latest = sample,
            samples = sample.sample,
            hz = hz,
            recordedRows = recordedRows,
        )
    }

    @Synchronized
    fun onInvalidLine() {
        _state.value = _state.value.copy(invalidLines = _state.value.invalidLines + 1)
    }

    @Synchronized
    fun setRecording(active: Boolean, name: String? = null) {
        _state.value = _state.value.copy(
            recording = active,
            recordingName = name,
            recordedRows = if (active) 0 else _state.value.recordedRows,
            analysisText = if (active) null else _state.value.analysisText,
            lastReportUri = if (active) null else _state.value.lastReportUri,
        )
    }

    @Synchronized
    fun setLastCsvUri(uri: Uri?) {
        _state.value = _state.value.copy(lastCsvUri = uri)
    }

    @Synchronized
    fun setAnalyzing(active: Boolean) {
        _state.value = _state.value.copy(analyzing = active)
    }

    @Synchronized
    fun setAnalysisResult(text: String?, reportUri: Uri?) {
        _state.value = _state.value.copy(
            analyzing = false,
            analysisText = text,
            lastReportUri = reportUri,
        )
    }
}
