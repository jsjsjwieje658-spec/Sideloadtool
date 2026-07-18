package com.superalpha.sideload.ui

import android.hardware.usb.UsbManager
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.PhoneIphone
import androidx.compose.material.icons.filled.Usb
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
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
import androidx.compose.ui.graphics.Color
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
    val logLines     by viewModel.log.collectAsState()
    val usbConnected by viewModel.usbConnected.collectAsState()
    val busy         by viewModel.busy.collectAsState()
    /* FIX v28: Observe trustRequired để hiện hướng dẫn inline trong PairingScreen */
    val trustRequired by viewModel.trustRequired.collectAsState()

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

        /*
         * FIX v28: Hiển thị hướng dẫn Trust inline trong PairingScreen.
         *
         * VẤN ĐỀ CŨ: Trust notification chỉ được set vào StateFlow nhưng
         * không có UI nào observe để hiện. Người dùng không biết phải bấm Trust.
         * Navigation.kt đã thêm TrustBanner toàn màn hình, nhưng cũng cần
         * hướng dẫn chi tiết ở màn hình Pairing để giải thích các bước cụ thể.
         *
         * Card này hiện khi: (a) đang trong quá trình ghép nối (busy=true)
         * hoặc (b) NativeBridge.trustRequired=true. Nó mô tả rõ các bước
         * người dùng cần làm trên iPhone.
         */
        if (trustRequired || (busy && usbConnected)) {
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(
                        color = if (trustRequired) Color(0xFFE65100) else Color(0xFF1565C0),
                        shape = MaterialTheme.shapes.medium
                    )
                    .padding(16.dp)
            ) {
                if (trustRequired) {
                    /* Trạng thái đang chờ Trust — hướng dẫn cụ thể */
                    Column {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Icon(
                                Icons.Filled.PhoneIphone,
                                contentDescription = null,
                                tint = Color.White
                            )
                            Spacer(Modifier.width(8.dp))
                            Text(
                                "⏳ Đang chờ xác nhận 'Tin cậy' trên iPhone",
                                color = Color.White,
                                style = MaterialTheme.typography.titleSmall
                            )
                        }
                        Spacer(Modifier.height(8.dp))
                        Text(
                            "1. Mở khoá iPhone (không để màn hình tối)\n" +
                            "2. Bấm 'Tin cậy Máy tính này' trên popup vừa xuất hiện\n" +
                            "3. Nhập mã PIN nếu được yêu cầu\n" +
                            "4. Bấm 'Đã bấm Trust' bên dưới để tiếp tục",
                            color = Color.White.copy(alpha = 0.9f),
                            style = MaterialTheme.typography.bodySmall
                        )
                        Spacer(Modifier.height(12.dp))
                        Button(
                            onClick = { viewModel.dismissTrust() },
                            colors = ButtonDefaults.buttonColors(
                                containerColor = Color.White,
                                contentColor = Color(0xFFE65100)
                            ),
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Text("Đã bấm Trust ✓")
                        }
                    }
                } else {
                    /* Đang ghép nối — hiện hướng dẫn chuẩn bị */
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        CircularProgressIndicator(
                            color = Color.White,
                            strokeWidth = 2.dp,
                            modifier = Modifier.width(20.dp).height(20.dp)
                        )
                        Spacer(Modifier.width(10.dp))
                        Text(
                            "Đang kết nối... Giữ iPhone mở khoá và sẵn sàng bấm 'Trust'",
                            color = Color.White,
                            style = MaterialTheme.typography.bodySmall
                        )
                    }
                }
            }
            Spacer(Modifier.height(16.dp))
        }

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
