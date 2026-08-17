package com.superalpha.sideload.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Divider
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.superalpha.sideload.bridge.AppPaths

/**
 * Cài đặt: Apple ID đã lưu (KHÔNG lưu mật khẩu — xem lý do trong
 * config_manager.get_apple_id ở phía Python), server Anisette (tự động dò từ
 * servers.sidestore.io, hoặc chọn tay một server cụ thể / nhập URL riêng), và
 * thông tin đường dẫn + cảnh báo rủi ro USB như bản trước.
 */
@Composable
fun SettingsScreen(viewModel: HomeViewModel) {
    val savedAppleId by viewModel.savedAppleId.collectAsState()
    val savedAnisetteUrl by viewModel.savedAnisetteUrl.collectAsState()
    val servers by viewModel.anisetteServers.collectAsState()
    val serversLoading by viewModel.anisetteServersLoading.collectAsState()

    var appleIdField by remember { mutableStateOf("") }
    var appleIdInitialized by remember { mutableStateOf(false) }
    LaunchedEffect(savedAppleId) {
        if (!appleIdInitialized) {
            appleIdField = savedAppleId
            appleIdInitialized = true
        }
    }

    var menuExpanded by remember { mutableStateOf(false) }
    var showCustomField by remember { mutableStateOf(false) }
    var customUrlField by remember { mutableStateOf("") }

    LaunchedEffect(Unit) { viewModel.loadAnisetteServersIfNeeded() }

    // remember: AppPaths.filesDir()/zsignPath() chỉ ghép chuỗi từ Context, không
    // đổi trong suốt đời sống Activity — không cần gọi lại mỗi lần recomposition.
    val filesDir = remember { AppPaths.filesDir() }
    val zsignPath = remember { AppPaths.zsignPath() }

    Column(modifier = Modifier.fillMaxSize().padding(16.dp)) {
        Text("Cài đặt", style = MaterialTheme.typography.titleLarge)
        Spacer(Modifier.height(16.dp))

        Text("Apple ID", style = MaterialTheme.typography.labelLarge)
        Spacer(Modifier.height(4.dp))
        Text(
            "Chỉ Apple ID (email) được lưu lại để tự điền ở tab Sideload/Thu hồi " +
                "certificate — KHÔNG lưu mật khẩu vì lý do bảo mật, bạn vẫn cần nhập " +
                "mật khẩu mỗi lần đăng nhập.",
            style = MaterialTheme.typography.bodySmall
        )
        Spacer(Modifier.height(8.dp))
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
            OutlinedTextField(
                value = appleIdField,
                onValueChange = { appleIdField = it },
                label = { Text("Apple ID") },
                singleLine = true,
                modifier = Modifier.weight(1f)
            )
            TextButton(onClick = { viewModel.saveAppleId(appleIdField.trim()) }) { Text("Lưu") }
        }

        Spacer(Modifier.height(20.dp))
        Divider()
        Spacer(Modifier.height(20.dp))

        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
            Text("Server Anisette", style = MaterialTheme.typography.labelLarge, modifier = Modifier.weight(1f))
            if (serversLoading) {
                CircularProgressIndicator(modifier = Modifier.height(16.dp), strokeWidth = 2.dp)
            } else {
                TextButton(onClick = { viewModel.reloadAnisetteServers() }) { Text("Tải lại danh sách") }
            }
        }
        Spacer(Modifier.height(4.dp))
        Text(
            "Server Anisette cấp thông tin xác thực thiết bị cần cho đăng nhập Apple " +
                "ID/2FA. Danh sách lấy trực tiếp từ servers.sidestore.io — chọn \"Tự " +
                "động\" để app tự tìm server đang phản hồi tốt nhất, hoặc chọn tay một " +
                "server cụ thể nếu server tự động không ổn định.",
            style = MaterialTheme.typography.bodySmall
        )
        Spacer(Modifier.height(8.dp))

        Box {
            val currentLabel = when {
                savedAnisetteUrl.isBlank() -> "Tự động (khuyến nghị)"
                else -> servers.firstOrNull { it.address == savedAnisetteUrl }
                    ?.let { "${it.name}  ·  ${it.address}" }
                    ?: "Tuỳ chỉnh: $savedAnisetteUrl"
            }
            OutlinedButton(onClick = { menuExpanded = true }, modifier = Modifier.fillMaxWidth()) {
                Text(currentLabel, modifier = Modifier.weight(1f))
            }
            DropdownMenu(expanded = menuExpanded, onDismissRequest = { menuExpanded = false }) {
                DropdownMenuItem(
                    text = { Text("Tự động (khuyến nghị)") },
                    onClick = {
                        menuExpanded = false
                        showCustomField = false
                        viewModel.saveAnisetteUrl("")
                    }
                )
                servers.forEach { server ->
                    DropdownMenuItem(
                        text = { Text("${server.name}  ·  ${server.address}") },
                        onClick = {
                            menuExpanded = false
                            showCustomField = false
                            viewModel.saveAnisetteUrl(server.address)
                        }
                    )
                }
                DropdownMenuItem(
                    text = { Text("Tuỳ chỉnh URL khác...") },
                    onClick = {
                        menuExpanded = false
                        showCustomField = true
                        customUrlField = savedAnisetteUrl
                    }
                )
            }
        }

        if (showCustomField) {
            Spacer(Modifier.height(8.dp))
            Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
                OutlinedTextField(
                    value = customUrlField,
                    onValueChange = { customUrlField = it },
                    label = { Text("URL server Anisette tuỳ chỉnh") },
                    singleLine = true,
                    modifier = Modifier.weight(1f)
                )
                TextButton(onClick = { viewModel.saveAnisetteUrl(customUrlField.trim()) }) { Text("Lưu") }
            }
        }

        Spacer(Modifier.height(20.dp))
        Divider()
        Spacer(Modifier.height(20.dp))

        Text("Thông tin & Cảnh báo", style = MaterialTheme.typography.titleLarge)
        Spacer(Modifier.height(12.dp))
        Text(
            "Ứng dụng này kết nối trực tiếp tới iPhone qua cổng USB bằng USB Host API " +
                "của Android (không cần Termux, không cần root). Lớp giao tiếp usbmux " +
                "(mux_usb.py) là phần tự triển khai lại từ giao thức gốc của libimobiledevice " +
                "và CHƯA được kiểm chứng trên phần cứng thật — hãy xem README.md, mục " +
                "\"Rủi ro đã biết\" trước khi dùng với thiết bị quan trọng.",
            style = MaterialTheme.typography.bodyMedium
        )
        Spacer(Modifier.height(16.dp))
        Divider()
        Spacer(Modifier.height(16.dp))
        Text("Thư mục dữ liệu ứng dụng:", style = MaterialTheme.typography.labelLarge)
        Text(filesDir, style = MaterialTheme.typography.bodySmall)
        Spacer(Modifier.height(8.dp))
        Text("Đường dẫn zsign:", style = MaterialTheme.typography.labelLarge)
        Text(zsignPath, style = MaterialTheme.typography.bodySmall)
    }
}
