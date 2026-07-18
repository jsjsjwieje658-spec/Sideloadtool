package com.superalpha.sideload.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.sp

val BrandBackground = Color(0xFF0B0F14)
val BrandSurface = Color(0xFF141B22)
val BrandPrimary = Color(0xFFF5C542)
val BrandPrimaryDark = Color(0xFFC79A1E)
val BrandAccent = Color(0xFF3DDC97)
val BrandDanger = Color(0xFFE5484D)
val BrandText = Color(0xFFEAF0F6)
val BrandTextDim = Color(0xFF8A97A5)

private val DarkColors = darkColorScheme(
    primary = BrandPrimary,
    onPrimary = Color(0xFF1A1400),
    secondary = BrandAccent,
    background = BrandBackground,
    surface = BrandSurface,
    onBackground = BrandText,
    onSurface = BrandText,
    error = BrandDanger,
)

val MonoTextStyle = TextStyle(fontFamily = FontFamily.Monospace, fontSize = 12.sp)

@Composable
fun SuperAlphaTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = DarkColors, content = content)
}
