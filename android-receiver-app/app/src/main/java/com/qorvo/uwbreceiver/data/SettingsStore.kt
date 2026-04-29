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
    private val keyAcquisitionPeriodMs = intPreferencesKey("acquisition_period_ms")
    private val keyRangingMode = intPreferencesKey("ranging_mode")
    private val keyTestProfile = intPreferencesKey("test_profile")

    val thresholds: Flow<DistanceThresholds> = context.dataStore.data.map { pref ->
        DistanceThresholds(
            greenMax = pref[keyGreen] ?: 1.0f,
            orangeMax = pref[keyOrange] ?: 2.0f,
        )
    }

    val controls: Flow<UwbControlSettings> = context.dataStore.data.map { pref ->
        val testProfileRaw = pref[keyTestProfile] ?: TestProfile.STABLE_FULL.ordinal
        UwbControlSettings(
            medianWindow = (pref[keyMedianWindow] ?: 5).coerceIn(1, 31),
            uwbDataRateKbps = 6800,
            acquisitionPeriodMs = (pref[keyAcquisitionPeriodMs] ?: 20).coerceIn(1, 200),
            rangingMode = RangingMode.SS_TWR,
            testProfile = TestProfile.entries.getOrElse(testProfileRaw) { TestProfile.STABLE_FULL },
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

    suspend fun updateAcquisitionPeriodMs(value: Int) {
        context.dataStore.edit { pref ->
            pref[keyAcquisitionPeriodMs] = value.coerceIn(1, 200)
        }
    }

    suspend fun updateRangingMode(value: RangingMode) {
        context.dataStore.edit { pref ->
            pref[keyRangingMode] = if (value == RangingMode.SS_TWR) 1 else 0
        }
    }

    suspend fun updateTestProfile(value: TestProfile) {
        context.dataStore.edit { pref ->
            pref[keyTestProfile] = value.ordinal
        }
    }
}
