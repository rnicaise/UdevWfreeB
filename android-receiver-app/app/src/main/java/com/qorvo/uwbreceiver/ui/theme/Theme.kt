package com.qorvo.uwbreceiver.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable

private val UwbColors = darkColorScheme(
    primary = Accent,
    secondary = GreenGood,
    background = Bg,
    surface = SurfaceCard,
)

@Composable
fun UwbReceiverTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = UwbColors,
        typography = Typography,
        content = content,
    )
}
