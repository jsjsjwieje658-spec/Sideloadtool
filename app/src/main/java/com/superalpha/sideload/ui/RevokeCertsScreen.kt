package com.superalpha.sideload.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.AccountCircle
import androidx.compose.material.icons.filled.Key
import androidx.compose.material.icons.filled.DeleteSweep
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.superalpha.sideload.bridge.NativeLog
import com.superalpha.sideload.python.PythonBridge
import com.superalpha.sideload.ui.theme.BrandTextDim
import com.superalpha.sideload.ui.theme.BrandAccent
import com.superalpha.sideload.ui.theme.screenBackgroundBrush
import kotlinx.coroutines.launch

/*
 * ════════════════════════════════════════════════════════════════════════
 *  v50 — RevokeCertsScreen (thiết kế lại).
 *
 *  Chức năng giữ nguyên: đăng nhập Apple ID → liệt kê chứng chỉ Development
 *  (in ra nhật ký kèm số thứ tự) → thu hồi "all" hoặc theo chỉ số.
 *  Thay ô nhập tự do "all|số" bằng thanh phân đoạn 2 lựa chọn — hết nhập
 *  nhầm "all"/"ALL"/khoảng trắng.
 * ════════════════════════════════════════════════════════════════════════
 */
@Composable
fun RevokeCertsScreen(viewModel: HomeViewModel) {
    val scope = rememberCoroutineScope()
    val busy by viewModel.busy.collectAsState()
    val busyText by viewModel.busyText.collectAsState()
    val savedAppleId by viewModel.savedAppleId.collectAsState()
    val savedPassword by viewModel.savedPassword.collectAsState()
    val savedAnisetteUrl by viewModel.savedAnisetteUrl.collectAsState()

    // v51: dùng Apple ID + mật khẩu đã lưu (màn đăng nhập) — không nhập lại
    var revokeAll by remember { mutableStateOf(true) }
    var certIndex by remember { mutableStateOf("1") }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(screenBackgroundBrush())
            .verticalScroll(rememberScrollState())
            .imePadding()
            .padding(horizontal = 16.dp)
    ) {
        Spacer(Modifier.height(16.dp))
        ScreenHeader(
            icon = Icons.Filled.Key,
            title = "Thu hồi chứng chỉ",
            subtitle = "Giải phóng chỗ khi Apple báo vượt giới hạn chứng chỉ"
        )

        Spacer(Modifier.height(14.dp))

        SectionCard(title = "Tài khoản Apple", icon = Icons.Filled.AccountCircle) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    modifier = Modifier
                        .size(38.dp)
                        .background(
                            MaterialTheme.colorScheme.surfaceContainerHighest,
                            RoundedCornerShape(19.dp)
                        ),
                    contentAlignment = Alignment.Center
                ) {
                    Icon(
                        Icons.Filled.AccountCircle,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.size(22.dp)
                    )
                }
                Spacer(Modifier.width(12.dp))
                Column(Modifier.weight(1f)) {
                    Text(
                        savedAppleId.ifBlank { "—" },
                        style = MaterialTheme.typography.titleMedium
                    )
                    Text(
                        "Đã lưu trên máy — đổi tài khoản trong tab Cài đặt",
                        style = MaterialTheme.typography.labelSmall,
                        color = BrandTextDim
                    )
                }
                StatusDot(BrandAccent)
            }
        }

        Spacer(Modifier.height(14.dp))

        SectionCard(title = "Chứng chỉ cần thu hồi", icon = Icons.Filled.DeleteSweep) {
            SegmentedOptions(
                options = listOf(true to "Tất cả chứng chỉ", false to "Theo số thứ tự"),
                selected = revokeAll,
                onSelect = { revokeAll = it }
            )
            if (!revokeAll) {
                Spacer(Modifier.height(10.dp))
                AppTextField(
                    value = certIndex,
                    onValueChange = { v -> certIndex = v.filter { it.isDigit() }.take(3) },
                    label = "Số thứ tự chứng chỉ",
                    keyboardType = KeyboardType.Number,
                    supportingText = "Danh sách chứng chỉ kèm số thứ tự được in ra nhật ký sau khi bấm Thu hồi."
                )
            }
            Spacer(Modifier.height(10.dp))
            Text(
                "Tài khoản Apple ID miễn phí chỉ được tối đa 2 chứng chỉ Development cùng lúc. " +
                    "Khi ký IPA gặp lỗi hết chỗ chứng chỉ, hãy thu hồi chứng chỉ cũ — chọn " +
                    "\"Tất cả chứng chỉ\" là an toàn nhất (app sẽ tự tạo chứng chỉ mới khi ký).",
                style = MaterialTheme.typography.bodySmall,
                color = BrandTextDim
            )
        }

        Spacer(Modifier.height(16.dp))

        PrimaryButton(
            text = "Thu hồi chứng chỉ",
            onClick = {
                val selector = if (revokeAll) "all" else certIndex.ifBlank { "1" }
                viewModel.setBusy(true)
                viewModel.setBusyText("Đang thu hồi…")
                scope.launch {
                    NativeLog.log("Đang đăng nhập & tra cứu chứng chỉ...")
                    val outcome = PythonBridge.revokeCerts(
                        savedAppleId, savedPassword, savedAnisetteUrl.ifBlank { null }, selector
                    )
                    if (!outcome.success && outcome.message.isNotBlank()) {
                        NativeLog.log("Lỗi: ${outcome.message}")
                    }
                    viewModel.setBusy(false)
                }
            },
            enabled = !busy,
            busy = busy,
            busyText = busyText.ifBlank { "Đang xử lý…" }
        )

        Spacer(Modifier.height(16.dp))

        LogConsole(Modifier.height(330.dp))

        Spacer(Modifier.height(16.dp))
    }
}
