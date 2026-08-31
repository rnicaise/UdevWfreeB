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
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.os.SystemClock
import androidx.core.app.NotificationCompat
import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber
import com.qorvo.uwbreceiver.MainActivity
import com.qorvo.uwbreceiver.R
import com.qorvo.uwbreceiver.data.CsvParser
import com.qorvo.uwbreceiver.data.CsvSample
import com.qorvo.uwbreceiver.data.LinkState
import com.qorvo.uwbreceiver.data.RecordingManager
import com.qorvo.uwbreceiver.data.RuntimeStore
import com.qorvo.uwbreceiver.data.SessionAnalyzer
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

    private var connectJob: Job? = null
    private var readJob: Job? = null

    private var shouldConnect = false
    private var activePort: UsbSerialPort? = null
    private var wakeLock: PowerManager.WakeLock? = null

    private var lastAcceptedSample: CsvSample? = null
    private var lastUiPushElapsedMs = 0L
    private var consecutivePlausibilityRejects = 0

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

        createNotificationChannel()
        startForeground(NOTIF_ID, buildNotification("Idle", recording = false))

        acquireWakeLock()
        registerUsbReceiver()

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
                closePort()
                RuntimeStore.setLinkState(LinkState.DISCONNECTED, "Disconnected")
                stopSelf()
            }

            ACTION_START_RECORDING -> startRecordingInternal()
            ACTION_STOP_RECORDING -> stopRecordingInternal()
        }

        return START_STICKY
    }

    override fun onDestroy() {
        shouldConnect = false
        stopRecordingInternal()
        closePort()

        connectJob?.cancel()
        readJob?.cancel()
        serviceScope.cancel()

        unregisterReceiverSafe()
        releaseWakeLock()

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

            if (!openPort(driver)) {
                RuntimeStore.setLinkState(LinkState.CONNECTING, "Open failed, retrying")
                delay(1_500)
            }
        }
    }

    private fun findFirstDriver(): UsbSerialDriver? {
        return UsbSerialProber.getDefaultProber().findAllDrivers(usbManager).firstOrNull()
    }

    private fun requestPermission(device: UsbDevice) {
        val intent = Intent(ACTION_USB_PERMISSION)
        val flags = PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        val pendingIntent = PendingIntent.getBroadcast(this, 1001, intent, flags)
        usbManager.requestPermission(device, pendingIntent)
    }

    private fun openPort(driver: UsbSerialDriver): Boolean {
        return try {
            val connection = usbManager.openDevice(driver.device) ?: return false
            val port = driver.ports.firstOrNull() ?: return false
            port.open(connection)
            port.setParameters(
                SERIAL_BAUD_RATE,
                8,
                UsbSerialPort.STOPBITS_1,
                UsbSerialPort.PARITY_NONE,
            )

            activePort = port
            lastAcceptedSample = null
            consecutivePlausibilityRejects = 0
            RuntimeStore.onConnected("USB connected @ ${SERIAL_BAUD_RATE / 1000} kbps")
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
            val buffer = ByteArray(16384)
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

                accumulator.append(String(buffer, 0, len, Charsets.US_ASCII))
                if (accumulator.length > MAX_ACCUMULATED_SERIAL_CHARS) {
                    accumulator.setLength(0)
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
        if (line.isEmpty() || line.startsWith("#")) {
            return
        }

        if (!line[0].isDigit()) {
            /* Boot/info lines: "UWB PURE INIT v1.0", "ROLE,INITIATOR", "ERR,..." */
            if (line.startsWith("ROLE,") || line.startsWith("ERR,") || line.startsWith("UWB")) {
                RuntimeStore.setLinkState(LinkState.CONNECTED, line)
            }
            return
        }

        val sample = CsvParser.parse(line)
        if (sample == null || !isPlausibleSample(sample)) {
            RuntimeStore.onInvalidLine()
            return
        }

        lastAcceptedSample = sample
        consecutivePlausibilityRejects = 0

        /* Recording captures every sample (raw line); the UI/state pipeline is
         * throttled to ~10 Hz so the 920 Hz stream cannot overwhelm Compose. */
        recordingManager.appendLine(line)

        val nowElapsed = SystemClock.elapsedRealtime()
        if (nowElapsed - lastUiPushElapsedMs >= UI_PUSH_INTERVAL_MS) {
            lastUiPushElapsedMs = nowElapsed
            RuntimeStore.onSample(sample, recordingManager.rowCount)
        }
    }

    private fun isPlausibleSample(sample: CsvSample): Boolean {
        if (sample.dist < -5f || sample.dist > 100f) {
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

    /* If a corrupted line with a bogus sample counter ever gets accepted, every
     * genuine sample afterwards fails the sampleDelta check. Accept after enough
     * consecutive rejects to re-baseline. */
    private fun rebaselineAfterRejects(): Boolean {
        consecutivePlausibilityRejects++
        if (consecutivePlausibilityRejects >= PLAUSIBILITY_RESYNC_REJECTS) {
            consecutivePlausibilityRejects = 0
            return true
        }
        return false
    }

    private fun startRecordingInternal() {
        try {
            val (uri, fileName) = recordingManager.start()
            RuntimeStore.setRecording(true, fileName)
            RuntimeStore.setLastCsvUri(uri)
            RuntimeStore.setLinkState(RuntimeStore.state.value.linkState, "Recording")
        } catch (e: Exception) {
            Timber.e(e, "Failed to start recording")
            RuntimeStore.setLinkState(RuntimeStore.state.value.linkState, "Recording failed")
        }
    }

    private fun stopRecordingInternal() {
        val wasRecording = RuntimeStore.state.value.recording
        val fileName = RuntimeStore.state.value.recordingName
        val savedUri = recordingManager.stop()
        RuntimeStore.setRecording(false, null)
        if (savedUri == null) {
            return
        }
        RuntimeStore.setLastCsvUri(savedUri)

        if (wasRecording) {
            RuntimeStore.setAnalyzing(true)
            RuntimeStore.setLinkState(RuntimeStore.state.value.linkState, "Analyzing")
            serviceScope.launch {
                val result = try {
                    SessionAnalyzer.analyze(applicationContext, savedUri, fileName)
                } catch (e: Exception) {
                    Timber.e(e, "Analysis failed")
                    SessionAnalyzer.Result("Analyse echouee: ${e.message}", null)
                }
                RuntimeStore.setAnalysisResult(result.text, result.reportUri)
                RuntimeStore.setLinkState(RuntimeStore.state.value.linkState, "Report ready")
            }
        }
    }

    private fun closePort() {
        readJob?.cancel()
        readJob = null
        try {
            activePort?.close()
        } catch (_: Exception) {
        }
        activePort = null
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
        private const val SERIAL_BAUD_RATE = 1_000_000
        private const val MAX_ACCUMULATED_SERIAL_CHARS = 65536
        private const val UI_PUSH_INTERVAL_MS = 100L
        private const val PLAUSIBILITY_RESYNC_REJECTS = 50

        const val ACTION_CONNECT = "com.qorvo.uwbreceiver.action.CONNECT"
        const val ACTION_DISCONNECT = "com.qorvo.uwbreceiver.action.DISCONNECT"
        const val ACTION_START_RECORDING = "com.qorvo.uwbreceiver.action.START_RECORDING"
        const val ACTION_STOP_RECORDING = "com.qorvo.uwbreceiver.action.STOP_RECORDING"

        private const val ACTION_USB_PERMISSION = "com.qorvo.uwbreceiver.action.USB_PERMISSION"
    }
}
