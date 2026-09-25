package com.superalpha.sideload.ui

import android.content.Context
import android.hardware.usb.UsbManager
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.automirrored.filled.InsertDriveFile
import androidx.compose.material.icons.filled.RocketLaunch
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.superalpha.sideload.bridge.NativeLog
import com.superalpha.sideload.bridge.UsbPermissionManager
import com.superalpha.sideload.python.PythonBridge
import com.superalpha.sideload.ui.theme.BrandAccent
import com.superalpha.sideload.ui.theme.BrandTextDim
import com.superalpha.sideload.ui.theme.screenBackgroundBrush
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FileOutputStream

/*
 * ════════════════════════════════════════════════════════════════════════
 *  v50 — SideloadScreen (thiết kế lại).
 *
 *  Màn hình chính hiện gom mọi thứ về MỘT luồng duy nhất:
 *    [Thẻ iPhone] → [Chọn IPA] → [Apple ID + mật khẩu] → [Ký & Cài đặt]
 *    → [Nhật ký thời gian thực]
 *
 *  Việc "ghép nối" và "đăng ký UDID" không còn là tab riêng: do_sideload()
 *  (Python) tự connectAndPair + đăng ký UDID ngay ở Bước 0/5, còn thẻ
 *  DeviceCard hiển thị đúng trạng thái đó (cắm cáp / chờ Trust / sẵn sàng).
 *
 *  Hiệu năng:
 *   - Sao chép file IPA chọn từ SAF chạy trên Dispatchers.IO (bản cũ copy
 *     ngay trong callback главной thread — file vài trăm MB thì khớp UI).
 *   - LogConsole mới: batch 100 ms + key + không animation (xem LogConsole.kt).
 * ════════════════════════════════════════════════════════════════════════
 */
