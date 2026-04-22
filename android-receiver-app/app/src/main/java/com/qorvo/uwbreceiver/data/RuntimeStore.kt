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
        val current = _state.value
        _state.value = current.copy(linkState = linkState, status = status)
        if (linkState != LinkState.CONNECTED) {
            hzWindowStartElapsed = 0L
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
    fun onSample(sample: CsvSample) {
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

        _state.value = current.copy(
            latest = sample,
            samples = sample.sample,
            hz = hz,
        )
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
