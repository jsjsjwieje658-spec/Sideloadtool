package com.superalpha.sideload.ui

import android.hardware.usb.UsbManager
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Usb
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.superalpha.sideload.bridge.NativeLog
import com.superalpha.sideload.bridge.UsbPermissionManager

/**
 * PairingScreen: Cho phép người dùng ghép nối (Pair) thiết bị độc lập.
 *
 * ═══════════════════════════════════════════════════════════════════
 * FIX v21 (Bug 3 & Bug 5):
 *
 * Bug 3 — "Kết nối" button callback không gọi onUsbDeviceGranted():
 *   Trước đây: UsbPermissionManager.requestAndOpen(context, false) { _, msg -> log(msg) }
 *   Callback chỉ log message, không truyền UsbDevice về → setUsbFd() không bao giờ chạy.
 *   Fix: Callback mới nhận (ok, msg, device?) → gọi viewModel.onUsbDeviceGranted(device, mgr)
 *   khi ok=true để kích hoạt chuỗi setUsbFd → connect() đúng cách.
 *
 * Bug 5 — "Bắt đầu Ghép nối" gọi DeviceNative.connectAndPair() thay vì viewModel:
 *   Trước đây: DeviceNative.connectAndPair() dùng bridge instance riêng (khác HomeViewModel)
 *   → C global state bị chia sẻ nhưng Kotlin state thì không → race condition.
 *   Fix: Dùng viewModel.connectAndPair() — cùng bridge instance, cùng state.
 *   viewModel.connectAndPair() đã được sửa để tự gọi connect() trước nếu cần.
 * ═══════════════════════════════════════════════════════════════════
 */
@Composable
fun PairingScreen(viewModel: HomeViewModel) {
    val context = LocalContext.current
    val logLines    by viewModel.log.collectAsState()
    val usbConnected by viewModel.usbConnected.collectAsState()
    val busy        by viewModel.busy.collectAsState()

    Column(modifier = Modifier.fillMaxSize().padding(16.dp)) {
        Text(
            "Ghép nối thiết bị (Pairing)",
            style = MaterialTheme.typography.titleLarge
        )
        Spacer(Modifier.height(12.dp))

        // ── USB status ───────────────────────────────────────────────────────
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(
                Icons.Filled.Usb,
                contentDescription = null,
                tint = if (usbConnected)
                    com.superalpha.sideload.ui.theme.BrandAccent
                else
                    com.superalpha.sideload.ui.theme.BrandTextDim
            )
            Text(
                text = if (usbConnected) "Đã kết nối iPhone qua USB" else "Chưa kết nối USB",
                modifier = Modifier.padding(start = 8.dp)
            )
            Spacer(Modifier.weight(1f))

            // FIX v21 (Bug 3): Callback nhận (ok, msg, device?) để gọi onUsbDeviceGranted()
            TextButton(onClick = {
                val usbManager = context.getSystemService(android.content.Context.USB_SERVICE)
                        as UsbManager
                UsbPermissionManager.requestAndOpen(context, fromAutoAttach = false) { ok, msg, device ->
                    NativeLog.log(msg)
                    // FIX: Truyền device về viewModel để kích hoạt setUsbFd → connect()
                    if (ok && device != null) {
                        viewModel.onUsbDeviceGranted(device, usbManager)
                    }
                }
            }) { Text("Kết nối") }
        }

        Spacer(Modifier.height(16.dp))

        Text(
            "Bước này sẽ thực hiện bắt tay (handshake) với iPhone, yêu cầu bạn xác nhận 'Tin cậy' trên điện thoại nếu đây là lần đầu kết nối.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )

        Spacer(Modifier.height(24.dp))

        // FIX v21 (Bug 5): Dùng viewModel.connectAndPair() thay vì DeviceNative.connectAndPair()
        // viewModel.connectAndPair() dùng cùng bridge instance và tự gọi connect() trước.
        Button(
            enabled = !busy && usbConnected,
            onClick = {
                NativeLog.log("Bắt đầu quá trình ghép nối...")
                viewModel.connectAndPair()
            },
            modifier = Modifier.fillMaxWidth()
        ) {
            if (busy) {
                CircularProgressIndicator(modifier = Modifier.height(16.dp), strokeWidth = 2.dp)
            } else {
                Text("Bắt đầu Ghép nối")
            }
        }

        Spacer(Modifier.height(24.dp))
        Text("Nhật ký:", style = MaterialTheme.typography.labelLarge)
        Spacer(Modifier.height(4.dp))
        LogConsole(lines = logLines, modifier = Modifier.weight(1f))
    }
}
