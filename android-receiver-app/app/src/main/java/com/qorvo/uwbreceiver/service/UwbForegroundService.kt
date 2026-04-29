package com.qorvo.uwbreceiver.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.media.AudioManager
import android.media.ToneGenerator
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber
import com.qorvo.uwbreceiver.MainActivity
import com.qorvo.uwbreceiver.R
import com.qorvo.uwbreceiver.data.CsvParser
import com.qorvo.uwbreceiver.data.LinkState
import com.qorvo.uwbreceiver.data.PhoneTelemetry
import com.qorvo.uwbreceiver.data.RecordingManager
import com.qorvo.uwbreceiver.data.RuntimeStore
import com.qorvo.uwbreceiver.data.SettingsStore
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import timber.log.Timber
import java.io.IOException

class UwbForegroundService : Service() {
    private val serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    private lateinit var usbManager: UsbManager
    private lateinit var recordingManager: RecordingManager
    private lateinit var settingsStore: SettingsStore
    private lateinit var sensorManager: SensorManager
    private lateinit var locationManager: LocationManager

    private var connectJob: Job? = null
    private var readJob: Job? = null
    private var settingsJob: Job? = null

    private var shouldConnect = false

    private var activeDriver: UsbSerialDriver? = null
    private var activePort: UsbSerialPort? = null

    private var wakeLock: PowerManager.WakeLock? = null
    private var toneGenerator: ToneGenerator? = null
    private var alertJob: Job? = null
    private var distanceAlertActive = false

    private var medianWindow = 5
    private var requestedUwbDataRateKbps = 6800
    private var requestedAcquisitionPeriodMs = 20
    private var lastAppliedUwbDataRateKbps: Int? = null
    private val distWindow = ArrayDeque<Float>()

    @Volatile
    private var latestGyroX: Float? = null
    @Volatile
    private var latestGyroY: Float? = null
    @Volatile
    private var latestGyroZ: Float? = null
    @Volatile
    private var latestLocation: Location? = null

    private val gyroListener = object : SensorEventListener {
        override fun onSensorChanged(event: SensorEvent?) {
            if (event == null || event.values.size < 3) {
                return
            }
            latestGyroX = event.values[0]
            latestGyroY = event.values[1]
            latestGyroZ = event.values[2]
        }

        override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {
            (sensor)
            (accuracy)
        }
    }

    private val locationListener = LocationListener { location ->
        latestLocation = location
    }

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            val action = intent?.action ?: return
            if (action == ACTION_USB_PERMISSION) {
                val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                val device = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
                } else {
                    @Suppress("DEPRECATION")
                    intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                }

