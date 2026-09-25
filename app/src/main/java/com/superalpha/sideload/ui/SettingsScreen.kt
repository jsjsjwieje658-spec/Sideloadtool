package com.superalpha.sideload.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.AccountCircle
import androidx.compose.material.icons.filled.Dns
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.superalpha.sideload.bridge.AppPaths
import com.superalpha.sideload.ui.theme.BrandTextDim
import com.superalpha.sideload.ui.theme.screenBackgroundBrush

/*
 * ════════════════════════════════════════════════════════════════════════
 *  v50 — SettingsScreen (thiết kế lại): gom thành 3 thẻ rõ ràng
 *    1. Apple ID (chỉ lưu email để tự điền — không lưu mật khẩu)
 *    2. Server Anisette (tự dò từ servers.sidestore.io / chọn tay / tuỳ chỉnh)
 *    3. Thông tin ứng dụng (phiên bản, đường dẫn, lưu ý)
 *  Bố cục cũ dùng Divider (deprecated) + dàn chữ dày — bỏ hết.
 * ════════════════════════════════════════════════════════════════════════
 */
@Composable
fun SettingsScreen(viewModel: HomeViewModel) {
    val context = LocalContext.current
    val savedAppleId by viewModel.savedAppleId.collectAsState()
    val busy by viewModel.busy.collectAsState()
    val savedAnisetteUrl by viewModel.savedAnisetteUrl.collectAsState()
    val servers by viewModel.anisetteServers.collectAsState()
    val serversLoading by viewModel.anisetteServersLoading.collectAsState()

    var menuExpanded by remember { mutableStateOf(false) }
    var showCustomField by remember { mutableStateOf(false) }
    var customUrlField by remember { mutableStateOf("") }

    LaunchedEffect(Unit) { viewModel.loadAnisetteServersIfNeeded() }

    val filesDir = remember { AppPaths.filesDir() }
    val zsignPath = remember { AppPaths.zsignPath() }
    val versionName = remember {
        try {
            context.packageManager.getPackageInfo(context.packageName, 0).versionName ?: "?"
        } catch (_: Exception) { "?" }
    }

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
            icon = Icons.Filled.Settings,
            title = "Cài đặt",
            subtitle = "Tài khoản, server Anisette và thông tin ứng dụng"
        )

        Spacer(Modifier.height(14.dp))

        // ── Tài khoản Apple (v51: đã lưu từ màn đăng nhập, Đăng xuất ở đây) ──
        var confirmSignOut by remember { mutableStateOf(false) }

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
                        savedAppleId.ifBlank { "Chưa đăng nhập" },
                        style = MaterialTheme.typography.titleMedium
                    )
                    Text(
                        "Apple ID + mật khẩu đã lưu riêng tư trên máy — mọi lần ký & cài " +
                            "đặt dùng luôn, không cần nhập lại.",
                        style = MaterialTheme.typography.labelSmall,
                        color = BrandTextDim
                    )
                }
            }
            Spacer(Modifier.height(12.dp))
            OutlinedButton(
                onClick = { confirmSignOut = true },
                enabled = !busy,
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(12.dp)
            ) {
                Text("Đăng xuất Apple ID", color = MaterialTheme.colorScheme.error)
            }

            if (confirmSignOut) {
                AlertDialog(
                    onDismissRequest = { confirmSignOut = false },
                    title = { Text("Đăng xuất Apple ID?") },
                    text = {
                        Text(
                            "App sẽ xoá Apple ID và mật khẩu đã lưu trên máy. Lần mở app " +
                                "tiếp theo bạn cần đăng nhập lại mới vào được app chính.\n\n" +
                                "Ghép nối với iPhone và cài đặt đã có trên máy được giữ nguyên."
                        )
                    },
                    confirmButton = {
                        TextButton(onClick = {
                            confirmSignOut = false
                            viewModel.signOut()
                        }) { Text("Đăng xuất", color = MaterialTheme.colorScheme.error) }
                    },
                    dismissButton = {
                        TextButton(onClick = { confirmSignOut = false }) { Text("Huỷ") }
                    }
                )
            }
        }

        Spacer(Modifier.height(14.dp))

        // ── Server Anisette ─────────────────────────────────────────────────
        SectionCard(title = "Server Anisette", icon = Icons.Filled.Dns) {
            Box {
                val currentLabel = when {
                    savedAnisetteUrl.isBlank() -> "Tự động (khuyến nghị)"
                    else -> servers.firstOrNull { it.address == savedAnisetteUrl }
                        ?.let { "${it.name} · ${it.address}" }
                        ?: "Tuỳ chỉnh: $savedAnisetteUrl"
                }
                OutlinedButton(
                    onClick = { menuExpanded = true },
                    modifier = Modifier.fillMaxWidth(),
                    shape = RoundedCornerShape(12.dp)
                ) {
                    Text(currentLabel, modifier = Modifier.weight(1f), maxLines = 1)
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
                            text = { Text("${server.name} · ${server.address}") },
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
                Spacer(Modifier.height(10.dp))
                Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
                    AppTextField(
                        value = customUrlField,
                        onValueChange = { customUrlField = it },
                        label = "URL server Anisette",
                        modifier = Modifier.weight(1f)
                    )
                    Spacer(Modifier.width(8.dp))
                    TextButton(onClick = { viewModel.saveAnisetteUrl(customUrlField.trim()) }) {
                        Text("Lưu")
                    }
                }
            }

            Spacer(Modifier.height(10.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    "Server Anisette cấp dữ liệu xác thực thiết bị cho đăng nhập Apple ID/2FA.",
                    style = MaterialTheme.typography.bodySmall,
                    color = BrandTextDim,
                    modifier = Modifier.weight(1f)
                )
                if (serversLoading) {
                    Spacer(Modifier.width(8.dp))
                    // v53: text tĩnh thay spinner — không animation nền
                    Text("Đang tải…", style = MaterialTheme.typography.labelMedium, color = BrandTextDim)
                } else {
                    TextButton(
                        onClick = { viewModel.reloadAnisetteServers() },
                        contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 8.dp)
                    ) { Text("Tải lại") }
                }
            }
        }

        Spacer(Modifier.height(14.dp))

        // ── Thông tin ứng dụng ──────────────────────────────────────────────
        SectionCard(title = "Thông tin ứng dụng", icon = Icons.Filled.Info) {
            InfoRow("Phiên bản", versionName)
            Spacer(Modifier.height(10.dp))
            InfoRow("Thư mục dữ liệu", filesDir)
            Spacer(Modifier.height(10.dp))
            InfoRow("Đường dẫn zsign", zsignPath)
            Spacer(Modifier.height(12.dp))
            Text(
                "Ứng dụng kết nối trực tiếp với iPhone qua USB Host API của Android " +
                    "(không cần Termux, không cần root). Lớp usbmux/lockdown được triển khai " +
                    "lại từ giao thức gốc của libimobiledevice và đã kiểm chứng trên máy thật. " +
                    "Sau khi cài app lần đầu, nhớ tin cậy Apple ID trong Cài đặt iPhone.",
                style = MaterialTheme.typography.bodySmall,
                color = BrandTextDim
            )
        }

        Spacer(Modifier.height(16.dp))
    }
}

@Composable
private fun InfoRow(label: String, value: String) {
    Column {
        Text(label, style = MaterialTheme.typography.labelMedium, color = BrandTextDim)
        Text(
            value,
            style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace, fontSize = 11.sp)
        )
    }
}
