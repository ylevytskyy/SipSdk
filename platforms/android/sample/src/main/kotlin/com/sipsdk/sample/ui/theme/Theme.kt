package com.sipsdk.sample.ui.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

val IosBlue = Color(0xFF007AFF)
val IosGreen = Color(0xFF34C759)
val IosRed = Color(0xFFFF3B30)
val IosOrange = Color(0xFFFF9500)
val IosYellow = Color(0xFFFFCC00)
val IosGray = Color(0xFF8E8E93)

// Light Palette
val IosLightBg = Color(0xFFF2F2F7)
val IosLightCard = Color(0xFFFFFFFF)
val IosLightTextPrimary = Color(0xFF000000)
val IosLightTextSecondary = Color(0xFF8E8E93)
val IosLightSeparator = Color(0xFFE5E5EA)
val IosLightFill = Color(0xFFE5E5EA)

// Dark Palette
val IosDarkBg = Color(0xFF000000)
val IosDarkCard = Color(0xFF1C1C1E)
val IosDarkTextPrimary = Color(0xFFFFFFFF)
val IosDarkTextSecondary = Color(0xFF8E8E93)
val IosDarkSeparator = Color(0xFF38383A)
val IosDarkFill = Color(0xFF2C2C2E)

private val LightColorScheme = lightColorScheme(
    primary = IosBlue,
    onPrimary = Color.White,
    background = IosLightBg,
    onBackground = IosLightTextPrimary,
    surface = IosLightCard,
    onSurface = IosLightTextPrimary,
    surfaceVariant = IosLightFill,
    onSurfaceVariant = IosLightTextSecondary,
    outline = IosLightSeparator,
    error = IosRed
)

private val DarkColorScheme = darkColorScheme(
    primary = IosBlue,
    onPrimary = Color.White,
    background = IosDarkBg,
    onBackground = IosDarkTextPrimary,
    surface = IosDarkCard,
    onSurface = IosDarkTextPrimary,
    surfaceVariant = IosDarkFill,
    onSurfaceVariant = IosDarkTextSecondary,
    outline = IosDarkSeparator,
    error = IosRed
)

@Composable
fun SipPhoneTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    content: @Composable () -> Unit
) {
    val colorScheme = if (darkTheme) DarkColorScheme else LightColorScheme

    MaterialTheme(
        colorScheme = colorScheme,
        content = content
    )
}
