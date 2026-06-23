package com.qorvo.uwbreceiver.service

import android.Manifest
import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
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
import com.qorvo.uwbreceiver.data.ConnectedUwbRole
import com.qorvo.uwbreceiver.data.CsvParser
import com.qorvo.uwbreceiver.data.CsvSample
import com.qorvo.uwbreceiver.data.ExperimentSettings
import com.qorvo.uwbreceiver.data.LinkState
import com.qorvo.uwbreceiver.data.LinkSource
import com.qorvo.uwbreceiver.data.PhoneTelemetry
import com.qorvo.uwbreceiver.data.RecordingManager
import com.qorvo.uwbreceiver.data.RuntimeStore
import com.qorvo.uwbreceiver.data.SafetyArmMode
import com.qorvo.uwbreceiver.data.SettingsStore
import com.qorvo.uwbreceiver.data.UwbControlSettings
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import timber.log.Timber
import java.io.IOException
import java.util.UUID
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.sqrt

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
    private val commandMutex = Mutex()

    private var shouldConnect = false

    private var activeDriver: UsbSerialDriver? = null
    private var activePort: UsbSerialPort? = null
    private var bleScanCallback: ScanCallback? = null
    private var activeBleGatt: BluetoothGatt? = null
    private var bleCommandCharacteristic: BluetoothGattCharacteristic? = null
    private var bleGattAccumulator = StringBuilder()
    private var bleSampleCounterEpoch = 0L
    private var bleLastCounter16: Int? = null
    private var bleLastStatusElapsedMs = 0L
    private var bleSettingsWriteInFlight = false
    private var bleSettingsSaveAckSeen = false

    private var wakeLock: PowerManager.WakeLock? = null
    private var toneGenerator: ToneGenerator? = null
    private var safetyArmMode = SafetyArmMode.DISARMED
    private var tiltBaseline: TiltAngles? = null

    private var medianWindow = 1
    private var currentExperiment = ExperimentSettings()
    private var connectedRole = ConnectedUwbRole.UNKNOWN
    private val distWindow = ArrayDeque<Float>()
    private var lastAcceptedSample: CsvSample? = null
    private var lastUiPushElapsedMs = 0L
    private var consecutivePlausibilityRejects = 0

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
                closeBleScan()
                closeBleGatt()
                RuntimeStore.setLinkState(LinkState.CONNECTING, "Connecting")
                if (connectJob?.isActive != true) {
                    connectJob = serviceScope.launch { connectLoop() }
                }
            }

            ACTION_START_BLE_SCAN -> startBleScan()

            ACTION_DISCONNECT -> {
                shouldConnect = false
                stopRecordingInternal()
                disarmSafetyTrigger("Disconnected")
                closePort()
                closeBleScan()
                closeBleGatt()
                RuntimeStore.setLinkState(LinkState.DISCONNECTED, "Disconnected")
                stopSelf()
            }

            ACTION_START_RECORDING -> startRecordingInternal()
            ACTION_STOP_RECORDING -> stopRecordingInternal()
            ACTION_FIRE -> {
                if (RuntimeStore.state.value.linkSource == LinkSource.BLE_GATT) {
                    RuntimeStore.setLinkState(RuntimeStore.state.value.linkState, "BLE settings mode: use ARM buttons")
                    return START_STICKY
                }
                serviceScope.launch {
                    val sent = firePyro("Manual FIRE", immediate = false)
                    RuntimeStore.setLinkState(
                        RuntimeStore.state.value.linkState,
                        if (sent) "Manual FIRE command sent" else "Manual FIRE send failed",
                    )
                }
            }

            ACTION_ARM_DISTANCE_2M -> {
                if (RuntimeStore.state.value.linkSource == LinkSource.BLE_GATT) {
                    serviceScope.launch { sendBleAdminArmCommand("ARM,DIST,2.00\n", SafetyArmMode.DISTANCE_2M, "Arming card: distance >= 2.00 m") }
                } else {
                    armDistanceTrigger()
                }
            }
            ACTION_ARM_TILT_50_DEG -> {
                if (RuntimeStore.state.value.linkSource == LinkSource.BLE_GATT) {
                    serviceScope.launch { sendBleAdminArmCommand("ARM,TILT,50\n", SafetyArmMode.TILT_50_DEG, "Arming card: tilt delta >= 50°") }
                } else {
                    armTiltTrigger()
                }
            }
        }

        return START_STICKY
    }

    override fun onDestroy() {
        shouldConnect = false
        stopRecordingInternal()
        disarmSafetyTrigger("Service stopped")
        closePort()
        closeBleScan()
        closeBleGatt()

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
                SERIAL_BAUD_RATE,
                8,
                UsbSerialPort.STOPBITS_1,
                UsbSerialPort.PARITY_NONE,
            )

            activeDriver = driver
            activePort = port
            connectedRole = ConnectedUwbRole.UNKNOWN
            RuntimeStore.onConnected("USB connected, detecting role")
            startReadLoop(port)
            requestConnectedRole()
            true
        } catch (e: Exception) {
            Timber.e(e, "Failed to open USB serial port")
            false
        }
    }

    private fun startReadLoop(port: UsbSerialPort) {
        readJob?.cancel()
        readJob = serviceScope.launch {
            val buffer = ByteArray(4096)
            val accumulator = StringBuilder()
            var discardNextLine = false

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
                if (accumulator.length > MAX_ACCUMULATED_SERIAL_CHARS) {
                    accumulator.clear()
                    /* We dropped mid-line: the next extracted "line" is a tail
                     * fragment that must not reach the parser. */
                    discardNextLine = true
                    RuntimeStore.onInvalidLine()
                    continue
                }

                var newlineIndex = accumulator.indexOf("\n")
                while (newlineIndex >= 0) {
                    val line = accumulator.substring(0, newlineIndex).trim('\r', '\n', ' ')
                    accumulator.delete(0, newlineIndex + 1)
                    if (discardNextLine) {
                        discardNextLine = false
                    } else {
                        consumeLine(line)
                    }
                    newlineIndex = accumulator.indexOf("\n")
                }
            }
        }
    }

    private fun consumeLine(line: String) {
        if (line.isBlank() || line.startsWith("#")) {
            return
        }

        if (line.startsWith("ROLE,")) {
            val role = when (line.substringAfter("ROLE,").trim().uppercase()) {
                "INITIATOR" -> ConnectedUwbRole.INITIATOR
                "RESPONDER" -> ConnectedUwbRole.RESPONDER
                else -> ConnectedUwbRole.UNKNOWN
            }
            if (role != ConnectedUwbRole.UNKNOWN) {
                connectedRole = role
                RuntimeStore.setConnectedRole(role)
                RuntimeStore.setLinkState(LinkState.CONNECTED, "Detected role: ${role.name}")
            }
            return
        }

        if (line.startsWith("ARM,") || line.startsWith("ACK,ARM,")) {
            if (line.startsWith("ACK,ARM,") && line.contains(",SAVED")) {
                bleSettingsSaveAckSeen = true
                bleSettingsWriteInFlight = false
            }
            updateFirmwareArmState(line)
            RuntimeStore.setLinkState(RuntimeStore.state.value.linkState, line)
            return
        }

        if (line.startsWith("ACCEL_SRC,") || line.startsWith("ACCEL,")) {
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

        if (!isPlausibleSample(sample)) {
            RuntimeStore.onInvalidLine()
            return
        }

        lastAcceptedSample = sample
        consecutivePlausibilityRejects = 0

        val filtered = filterDistance(sample.dist)
        updateSafetyTrigger(sample, filtered)
        val phoneTelemetry = snapshotPhoneTelemetry()

        /* At ~390 Hz the per-sample stats/state pipeline (synchronized state
         * copy + 30 s rolling std) cannot keep up and the USB buffer overruns,
         * corrupting lines. Throttle UI/stats updates to ~20 Hz; recording
         * still captures every sample. */
        val nowElapsed = android.os.SystemClock.elapsedRealtime()
        if (nowElapsed - lastUiPushElapsedMs >= UI_PUSH_INTERVAL_MS) {
            lastUiPushElapsedMs = nowElapsed
            RuntimeStore.onSample(sample, filtered, phoneTelemetry)
        }
        recordingManager.appendEnrichedSample(
            sample,
            filtered,
            phoneTelemetry,
            RuntimeStore.state.value.transmissionQuality,
            UwbControlSettings(),
            currentExperiment,
            connectedRole,
        )
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

    private fun isPlausibleSample(sample: CsvSample): Boolean {
        if (sample.ms < 0) {
            return false
        }
        if (!sample.dist.isFinite() || sample.dist < -5f || sample.dist > 30f) {
            return false
        }

        val previous = lastAcceptedSample ?: return true
        val sampleDelta = sample.sample - previous.sample
        if (sampleDelta <= 0L) {
            return rebaselineAfterRejects()
        }

        val distanceDelta = kotlin.math.abs(sample.dist - previous.dist)
        if (sampleDelta <= 3L && distanceDelta > 5f) {
            return rebaselineAfterRejects()
        }
        return true
    }

    /* If a corrupted line with a bogus (huge) sample counter ever gets accepted,
     * every genuine sample afterwards fails the sampleDelta check and the display
     * freezes. Accept after enough consecutive rejects to re-baseline. */
    private fun rebaselineAfterRejects(): Boolean {
        consecutivePlausibilityRejects++
        if (consecutivePlausibilityRejects >= PLAUSIBILITY_RESYNC_REJECTS) {
            consecutivePlausibilityRejects = 0
            return true
        }
        return false
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
            launch {
                settingsStore.experiment.collectLatest { experiment ->
                    currentExperiment = experiment
                }
            }
        }
    }

    private fun requestConnectedRole() {
        serviceScope.launch {
            sendCommandSlowly("CFG,GET_ROLE\n")
        }
    }

    private fun requestFirmwareArmState() {
        serviceScope.launch {
            sendCommandSlowly("ARM,GET\n")
        }
    }

    private suspend fun sendCommandSlowly(command: String): Boolean {
        return commandMutex.withLock {
            if (RuntimeStore.state.value.linkSource == LinkSource.BLE_GATT) {
                return@withLock writeBleCommand(command)
            }

            val port = activePort ?: return@withLock false
            try {
                val payload = command.toByteArray(Charsets.US_ASCII)
                val singleByte = ByteArray(1)
                for (byte in payload) {
                    if (activePort !== port) {
                        return@withLock false
                    }
                    singleByte[0] = byte
                    port.write(singleByte, 200)
                    delay(COMMAND_BYTE_DELAY_MS)
                }
                Timber.i("Sent command: %s", command.trim())
                true
            } catch (e: Exception) {
                Timber.w(e, "Failed to send command")
                false
            }
        }
    }

    private fun closePort() {
        readJob?.cancel()
        readJob = null
        disarmSafetyTrigger("USB closed")

        try {
            activePort?.close()
        } catch (_: Exception) {
        }
        activePort = null
        activeDriver = null
        connectedRole = ConnectedUwbRole.UNKNOWN
    }

    @SuppressLint("MissingPermission")
    private fun startBleScan() {
        shouldConnect = false
        closePort()
        closeBleGatt()
        disarmSafetyTrigger("BLE scan")

        if (!hasBleScanPermission()) {
            RuntimeStore.setLinkState(LinkState.DISCONNECTED, "BLE scan permission missing")
            return
        }

        val bluetoothManager = getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        val adapter = bluetoothManager.adapter
        if (adapter == null || !adapter.isEnabled) {
            RuntimeStore.setLinkState(LinkState.DISCONNECTED, "Bluetooth disabled")
            return
        }

        val scanner = adapter.bluetoothLeScanner
        if (scanner == null) {
            RuntimeStore.setLinkState(LinkState.DISCONNECTED, "BLE scanner unavailable")
            return
        }

        closeBleScan()
        bleSampleCounterEpoch = 0L
        bleLastCounter16 = null
        bleLastStatusElapsedMs = 0L
        connectedRole = ConnectedUwbRole.INITIATOR

        RuntimeStore.setLinkState(LinkState.CONNECTING, "BLE scan listening for command mode", LinkSource.BLE_ADV)
        RuntimeStore.setConnectedRole(ConnectedUwbRole.INITIATOR)

        val callback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                consumeBleScanResult(result)
                maybeConnectBleGatt(result)
            }

            override fun onBatchScanResults(results: MutableList<ScanResult>) {
                results.forEach {
                    consumeBleScanResult(it)
                    maybeConnectBleGatt(it)
                }
            }

            override fun onScanFailed(errorCode: Int) {
                RuntimeStore.setLinkState(LinkState.DISCONNECTED, "BLE scan failed: $errorCode")
                closeBleScan()
            }
        }
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        bleScanCallback = callback
        scanner.startScan(null, settings, callback)
        RuntimeStore.setLinkState(LinkState.CONNECTING, "BLE scan active", LinkSource.BLE_ADV)
    }

    @SuppressLint("MissingPermission")
    private fun closeBleScan() {
        val callback = bleScanCallback ?: return
        bleScanCallback = null
        try {
            val bluetoothManager = getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
            bluetoothManager.adapter?.bluetoothLeScanner?.stopScan(callback)
        } catch (_: Exception) {
        }
    }

    private fun consumeBleScanResult(result: ScanResult) {
        val record = result.scanRecord ?: return
        val manufacturerData = record.getManufacturerSpecificData(BLE_DEVELOPMENT_COMPANY_ID) ?: return
        if (manufacturerData.size < 4) {
            return
        }

        val deviceName = record.deviceName
        if (deviceName != null && deviceName != BLE_DEVICE_NAME) {
            return
        }

        val distanceCm = ((manufacturerData[0].toInt() and 0xFF) or (manufacturerData[1].toInt() shl 8)).toShort().toInt()
        val counter16 = (manufacturerData[2].toInt() and 0xFF) or ((manufacturerData[3].toInt() and 0xFF) shl 8)
        val sampleCounter = unwrapBleCounter(counter16)
        val distanceM = distanceCm / 100f
        if (!distanceM.isFinite() || distanceM < -5f || distanceM > 30f) {
            return
        }

        val nowElapsed = android.os.SystemClock.elapsedRealtime()
        val sessionStart = RuntimeStore.state.value.sessionStartElapsedMs ?: nowElapsed
        val sample = CsvSample(
            ms = nowElapsed - sessionStart,
            sample = sampleCounter,
            dist = distanceM,
            iax = 0,
            iay = 0,
            iaz = 0,
            rax = 0,
            ray = 0,
            raz = 0,
            rxPowerDbm = result.rssi.toFloat(),
            signalQuality10 = rssiQuality10(result.rssi),
            firmwareValid = true,
            firmwareDistFilt = distanceM,
            firmwareDistSmooth = distanceM,
        )
        val phoneTelemetry = snapshotPhoneTelemetry()

        RuntimeStore.onSample(sample, distanceM, phoneTelemetry)
        recordingManager.appendEnrichedSample(
            sample,
            distanceM,
            phoneTelemetry,
            RuntimeStore.state.value.transmissionQuality,
            UwbControlSettings(acquisitionPeriodMs = BLE_ADV_PERIOD_MS.toInt()),
            currentExperiment,
            ConnectedUwbRole.INITIATOR,
        )

        if (nowElapsed - bleLastStatusElapsedMs >= BLE_STATUS_INTERVAL_MS) {
            bleLastStatusElapsedMs = nowElapsed
            RuntimeStore.setLinkState(LinkState.CONNECTED, "BLE UWB ${String.format("%.2f", distanceM)} m", LinkSource.BLE_ADV)
        }
    }

    @SuppressLint("MissingPermission")
    private fun maybeConnectBleGatt(result: ScanResult) {
        if (activeBleGatt != null || bleCommandCharacteristic != null) {
            return
        }
        if (!hasBleConnectPermission()) {
            return
        }
        if (!result.isConnectable) {
            return
        }
        val record = result.scanRecord ?: return
        val serviceUuids = record.serviceUuids?.map { it.uuid }.orEmpty()
        val name = record.deviceName ?: result.device.name
        if (name == BLE_DEVICE_NAME || serviceUuids.contains(NUS_SERVICE_UUID)) {
            Timber.i(
                "BLE scan candidate name=%s address=%s connectable=%s rssi=%d services=%s",
                name,
                result.device.address,
                result.isConnectable,
                result.rssi,
                serviceUuids.joinToString(),
            )
        }
        if (name == null) {
            return
        }
        if (name != BLE_DEVICE_NAME) {
            return
        }

        RuntimeStore.setLinkState(LinkState.CONNECTING, "BLE GATT connecting", LinkSource.BLE_ADV)
        closeBleScan()
        Timber.i("BLE GATT connectGatt address=%s name=%s", result.device.address, name)
        activeBleGatt = result.device.connectGatt(this, false, bleGattCallback)
    }

    private val bleGattCallback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            Timber.i("BLE GATT state status=%d newState=%d address=%s", status, newState, gatt.device.address)
            if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                val normalAdminDisconnect = RuntimeStore.state.value.linkSource == LinkSource.BLE_GATT &&
                    (bleSettingsWriteInFlight || bleSettingsSaveAckSeen || status == 22 || status == 19 || status == BluetoothGatt.GATT_SUCCESS)
                val message = when {
                    bleSettingsSaveAckSeen -> "BLE settings saved, disconnected"
                    normalAdminDisconnect -> "BLE command session disconnected"
                    status == BluetoothGatt.GATT_SUCCESS -> "BLE GATT disconnected"
                    else -> "BLE GATT error: $status"
                }
                bleSettingsWriteInFlight = false
                bleSettingsSaveAckSeen = false
                RuntimeStore.setLinkState(LinkState.DISCONNECTED, message)
                closeBleGatt()
                return
            }

            if (status != BluetoothGatt.GATT_SUCCESS) {
                RuntimeStore.setLinkState(LinkState.DISCONNECTED, "BLE GATT error: $status")
                closeBleGatt()
                return
            }

            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    RuntimeStore.setLinkState(LinkState.CONNECTING, "BLE GATT discovering", LinkSource.BLE_GATT)
                    gatt.discoverServices()
                }
            }
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            Timber.i(
                "BLE GATT services status=%d services=%s",
                status,
                gatt.services.joinToString { service ->
                    "${service.uuid}[${service.characteristics.joinToString { it.uuid.toString() }}]"
                },
            )
            if (status != BluetoothGatt.GATT_SUCCESS) {
                RuntimeStore.setLinkState(LinkState.DISCONNECTED, "BLE services error: $status")
                closeBleGatt()
                return
            }

            val service = gatt.getService(NUS_SERVICE_UUID)
            val command = service?.getCharacteristic(NUS_RX_CHAR_UUID)
            val notify = service?.getCharacteristic(NUS_TX_CHAR_UUID)
            if (command == null) {
                Timber.w("BLE GATT NUS RX characteristic missing")
                RuntimeStore.setLinkState(LinkState.CONNECTED, "BLE monitor only", LinkSource.BLE_ADV)
                return
            }

            bleCommandCharacteristic = command
            connectedRole = ConnectedUwbRole.INITIATOR
            RuntimeStore.onConnected("BLE GATT connected", LinkSource.BLE_GATT)
            RuntimeStore.setConnectedRole(ConnectedUwbRole.INITIATOR)

            if (notify != null) {
                gatt.setCharacteristicNotification(notify, true)
                val descriptor = notify.getDescriptor(CLIENT_CHARACTERISTIC_CONFIG_UUID)
                if (descriptor != null) {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        gatt.writeDescriptor(descriptor, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                    } else {
                        @Suppress("DEPRECATION")
                        descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                        @Suppress("DEPRECATION")
                        gatt.writeDescriptor(descriptor)
                    }
                }
                serviceScope.launch {
                    delay(300)
                    requestFirmwareArmState()
                }
            }
        }

        override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray) {
            if (characteristic.uuid == NUS_TX_CHAR_UUID) {
                consumeBleText(value.toString(Charsets.US_ASCII))
            }
        }

        @Deprecated("Deprecated in Android API")
        override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            if (characteristic.uuid == NUS_TX_CHAR_UUID) {
                @Suppress("DEPRECATION")
                consumeBleText(characteristic.value.toString(Charsets.US_ASCII))
            }
        }
    }

    private fun consumeBleText(text: String) {
        bleGattAccumulator.append(text)
        if (bleGattAccumulator.length > MAX_ACCUMULATED_SERIAL_CHARS) {
            bleGattAccumulator.clear()
            RuntimeStore.onInvalidLine()
            return
        }

        var newlineIndex = bleGattAccumulator.indexOf("\n")
        while (newlineIndex >= 0) {
            val line = bleGattAccumulator.substring(0, newlineIndex).trim('\r', '\n', ' ')
            bleGattAccumulator.delete(0, newlineIndex + 1)
            consumeLine(line)
            newlineIndex = bleGattAccumulator.indexOf("\n")
        }
    }

    @SuppressLint("MissingPermission")
    private fun writeBleCommand(command: String): Boolean {
        val gatt = activeBleGatt ?: return false
        val characteristic = bleCommandCharacteristic ?: return false
        if (!hasBleConnectPermission()) {
            RuntimeStore.setLinkState(LinkState.DISCONNECTED, "BLE connect permission missing")
            return false
        }

        val payload = command.toByteArray(Charsets.US_ASCII)
        characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        val accepted = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            gatt.writeCharacteristic(characteristic, payload, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            characteristic.value = payload
            @Suppress("DEPRECATION")
            gatt.writeCharacteristic(characteristic)
        }
        if (accepted) {
            Timber.i("Sent BLE command: %s", command.trim())
        }
        return accepted
    }

    @SuppressLint("MissingPermission")
    private fun closeBleGatt() {
        bleCommandCharacteristic = null
        bleGattAccumulator.clear()
        try {
            activeBleGatt?.disconnect()
            activeBleGatt?.close()
        } catch (_: Exception) {
        }
        activeBleGatt = null
    }

    private fun unwrapBleCounter(counter16: Int): Long {
        val previous = bleLastCounter16
        if (previous != null && counter16 + BLE_COUNTER_WRAP_HALF < previous) {
            bleSampleCounterEpoch += BLE_COUNTER_WRAP
        }
        bleLastCounter16 = counter16
        return bleSampleCounterEpoch + counter16.toLong()
    }

    private fun rssiQuality10(rssi: Int): Float {
        return ((rssi + 100).toFloat() / 5f).coerceIn(1f, 10f)
    }

    private fun armDistanceTrigger() {
        safetyArmMode = SafetyArmMode.DISTANCE_2M
        tiltBaseline = null
        RuntimeStore.setSafetyArmState(SafetyArmMode.DISTANCE_2M, "Armed: distance >= 2.00 m")
        toneGenerator?.startTone(ToneGenerator.TONE_PROP_BEEP, ARM_BEEP_DURATION_MS)
    }

    private suspend fun sendBleAdminArmCommand(command: String, mode: SafetyArmMode, status: String) {
        bleSettingsWriteInFlight = true
        bleSettingsSaveAckSeen = false
        RuntimeStore.setSafetyArmState(mode, status)
        val sent = sendCommandSlowly(command)
        if (!sent) {
            bleSettingsWriteInFlight = false
        }
        RuntimeStore.setSafetyArmState(
            if (sent) mode else SafetyArmMode.DISARMED,
            if (sent) "Waiting card setting save ACK" else "Card arm command failed",
        )
        if (sent) {
            toneGenerator?.startTone(ToneGenerator.TONE_PROP_BEEP, ARM_BEEP_DURATION_MS)
        }
    }

    private fun updateFirmwareArmState(line: String) {
        val normalized = line.uppercase()
        when {
            normalized.contains("DISTANCE_2M") -> RuntimeStore.setSafetyArmState(
                SafetyArmMode.DISTANCE_2M,
                "Card setting: armed distance >= 2.00 m",
            )
            normalized.contains("TILT_50") -> RuntimeStore.setSafetyArmState(
                SafetyArmMode.TILT_50_DEG,
                "Card setting: armed tilt delta >= 50°",
            )
            normalized.contains("DISARMED") -> RuntimeStore.setSafetyArmState(
                SafetyArmMode.DISARMED,
                "Card setting: disarmed",
            )
            normalized.contains("WRITE_PENDING") -> RuntimeStore.setSafetyArmState(
                RuntimeStore.state.value.safetyArmMode,
                "Card setting write pending",
            )
        }
    }

    private fun armTiltTrigger() {
        val sample = lastAcceptedSample ?: RuntimeStore.state.value.latest
        val baseline = sample?.let { initiatorTiltAngles(it) }
        if (baseline == null) {
            RuntimeStore.setSafetyArmState(SafetyArmMode.DISARMED, "Tilt arm failed: no initiator accel")
            return
        }

        safetyArmMode = SafetyArmMode.TILT_50_DEG
        tiltBaseline = baseline
        RuntimeStore.setSafetyArmState(SafetyArmMode.TILT_50_DEG, "Armed: initiator pitch/roll delta >= 50°")
        toneGenerator?.startTone(ToneGenerator.TONE_PROP_BEEP, ARM_BEEP_DURATION_MS)
    }

    private fun updateSafetyTrigger(sample: CsvSample, filteredDistanceM: Float) {
        when (safetyArmMode) {
            SafetyArmMode.DISARMED -> return
            SafetyArmMode.DISTANCE_2M -> {
                if (filteredDistanceM >= DISTANCE_TRIGGER_THRESHOLD_M) {
                    triggerAndDisarm("distance ${String.format("%.2f", filteredDistanceM)} m")
                }
            }
            SafetyArmMode.TILT_50_DEG -> {
                val baseline = tiltBaseline ?: return
                val current = initiatorTiltAngles(sample) ?: return
                val pitchDelta = abs(current.pitchDeg - baseline.pitchDeg)
                val rollDelta = abs(current.rollDeg - baseline.rollDeg)
                if (pitchDelta >= TILT_TRIGGER_THRESHOLD_DEG || rollDelta >= TILT_TRIGGER_THRESHOLD_DEG) {
                    triggerAndDisarm("tilt pitch ${String.format("%.1f", pitchDelta)}° roll ${String.format("%.1f", rollDelta)}°")
                }
            }
        }
    }

    private fun triggerAndDisarm(reason: String) {
        val triggeredMode = safetyArmMode
        if (triggeredMode == SafetyArmMode.DISARMED) {
            return
        }

        safetyArmMode = SafetyArmMode.DISARMED
        tiltBaseline = null
        RuntimeStore.setSafetyArmState(SafetyArmMode.DISARMED, "Triggered ${safetyArmLabel(triggeredMode)}: $reason")
        serviceScope.launch {
            val sent = firePyro("Triggered ${safetyArmLabel(triggeredMode)}", immediate = true)
            RuntimeStore.setSafetyArmState(
                SafetyArmMode.DISARMED,
                if (sent) "Triggered ${safetyArmLabel(triggeredMode)}: FIRE sent" else "Triggered ${safetyArmLabel(triggeredMode)}: FIRE send failed",
            )
        }
    }

    private fun disarmSafetyTrigger(status: String) {
        safetyArmMode = SafetyArmMode.DISARMED
        tiltBaseline = null
        toneGenerator?.stopTone()
        RuntimeStore.setSafetyArmState(SafetyArmMode.DISARMED, status)
    }

    private suspend fun firePyro(reason: String, immediate: Boolean): Boolean {
        toneGenerator?.startTone(ToneGenerator.TONE_CDMA_ALERT_CALL_GUARD, TRIGGER_BEEP_DURATION_MS)
        RuntimeStore.setSafetyArmState(SafetyArmMode.DISARMED, "$reason: sending ${if (immediate) "FIRE_NOW" else "FIRE"}")
        return sendCommandSlowly(if (immediate) "PYRO,FIRE_NOW\n" else "PYRO,FIRE\n")
    }

    private fun initiatorTiltAngles(sample: CsvSample): TiltAngles? {
        val x = sample.iax.toFloat()
        val y = sample.iay.toFloat()
        val z = sample.iaz.toFloat()
        val norm = sqrt((x * x) + (y * y) + (z * z))
        if (!norm.isFinite() || norm < MIN_ACCEL_NORM_RAW) {
            return null
        }
        val pitch = atan2(x, sqrt((y * y) + (z * z))) * RAD_TO_DEG
        val roll = atan2(y, sqrt((x * x) + (z * z))) * RAD_TO_DEG
        return TiltAngles(pitch, roll)
    }

    private fun safetyArmLabel(mode: SafetyArmMode): String {
        return when (mode) {
            SafetyArmMode.DISARMED -> "disarmed"
            SafetyArmMode.DISTANCE_2M -> "distance-armed-2m"
            SafetyArmMode.TILT_50_DEG -> "tilt-armed-50°"
        }
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

    private fun hasBleScanPermission(): Boolean {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED
        } else {
            hasLocationPermission()
        }
    }

    private fun hasBleConnectPermission(): Boolean {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
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
        private const val DISTANCE_TRIGGER_THRESHOLD_M = 2.0f
        private const val TILT_TRIGGER_THRESHOLD_DEG = 50.0f
        private const val MIN_ACCEL_NORM_RAW = 100.0f
        private const val ARM_BEEP_DURATION_MS = 120
        private const val TRIGGER_BEEP_DURATION_MS = 500
        private const val RAD_TO_DEG = 57.29578f
        private const val SERIAL_BAUD_RATE = 460800
        private const val BLE_DEVICE_NAME = "UWB"
        private const val BLE_DEVELOPMENT_COMPANY_ID = 0xFFFF
        private const val BLE_ADV_PERIOD_MS = 100L
        private const val BLE_STATUS_INTERVAL_MS = 1_000L
        private const val BLE_COUNTER_WRAP = 65_536L
        private const val BLE_COUNTER_WRAP_HALF = 32_768
        private val NUS_SERVICE_UUID: UUID = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e")
        private val NUS_RX_CHAR_UUID: UUID = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e")
        private val NUS_TX_CHAR_UUID: UUID = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e")
        private val CLIENT_CHARACTERISTIC_CONFIG_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        private const val MAX_ACCUMULATED_SERIAL_CHARS = 65536
        private const val UI_PUSH_INTERVAL_MS = 50L
        private const val PLAUSIBILITY_RESYNC_REJECTS = 50
        private const val COMMAND_BYTE_DELAY_MS = 15L

        const val ACTION_CONNECT = "com.qorvo.uwbreceiver.action.CONNECT"
        const val ACTION_START_BLE_SCAN = "com.qorvo.uwbreceiver.action.START_BLE_SCAN"
        const val ACTION_DISCONNECT = "com.qorvo.uwbreceiver.action.DISCONNECT"
        const val ACTION_START_RECORDING = "com.qorvo.uwbreceiver.action.START_RECORDING"
        const val ACTION_STOP_RECORDING = "com.qorvo.uwbreceiver.action.STOP_RECORDING"
        const val ACTION_FIRE = "com.qorvo.uwbreceiver.action.FIRE"
        const val ACTION_ARM_DISTANCE_2M = "com.qorvo.uwbreceiver.action.ARM_DISTANCE_2M"
        const val ACTION_ARM_TILT_50_DEG = "com.qorvo.uwbreceiver.action.ARM_TILT_50_DEG"

        private const val ACTION_USB_PERMISSION = "com.qorvo.uwbreceiver.action.USB_PERMISSION"
    }

    private data class TiltAngles(
        val pitchDeg: Float,
        val rollDeg: Float,
    )
}
