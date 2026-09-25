package com.superalpha.sideload.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.superalpha.sideload.bridge.DeviceStatus
import com.superalpha.sideload.bridge.UsbReconnectManager
import com.superalpha.sideload.ui.theme.BrandAccent
import com.superalpha.sideload.ui.theme.BrandText
import com.superalpha.sideload.ui.theme.BrandTextDim
import com.superalpha.sideload.ui.theme.BrandWarn
import com.superalpha.sideload.ui.theme.heroBrush
import com.superalpha.sideload.ui.theme.primaryButtonBrush

/*
 * ════════════════════════════════════════════════════════════════════════
 *  v50 — Bộ thành phần UI dùng chung cho cả 3 màn hình.
 *
 *  Thiết kế: nền gradient tối, thẻ bo góc 16dp viền mảnh, điểm nhấn vàng
 *  Super Alpha, xanh mint cho trạng thái tốt, cam cho cảnh báo Trust.
 *  Tất cả là composable "nguội" (chỉ nhận state, không collect flow riêng)
 *  để Compose có thể bỏ qua recomposition khi tham số không đổi.
 * ════════════════════════════════════════════════════════════════════════
 */

/** Chấm tròn trạng thái. */
@Composable
fun StatusDot(color: Color, modifier: Modifier = Modifier, sizeDp: Int = 10) {
    Box(
        modifier = modifier
            .size(sizeDp.dp)
            .background(color, CircleShape)
    )
}

/** Banner đầu màn hình: logo + tiêu đề + mô tả ngắn. */
@Composable
fun ScreenHeader(icon: ImageVector, title: String, subtitle: String, modifier: Modifier = Modifier) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .background(heroBrush(), RoundedCornerShape(18.dp))
            .padding(16.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(
            modifier = Modifier
                .size(44.dp)
                .background(MaterialTheme.colorScheme.primaryContainer, RoundedCornerShape(13.dp)),
            contentAlignment = Alignment.Center
        ) {
            Icon(
                icon, contentDescription = null,
                tint = MaterialTheme.colorScheme.primary,
                modifier = Modifier.size(24.dp)
            )
        }
        Spacer(Modifier.width(14.dp))
        Column {
            Text(title, style = MaterialTheme.typography.titleLarge, color = BrandText)
            Text(subtitle, style = MaterialTheme.typography.bodySmall, color = BrandTextDim)
        }
    }
}

/** Thẻ phần: tiêu đề (kèm icon) + nội dung. */
@Composable
fun SectionCard(
    title: String,
    modifier: Modifier = Modifier,
    icon: ImageVector? = null,
    content: @Composable ColumnScope.() -> Unit
) {
    Column(
        modifier = modifier
            .fillMaxWidth()
            .background(MaterialTheme.colorScheme.surface, RoundedCornerShape(16.dp))
            .border(1.dp, MaterialTheme.colorScheme.outlineVariant, RoundedCornerShape(16.dp))
            .padding(16.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            if (icon != null) {
                Box(
                    modifier = Modifier
                        .size(26.dp)
                        .background(MaterialTheme.colorScheme.surfaceContainerHighest, RoundedCornerShape(8.dp)),
                    contentAlignment = Alignment.Center
                ) {
                    Icon(
                        icon, contentDescription = null,
                        tint = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.size(15.dp)
                    )
                }
                Spacer(Modifier.width(10.dp))
            }
            Text(title, style = MaterialTheme.typography.titleMedium)
        }
        Spacer(Modifier.height(12.dp))
        content()
    }
}

