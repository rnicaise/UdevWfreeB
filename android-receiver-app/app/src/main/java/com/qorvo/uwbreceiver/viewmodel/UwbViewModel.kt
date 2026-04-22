package com.qorvo.uwbreceiver.viewmodel

import android.app.Application
import android.content.Intent
import android.net.Uri
import androidx.core.content.ContextCompat
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.qorvo.uwbreceiver.data.RuntimeStore
import com.qorvo.uwbreceiver.data.SettingsStore
import com.qorvo.uwbreceiver.data.UwbUiState
import com.qorvo.uwbreceiver.service.UwbForegroundService
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

class UwbViewModel(app: Application) : AndroidViewModel(app) {
    private val settingsStore = SettingsStore(app)
    private val shareRequests = MutableStateFlow<Uri?>(null)

    private val ticker = flow {
        while (true) {
            emit(System.currentTimeMillis())
            delay(1_000)
        }
    }

    val uiState = combine(
        RuntimeStore.state,
        settingsStore.thresholds,
        ticker,
    ) { runtime, thresholds, _ ->
        val nowElapsed = android.os.SystemClock.elapsedRealtime()
        val elapsed = runtime.sessionStartElapsedMs?.let { (nowElapsed - it) / 1000 } ?: 0
        UwbUiState(
            runtime = runtime,
            thresholds = thresholds,
            elapsedSec = elapsed,
        )
    }.stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5_000),
        initialValue = UwbUiState(),
    )

    val shareUri = shareRequests.stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5_000),
        initialValue = null,
    )

    fun connect() {
        sendServiceAction(UwbForegroundService.ACTION_CONNECT)
    }

    fun disconnect() {
        sendServiceAction(UwbForegroundService.ACTION_DISCONNECT)
    }

    fun startRecording() {
        sendServiceAction(UwbForegroundService.ACTION_START_RECORDING)
    }

    fun stopRecording() {
        sendServiceAction(UwbForegroundService.ACTION_STOP_RECORDING)
    }

    fun updateGreenMax(value: Float) {
        viewModelScope.launch {
            settingsStore.updateGreenMax(value)
        }
    }

    fun updateOrangeMax(value: Float) {
        viewModelScope.launch {
            settingsStore.updateOrangeMax(value)
        }
    }

    fun requestShare(uri: Uri?) {
        shareRequests.value = uri
    }

    fun consumeShareRequest() {
        shareRequests.value = null
    }

    private fun sendServiceAction(action: String) {
        val context = getApplication<Application>()
        val intent = Intent(context, UwbForegroundService::class.java).apply {
            this.action = action
        }
        ContextCompat.startForegroundService(context, intent)
    }
}
