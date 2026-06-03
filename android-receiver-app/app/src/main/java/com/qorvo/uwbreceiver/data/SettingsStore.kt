package com.qorvo.uwbreceiver.data

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.floatPreferencesKey
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map

private val Context.dataStore: DataStore<Preferences> by preferencesDataStore(name = "uwb_settings")

class SettingsStore(private val context: Context) {
    private val keyGreen = floatPreferencesKey("threshold_green_max")
    private val keyOrange = floatPreferencesKey("threshold_orange_max")
    private val keyMedianWindow = intPreferencesKey("median_window")
    private val keyUwbDataRateKbps = intPreferencesKey("uwb_data_rate_kbps")
    private val keyRfChannel = intPreferencesKey("rf_channel")
    private val keyAcquisitionPeriodMs = intPreferencesKey("acquisition_period_ms")
    private val keyRangingMode = intPreferencesKey("ranging_mode")
    private val keyTestProfile = intPreferencesKey("test_profile")
    private val keyBikeBoxPosition = intPreferencesKey("bike_box_position")
    private val keyVestBoxPosition = intPreferencesKey("vest_box_position")

    val thresholds: Flow<DistanceThresholds> = context.dataStore.data.map { pref ->
        DistanceThresholds(
            greenMax = pref[keyGreen] ?: 1.0f,
            orangeMax = pref[keyOrange] ?: 2.0f,
        )
    }

    val controls: Flow<UwbControlSettings> = context.dataStore.data.map { pref ->
        val testProfileRaw = pref[keyTestProfile] ?: TestProfile.TURBO_DISTANCE_ONLY.ordinal
        val testProfileSupported = testProfileRaw in TestProfile.entries.indices
        val testProfile = TestProfile.entries.getOrElse(testProfileRaw) { TestProfile.TURBO_DISTANCE_ONLY }
        UwbControlSettings(
            medianWindow = if (testProfileSupported) (pref[keyMedianWindow] ?: 1).coerceIn(1, 31) else 1,
            uwbDataRateKbps = 6800,
            rfChannel = sanitizeRfChannel(pref[keyRfChannel] ?: 5),
            acquisitionPeriodMs = if (testProfileSupported) (pref[keyAcquisitionPeriodMs] ?: 1).coerceIn(1, 200) else 1,
            rangingMode = RangingMode.SS_TWR,
            testProfile = testProfile,
        )
    }

    val experiment: Flow<ExperimentSettings> = context.dataStore.data.map { pref ->
        ExperimentSettings(
            bikeBoxPosition = sanitizeBoxPosition(pref[keyBikeBoxPosition] ?: 1),
            vestBoxPosition = sanitizeBoxPosition(pref[keyVestBoxPosition] ?: 1),
        )
    }

    suspend fun updateGreenMax(value: Float) {
        context.dataStore.edit { pref ->
            pref[keyGreen] = value
        }
    }

    suspend fun updateOrangeMax(value: Float) {
        context.dataStore.edit { pref ->
            pref[keyOrange] = value
        }
    }

    suspend fun updateMedianWindow(value: Int) {
        context.dataStore.edit { pref ->
            pref[keyMedianWindow] = value.coerceIn(1, 31)
        }
    }

    suspend fun updateUwbDataRateKbps(value: Int) {
        context.dataStore.edit { pref ->
            pref[keyUwbDataRateKbps] = 6800
        }
    }

    suspend fun updateRfChannel(value: Int) {
        context.dataStore.edit { pref ->
            pref[keyRfChannel] = sanitizeRfChannel(value)
        }
    }

    suspend fun updateAcquisitionPeriodMs(value: Int) {
        context.dataStore.edit { pref ->
            pref[keyAcquisitionPeriodMs] = value.coerceIn(1, 200)
        }
    }

    suspend fun updateRangingMode(value: RangingMode) {
        context.dataStore.edit { pref ->
            pref[keyRangingMode] = RangingMode.SS_TWR.ordinal
        }
    }

    suspend fun updateTestProfile(value: TestProfile) {
        context.dataStore.edit { pref ->
            pref[keyTestProfile] = value.ordinal
        }
    }

    suspend fun updateBikeBoxPosition(value: Int) {
        context.dataStore.edit { pref ->
            pref[keyBikeBoxPosition] = sanitizeBoxPosition(value)
        }
    }

    suspend fun updateVestBoxPosition(value: Int) {
        context.dataStore.edit { pref ->
            pref[keyVestBoxPosition] = sanitizeBoxPosition(value)
        }
    }

    private fun sanitizeBoxPosition(value: Int): Int {
        return if (value in BOX_POSITIONS) value else 1
    }

    private fun sanitizeRfChannel(value: Int): Int {
        return if (value == 9) 9 else 5
    }

    companion object {
        val BOX_POSITIONS = listOf(1, -1, 2, -2, 3, -3)
    }
}
