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
import androidx.core.app.NotificationCompat
import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber
import com.qorvo.uwbreceiver.MainActivity
import com.qorvo.uwbreceiver.R
import com.qorvo.uwbreceiver.data.CsvParser
import com.qorvo.uwbreceiver.data.LinkState
import com.qorvo.uwbreceiver.data.RecordingManager
import com.qorvo.uwbreceiver.data.RuntimeStore
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

    private var activeDriver: UsbSerialDriver? = null
    private var activePort: UsbSerialPort? = null

    private var wakeLock: PowerManager.WakeLock? = null

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

        val sample = CsvParser.parse(line)
        if (sample == null) {
            RuntimeStore.onInvalidLine()
            return
        }

        RuntimeStore.onSample(sample)
        recordingManager.appendRawLine(line)
    }

    private fun closePort() {
        readJob?.cancel()
        readJob = null

        try {
            activePort?.close()
        } catch (_: Exception) {
        }
        activePort = null
        activeDriver = null
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

        const val ACTION_CONNECT = "com.qorvo.uwbreceiver.action.CONNECT"
        const val ACTION_DISCONNECT = "com.qorvo.uwbreceiver.action.DISCONNECT"
        const val ACTION_START_RECORDING = "com.qorvo.uwbreceiver.action.START_RECORDING"
        const val ACTION_STOP_RECORDING = "com.qorvo.uwbreceiver.action.STOP_RECORDING"

        private const val ACTION_USB_PERMISSION = "com.qorvo.uwbreceiver.action.USB_PERMISSION"
    }
}
