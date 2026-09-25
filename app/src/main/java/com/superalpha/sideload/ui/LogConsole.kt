package com.superalpha.sideload.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.superalpha.sideload.bridge.LogBuffer
import com.superalpha.sideload.bridge.NativeLog
import com.superalpha.sideload.ui.theme.BrandAccent
import com.superalpha.sideload.ui.theme.BrandDanger
import com.superalpha.sideload.ui.theme.BrandInfo
import com.superalpha.sideload.ui.theme.BrandText
import com.superalpha.sideload.ui.theme.BrandTextDim
import com.superalpha.sideload.ui.theme.BrandWarn
import kotlinx.coroutines.launch

/*
 * ════════════════════════════════════════════════════════════════════════
 *  v50 — LogConsole (viết lại).
 *
 *  Hiệu năng (so với bản cũ — nguồn "lag lag" trên máy thật):
 *   - Dữ liệu: LogBuffer ring buffer xuất SNAPSHOT theo lô 100 ms (tối đa
 *     10 lần/giây) thay vì mỗi dòng log phát một List mới.
 *   - LazyColumn có key = chỉ số toàn cục của dòng → khi có dòng mới, các
 *     item đang hiển thị KHÔNG được compose lại (bản cũ không key nên compose
 *     lại toàn bộ item trong viewport cho mỗi dòng log).
 *   - Cuộn: scrollToItem() không animation (bản cũ animateScrollToItem()
 *     chạy animation cuộn liên tục khi log dồn dập). Chỉ tự cuộn khi người
 *     dùng đang ở cuối danh sách; cuộn lên để đọc thì hiện nút "Cuộn xuống
 *     cuối" thay vì giật nội dung theo log mới.
 *   - Màu theo mức độ (lỗi đỏ/cảnh báo cam/thành công mint/tiến trình xanh)
 *   - Tiện ích: header có "Sao chép" (clipboard) và "Xoá".
 * ════════════════════════════════════════════════════════════════════════
 */
@Composable
fun LogConsole(modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val snapshot by LogBuffer.snapshot.collectAsState()
    val lines = snapshot.lines
    val listState = rememberLazyListState()
    val scope = rememberCoroutineScope()

    // "Đang ở cuối" = item áp chót đã hiển thị. Dùng làm cờ auto-follow:
    // người dùng cuộn lên để đọc log cũ → không tự kéo xuống nữa.
    val atEnd by remember {
        derivedStateOf {
            val info = listState.layoutInfo
            info.totalItemsCount == 0 ||
                (info.visibleItemsInfo.lastOrNull()?.index ?: 0) >= info.totalItemsCount - 2
        }
    }

    LaunchedEffect(snapshot.nextIndex) {
        if (lines.isEmpty()) return@LaunchedEffect
        if (atEnd) listState.scrollToItem(lines.size - 1)
    }

    Column(modifier = modifier.fillMaxWidth()) {
        // ── Header: tiêu đề + số dòng + nút tiện ích ──────────────────────
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                if (lines.isEmpty()) "NHẬT KÝ" else "NHẬT KÝ · ${lines.size} dòng",
                style = MaterialTheme.typography.labelMedium,
                color = BrandTextDim,
                modifier = Modifier.weight(1f)
            )
            TextButton(
                onClick = {
                    val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                    clipboard.setPrimaryClip(
                        ClipData.newPlainText("Sideloadtool log", LogBuffer.copyText())
                    )
                    NativeLog.log("Đã sao chép ${lines.size} dòng nhật ký.")
                },
                enabled = lines.isNotEmpty(),
                contentPadding = PaddingValues(horizontal = 8.dp)
            ) { Text("Sao chép", style = MaterialTheme.typography.labelMedium) }
            TextButton(
                onClick = { LogBuffer.clear() },
                enabled = lines.isNotEmpty(),
                contentPadding = PaddingValues(horizontal = 8.dp)
            ) { Text("Xoá", style = MaterialTheme.typography.labelMedium) }
        }
        Spacer(Modifier.height(4.dp))

        // ── Vùng console ───────────────────────────────────────────────────
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .weight(1f)
                .background(Color(0xFF070A0E), RoundedCornerShape(12.dp))
                .border(1.dp, MaterialTheme.colorScheme.outlineVariant, RoundedCornerShape(12.dp))
        ) {
            if (lines.isEmpty()) {
                Text(
                    "Chưa có nhật ký nào.\nChọn file IPA rồi bấm \"Ký & Cài đặt\" để bắt đầu.",
                    style = MaterialTheme.typography.bodySmall,
                    color = BrandTextDim,
                    textAlign = androidx.compose.ui.text.style.TextAlign.Center,
                    modifier = Modifier.align(Alignment.Center).padding(16.dp)
                )
            } else {
                LazyColumn(
                    state = listState,
                    modifier = Modifier.fillMaxSize(),
                    contentPadding = PaddingValues(horizontal = 10.dp, vertical = 8.dp)
                ) {
                    itemsIndexed(lines, key = { i, _ -> snapshot.firstIndex + i }) { _, line ->
                        Text(
                            text = line,
                            color = logColor(line),
                            fontFamily = FontFamily.Monospace,
                            fontSize = 11.sp,
                            lineHeight = 15.sp,
                            softWrap = true,
                            modifier = Modifier.padding(vertical = 1.dp)
                        )
                    }
                }

                // Làm mờ dần mép trên (chỉ draw — không tốn recomposition)
                Box(
                    Modifier
                        .fillMaxWidth()
                        .height(16.dp)
                        .background(
                            Brush.verticalGradient(
                                listOf(Color(0xFF070A0E), Color(0x00070A0E))
                            )
                        )
                )

                // Người dùng đang đọc log cũ → nút nhảy xuống cuối
                if (!atEnd) {
                    Surface(
                        onClick = {
                            scope.launch { listState.scrollToItem(lines.size - 1) }
                        },
                        shape = RoundedCornerShape(50),
                        color = MaterialTheme.colorScheme.surfaceContainerHighest,
                        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant),
                        modifier = Modifier
                            .align(Alignment.BottomCenter)
                            .padding(bottom = 10.dp)
                    ) {
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp)
                        ) {
                            Icon(
                                Icons.Filled.KeyboardArrowDown,
                                contentDescription = null,
                                modifier = Modifier.size(16.dp),
                                tint = BrandTextDim
                            )
                            Spacer(Modifier.width(4.dp))
                            Text(
                                "Cuộn xuống cuối",
                                style = MaterialTheme.typography.labelMedium,
                                color = BrandText
                            )
                        }
                    }
                }
            }
        }
    }
}

/** Màu dòng log theo mức độ — giúp đọc nhanh kết quả mà không cần scan từng dòng. */
private fun logColor(line: String): Color = when {
    line.contains("❌") || line.contains("thất bại", ignoreCase = true) ||
        line.contains("Lỗi", ignoreCase = true)          -> BrandDanger
    line.contains("⚠") || line.contains("⚠️")            -> BrandWarn
    line.contains("✅") || line.contains("thành công", ignoreCase = true) -> BrandAccent
    line.startsWith("[afc]") || line.startsWith("[instproxy]") ||
        line.startsWith("[reconnect]") || line.startsWith("[python]")    -> BrandInfo
    else                                                  -> Color(0xFFB9C4CF)
}
