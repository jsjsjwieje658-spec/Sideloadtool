package com.superalpha.sideload

import android.app.Application
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import com.superalpha.sideload.bridge.AppConfig
import com.superalpha.sideload.bridge.AppPaths
import com.superalpha.sideload.bridge.DeviceNative
import com.superalpha.sideload.bridge.NativeLog
import com.superalpha.sideload.bridge.UsbReconnectManager
import com.superalpha.sideload.bridge.UsbTransport
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

/**
 * v8: Khôi phục Chaquopy Python cho apple_auth / developer_api / sideload_core.
 * USB/lockdown vẫn dùng native C — chỉ mux_usb.py và device_link.py được port sang native.
 */
class SuperAlphaApp : Application() {

    private val appScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    override fun onCreate() {
        super.onCreate()
        AppConfig.init(this)
        AppPaths.init(this)

        /*
         * ═════════════════════════════════════════════════════════════════
         * v52 — Khởi động NHANH: hết "0.5 s màn đen" khi mở app.
         *
         * Trước đây Python.start(AndroidPlatform) (Chaquopy: 300–800 ms —
         * giải nén bootstrap + load libpython + init interpreter) và
         * DeviceNative.init() (loadLibrary libsideloadnative 5.5 MB +
         * nativeInit + usbmuxd threads) chạy NGAY TRÊN MAIN THREAD trong
         * Application.onCreate() → frame đầu tiên của Activity phải chờ
         * hết chỗ đó → màn hình đen (windowBackground) nửa giây rồi UI
         * mới hiện.
         *
         * Nay hai việc nặng chạy trên THREAD NỀN, song song với việc vẽ
         * frame đầu tiên:
         *   - UI hiện gần như tức thì (màn splash màu thương hiệu).
         *   - Action đầu tiên cần Python/bridge (bấm "Đăng nhập", "Kết
         *     nối") đi qua ensurePython()/getBridge() — có lock, nếu
         * warm-up chưa xong thì chờ tối đa vài trăm ms.
         * DeviceNative.getBridge() đã được làm race-safe (double-check
         * trong cùng một khối synchronized) để chắc chắn vẫn MỘT instance
         * duy nhất — giữ nguyên fix v21.
         * ═════════════════════════════════════════════════════════════════
         */
        appScope.launch {
            try {
                DeviceNative.init(this@SuperAlphaApp)
            } catch (e: Exception) {
                NativeLog.emit("[app] ❌ Khởi tạo native bridge thất bại: ${e.message}")
            }
            try {
                com.superalpha.sideload.python.PythonBridge.ensurePython(this@SuperAlphaApp)
            } catch (e: Exception) {
                NativeLog.emit("[app] ❌ Khởi động Python thất bại: ${e.message}")
            }
            NativeLog.emit("[app] SuperAlpha Sideload sẵn sàng (native USB + Python API mode).")
        }

        registerUsbDetachReceiver()
    }

    private fun registerUsbDetachReceiver() {
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(ctx: Context, intent: Intent) {
                if (intent.action != UsbManager.ACTION_USB_DEVICE_DETACHED) return
                val device: UsbDevice? = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                if (device != null && device.vendorId != UsbTransport.VENDOR_ID_APPLE) return
                /* Dọn native trước khi đóng Android fd; thứ tự này ngăn
                 * libusb/usbmuxd tiếp tục đọc một descriptor đã mất. */
                DeviceNative.reset()
                UsbTransport.close()
                UsbReconnectManager.notifyDisconnected()
                NativeLog.emit("[usb] Thiết bị USB đã rút — đã dọn session, chờ reconnect.")
            }
        }
        val filter = IntentFilter(UsbManager.ACTION_USB_DEVICE_DETACHED)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            registerReceiver(receiver, filter, Context.RECEIVER_EXPORTED)
        else registerReceiver(receiver, filter)
    }
}
