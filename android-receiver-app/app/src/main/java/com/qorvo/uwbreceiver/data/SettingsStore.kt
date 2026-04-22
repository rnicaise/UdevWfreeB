package com.qorvo.uwbreceiver.data

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.floatPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map

private val Context.dataStore: DataStore<Preferences> by preferencesDataStore(name = "uwb_settings")

class SettingsStore(private val context: Context) {
    private val keyGreen = floatPreferencesKey("threshold_green_max")
    private val keyOrange = floatPreferencesKey("threshold_orange_max")

    val thresholds: Flow<DistanceThresholds> = context.dataStore.data.map { pref ->
        DistanceThresholds(
            greenMax = pref[keyGreen] ?: 1.0f,
            orangeMax = pref[keyOrange] ?: 2.0f,
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
}