/** Ô nhập liệu thống nhất (bo góc, viền theo theme). */
@Composable
fun AppTextField(
    value: String,
    onValueChange: (String) -> Unit,
    label: String,
    modifier: Modifier = Modifier,
    password: Boolean = false,
    keyboardType: KeyboardType? = null,
    supportingText: String? = null
) {
    OutlinedTextField(
        value = value,
        onValueChange = onValueChange,
        label = { Text(label) },
        modifier = modifier.fillMaxWidth(),
        singleLine = true,
        shape = RoundedCornerShape(12.dp),
        visualTransformation = if (password) PasswordVisualTransformation() else VisualTransformation.None,
        keyboardOptions = if (keyboardType != null) {
            KeyboardOptions(keyboardType = keyboardType)
        } else {
            KeyboardOptions.Default
        },
        supportingText = if (supportingText != null) {
            { Text(supportingText, style = MaterialTheme.typography.labelSmall) }
        } else null,
        colors = OutlinedTextFieldDefaults.colors(
            focusedBorderColor = MaterialTheme.colorScheme.primary,
            unfocusedBorderColor = MaterialTheme.colorScheme.outline,
            focusedLabelColor = MaterialTheme.colorScheme.primary,
            cursorColor = MaterialTheme.colorScheme.primary
        )
    )
}

/**
 * Nút hành động chính — nền gradient vàng, chiếm trọn bề ngang.
 * Khi [busy]: hiện spinner + [busyText] (nút vẫn không cho bấm).
 */
@Composable
fun PrimaryButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    busy: Boolean = false,
    busyText: String? = null
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .heightIn(min = 52.dp)
            .background(
                if (enabled) primaryButtonBrush()
                else Brush.horizontalGradient(listOf(Color(0xFF27313C), Color(0xFF202932))),
                RoundedCornerShape(14.dp)
            )
            .clickable(enabled = enabled) { onClick() },
        horizontalArrangement = Arrangement.Center,
        verticalAlignment = Alignment.CenterVertically
    ) {
        if (busy) {
            CircularProgressIndicator(
                modifier = Modifier.size(18.dp),
                strokeWidth = 2.dp,
                color = MaterialTheme.colorScheme.onPrimary
            )
            Spacer(Modifier.width(10.dp))
        }
        Text(
            if (busy) (busyText ?: text) else text,
            color = if (enabled) MaterialTheme.colorScheme.onPrimary else BrandTextDim,
            style = MaterialTheme.typography.titleMedium
        )
    }
}

/** Thanh chọn dạng phân đoạn (dùng cho "Thu hồi: Tất cả / Số thứ tự"). */
@Composable
fun <T> SegmentedOptions(
    options: List<Pair<T, String>>,
    selected: T,
    onSelect: (T) -> Unit,
    modifier: Modifier = Modifier
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .background(MaterialTheme.colorScheme.surfaceContainerLowest, RoundedCornerShape(12.dp))
            .border(1.dp, MaterialTheme.colorScheme.outlineVariant, RoundedCornerShape(12.dp))
            .padding(4.dp),
        horizontalArrangement = Arrangement.spacedBy(4.dp)
    ) {
        options.forEach { (value, label) ->
            val isSelected = value == selected
            Box(
                modifier = Modifier
                    .weight(1f)
                    .background(
                        if (isSelected) MaterialTheme.colorScheme.primary else Color.Transparent,
                        RoundedCornerShape(9.dp)
                    )
                    .clickable { onSelect(value) }
                    .padding(vertical = 8.dp),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    label,
                    color = if (isSelected) MaterialTheme.colorScheme.onPrimary
                            else MaterialTheme.colorScheme.onSurfaceVariant,
                    style = MaterialTheme.typography.labelLarge
                )
            }
        }
    }
}

/**
 * Thẻ trạng thái iPhone trên màn chính — thay thế 2 tab "Ghép nối" và
 * "Đăng ký UDID" đã xoá. Luồng sideload tự ghép nối + tự đăng ký UDID,
 * nên người dùng chỉ cần biết: cáp cắm chưa, Trust chưa, sẵn sàng chưa.
 */
