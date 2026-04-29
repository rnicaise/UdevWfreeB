package com.qorvo.uwbreceiver

import android.Manifest
import android.content.Intent
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.qorvo.uwbreceiver.ui.UwbMainScreen
import com.qorvo.uwbreceiver.ui.theme.UwbReceiverTheme
import com.qorvo.uwbreceiver.viewmodel.UwbViewModel

class MainActivity : ComponentActivity() {
    private val viewModel: UwbViewModel by viewModels()

    private val permissionsLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) { }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        val permissions = mutableListOf(
            Manifest.permission.ACCESS_COARSE_LOCATION,
            Manifest.permission.ACCESS_FINE_LOCATION,
        )
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            permissions.add(Manifest.permission.POST_NOTIFICATIONS)
        }
        permissionsLauncher.launch(permissions.toTypedArray())

        setContent {
            UwbReceiverTheme {
                val uiState by viewModel.uiState.collectAsStateWithLifecycle()
                val shareUri by viewModel.shareUri.collectAsStateWithLifecycle()

                UwbMainScreen(
                    state = uiState,
                    onConnect = viewModel::connect,
                    onDisconnect = viewModel::disconnect,
                    onStartRecording = viewModel::startRecording,
                    onStopRecording = viewModel::stopRecording,
                    onShare = { viewModel.requestShare(uiState.runtime.lastSavedUri) },
                    onGreenChange = viewModel::updateGreenMax,
                    onOrangeChange = viewModel::updateOrangeMax,
                    onMedianWindowChange = viewModel::updateMedianWindow,
                    onUwbDataRateChange = viewModel::updateUwbDataRateKbps,
                    onAcquisitionPeriodChange = viewModel::updateAcquisitionPeriodMs,
                    onRangingModeChange = viewModel::updateRangingMode,
                    onTestProfileChange = viewModel::applyTestProfile,
                    onPreset20msStable = viewModel::applyPreset20msStable,
                    onPresetMaxSpeed = viewModel::applyPresetMaxSpeed,
                    onPresetOutdoorRobust = viewModel::applyPresetOutdoorRobust,
                    onApplyUwbSettings = viewModel::applyUwbSettings,
                )

                LaunchedEffect(shareUri) {
                    val uri = shareUri ?: return@LaunchedEffect
                    val sendIntent = Intent(Intent.ACTION_SEND).apply {
                        type = "text/csv"
                        putExtra(Intent.EXTRA_STREAM, uri)
                        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    }
                    startActivity(Intent.createChooser(sendIntent, "Share CSV"))
                    viewModel.consumeShareRequest()
                }
            }
        }
    }
}