@Composable
fun SideloadScreen(viewModel: HomeViewModel) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val status by viewModel.deviceStatus.collectAsState()
    val busy by viewModel.busy.collectAsState()
    val busyText by viewModel.busyText.collectAsState()
    val trustRequired by viewModel.trustRequired.collectAsState()
    val reconnectState by viewModel.reconnectState.collectAsState()
    val savedAppleId by viewModel.savedAppleId.collectAsState()
    val savedAnisetteUrl by viewModel.savedAnisetteUrl.collectAsState()

    var ipaPath by remember { mutableStateOf<String?>(null) }
    var ipaName by remember { mutableStateOf<String?>(null) }
    var ipaSizeMb by remember { mutableStateOf(0.0) }
    // v51: Apple ID + mật khẩu đã lưu từ màn đăng nhập — không nhập lại ở đây
    val savedPassword by viewModel.savedPassword.collectAsState()

    val pickIpaLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        if (uri == null) return@rememberLauncherForActivityResult
        // Copy trên IO — file IPA có thể vài trăm MB, copy trên main thread
        // sẽ đóng băng UI (nguyên nhân "lag" khi chọn file ở bản cũ).
        scope.launch(Dispatchers.IO) {
            try {
                val dest = File(context.filesDir, "picked.ipa")
                val opened = context.contentResolver.openInputStream(uri)
                if (opened == null) {
                    NativeLog.log("❌ Không đọc được file IPA từ bộ nhớ.")
                    return@launch
                }
                opened.use { input -> FileOutputStream(dest).use { input.copyTo(it) } }
                val sizeMb = dest.length() / 1048576.0
                val name = prettyFileName(uri)
                withContext(Dispatchers.Main) {
                    ipaPath = dest.absolutePath
                    ipaName = name
                    ipaSizeMb = sizeMb
                }
                NativeLog.log("Đã chọn file IPA: $name (${"%.1f".format(sizeMb)} MB)")
            } catch (e: Exception) {
                NativeLog.log("❌ Không sao chép được file IPA: ${e.message}")
            }
        }
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
            icon = Icons.Filled.RocketLaunch,
            title = "Cài IPA lên iPhone",
            subtitle = "Ký & cài ứng dụng .ipa bằng Apple ID miễn phí"
        )

        Spacer(Modifier.height(14.dp))

        // ── Thẻ trạng thái iPhone (thay tab Ghép nối + Đăng ký UDID) ───────
        DeviceCard(
            status = status,
            trustRequired = trustRequired,
            reconnectState = reconnectState,
            onConnect = {
                val usbManager = context.getSystemService(Context.USB_SERVICE) as UsbManager
                UsbPermissionManager.requestAndOpen(
                    context, fromAutoAttach = false
                ) { ok, msg, device ->
                    NativeLog.log(msg)
                    if (ok && device != null) viewModel.onUsbDeviceGranted(device, usbManager)
                }
            },
            onTrustDone = { viewModel.dismissTrust() }
        )

        Spacer(Modifier.height(14.dp))

        // ── Chọn file IPA ───────────────────────────────────────────────────
        SectionCard(title = "Ứng dụng", icon = Icons.AutoMirrored.Filled.InsertDriveFile) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(MaterialTheme.colorScheme.surfaceContainerLowest, RoundedCornerShape(12.dp))
                    .border(1.dp, MaterialTheme.colorScheme.outlineVariant, RoundedCornerShape(12.dp))
                    .clickable {
                        pickIpaLauncher.launch(arrayOf("application/octet-stream", "*/*"))
                    }
                    .padding(14.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Icon(
                    if (ipaPath != null) Icons.Filled.CheckCircle else Icons.AutoMirrored.Filled.InsertDriveFile,
                    contentDescription = null,
                    tint = if (ipaPath != null) BrandAccent else BrandTextDim,
                    modifier = Modifier.size(22.dp)
                )
                Spacer(Modifier.width(12.dp))
                Column(Modifier.weight(1f)) {
                    Text(
                        ipaName ?: "Chọn file IPA từ bộ nhớ",
                        style = MaterialTheme.typography.titleMedium,
                        color = MaterialTheme.colorScheme.onSurface
                    )
                    if (ipaPath != null) {
                        Text(
                            "%.1f MB · sẵn sàng".format(ipaSizeMb),
                            style = MaterialTheme.typography.labelSmall,
                            color = BrandTextDim
                        )
                    } else {
                        Text(
                            "Tệp .ipa đã tải về (Tải xuống/Downloads)",
                            style = MaterialTheme.typography.labelSmall,
                            color = BrandTextDim
                        )
                    }
                }
                Text(
                    if (ipaPath == null) "Chọn" else "Đổi",
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.primary
                )
            }
        }

        Spacer(Modifier.height(14.dp))

        // ── Tài khoản Apple (đã lưu từ màn đăng nhập — v51) ─────────────────
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

        Spacer(Modifier.height(16.dp))

        PrimaryButton(
            text = "Ký & Cài đặt",
            onClick = {
                val path = ipaPath ?: return@PrimaryButton
                viewModel.setBusy(true)
                viewModel.setBusyText("Đang ký & cài đặt…")
                scope.launch {
                    NativeLog.log("Bắt đầu quá trình ký & cài đặt...")
                    val outcome = PythonBridge.sideload(
                        path, savedAppleId, savedPassword, null,
                        savedAnisetteUrl.ifBlank { null }
                    )
                    if (!outcome.success && outcome.message.isNotBlank()) {
                        NativeLog.log("Lỗi: ${outcome.message}")
                    }
                    viewModel.setBusy(false)
                }
            },
            enabled = !busy
                && status.usbConnected
                && ipaPath != null,
            busy = busy,
            busyText = busyText.ifBlank { "Đang xử lý…" }
        )

        Spacer(Modifier.height(16.dp))

        LogConsole(Modifier.height(330.dp))

        Spacer(Modifier.height(16.dp))
    }
}

private fun prettyFileName(uri: Uri): String {
    val raw = uri.lastPathSegment ?: uri.path ?: "IPA đã chọn"
    return raw.substringAfterLast('/').ifBlank { "IPA đã chọn" }
}
