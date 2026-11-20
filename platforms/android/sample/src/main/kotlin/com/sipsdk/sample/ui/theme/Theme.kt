package com.sipsdk.sample.ui.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

// Brand & Semantic Accents (exported names preserved for compatibility)
val IosBlue = Color(0xFF2563EB)
val IosGreen = Color(0xFF10B981)
val IosRed = Color(0xFFEF4444)
val IosOrange = Color(0xFFF59E0B)
val IosYellow = Color(0xFFFACC15)
val IosGray = Color(0xFF64748B)

// Call Surface Gradients
val CallGradientTop = Color(0xFF131D35)
val CallGradientBottom = Color(0xFF070B14)

// Light Palette
val IosLightBg = Color(0xFFF4F6FA)
val IosLightCard = Color(0xFFFFFFFF)
val IosLightTextPrimary = Color(0xFF0F172A)
val IosLightTextSecondary = Color(0xFF64748B)
val IosLightSeparator = Color(0xFFE2E8F0)
val IosLightFill = Color(0xFFEEF2F7)

// Dark Palette
val IosDarkBg = Color(0xFF0B1120)
val IosDarkCard = Color(0xFF141E33)
val IosDarkTextPrimary = Color(0xFFF8FAFC)
val IosDarkTextSecondary = Color(0xFF94A3B8)
val IosDarkSeparator = Color(0xFF23304A)
val IosDarkFill = Color(0xFF1E293B)

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
