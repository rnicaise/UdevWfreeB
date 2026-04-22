package com.qorvo.uwbreceiver

import android.app.Application
import timber.log.Timber

class UwbReceiverApp : Application() {
    override fun onCreate() {
        super.onCreate()
        Timber.plant(Timber.DebugTree())
    }
}