                if (granted && device != null) {
                    Timber.i("USB permission granted for %s", device.deviceName)
                } else {
                    RuntimeStore.setLinkState(LinkState.DISCONNECTED, "USB permission denied")
                    Timber.w("USB permission denied")
                }
                return
            }

            if (action == UsbManager.ACTION_USB_DEVICE_DETACHED) {
                RuntimeStore.setLinkState(LinkState.DISCONNECTED, "USB detached")
                closePort()
            }
        }
    }

    override fun onCreate() {
        super.onCreate()
        usbManager = getSystemService(Context.USB_SERVICE) as UsbManager
        recordingManager = RecordingManager(applicationContext)
        settingsStore = SettingsStore(applicationContext)
        sensorManager = getSystemService(Context.SENSOR_SERVICE) as SensorManager
        locationManager = getSystemService(Context.LOCATION_SERVICE) as LocationManager
        toneGenerator = try {
            ToneGenerator(AudioManager.STREAM_ALARM, 100)
        } catch (_: Exception) {
            null
        }

        createNotificationChannel()
        startForeground(NOTIF_ID, buildNotification("Idle", recording = false))

        acquireWakeLock()
        registerUsbReceiver()
        startPhoneTelemetry()
        startSettingsObserver()

        serviceScope.launch {
            RuntimeStore.state.collectLatest { state ->
                val distance = state.latest?.dist?.let { String.format("%.2f m", it) } ?: "--"
                val text = "${state.status} | $distance | ${state.hz.toInt()} Hz"
                val notification = buildNotification(text, state.recording)
                val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
                manager.notify(NOTIF_ID, notification)
            }
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_CONNECT -> {
                shouldConnect = true
                RuntimeStore.setLinkState(LinkState.CONNECTING, "Connecting")
                if (connectJob?.isActive != true) {
                    connectJob = serviceScope.launch { connectLoop() }
                }
            }

            ACTION_DISCONNECT -> {
                shouldConnect = false
                stopRecordingInternal()
                stopDistanceAlert()
                closePort()
                RuntimeStore.setLinkState(LinkState.DISCONNECTED, "Disconnected")
                stopSelf()
            }

            ACTION_START_RECORDING -> startRecordingInternal()
            ACTION_STOP_RECORDING -> stopRecordingInternal()
            ACTION_APPLY_UWB_SETTINGS -> applyUwbSettings()
        }

        return START_STICKY
    }

    override fun onDestroy() {
        shouldConnect = false
        stopRecordingInternal()
        stopDistanceAlert()
        closePort()

        connectJob?.cancel()
        readJob?.cancel()
        settingsJob?.cancel()
        serviceScope.cancel()

        stopPhoneTelemetry()
        unregisterReceiverSafe()
        releaseWakeLock()
        toneGenerator?.release()
        toneGenerator = null

        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private suspend fun connectLoop() {
        while (shouldConnect && serviceScope.isActive) {
            if (activePort != null) {
                delay(1_000)
                continue
            }

            val driver = findFirstDriver()
            if (driver == null) {
                RuntimeStore.setLinkState(LinkState.CONNECTING, "USB device not found")
                delay(2_000)
                continue
            }

            if (!usbManager.hasPermission(driver.device)) {
                requestPermission(driver.device)
                RuntimeStore.setLinkState(LinkState.CONNECTING, "Waiting USB permission")
                delay(1_500)
                continue
            }

            val opened = openPort(driver)
            if (!opened) {
                RuntimeStore.setLinkState(LinkState.CONNECTING, "Open failed, retrying")
                delay(1_500)
            }
        }
    }

    private fun findFirstDriver(): UsbSerialDriver? {
        val drivers = UsbSerialProber.getDefaultProber().findAllDrivers(usbManager)
        if (drivers.isEmpty()) {
            return null
        }
        return drivers.first()
    }

    private fun requestPermission(device: UsbDevice) {
        val intent = Intent(ACTION_USB_PERMISSION)
        val flags = PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        val pendingIntent = PendingIntent.getBroadcast(this, 1001, intent, flags)
        usbManager.requestPermission(device, pendingIntent)
    }

    private fun openPort(driver: UsbSerialDriver): Boolean {
        return try {
            val connection = usbManager.openDevice(driver.device)
            if (connection == null) {
                Timber.w("openDevice returned null")
                return false
            }

            val port = driver.ports.firstOrNull() ?: return false
            port.open(connection)
            port.setParameters(
                115200,
                8,
                UsbSerialPort.STOPBITS_1,
                UsbSerialPort.PARITY_NONE,
            )

            activeDriver = driver
            activePort = port
            RuntimeStore.onConnected("USB connected")
            applyUwbSettings()
            startReadLoop(port)
            true
        } catch (e: Exception) {
            Timber.e(e, "Failed to open USB serial port")
            false
        }
    }

    private fun startReadLoop(port: UsbSerialPort) {
        readJob?.cancel()
        readJob = serviceScope.launch {
            val buffer = ByteArray(1024)
            val accumulator = StringBuilder()

            while (isActive && shouldConnect && activePort === port) {
                val len = try {
                    port.read(buffer, 200)
                } catch (e: IOException) {
                    Timber.w(e, "USB read error")
                    RuntimeStore.setLinkState(LinkState.CONNECTING, "Read error, reconnecting")
                    closePort()
                    break
                }

                if (len <= 0) {
                    continue
                }

                val chunk = String(buffer, 0, len)
                accumulator.append(chunk)

                var newlineIndex = accumulator.indexOf("\n")
                while (newlineIndex >= 0) {
                    val line = accumulator.substring(0, newlineIndex).trim('\r', '\n', ' ')
                    accumulator.delete(0, newlineIndex + 1)
                    consumeLine(line)
                    newlineIndex = accumulator.indexOf("\n")
                }
            }
        }
    }

    private fun consumeLine(line: String) {
        if (line.isBlank() || line.startsWith("#")) {
            return
        }

        if (line.startsWith("ACK,") || line.startsWith("ERR,")) {
            RuntimeStore.setLinkState(RuntimeStore.state.value.linkState, line)
            return
        }

        val sample = CsvParser.parse(line)
        if (sample == null) {
            RuntimeStore.onInvalidLine()
            return
        }

        val filtered = filterDistance(sample.dist)
        updateDistanceAlert(filtered)
        val phoneTelemetry = snapshotPhoneTelemetry()

        RuntimeStore.onSample(sample, filtered, phoneTelemetry)
        recordingManager.appendEnrichedSample(sample, filtered, phoneTelemetry)
    }

    private fun filterDistance(rawDistance: Float): Float {
        distWindow.addLast(rawDistance)
        while (distWindow.size > medianWindow.coerceAtLeast(1)) {
            distWindow.removeFirst()
        }

        val sorted = distWindow.toMutableList().sorted()
        if (sorted.isEmpty()) {
            return rawDistance
        }

        val mid = sorted.size / 2
        return if (sorted.size % 2 == 1) {
            sorted[mid]
        } else {
            (sorted[mid - 1] + sorted[mid]) / 2f
        }
    }

    private fun snapshotPhoneTelemetry(): PhoneTelemetry {
        val loc = latestLocation
        val nowElapsed = android.os.SystemClock.elapsedRealtime()
        val fixElapsed = loc?.elapsedRealtimeNanos?.let { nanos ->
            nowElapsed - (nanos / 1_000_000L)
        }

        return PhoneTelemetry(
            gyroX = latestGyroX,
            gyroY = latestGyroY,
            gyroZ = latestGyroZ,
            latitude = loc?.latitude,
            longitude = loc?.longitude,
            altitudeM = loc?.altitude,
            speedMps = loc?.speed,
            fixElapsedMs = fixElapsed,
        )
    }

    private fun startSettingsObserver() {
        settingsJob?.cancel()
        settingsJob = serviceScope.launch {
            settingsStore.controls.collectLatest { controls ->
                val previousMedian = medianWindow
                medianWindow = controls.medianWindow.coerceAtLeast(1)
                requestedUwbDataRateKbps = controls.uwbDataRateKbps
                requestedAcquisitionPeriodMs = controls.acquisitionPeriodMs
                if (medianWindow != previousMedian) {
                    distWindow.clear()
                }
            }
        }
    }

    private fun applyUwbSettings() {
        val sentRate = sendCommand("CFG,UWB_DATARATE_KBPS=$requestedUwbDataRateKbps\n")
        val sentPeriod = sendCommand("CFG,ACQ_PERIOD_MS=$requestedAcquisitionPeriodMs\n")

        if (sentRate && sentPeriod) {
            lastAppliedUwbDataRateKbps = requestedUwbDataRateKbps
            RuntimeStore.setLinkState(
                RuntimeStore.state.value.linkState,
                "Applied UWB: ${requestedUwbDataRateKbps} kbps, ${requestedAcquisitionPeriodMs} ms"
            )
        } else {
            RuntimeStore.setLinkState(RuntimeStore.state.value.linkState, "UWB cmd pending (connect first)")
        }
    }

    private fun sendCommand(command: String): Boolean {
        val port = activePort ?: return false
        return try {
            val payload = command.toByteArray(Charsets.US_ASCII)
            port.write(payload, 200)
            Timber.i("Sent command: %s", command.trim())
            true
        } catch (e: Exception) {
            Timber.w(e, "Failed to send command")
            false
        }
    }

    private fun closePort() {
        readJob?.cancel()
        readJob = null
        stopDistanceAlert()

        try {
            activePort?.close()
        } catch (_: Exception) {
        }
        activePort = null
        activeDriver = null
    }

    private fun updateDistanceAlert(filteredDistanceM: Float) {
        if (filteredDistanceM > DISTANCE_ALERT_THRESHOLD_M) {
            startDistanceAlert()
        } else {
            stopDistanceAlert()
        }
    }

    private fun startDistanceAlert() {
        if (distanceAlertActive) {
            return
        }

        distanceAlertActive = true
        alertJob?.cancel()
        alertJob = serviceScope.launch {
            while (isActive && distanceAlertActive) {
                toneGenerator?.startTone(ToneGenerator.TONE_CDMA_ALERT_CALL_GUARD, ALERT_BEEP_DURATION_MS)
                delay(ALERT_BEEP_PERIOD_MS.toLong())
            }
        }
    }

    private fun stopDistanceAlert() {
        if (!distanceAlertActive && alertJob == null) {
            return
        }

        distanceAlertActive = false
        alertJob?.cancel()
        alertJob = null
        toneGenerator?.stopTone()
    }

    private fun startRecordingInternal() {
        try {
            val (uri, fileName) = recordingManager.start()
            RuntimeStore.setRecording(true, fileName)
            RuntimeStore.setLastSavedUri(uri)
            RuntimeStore.setLinkState(RuntimeStore.state.value.linkState, "Recording")
        } catch (e: Exception) {
            Timber.e(e, "Failed to start recording")
            RuntimeStore.setLinkState(RuntimeStore.state.value.linkState, "Recording failed")
        }
    }

    private fun stopRecordingInternal() {
        val savedUri = recordingManager.stop()
        RuntimeStore.setRecording(false, null)
        if (savedUri != null) {
            RuntimeStore.setLastSavedUri(savedUri)
        }
    }

    private fun startPhoneTelemetry() {
        val gyro = sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
        if (gyro != null) {
            sensorManager.registerListener(gyroListener, gyro, SensorManager.SENSOR_DELAY_GAME)
        }

        if (hasLocationPermission()) {
            try {
                locationManager.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1000L, 0f, locationListener)
                locationManager.requestLocationUpdates(LocationManager.NETWORK_PROVIDER, 2000L, 0f, locationListener)
            } catch (_: Exception) {
            }
        }
    }

    private fun stopPhoneTelemetry() {
        sensorManager.unregisterListener(gyroListener)
        try {
            locationManager.removeUpdates(locationListener)
        } catch (_: Exception) {
        }
    }

    private fun hasLocationPermission(): Boolean {
        val fine = ContextCompat.checkSelfPermission(this, android.Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
        val coarse = ContextCompat.checkSelfPermission(this, android.Manifest.permission.ACCESS_COARSE_LOCATION) == PackageManager.PERMISSION_GRANTED
        return fine || coarse
    }

    private fun registerUsbReceiver() {
        val filter = IntentFilter().apply {
            addAction(ACTION_USB_PERMISSION)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(usbReceiver, filter, RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("DEPRECATION")
            registerReceiver(usbReceiver, filter)
        }
    }

    private fun unregisterReceiverSafe() {
        try {
            unregisterReceiver(usbReceiver)
        } catch (_: Exception) {
        }
    }

    private fun buildNotification(content: String, recording: Boolean): Notification {
        val launchIntent = Intent(this, MainActivity::class.java)
        val pendingLaunch = PendingIntent.getActivity(
            this,
            2001,
            launchIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )

        val title = if (recording) "UWB Receiver • Recording" else "UWB Receiver • Live"

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle(title)
            .setContentText(content)
            .setSmallIcon(android.R.drawable.stat_notify_sync)
            .setContentIntent(pendingLaunch)
            .setOngoing(true)
            .build()
    }

    private fun createNotificationChannel() {
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        val channel = NotificationChannel(
            CHANNEL_ID,
            getString(R.string.notif_channel_name),
            NotificationManager.IMPORTANCE_LOW,
        ).apply {
            description = getString(R.string.notif_channel_desc)
        }
        manager.createNotificationChannel(channel)
    }

    private fun acquireWakeLock() {
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "UWBReceiver:Acquisition").apply {
            setReferenceCounted(false)
            acquire()
        }
    }

    private fun releaseWakeLock() {
        try {
            wakeLock?.let { if (it.isHeld) it.release() }
        } catch (_: Exception) {
        } finally {
            wakeLock = null
        }
    }

    companion object {
        private const val CHANNEL_ID = "uwb-acquisition"
        private const val NOTIF_ID = 42
        private const val DISTANCE_ALERT_THRESHOLD_M = 1.0f
        private const val ALERT_BEEP_DURATION_MS = 250
        private const val ALERT_BEEP_PERIOD_MS = 300

        const val ACTION_CONNECT = "com.qorvo.uwbreceiver.action.CONNECT"
        const val ACTION_DISCONNECT = "com.qorvo.uwbreceiver.action.DISCONNECT"
        const val ACTION_START_RECORDING = "com.qorvo.uwbreceiver.action.START_RECORDING"
        const val ACTION_STOP_RECORDING = "com.qorvo.uwbreceiver.action.STOP_RECORDING"
        const val ACTION_APPLY_UWB_SETTINGS = "com.qorvo.uwbreceiver.action.APPLY_UWB_SETTINGS"

        private const val ACTION_USB_PERMISSION = "com.qorvo.uwbreceiver.action.USB_PERMISSION"
    }
}
