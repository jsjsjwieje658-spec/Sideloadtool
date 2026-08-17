package com.superalpha.sideload.ui

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Usb
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import com.superalpha.sideload.bridge.NativeBridge
import com.superalpha.sideload.bridge.NativeLog
import com.superalpha.sideload.bridge.UsbPermissionManager
import com.superalpha.sideload.python.PythonBridge
import kotlinx.coroutines.launch
import java.io.File
import java.io.FileOutputStream

/**
 * Main "sideload an IPA" flow: pick a file, connect USB, enter Apple ID, run.
 *
 * FIX: Nút "Ký & Cài đặt" bây giờ yêu cầu USB đã kết nối.
 * Root cause: PythonBridge.sideload() gọi device_link.pair_device() →
 * DeviceNative.connectAndPair() → NativeBridge.connect() → native C code
 * gọi UsbTransport.nativeBulkWrite/Read. Nếu UsbTransport chưa có connection
 * (connection == null), bulk I/O trả -1 → native connect thất bại ngay.
 * Fix: vô hiệu hoá nút "Ký & Cài đặt" khi chưa có USB; hướng dẫn người dùng
 * bấm "Kết nối" trước.
 */
@Composable
fun SideloadScreen(viewModel: HomeViewModel) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val logLines by viewModel.log.collectAsState()
    val usbConnected by viewModel.usbConnected.collectAsState()
    val busy by viewModel.busy.collectAsState()
    val trustRequired by NativeBridge.trustRequired.collectAsState()
    val savedAppleId by viewModel.savedAppleId.collectAsState()
    val savedAnisetteUrl by viewModel.savedAnisetteUrl.collectAsState()

    var ipaPath by remember { mutableStateOf<String?>(null) }
    var ipaName by remember { mutableStateOf("Chưa chọn file IPA") }
    var appleId by remember { mutableStateOf("") }
    var appleIdPrefilled by remember { mutableStateOf(false) }
    var password by remember { mutableStateOf("") }

    androidx.compose.runtime.LaunchedEffect(savedAppleId) {
        if (!appleIdPrefilled && savedAppleId.isNotBlank()) {
            if (appleId.isBlank()) appleId = savedAppleId
            appleIdPrefilled = true
        }
    }

    val pickIpaLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        if (uri == null) return@rememberLauncherForActivityResult
        val dest = File(context.filesDir, "picked.ipa")
        context.contentResolver.openInputStream(uri)?.use { input ->
            FileOutputStream(dest).use { output -> input.copyTo(output) }
        }
        ipaPath = dest.absolutePath
        ipaName = uri.lastPathSegment ?: "IPA đã chọn"
        NativeLog.log("Đã chọn file: $ipaName")
    }

    Column(modifier = Modifier.fillMaxSize().padding(16.dp)) {

        // ── Trust banner ───────────────────────────────────────────────────────
        if (trustRequired) {
            androidx.compose.material3.Card(
                modifier = Modifier.fillMaxWidth().padding(bottom = 8.dp),
                colors = androidx.compose.material3.CardDefaults.cardColors(
                    containerColor = MaterialTheme.colorScheme.errorContainer
                )
            ) {
                Text(
                    text = "⚠️  Bấm \"Tin cậy\" (Trust This Computer) trên màn hình iPhone!",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onErrorContainer,
                    modifier = Modifier.padding(12.dp)
                )
            }
        }

        Text(
            "Cài đặt ứng dụng (.ipa) lên iPhone",
            style = MaterialTheme.typography.titleLarge
        )
        Spacer(Modifier.height(12.dp))

        // ── USB status + Kết nối button ─────────────────────────────────────
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
            // fromAutoAttach=false → bỏ qua cooldown khi người dùng bấm tay
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

        // FIX: Hiển thị hướng dẫn kết nối USB khi chưa có kết nối
        if (!usbConnected) {
            Text(
                text = "⚠ Vui lòng cắm cáp USB và bấm \"Kết nối\" trước khi ký & cài đặt.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error,
                modifier = Modifier.padding(top = 4.dp)
            )
        }

        Spacer(Modifier.height(16.dp))

        Button(
            onClick = { pickIpaLauncher.launch(arrayOf("application/octet-stream", "*/*")) },
            modifier = Modifier.fillMaxWidth()
        ) {
            Text(ipaName)
        }

        Spacer(Modifier.height(16.dp))
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
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Button(
                // FIX: Thêm điều kiện usbConnected — không cho phép ký khi USB chưa kết nối
                // vì Python sẽ gọi DeviceNative.connectAndPair() ngay trong do_sideload()
                // và sẽ thất bại nếu UsbTransport.connection == null
                enabled = !busy
                    && usbConnected
                    && ipaPath != null
                    && appleId.isNotBlank()
                    && password.isNotBlank(),
                onClick = {
                    val path = ipaPath ?: return@Button
                    viewModel.setBusy(true)
                    scope.launch {
                        NativeLog.log("Bắt đầu quá trình ký & cài đặt...")
                        val outcome = PythonBridge.sideload(
                            path, appleId, password, null,
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
                    CircularProgressIndicator(
                        modifier = Modifier.height(16.dp),
                        strokeWidth = 2.dp
                    )
                } else {
                    Text("Ký & Cài đặt")
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
