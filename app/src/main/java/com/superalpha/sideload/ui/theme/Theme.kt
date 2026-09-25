package com.superalpha.sideload.ui.theme

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/*
 * ════════════════════════════════════════════════════════════════════════
 *  v50 — UI redesign
 *
 *  Bảng màu "Super Alpha" dark được mở rộng đầy đủ (thay vì chỉ 8 màu rời
 *  trước đây): mọi role M3 (container, outline, error container…) đều được
 *  đặt chủ động để các thành phần Material 3 (TextField, Card, Dialog,
 *  NavigationBar…) không rơi về màu xám mặc định của darkColorScheme.
 *
 *  Giữ nguyên tên các màu cũ (BrandBackground, BrandSurface, BrandPrimary,
 *  BrandAccent, BrandDanger, BrandText, BrandTextDim) để các file khác
 *  đang import không vỡ.
 * ════════════════════════════════════════════════════════════════════════
 */

val BrandBackground  = Color(0xFF0A0E13)   // nền sâu nhất (status bar/nav bar)
val BrandSurface     = Color(0xFF121A23)   // thẻ, surface chính
val BrandSurfaceAlt  = Color(0xFF18222D)   // surface nhấn/highlight
val BrandOutline     = Color(0xFF2A3947)   // viền
val BrandPrimary     = Color(0xFFF5C542)   // vàng Super Alpha
val BrandPrimaryDeep = Color(0xFFE2A62F)   // vàng đậm — gradient nút chính
val BrandPrimaryDark = Color(0xFFC79A1E)   // (giữ tên cũ)
val BrandAccent      = Color(0xFF3DDC97)   // mint — USB / thành công
val BrandInfo        = Color(0xFF57A8FF)   // xanh dương — tiến trình / log hệ thống
val BrandWarn        = Color(0xFFFFB224)   // cam — cảnh báo / chờ Trust
val BrandDanger      = Color(0xFFFF5C5C)   // đỏ — lỗi
val BrandText        = Color(0xFFEAF0F6)
val BrandTextDim     = Color(0xFF8A97A5)

private val DarkColors = darkColorScheme(
    primary = BrandPrimary,
    onPrimary = Color(0xFF231B04),
    primaryContainer = Color(0xFF3A300E),
    onPrimaryContainer = Color(0xFFF6D878),
    secondary = BrandAccent,
    onSecondary = Color(0xFF06281B),
    secondaryContainer = Color(0xFF123B2B),
    onSecondaryContainer = Color(0xFF9FE8C8),
    tertiary = BrandInfo,
    onTertiary = Color(0xFF0B2340),
    tertiaryContainer = Color(0xFF15304C),
    onTertiaryContainer = Color(0xFFA8CDFF),
    background = BrandBackground,
    onBackground = BrandText,
    surface = BrandSurface,
    onSurface = BrandText,
    surfaceVariant = BrandSurfaceAlt,
    onSurfaceVariant = BrandTextDim,
    surfaceContainerLowest = Color(0xFF080C10),
    surfaceContainerLow = Color(0xFF0F151C),
    surfaceContainer = BrandSurface,
    surfaceContainerHigh = BrandSurfaceAlt,
    surfaceContainerHighest = Color(0xFF1D2833),
    outline = BrandOutline,
    outlineVariant = Color(0xFF1E2B37),
    error = BrandDanger,
    onError = Color(0xFF400809),
    errorContainer = Color(0xFF471013),
    onErrorContainer = Color(0xFFFFB4B4),
)

private val AppTypography = Typography(
    headlineMedium = TextStyle(fontWeight = FontWeight.Bold, fontSize = 26.sp, lineHeight = 32.sp),
    titleLarge = TextStyle(fontWeight = FontWeight.Bold, fontSize = 20.sp, lineHeight = 26.sp),
    titleMedium = TextStyle(fontWeight = FontWeight.SemiBold, fontSize = 16.sp, lineHeight = 22.sp),
    labelLarge = TextStyle(fontWeight = FontWeight.SemiBold, fontSize = 14.sp, lineHeight = 20.sp),
    labelMedium = TextStyle(fontWeight = FontWeight.Medium, fontSize = 12.sp, lineHeight = 16.sp),
    labelSmall = TextStyle(fontWeight = FontWeight.Medium, fontSize = 11.sp, lineHeight = 14.sp),
)

private val AppShapes = Shapes(
    extraSmall = RoundedCornerShape(6.dp),
    small = RoundedCornerShape(10.dp),
    medium = RoundedCornerShape(14.dp),
    large = RoundedCornerShape(18.dp),
    extraLarge = RoundedCornerShape(26.dp),
)

/** Giữ lại cho tương thích (LogConsole cũ dùng); phiên bản mới dùng kiểu nội bộ riêng. */
val MonoTextStyle = TextStyle(fontFamily = FontFamily.Monospace, fontSize = 12.sp)

/* ── Brush dùng chung ────────────────────────────────────────────────────────
 * v53: mỗi lần gọi hàm cũ tạo MỘT ĐỐI TƯỢNG Brush mới → rác + modifier bị
 * coi là đổi ở mỗi recomposition. Brush là đối tượng bất biến, an toàn dùng
 * chung toàn app → tạo MỘT LẦN ở top-level, hàm chỉ trả lại instance đó.
 */
private val ScreenBgBrush by lazy {
    Brush.verticalGradient(listOf(Color(0xFF101823), BrandBackground))
}
private val HeroBrush by lazy {
    Brush.horizontalGradient(listOf(Color(0xFF1B2C3D), Color(0xFF121A23)))
}
private val PrimaryBtnBrush by lazy {
    Brush.horizontalGradient(listOf(BrandPrimary, BrandPrimaryDeep))
}

fun screenBackgroundBrush(): Brush = ScreenBgBrush
fun heroBrush(): Brush = HeroBrush
fun primaryButtonBrush(): Brush = PrimaryBtnBrush

@Composable
fun SuperAlphaTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = DarkColors,
        typography = AppTypography,
        shapes = AppShapes,
        content = content
    )
}
