package com.superalpha.sideload.ui

import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import com.superalpha.sideload.bridge.AppConfig
import com.superalpha.sideload.bridge.NativeLog
import com.superalpha.sideload.bridge.UsbPermissionManager
import com.superalpha.sideload.python.PythonBridge
import kotlinx.coroutines.launch

/**
 * BUGFIX v15 [NEW]: màn hình riêng để đăng ký UDID thiết bị iOS vào tài khoản
 * Apple Developer — độc lập với luồng "Ký & Cài đặt" (SideloadScreen), vốn
 * trước đây là cách DUY NHẤT để đăng ký UDID (chỉ xảy ra ngầm khi đã chọn IPA
 * và kết nối USB). Màn hình này cho phép:
 *   - Kết nối USB rồi lấy UDID tự động từ thiết bị đang cắm (giống SideloadScreen).
 *   - Hoặc nhập tay UDID (vd copy từ Cài đặt > Chung > Giới thiệu trên máy tính/
 *     iPhone khác) khi không có sẵn cáp USB lúc này.
 * Gọi PythonBridge.registerDevice() → do_register_device() trong
 * sideload_core.py, dùng lại đúng logic chống đăng ký trùng + kiểm tra lỗi
 * (dev_api.last_error) đã có trong luồng sideload.
 */
@Composable
fun RegisterDeviceScreen(viewModel: HomeViewModel) {
    val context = androidx.compose.ui.platform.LocalContext.current
    val scope = rememberCoroutineScope()
    val logLines by viewModel.log.collectAsState()
    val busy by viewModel.busy.collectAsState()
    val usbConnected by viewModel.usbConnected.collectAsState()
    val savedAppleId by viewModel.savedAppleId.collectAsState()
    val savedAnisetteUrl by viewModel.savedAnisetteUrl.collectAsState()

    var appleId by remember { mutableStateOf("") }
    var appleIdPrefilled by remember { mutableStateOf(false) }
    var password by remember { mutableStateOf("") }
    var udid by remember { mutableStateOf(AppConfig.lastUdid) }
    var udidEditedByUser by remember { mutableStateOf(false) }
    var deviceName by remember { mutableStateOf("iPhone (Android Sideload)") }

    LaunchedEffect(savedAppleId) {
        if (!appleIdPrefilled && savedAppleId.isNotBlank()) {
            if (appleId.isBlank()) appleId = savedAppleId
            appleIdPrefilled = true
        }
    }

    // Khi USB vừa kết nối và onUsbReady() cập nhật AppConfig.lastUdid, tự điền
    // vào ô UDID — trừ khi người dùng đã tự tay sửa ô này (không ghi đè lựa chọn thủ công).
    LaunchedEffect(usbConnected) {
        if (usbConnected && !udidEditedByUser && AppConfig.lastUdid.isNotBlank()) {
            udid = AppConfig.lastUdid
        }
    }

    Column(modifier = Modifier.fillMaxSize().padding(16.dp)) {
        Text("Đăng ký UDID thiết bị", style = MaterialTheme.typography.titleLarge)
        Spacer(Modifier.height(8.dp))
        Text(
            "Đăng ký UDID của iPhone vào tài khoản Apple Developer trước, tách " +
                "riêng khỏi bước ký & cài IPA. Hữu ích khi muốn thêm thiết bị vào " +
                "team ngay cả khi chưa có sẵn file IPA, hoặc muốn đăng ký một UDID " +
                "không phải máy đang cắm USB lúc này.",
            style = MaterialTheme.typography.bodySmall
        )

        Spacer(Modifier.height(16.dp))
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
            TextButton(onClick = {
                val usbManager = context.getSystemService(android.content.Context.USB_SERVICE)
                        as android.hardware.usb.UsbManager
                // FIX v21: callback nhận (ok, msg, device?) để kích hoạt onUsbDeviceGranted()
                UsbPermissionManager.requestAndOpen(
                    context,
                    fromAutoAttach = false
                ) { ok, msg, device ->
                    NativeLog.log(msg)
                    if (ok && device != null) viewModel.onUsbDeviceGranted(device, usbManager)
                }
            }) { Text("Kết nối") }
        }
        Spacer(Modifier.height(4.dp))
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
            Button(
                enabled = usbConnected && !busy,
                onClick = { viewModel.onUsbReady() }
            ) { Text("Lấy UDID từ USB") }
        }

        Spacer(Modifier.height(16.dp))
        OutlinedTextField(
            value = udid,
            onValueChange = { udid = it; udidEditedByUser = true },
            label = { Text("UDID thiết bị") },
            modifier = Modifier.fillMaxWidth()
        )
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = deviceName, onValueChange = { deviceName = it },
            label = { Text("Tên thiết bị (hiển thị trên Apple Developer)") },
            modifier = Modifier.fillMaxWidth()
        )
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = appleId, onValueChange = { appleId = it },
            label = { Text("Apple ID") }, modifier = Modifier.fillMaxWidth()
        )
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = password, onValueChange = { password = it },
            label = { Text("Mật khẩu Apple ID") },
            visualTransformation = PasswordVisualTransformation(),
            modifier = Modifier.fillMaxWidth()
        )

        Spacer(Modifier.height(16.dp))
        Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Button(
                enabled = !busy
                    && udid.isNotBlank()
                    && appleId.isNotBlank()
                    && password.isNotBlank(),
                onClick = {
                    viewModel.setBusy(true)
                    scope.launch {
                        NativeLog.log("Bắt đầu đăng ký UDID thiết bị...")
                        val outcome = PythonBridge.registerDevice(
                            appleId, password, udid.trim(), deviceName.trim(),
                            savedAnisetteUrl.ifBlank { null }
                        )
                        if (!outcome.success && outcome.message.isNotBlank()) {
                            NativeLog.log("Lỗi: ${outcome.message}")
                        }
                        viewModel.setBusy(false)
                    }
                }
            ) {
                if (busy) {
                    CircularProgressIndicator(modifier = Modifier.height(16.dp), strokeWidth = 2.dp)
                } else {
                    Text("Đăng ký UDID")
                }
            }
            TextButton(onClick = { viewModel.clearLog() }) { Text("Xoá log") }
        }

        Spacer(Modifier.height(12.dp))
        Text("Nhật ký:", style = MaterialTheme.typography.labelLarge)
        Spacer(Modifier.height(4.dp))
        LogConsole(lines = logLines, modifier = Modifier.weight(1f))
    }
}
