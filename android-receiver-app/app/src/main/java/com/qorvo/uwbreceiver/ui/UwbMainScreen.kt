package com.qorvo.uwbreceiver.ui

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.qorvo.uwbreceiver.data.CsvSample
import com.qorvo.uwbreceiver.data.LinkState
import com.qorvo.uwbreceiver.data.UwbControlSettings
import com.qorvo.uwbreceiver.data.UwbUiState
import com.qorvo.uwbreceiver.ui.theme.GreenGood
import com.qorvo.uwbreceiver.ui.theme.OrangeWarn
import com.qorvo.uwbreceiver.ui.theme.RedAlert
import com.qorvo.uwbreceiver.ui.theme.SurfaceCard
import com.qorvo.uwbreceiver.ui.theme.SurfaceCardAlt
import com.qorvo.uwbreceiver.ui.theme.TextSecondary

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun UwbMainScreen(
    state: UwbUiState,
    onConnect: () -> Unit,
    onDisconnect: () -> Unit,
    onStartRecording: () -> Unit,
    onStopRecording: () -> Unit,
    onShare: () -> Unit,
    onGreenChange: (Float) -> Unit,
    onOrangeChange: (Float) -> Unit,
    onMedianWindowChange: (Int) -> Unit,
    onUwbDataRateChange: (Int) -> Unit,
    onAcquisitionPeriodChange: (Int) -> Unit,
    onPreset20msStable: () -> Unit,
    onPresetMaxSpeed: () -> Unit,
    onPresetOutdoorRobust: () -> Unit,
    onApplyUwbSettings: () -> Unit,
) {
    val sample = state.runtime.latest
    val rawDistance = sample?.dist ?: 0f
    val distance = state.runtime.displayDist ?: rawDistance
    val maxGauge = (state.thresholds.orangeMax + 1.0f).coerceAtLeast(1.5f)

    val gaugeProgress = (distance / maxGauge).coerceIn(0f, 1f)
    val animatedGauge = animateFloatAsState(
        targetValue = gaugeProgress,
        animationSpec = tween(280),
        label = "gauge",
    ).value

    val gaugeColor = when {
        distance <= state.thresholds.greenMax -> GreenGood
        distance <= state.thresholds.orangeMax -> OrangeWarn
        else -> RedAlert
    }

    LazyColumn(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .padding(horizontal = 14.dp, vertical = 10.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            Text(
                text = "UWB Receiver",
                fontSize = 26.sp,
                fontWeight = FontWeight.ExtraBold,
            )
            Text(
                text = "USB OTG live acquisition",
                color = TextSecondary,
            )
            Text(
                text = "std(5s): ${state.runtime.std5s?.let { String.format("%.3f m", it) } ?: "--"} | std(30s): ${state.runtime.std30s?.let { String.format("%.3f m", it) } ?: "--"}",
                color = TextSecondary,
            )
        }

        item {
            CardBlock {
                Text("Distance", fontWeight = FontWeight.Bold)
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        text = if (sample == null) "--" else String.format("%.2f m", distance),
                        fontSize = 46.sp,
                        fontWeight = FontWeight.ExtraBold,
                        color = gaugeColor,
                    )

                    Box(
                        modifier = Modifier
                            .width(140.dp)
                            .height(90.dp),
                        contentAlignment = Alignment.Center,
                    ) {
                        Canvas(modifier = Modifier.fillMaxSize()) {
                            val stroke = 14.dp.toPx()
                            val radius = size.minDimension / 2f - stroke
                            val center = Offset(size.width / 2f, size.height)
                            drawArc(
                                color = SurfaceCardAlt,
                                startAngle = 180f,
                                sweepAngle = 180f,
                                useCenter = false,
                                topLeft = Offset(center.x - radius, center.y - radius),
                                size = androidx.compose.ui.geometry.Size(radius * 2, radius * 2),
                                style = Stroke(width = stroke, cap = StrokeCap.Round),
                            )
                            drawArc(
                                color = gaugeColor,
                                startAngle = 180f,
                                sweepAngle = 180f * animatedGauge,
                                useCenter = false,
                                topLeft = Offset(center.x - radius, center.y - radius),
                                size = androidx.compose.ui.geometry.Size(radius * 2, radius * 2),
                                style = Stroke(width = stroke, cap = StrokeCap.Round),
                            )
                        }
                    }
                }
                Text(
                    text = "Thresholds: green <= ${state.thresholds.greenMax}m, orange <= ${state.thresholds.orangeMax}m",
                    color = TextSecondary,
                )
                if (sample != null) {
                    Text(
                        text = "Raw ${String.format("%.2f", rawDistance)} m | Filtered ${String.format("%.2f", distance)} m",
                        color = TextSecondary,
                    )
                }
            }
        }

        item {
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                CardBlock(modifier = Modifier.weight(1f)) {
                    Text("Initiator accel", fontWeight = FontWeight.Bold)
                    TripleValues(sample, true)
                }
                CardBlock(modifier = Modifier.weight(1f)) {
                    Text("Receiver accel", fontWeight = FontWeight.Bold)
                    TripleValues(sample, false)
                }
            }
        }

        item {
            CardBlock {
                Text("Session", fontWeight = FontWeight.Bold)
                StatRow("Connection", state.runtime.linkState.name)
                StatRow("Status", state.runtime.status)
                StatRow("Real Hz", String.format("%.1f", state.runtime.hz))
                StatRow("Samples", state.runtime.samples.toString())
                StatRow("Duration", formatDuration(state.elapsedSec))
                StatRow("Recording", if (state.runtime.recording) "ON" else "OFF")
                StatRow("File", state.runtime.recordingName ?: "-")
                StatRow("Invalid lines", state.runtime.invalidLines.toString())
            }
        }

        item {
            CardBlock {
                Text("Phone sensors", fontWeight = FontWeight.Bold)
                val phone = state.runtime.phoneTelemetry
                StatRow(
                    "Gyro rad/s",
                    listOf(phone.gyroX, phone.gyroY, phone.gyroZ)
                        .joinToString(",") { v -> if (v == null) "--" else String.format("%.2f", v) }
                )
                StatRow(
                    "GPS",
                    if (phone.latitude == null || phone.longitude == null) {
                        "no fix"
                    } else {
                        String.format("%.6f, %.6f", phone.latitude, phone.longitude)
                    },
                )
                StatRow("Alt m", phone.altitudeM?.let { String.format("%.1f", it) } ?: "--")
                StatRow("Speed m/s", phone.speedMps?.let { String.format("%.2f", it) } ?: "--")
                StatRow("GPS age ms", phone.fixElapsedMs?.toString() ?: "--")
            }
        }

        item {
            CardBlock {
                Text("Controls", fontWeight = FontWeight.Bold)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = onConnect, modifier = Modifier.weight(1f)) { Text("Connect") }
                    Button(onClick = onDisconnect, modifier = Modifier.weight(1f)) { Text("Disconnect") }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = onStartRecording,
                        modifier = Modifier.weight(1f),
                        enabled = state.runtime.linkState == LinkState.CONNECTED && !state.runtime.recording,
                    ) { Text("Start Recording") }
                    Button(
                        onClick = onStopRecording,
                        modifier = Modifier.weight(1f),
                        enabled = state.runtime.recording,
                    ) { Text("Stop Recording") }
                }
                Button(
                    onClick = onShare,
                    enabled = state.runtime.lastSavedUri != null,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Text("Share last CSV")
                }
            }
        }

        item {
            CardBlock {
                Text("Distance settings", fontWeight = FontWeight.Bold)
                Text("Green max: ${String.format("%.2f", state.thresholds.greenMax)} m", color = TextSecondary)
                Slider(
                    value = state.thresholds.greenMax,
                    onValueChange = onGreenChange,
                    valueRange = 0.1f..5.0f,
                )

                Text("Orange max: ${String.format("%.2f", state.thresholds.orangeMax)} m", color = TextSecondary)
                Slider(
                    value = state.thresholds.orangeMax,
                    onValueChange = { onOrangeChange(it.coerceAtLeast(state.thresholds.greenMax + 0.1f)) },
                    valueRange = 0.2f..7.0f,
                )
            }
        }

        item {
            CardBlock {
                Text("UWB controls", fontWeight = FontWeight.Bold)
                val presetName = presetLabel(state.controls)
                Text("Preset actif: $presetName", color = TextSecondary)

                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = onPreset20msStable,
                        modifier = Modifier.weight(1f),
                        enabled = state.runtime.linkState == LinkState.CONNECTED,
                    ) {
                        Text("20ms stable")
                    }
                    Button(
                        onClick = onPresetMaxSpeed,
                        modifier = Modifier.weight(1f),
                        enabled = state.runtime.linkState == LinkState.CONNECTED,
                    ) {
                        Text("Max speed")
                    }
                }
                Button(
                    onClick = onPresetOutdoorRobust,
                    modifier = Modifier.fillMaxWidth(),
                    enabled = state.runtime.linkState == LinkState.CONNECTED,
                ) {
                    Text("Outdoor robust")
                }

                Text("Median window: ${state.controls.medianWindow}", color = TextSecondary)
                Slider(
                    value = state.controls.medianWindow.toFloat(),
                    onValueChange = { onMedianWindowChange(it.toInt().coerceIn(1, 31)) },
                    valueRange = 1f..31f,
                )

                Text(
                    "Requested UWB data rate: ${state.controls.uwbDataRateKbps} kbps",
                    color = TextSecondary,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = { onUwbDataRateChange(850) },
                        modifier = Modifier.weight(1f),
                    ) {
                        Text("850 kbps")
                    }
                    Button(
                        onClick = { onUwbDataRateChange(6800) },
                        modifier = Modifier.weight(1f),
                    ) {
                        Text("6.8 Mbps")
                    }
                }

                Text(
                    "Acquisition period target: ${state.controls.acquisitionPeriodMs} ms (${String.format("%.1f", 1000f / state.controls.acquisitionPeriodMs)} Hz target)",
                    color = TextSecondary,
                )
                Slider(
                    value = state.controls.acquisitionPeriodMs.toFloat(),
                    onValueChange = { onAcquisitionPeriodChange(it.toInt().coerceIn(1, 200)) },
                    valueRange = 1f..200f,
                )

                Button(
                    onClick = onApplyUwbSettings,
                    enabled = state.runtime.linkState == LinkState.CONNECTED,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Text("Apply UWB settings")
                }
                Text(
                    text = "Note: data-rate command requires firmware command support.",
                    color = TextSecondary,
                )
                Text(
                    text = "Note: Real Hz est la fréquence réellement observée, elle dépend du débit radio, des timeouts et de l'environnement.",
                    color = TextSecondary,
                )
            }
        }
    }
}

