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

    private val notificationPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
        }

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