@Composable
fun DeviceCard(
    status: DeviceStatus.Snapshot,
    trustRequired: Boolean,
    reconnectState: UsbReconnectManager.State,
    onConnect: () -> Unit,
    onTrustDone: () -> Unit,
    modifier: Modifier = Modifier
) {
    val reconnecting = reconnectState == UsbReconnectManager.State.RECONNECTING
    val (label, color) = when {
        !status.usbConnected -> "Chưa kết nối iPhone" to BrandTextDim
        reconnecting -> "Đang tự kết nối lại…" to MaterialTheme.colorScheme.tertiary
        status.ready -> "iPhone sẵn sàng" to BrandAccent
        status.needsTrust -> "Đã kết nối — chờ ghép nối" to BrandWarn
        else -> "Đang kết nối…" to BrandWarn
    }

    Column(
        modifier = modifier
            .fillMaxWidth()
            .background(MaterialTheme.colorScheme.surface, RoundedCornerShape(16.dp))
            .border(1.dp, MaterialTheme.colorScheme.outlineVariant, RoundedCornerShape(16.dp))
    ) {
        Row(Modifier.fillMaxWidth().height(IntrinsicSize.Min)) {
            // Viền trái đổi màu theo trạng thái
            Box(
                Modifier
                    .width(3.dp)
                    .fillMaxHeight()
                    .background(color)
            )
            Column(Modifier.weight(1f).padding(start = 14.dp, end = 8.dp, top = 14.dp, bottom = 4.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    StatusDot(color)
                    Spacer(Modifier.width(8.dp))
                    Text(label, style = MaterialTheme.typography.titleMedium, color = color)
                    Spacer(Modifier.weight(1f))
                    if (!status.usbConnected) {
                        TextButton(onClick = onConnect) { Text("Kết nối") }
                    }
                }
                Spacer(Modifier.height(4.dp))
                when {
                    !status.usbConnected ->
                        Text(
                            "Cắm cáp USB (có data) vào iPhone rồi bấm \"Kết nối\".",
                            style = MaterialTheme.typography.bodySmall, color = BrandTextDim
                        )
                    status.ready ->
                        Text(
                            if (status.paired) "Đã ghép nối — sẵn sàng ký & cài đặt."
                            else "Phiên SSL đã mở — sẵn sàng ký & cài đặt.",
                            style = MaterialTheme.typography.bodySmall, color = BrandTextDim
                        )
                    status.needsTrust && !trustRequired ->
                        Text(
                            "Lần đầu kết nối: mở khoá iPhone, bấm \"Tin cậy\" và nhập mã khi được hỏi.",
                            style = MaterialTheme.typography.bodySmall, color = BrandWarn
                        )
                    else ->
                        Text(
                            "Đang thiết lập phiên làm việc với iPhone…",
                            style = MaterialTheme.typography.bodySmall, color = BrandTextDim
                        )
                }
                if (status.udid.isNotBlank()) {
                    Spacer(Modifier.height(2.dp))
                    Text(
                        "UDID  ${status.udid}",
                        style = MaterialTheme.typography.labelSmall.copy(fontFamily = FontFamily.Monospace),
                        color = BrandTextDim
                    )
                }
            }
        }

        // ── Vùng chờ Trust — thay banner riêng của tab Ghép nối cũ ──────────
        if (trustRequired) {
            Column(
                Modifier
                    .fillMaxWidth()
                    .background(Color(0xFF402600))
                    .padding(horizontal = 16.dp, vertical = 12.dp)
            ) {
                Text(
                    "⏳ Mở khoá iPhone → bấm \"Tin cậy Máy tính này\" → nhập mã PIN",
                    style = MaterialTheme.typography.titleMedium,
                    color = Color(0xFFFFD9A0),
                    fontWeight = FontWeight.SemiBold
                )
                Spacer(Modifier.height(8.dp))
                TextButton(onClick = onTrustDone) { Text("Đã bấm Tin cậy ✓") }
            }
        }
    }
}
