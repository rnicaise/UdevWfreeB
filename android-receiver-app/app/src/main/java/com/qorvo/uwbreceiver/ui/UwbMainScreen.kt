package com.qorvo.uwbreceiver.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.qorvo.uwbreceiver.data.LinkState
import com.qorvo.uwbreceiver.data.UwbUiState
import com.qorvo.uwbreceiver.ui.theme.GreenGood
import com.qorvo.uwbreceiver.ui.theme.OrangeWarn
import com.qorvo.uwbreceiver.ui.theme.RedAlert
import com.qorvo.uwbreceiver.ui.theme.SurfaceCard
import com.qorvo.uwbreceiver.ui.theme.TextSecondary

@Composable
fun UwbMainScreen(
    state: UwbUiState,
    onConnect: () -> Unit,
    onDisconnect: () -> Unit,
    onStartRecording: () -> Unit,
    onStopRecording: () -> Unit,
    onShareCsv: () -> Unit,
    onShareReport: () -> Unit,
) {
    val runtime = state.runtime
    val sample = runtime.latest

    LazyColumn(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .padding(horizontal = 14.dp, vertical = 10.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            Text(
                text = "UWB Pure Ranging",
                fontSize = 26.sp,
                fontWeight = FontWeight.ExtraBold,
            )
            val linkColor = when (runtime.linkState) {
                LinkState.CONNECTED -> GreenGood
                LinkState.CONNECTING -> OrangeWarn
                LinkState.DISCONNECTED -> RedAlert
            }
            Text(text = runtime.status, color = linkColor, fontWeight = FontWeight.Bold)
        }

        item {
            CardBlock {
                Text("Distance", color = TextSecondary, fontWeight = FontWeight.Bold)
                Text(
                    text = if (sample == null) "--" else String.format("%.2f m", sample.dist),
                    fontSize = 56.sp,
                    fontWeight = FontWeight.ExtraBold,
                    color = if (sample?.valid == false) OrangeWarn else GreenGood,
                )
                if (sample != null) {
                    Text(
                        text = "filt ${sample.distFilt?.let { String.format("%.2f", it) } ?: "--"} m" +
                            " | smooth ${sample.distSmooth?.let { String.format("%.2f", it) } ?: "--"} m" +
                            " | cppm ${sample.cppm?.let { String.format("%.1f", it) } ?: "--"}",
                        color = TextSecondary,
                    )
                }
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                ) {
                    Text("${runtime.hz.toInt()} Hz", color = TextSecondary)
                    Text("samples ${runtime.samples}", color = TextSecondary)
                    Text("invalid ${runtime.invalidLines}", color = TextSecondary)
                }
            }
        }

        item {
            CardBlock {
                Text("Acquisition", fontWeight = FontWeight.Bold)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = onConnect,
                        modifier = Modifier.weight(1f),
                        enabled = runtime.linkState == LinkState.DISCONNECTED,
                    ) { Text("Connect") }
                    Button(
                        onClick = onDisconnect,
                        modifier = Modifier.weight(1f),
                        enabled = runtime.linkState != LinkState.DISCONNECTED,
                    ) { Text("Disconnect") }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = onStartRecording,
                        modifier = Modifier.weight(1f),
                        enabled = runtime.linkState == LinkState.CONNECTED && !runtime.recording,
                    ) { Text("Start Recording") }
                    Button(
                        onClick = onStopRecording,
                        modifier = Modifier.weight(1f),
                        enabled = runtime.recording,
                    ) { Text("Stop + Analyze") }
                }
                if (runtime.recording) {
                    Text(
                        text = "REC ${runtime.recordingName ?: ""} | rows ${runtime.recordedRows}",
                        color = RedAlert,
                        fontWeight = FontWeight.Bold,
                    )
                }
            }
        }

        item {
            CardBlock {
                Text("Export", fontWeight = FontWeight.Bold)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = onShareCsv,
                        modifier = Modifier.weight(1f),
                        enabled = runtime.lastCsvUri != null && !runtime.recording,
                    ) { Text("Share CSV") }
                    Button(
                        onClick = onShareReport,
                        modifier = Modifier.weight(1f),
                        enabled = runtime.lastReportUri != null,
                    ) { Text("Share report") }
                }
            }
        }

        if (runtime.analyzing || runtime.analysisText != null) {
            item {
                CardBlock {
                    Text("Rapport qualite", fontWeight = FontWeight.Bold)
                    if (runtime.analyzing) {
                        Text("Analyse en cours…", color = OrangeWarn)
                    } else {
                        Text(
                            text = runtime.analysisText ?: "",
                            color = TextSecondary,
                            fontFamily = FontFamily.Monospace,
                            fontSize = 12.sp,
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun CardBlock(
    modifier: Modifier = Modifier,
    content: @Composable () -> Unit,
) {
    Card(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(14.dp),
        colors = CardDefaults.cardColors(containerColor = SurfaceCard),
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
            horizontalAlignment = Alignment.Start,
        ) {
            content()
        }
    }
}
