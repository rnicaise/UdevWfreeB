package com.qorvo.uwbreceiver.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
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
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.qorvo.uwbreceiver.data.ConnectedUwbRole
import com.qorvo.uwbreceiver.data.CsvSample
import com.qorvo.uwbreceiver.data.LinkState
import com.qorvo.uwbreceiver.data.SignalQualityCalculator
import com.qorvo.uwbreceiver.data.TestProfile
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
    onRfChannelChange: (Int) -> Unit,
    onAcquisitionPeriodChange: (Int) -> Unit,
    onBikeBoxPositionChange: (Int) -> Unit,
    onVestBoxPositionChange: (Int) -> Unit,
    onTestProfileChange: (TestProfile) -> Unit,
    onPresetMaxSpeed: () -> Unit,
    onApplyUwbSettings: () -> Unit,
) {
    val sample = state.runtime.latest
    val quality = state.runtime.sessionQuality
    val rawDistance = sample?.dist ?: 0f
    val distance = state.runtime.displayDist ?: rawDistance
    val transmission = state.runtime.transmissionQuality
    val distanceColor = when {
        distance <= state.thresholds.greenMax -> GreenGood
        distance <= state.thresholds.orangeMax -> OrangeWarn
        else -> RedAlert
    }
    val signal = SignalQualityCalculator.fromSample(sample)
    val selectedMode = "SS_TWR"
    val roleSubtitle = when (state.runtime.connectedRole) {
        ConnectedUwbRole.INITIATOR -> "Initiator detecte · SS-TWR actif"
        ConnectedUwbRole.RESPONDER -> "Responder detecte · SS-TWR responder"
        ConnectedUwbRole.UNKNOWN -> "Connecte un boitier; l'app detecte automatiquement son role"
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
                text = "UWB Ranging",
                fontSize = 26.sp,
                fontWeight = FontWeight.ExtraBold,
            )
            Text(
                text = roleSubtitle,
                color = TextSecondary,
            )
            Text(
                text = "std(5s): ${state.runtime.std5s?.let { String.format("%.3f m", it) } ?: "--"} | std(30s): ${state.runtime.std30s?.let { String.format("%.3f m", it) } ?: "--"}",
                color = TextSecondary,
            )
        }

        item {
            CardBlock {
                Text("Distance + Transmission quality", fontWeight = FontWeight.Bold)
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text("Distance", color = TextSecondary, fontWeight = FontWeight.Bold)
                        Text(
                            text = if (sample == null) "--" else String.format("%.2f m", distance),
                            fontSize = 46.sp,
                            fontWeight = FontWeight.ExtraBold,
                            color = distanceColor,
                        )
                    }

                    Column(
                        modifier = Modifier.weight(1.25f),
                        verticalArrangement = Arrangement.spacedBy(6.dp),
                    ) {
                        Text("Transmission", color = TextSecondary, fontWeight = FontWeight.Bold)
                        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                            SignalCategoryTile("Link", transmission.linkReliabilityScore10, formatPercent(transmission.validRate5s), Modifier.weight(1f))
                            SignalCategoryTile("Stab", transmission.stabilityScore10, formatMeters(transmission.rollingStd5sM), Modifier.weight(1f))
                        }
                        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                            SignalCategoryTile("Jump", transmission.jumpScore10, formatPercent(transmission.jumpRate5s), Modifier.weight(1f))
                            SignalCategoryTile("NLOS", transmission.nlosScore10, formatPeakGap(sample?.peakToFirstPathSamples), Modifier.weight(1f))
                        }
                    }
                }
                Text(
                    text = "Qualite utile: link reliability, stabilité, jump rate, burst/dropouts et NLOS/CIR. Les dB restent indicatifs seulement.",
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
                StatRow("Connected role", state.runtime.connectedRole.name)
                StatRow("Requested mode", selectedMode)
                StatRow("Real Hz", String.format("%.1f", state.runtime.hz))
                StatRow("Resp acq ms", sample?.responderAcquisitionPeriodMs?.toString() ?: "--")
                StatRow("Init acq ms", sample?.initiatorAcquisitionPeriodMs?.toString() ?: "--")
                StatRow("Resp profile", profileLabel(sample?.responderProfileOpt))
                StatRow("Init profile", profileLabel(sample?.initiatorProfileOpt))
                StatRow("Bike box position", state.experiment.bikeBoxPosition.toString())
                StatRow("Vest box position", state.experiment.vestBoxPosition.toString())
                StatRow("App preset", presetLabel(state.controls))
                StatRow("App test profile", state.controls.testProfile.name)
                StatRow("App RF channel", state.controls.rfChannel.toString())
                StatRow("Samples", state.runtime.samples.toString())
                StatRow("Duration", formatDuration(state.elapsedSec))
                StatRow("Recording", if (state.runtime.recording) "ON" else "OFF")
                StatRow("File", state.runtime.recordingName ?: "-")
                Text("Transmission quality", fontWeight = FontWeight.Bold)
                StatRow("Link reliability", formatScoreWithDetail(transmission.linkReliabilityScore10, formatPercent(transmission.validRate5s)))
                StatRow("Stability", formatScoreWithDetail(transmission.stabilityScore10, formatMeters(transmission.rollingStd5sM)))
                StatRow("Smoothness", formatScoreWithDetail(transmission.smoothnessScore10, formatMps(transmission.lastRelativeSpeedMps)))
                StatRow("Jump rate", formatScoreWithDetail(transmission.jumpScore10, formatPercent(transmission.jumpRate5s)))
                StatRow("Timing", formatScoreWithDetail(transmission.timingScore10, formatHz(transmission.lastInstantHz)))
                StatRow("Dropout", formatScoreWithDetail(transmission.dropoutScore10, formatPercent(transmission.timingAnomalyRate)))
                StatRow("Bad burst max", transmission.badBurstMax.toString())
                StatRow("NLOS/CIR", formatScoreWithDetail(transmission.nlosScore10, "gap ${formatPeakGap(sample?.peakToFirstPathSamples)} · conf ${sample?.firstPathConfidence ?: "--"}"))
                Text("Radio raw", fontWeight = FontWeight.Bold)
                StatRow("RX / first path", "${formatDbm(sample?.rxPowerDbm)} / ${formatDbm(sample?.firstPathPowerDbm)}")
                StatRow("Multipath gap", formatDb(signal.multipathGapDb))
                StatRow("Clock offset", formatPpm(sample?.clockOffsetPpm))
                StatRow("Session std", quality.sessionStd?.let { String.format("%.3f m", it) } ?: "--")
                StatRow("Speed spikes >10m/s", quality.speedSpikeCount.toString())
                StatRow("Hz anomalies >100", quality.frequencyAnomalyCount.toString())
                StatRow("Last rel speed", quality.lastRelativeSpeedMps?.let { String.format("%.2f m/s", it) } ?: "--")
                StatRow("Last inst Hz", quality.lastInstantHz?.let { String.format("%.1f", it) } ?: "--")
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
                Text("Plan d'expérience", fontWeight = FontWeight.Bold)
                Text(
                    "Choix manuel enregistré dans chaque ligne du CSV pour comparer les essais.",
                    color = TextSecondary,
                )
                ExperimentPositionSelector(
                    title = "Boîtier vélo",
                    selected = state.experiment.bikeBoxPosition,
                    onSelect = onBikeBoxPositionChange,
                )
                ExperimentPositionSelector(
                    title = "Veste",
                    selected = state.experiment.vestBoxPosition,
                    onSelect = onVestBoxPositionChange,
                )
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
                Text("Test profile: ${testProfileLabel(state.controls.testProfile)}", color = TextSecondary)

                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = { onTestProfileChange(TestProfile.FAST_DISTANCE_ONLY) },
                        modifier = Modifier.weight(1f),
                        enabled = state.runtime.linkState == LinkState.CONNECTED,
                    ) {
                        Text("Fast dist")
                    }
                    Button(
                        onClick = { onTestProfileChange(TestProfile.TURBO_DISTANCE_ONLY) },
                        modifier = Modifier.weight(1f),
                        enabled = state.runtime.linkState == LinkState.CONNECTED,
                    ) {
                        Text("Turbo max")
                    }
                }

                Text("Ranging mode: $selectedMode", color = TextSecondary)

                Button(
                    onClick = onPresetMaxSpeed,
                    modifier = Modifier.fillMaxWidth(),
                    enabled = state.runtime.linkState == LinkState.CONNECTED,
                ) {
                    Text("Max speed")
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
                        enabled = false,
                    ) {
                        Text("850 boot only")
                    }
                    Button(
                        onClick = { onUwbDataRateChange(6800) },
                        modifier = Modifier.weight(1f),
                    ) {
                        Text("6.8 Mbps")
                    }
                }

                Text("RF channel: ${state.controls.rfChannel}", color = TextSecondary)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = { onRfChannelChange(5) },
                        modifier = Modifier.weight(1f),
                        enabled = state.runtime.linkState == LinkState.CONNECTED,
                    ) {
                        Text(if (state.controls.rfChannel == 5) "✓ Channel 5" else "Channel 5")
                    }
                    Button(
                        onClick = { onRfChannelChange(9) },
                        modifier = Modifier.weight(1f),
                        enabled = state.runtime.linkState == LinkState.CONNECTED,
                    ) {
                        Text(if (state.controls.rfChannel == 9) "✓ Channel 9" else "Channel 9")
                    }
                }
                Text("Ch5 = actuel; Ch9 = test RF alternatif pour multipath/cadre métallique.", color = TextSecondary)

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
                    text = "Note: 850 kbps est expérimental et non-switchable à chaud; il faudra un profil firmware boot dédié.",
                    color = TextSecondary,
                )
                Text(
                    text = "Note: le mode est choisi automatiquement selon le role du boitier branche en USB.",
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
private fun ExperimentPositionSelector(title: String, selected: Int, onSelect: (Int) -> Unit) {
    val positions = listOf(1, -1, 2, -2, 3, -3)
    Text("$title: $selected", color = TextSecondary, fontWeight = FontWeight.Bold)
    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        positions.forEach { position ->
            Button(
                onClick = { onSelect(position) },
                modifier = Modifier.weight(1f),
            ) {
                Text(if (position == selected) "✓ $position" else position.toString())
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
private fun SignalCategoryTile(title: String, score: Float?, detail: String, modifier: Modifier = Modifier) {
    Column(
        modifier = modifier
            .clip(RoundedCornerShape(12.dp))
            .background(SurfaceCardAlt)
            .padding(horizontal = 8.dp, vertical = 7.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(title, color = TextSecondary, fontSize = 12.sp, fontWeight = FontWeight.Bold)
        Text(
            text = score?.let { String.format("%.0f", it) } ?: "--",
            fontSize = 25.sp,
            fontWeight = FontWeight.ExtraBold,
            color = signalQualityColor(score),
        )
        Text(detail, color = TextSecondary, fontSize = 10.sp)
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
        controls.uwbDataRateKbps == 6800 && controls.acquisitionPeriodMs == 1 && controls.medianWindow == 1 && controls.testProfile == TestProfile.TURBO_DISTANCE_ONLY -> "Turbo experimental"
        controls.uwbDataRateKbps == 6800 && controls.acquisitionPeriodMs == 1 && controls.medianWindow == 1 && controls.testProfile == TestProfile.FAST_DISTANCE_ONLY -> "Max speed safe"
        else -> "Custom"
    }
}

private fun testProfileLabel(profile: TestProfile): String {
    return when (profile) {
        TestProfile.TURBO_DISTANCE_ONLY -> "Turbo max"
        TestProfile.FAST_DISTANCE_ONLY -> "Fast distance only"
    }
}

private fun signalQualityColor(score: Float?) = when {
    score == null -> TextSecondary
    score >= 8f -> GreenGood
    score >= 5f -> OrangeWarn
    else -> RedAlert
}

private fun formatDbm(value: Float?): String {
    return value?.let { String.format("%.1f dBm", it) } ?: "--"
}

private fun formatDb(value: Float?): String {
    return value?.let { String.format("%.1f dB", it) } ?: "--"
}

private fun formatMeters(value: Float?): String {
    return value?.let { String.format("%.3f m", it) } ?: "--"
}

private fun formatMps(value: Float?): String {
    return value?.let { String.format("%.2f m/s", it) } ?: "--"
}

private fun formatHz(value: Float?): String {
    return value?.let { String.format("%.1f Hz", it) } ?: "--"
}

private fun formatPercent(value: Float?): String {
    return value?.let { String.format("%.0f%%", it * 100f) } ?: "--"
}

private fun formatPeakGap(value: Float?): String {
    return value?.let { String.format("%.1f spl", it) } ?: "--"
}

private fun formatPpm(value: Float?): String {
    return value?.let { String.format("%.2f ppm", it) } ?: "--"
}

private fun formatScoreWithDetail(score: Float?, detail: String): String {
    val scoreText = score?.let { String.format("%.0f/10", it) } ?: "--"
    return if (detail == "--") scoreText else "$scoreText · $detail"
}

private fun profileLabel(profileOpt: Int?): String {
    return when (profileOpt) {
        35 -> "35 / 6.8M stable"
        36 -> "36 / 6.8M ch9"
        40 -> "40 / 850K robust"
        null -> "--"
        else -> profileOpt.toString()
    }
}
