package com.qorvo.uwbreceiver.viewmodel

import android.app.Application
import android.content.Intent
import android.net.Uri
import androidx.core.content.ContextCompat
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.qorvo.uwbreceiver.data.RuntimeStore
import com.qorvo.uwbreceiver.data.RangingMode
import com.qorvo.uwbreceiver.data.SettingsStore
import com.qorvo.uwbreceiver.data.TestProfile
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
        settingsStore.controls,
        ticker,
    ) { runtime, thresholds, controls, _ ->
        val nowElapsed = android.os.SystemClock.elapsedRealtime()
        val elapsed = runtime.sessionStartElapsedMs?.let { (nowElapsed - it) / 1000 } ?: 0
        UwbUiState(
            runtime = runtime,
            thresholds = thresholds,
            controls = controls,
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

    fun updateMedianWindow(value: Int) {
        viewModelScope.launch {
            settingsStore.updateMedianWindow(value)
        }
    }

    fun updateUwbDataRateKbps(value: Int) {
        viewModelScope.launch {
            settingsStore.updateUwbDataRateKbps(value)
        }
    }

    fun updateAcquisitionPeriodMs(value: Int) {
        viewModelScope.launch {
            settingsStore.updateAcquisitionPeriodMs(value)
        }
    }

    fun updateRangingMode(value: RangingMode) {
        viewModelScope.launch {
            settingsStore.updateRangingMode(value)
        }
    }

    fun applyTestProfile(value: TestProfile) {
        viewModelScope.launch {
            settingsStore.updateTestProfile(value)
            settingsStore.updateUwbDataRateKbps(6800)
            when (value) {
                TestProfile.TURBO_DISTANCE_ONLY -> {
                    settingsStore.updateMedianWindow(1)
                    settingsStore.updateAcquisitionPeriodMs(1)
                }
                TestProfile.FAST_DISTANCE_ONLY -> {
                    settingsStore.updateMedianWindow(1)
                    settingsStore.updateAcquisitionPeriodMs(1)
                }
                TestProfile.FAST_ACCEL_DECIMATED -> {
                    settingsStore.updateMedianWindow(3)
                    settingsStore.updateAcquisitionPeriodMs(1)
                }
                TestProfile.STABLE_FULL -> {
                    settingsStore.updateMedianWindow(5)
                    settingsStore.updateAcquisitionPeriodMs(20)
                }
                TestProfile.ROBUST_DETECTION -> {
                    settingsStore.updateMedianWindow(7)
                    settingsStore.updateAcquisitionPeriodMs(30)
                }
                TestProfile.DIAGNOSTICS_FULL -> {
                    settingsStore.updateMedianWindow(1)
                    settingsStore.updateAcquisitionPeriodMs(50)
                }
            }
            delay(200)
            sendServiceAction(UwbForegroundService.ACTION_APPLY_UWB_SETTINGS)
        }
    }

    fun applyUwbSettings() {
        sendServiceAction(UwbForegroundService.ACTION_APPLY_UWB_SETTINGS)
    }

    fun applyPreset20msStable() {
        viewModelScope.launch {
            settingsStore.updateTestProfile(TestProfile.STABLE_FULL)
            settingsStore.updateMedianWindow(5)
            settingsStore.updateUwbDataRateKbps(6800)
            settingsStore.updateAcquisitionPeriodMs(20)
            delay(200)
            sendServiceAction(UwbForegroundService.ACTION_APPLY_UWB_SETTINGS)
        }
    }

    fun applyPresetMaxSpeed() {
        viewModelScope.launch {
            settingsStore.updateTestProfile(TestProfile.FAST_DISTANCE_ONLY)
            settingsStore.updateMedianWindow(1)
            settingsStore.updateUwbDataRateKbps(6800)
            settingsStore.updateAcquisitionPeriodMs(1)
            delay(200)
            sendServiceAction(UwbForegroundService.ACTION_APPLY_UWB_SETTINGS)
        }
    }

    fun applyPresetOutdoorRobust() {
        viewModelScope.launch {
            settingsStore.updateTestProfile(TestProfile.ROBUST_DETECTION)
            settingsStore.updateMedianWindow(7)
            settingsStore.updateUwbDataRateKbps(6800)
            settingsStore.updateAcquisitionPeriodMs(30)
            delay(200)
            sendServiceAction(UwbForegroundService.ACTION_APPLY_UWB_SETTINGS)
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
