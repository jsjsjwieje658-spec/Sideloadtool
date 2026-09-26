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
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.PhonelinkSetup
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.SaveAlt
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.superalpha.sideload.bridge.NativeLog
import com.superalpha.sideload.ui.theme.BrandAccent
import com.superalpha.sideload.ui.theme.BrandTextDim
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import androidx.compose.runtime.rememberCoroutineScope

/*
 * ════════════════════════════════════════════════════════════════════════
 *  v56 — PairingFileManagerScreen: tab "File ghép nối".
 *
 *  Mô hình học từ iLoader (github.com/nab138/iloader, src/pairing.rs):
 *   • File ghép nối = pair record lockdown (XML plist) + khóa UDID — đúng
 *     định dạng AltStore/SideStore dùng (.mobiledevicepairing).
 *   • Nhúng vào app đã cài: house_arrest → VendDocuments(bundle_id) →
 *     ghi file vào Documents của app. SideStore đọc
 *     "ALTPairingFile.mobiledevicepairing", LiveContainer đọc
 *     "SideStore/Documents/ALTPairingFile.mobiledevicepairing"…
 *   • Nhúng xong app dùng pair record này ngay — không cần ghép nối lại.
 *
 *  Ba việc trong tab này:
 *   1. Xem trạng thái + xuất file ra máy (Saf).
 *   2. Công tắc "tự động nhúng khi cài app" (do_sideload Bước cuối).
 *   3. Quét app đã cài và nhúng thủ công từng app.
 * ════════════════════════════════════════════════════════════════════════
 */