@Composable
private fun CardBlock(modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    Card(
        modifier = modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = SurfaceCard),
        shape = RoundedCornerShape(18.dp),
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(14.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
            content = content,
        )
    }
}

@Composable
private fun TripleValues(sample: CsvSample?, initiator: Boolean) {
    val x = if (initiator) sample?.iax else sample?.rax
    val y = if (initiator) sample?.iay else sample?.ray
    val z = if (initiator) sample?.iaz else sample?.raz

    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text("X: ${x ?: "--"}")
        Text("Y: ${y ?: "--"}")
        Text("Z: ${z ?: "--"}")
    }
}

@Composable
private fun StatRow(label: String, value: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(10.dp))
            .background(SurfaceCardAlt)
            .padding(horizontal = 10.dp, vertical = 8.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Text(label, color = TextSecondary)
        Text(value, fontWeight = FontWeight.Medium)
    }
}

private fun formatDuration(sec: Long): String {
    val h = sec / 3600
    val m = (sec % 3600) / 60
    val s = sec % 60
    return "%02d:%02d:%02d".format(h, m, s)
}

private fun presetLabel(controls: UwbControlSettings): String {
    return when {
        controls.uwbDataRateKbps == 6800 && controls.acquisitionPeriodMs == 20 && controls.medianWindow == 5 -> "20ms stable"
        controls.uwbDataRateKbps == 6800 && controls.acquisitionPeriodMs == 1 && controls.medianWindow == 3 -> "Max speed"
        controls.uwbDataRateKbps == 850 && controls.acquisitionPeriodMs == 20 && controls.medianWindow == 7 -> "Outdoor robust"
        else -> "Custom"
    }
}
