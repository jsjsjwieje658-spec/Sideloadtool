package com.superalpha.sideload

import android.content.Context
import android.content.Intent
import android.hardware.usb.UsbManager
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.viewModels
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Surface
import androidx.compose.ui.Modifier
import com.superalpha.sideload.bridge.AppPaths
import com.superalpha.sideload.bridge.NativeLog
import com.superalpha.sideload.bridge.UsbPermissionManager
import com.superalpha.sideload.ui.AppNavHost
import com.superalpha.sideload.ui.HomeViewModel
import com.superalpha.sideload.ui.PromptDialogHost
import com.superalpha.sideload.ui.theme.SuperAlphaTheme

/**
 * ═══════════════════════════════════════════════════════════════════
 * FIX v21 (Bug 3 — MainActivity không gọi onUsbDeviceGranted()):
 *
 * Trước đây: handleUsbAttachIntent() gọi UsbPermissionManager.requestAndOpen()
 * với callback chỉ log message. UsbDevice sau khi cấp quyền không được
 * truyền về viewModel → viewModel.onUsbDeviceGranted() không bao giờ được
 * gọi → setUsbFd() không chạy → nativeConnect() thất bại với "USB bridge
 * chưa khởi tạo".
 *
 * Fix: Callback mới nhận (Boolean, String, UsbDevice?) — khi ok=true và
 * device != null, gọi viewModel.onUsbDeviceGranted(device, usbManager)
 * để kích hoạt chuỗi setUsbFd → connect() đúng cách.
 *
 * Note: NativeBridge.connect() đã được sửa để tự gọi setUsbFd() nên
 * onUsbDeviceGranted() chỉ cần gọi connect() — setUsbFd là implicit.
 * ═══════════════════════════════════════════════════════════════════
 */
class MainActivity : ComponentActivity() {
    private val viewModel: HomeViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        AppPaths.init(applicationContext)
        NativeLog.log("SUPER ALPHA Sideload đã khởi động.")

        setContent {
            SuperAlphaTheme {
                Surface(modifier = Modifier.fillMaxSize()) {
                    AppNavHost(viewModel = viewModel)
                    PromptDialogHost()
                }
            }
        }

        handleUsbAttachIntent(intent)
    }

    // launchMode="singleTop" → Android gửi USB_DEVICE_ATTACHED mới vào đây
    // khi app đang chạy. fromAutoAttach=true áp dụng cooldown 8s.
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleUsbAttachIntent(intent)
    }

    private fun handleUsbAttachIntent(intent: Intent?) {
        if (intent?.action != UsbManager.ACTION_USB_DEVICE_ATTACHED) return
        NativeLog.log("Đã phát hiện iPhone/iPad vừa cắm vào — đang tự động kết nối...")

        val usbManager = getSystemService(Context.USB_SERVICE) as UsbManager

        // FIX v21: callback nhận (ok, msg, device?) để gọi onUsbDeviceGranted()
        UsbPermissionManager.requestAndOpen(
            context      = this,
            fromAutoAttach = true
        ) { ok, msg, device ->
            NativeLog.log(msg)
            // FIX: Sau khi USB mở thành công, kích hoạt connect() qua viewModel
            // viewModel.onUsbDeviceGranted() → nativeBridge.connect() → tự setUsbFd()
            if (ok && device != null) {
                viewModel.onUsbDeviceGranted(device, usbManager)
            }
        }
    }
}