@Composable
fun PairingFileManagerScreen(viewModel: HomeViewModel) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    val status by viewModel.deviceStatus.collectAsState()
    val busy by viewModel.busy.collectAsState()
    val pairingFileReady by viewModel.pairingFileReady.collectAsState()
    val autoEmbed by viewModel.autoEmbedPairing.collectAsState()
    val pairingApps by viewModel.pairingApps.collectAsState()

    // Vào tab / iPhone mới ghép nối xong → làm mới trạng thái 1 lần.
    LaunchedEffect(status.udid, status.paired) {
        if (status.paired) viewModel.refreshPairingState()
    }

    val exportLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("application/octet-stream")
    ) { uri: Uri? ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch(Dispatchers.IO) {
            try {
                val content = viewModel.nativeBridge.getPairingFile()
                if (content == null) {
                    NativeLog.emit("[pairing] ❌ Chưa có pair record — không xuất được file.")
                    return@launch
                }
                context.contentResolver.openOutputStream(uri)?.use { os ->
                    os.write(content.toByteArray(Charsets.UTF_8))
                } ?: run {
                    NativeLog.emit("[pairing] ❌ Không mở được file để ghi.")
                    return@launch
                }
                NativeLog.emit("[pairing] ✅ Đã xuất file ghép nối (${content.length} ký tự).")
            } catch (e: Exception) {
                NativeLog.emit("[pairing] ❌ Xuất file lỗi: ${e.message}")
            }
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp)
    ) {
        ScreenHeader(
            icon = Icons.Filled.PhonelinkSetup,
            title = "File ghép nối",
            subtitle = "Cho SideStore / LiveContainer… dùng pair record của tool"
        )

        // ── Trạng thái ──────────────────────────────────────────────────────
        SectionCard(title = "Trạng thái", icon = Icons.Filled.PhonelinkSetup) {
            InfoRowPairing("iPhone", if (status.paired) "Đã ghép nối" else if (status.usbConnected) "Đã cắm — chưa ghép nối" else "Chưa kết nối")
            if (status.udid.isNotBlank()) InfoRowPairing("UDID", status.udid)
            InfoRowPairing(
                "File ghép nối",
                if (pairingFileReady) "✅ Sẵn sàng (xuất / nhúng được)" else "Chưa có — kết nối + ghép nối iPhone trước"
            )
            Text(
                "File ghép nối (.mobiledevicepairing) là pair record giữa iPhone và tool, " +
                        "kèm UDID máy — SideStore/LiveContainer đọc file này để tự ghép nối " +
                        "không cần bấm \"Tin cậy\" lại.",
                style = MaterialTheme.typography.bodySmall,
                color = BrandTextDim
            )
        }

        // ── Xuất file ──────────────────────────────────────────────────────
        SectionCard(title = "Xuất file ra máy", icon = Icons.Filled.SaveAlt) {
            Text(
                "Lưu bản sao file ghép nối (dạng .mobiledevicepairing) vào Downloads — " +
                        "dùng để khôi phục sau khi cài lại tool, hoặc nộp cho công cụ khác.",
                style = MaterialTheme.typography.bodySmall,
                color = BrandTextDim
            )
            PrimaryButton(
                text = "Xuất file ghép nối",
                onClick = {
                    exportLauncher.launch("pairing-${status.udid.ifBlank { "iphone" }}.mobiledevicepairing")
                },
                enabled = pairingFileReady && !busy,
                modifier = Modifier.padding(top = 10.dp)
            )
        }

        // ── Tự động nhúng khi cài ───────────────────────────────────────────
        SectionCard(title = "Tự động nhúng khi cài app", icon = Icons.Filled.Download) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        "Sau khi cài xong SideStore / LiveContainer…, tool tự ghi file ghép nối " +
                                "vào app (như iLoader) — mở app là dùng được ngay.",
                        style = MaterialTheme.typography.bodySmall,
                        color = BrandTextDim
                    )
                }
                Spacer(Modifier.width(10.dp))
                Switch(
                    checked = autoEmbed,
                    onCheckedChange = { viewModel.setAutoEmbedPairing(it) },
                    colors = SwitchDefaults.colors(checkedTrackColor = BrandAccent)
                )
            }
        }

        // ── Hướng dẫn kích hoạt (v58) ──────────────────────────────────────
        SectionCard(title = "Kích hoạt trong SideStore (1 lần duy nhất)", icon = Icons.Filled.PhonelinkSetup) {
            Text(
                "SideStore 0.7 trở lên KHÔNG tự nạp file ghép nối có sẵn — " +
                        "sau khi nhúng, làm 1 lần duy nhất:\n" +
                        "1. Mở SideStore → khi hiện hộp thoại chọn file ghép nối, bấm chọn file\n" +
                        "2. Chọn \"Trên iPhone của tôi\" → SideStore → PairingFile_Lockdown.plist\n" +
                        "3. Nếu không thấy hộp thoại: Cài đặt → Advanced → Pairing File → Import, " +
                        "chọn file như trên rồi KHỞI ĐỘNG LẠI SideStore",
                style = MaterialTheme.typography.bodySmall,
                color = BrandTextDim
            )
        }

        // ── Nhúng vào app đã cài ────────────────────────────────────────────
        SectionCard(title = "Nhúng vào app đã cài", icon = Icons.Filled.Refresh) {
            Text(
                "Quét iPhone tìm SideStore / LiveContainer… đã cài và ghi file ghép nối " +
                        "vào Documents của app đó.",
                style = MaterialTheme.typography.bodySmall,
                color = BrandTextDim
            )
            PrimaryButton(
                text = "Quét app trên iPhone",
                onClick = { viewModel.refreshPairingState() },
                enabled = status.paired && !busy,
                busy = busy,
                busyText = "Đang quét…",
                modifier = Modifier.padding(top = 10.dp)
            )

            if (pairingApps.isEmpty()) {
                Text(
                    if (status.paired)
                        "Chưa tìm thấy app nào cần file ghép nối. Cài SideStore hoặc LiveContainer rồi quét lại."
                    else
                        "Cắm iPhone, ghép nối xong rồi bấm \"Quét app trên iPhone\".",
                    style = MaterialTheme.typography.bodySmall,
                    color = BrandTextDim,
                    modifier = Modifier.padding(top = 10.dp)
                )
            } else {
                pairingApps.forEach { app ->
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(top = 10.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Column(modifier = Modifier.weight(1f)) {
                            Text(app.displayName, style = MaterialTheme.typography.titleSmall)
                            Text(
                                app.bundleId,
                                style = MaterialTheme.typography.bodySmall,
                                color = BrandTextDim,
                                fontFamily = FontFamily.Monospace
                            )
                            app.paths.forEach { path ->
                                Text(
                                    "Documents/$path",
                                    style = MaterialTheme.typography.bodySmall,
                                    color = BrandTextDim
                                )
                            }
                        }
                        Spacer(Modifier.width(8.dp))
                        Button(
                            onClick = { viewModel.embedPairingNow(app.bundleId, app.paths) },
                            enabled = pairingFileReady && !busy,
                            colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.primary)
                        ) {
                            Text("Nhúng")
                        }
                    }
                }
            }
        }

        Spacer(Modifier.height(6.dp))
    }
}

/** Hàng thông tin label : value dùng trong thẻ trạng thái (kiểu SettingsScreen). */
@Composable
private fun InfoRowPairing(label: String, value: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(label, style = MaterialTheme.typography.bodyMedium, color = BrandTextDim)
        Spacer(Modifier.width(12.dp))
        Text(
            value,
            style = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.weight(1f)
        )
    }
}
