package com.superalpha.sideload

import android.app.Application
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import com.chaquo.python.Python
import com.chaquo.python.android.AndroidPlatform
import com.superalpha.sideload.bridge.AppConfig
import com.superalpha.sideload.bridge.AppPaths
import com.superalpha.sideload.bridge.DeviceNative
import com.superalpha.sideload.bridge.NativeLog
import com.superalpha.sideload.bridge.UsbTransport

/**
 * v8: Khôi phục Chaquopy Python cho apple_auth / developer_api / sideload_core.
 * USB/lockdown vẫn dùng native C — chỉ mux_usb.py và device_link.py được port sang native.
 */
class SuperAlphaApp : Application() {
    override fun onCreate() {
        super.onCreate()
        AppConfig.init(this)
        AppPaths.init(this)

        // ── Khởi động Python runtime (Chaquopy) ──────────────────────────────
        if (!Python.isStarted()) {
            Python.start(AndroidPlatform(this))
        }

        // ── Khởi tạo DeviceNative với application context ────────────────────
        DeviceNative.init(this)

        NativeLog.emit("[app] SuperAlpha Sideload khởi động (native USB + Python API mode).")
        registerUsbDetachReceiver()
    }

    private fun registerUsbDetachReceiver() {
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(ctx: Context, intent: Intent) {
                if (intent.action != UsbManager.ACTION_USB_DEVICE_DETACHED) return
                val device: UsbDevice? = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                if (device != null && device.vendorId != UsbTransport.VENDOR_ID_APPLE) return
                UsbTransport.close()
                NativeLog.emit("[usb] Thiết bị USB đã rút — bấm Kết nối để thử lại.")
            }
        }
        val filter = IntentFilter(UsbManager.ACTION_USB_DEVICE_DETACHED)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            registerReceiver(receiver, filter, Context.RECEIVER_EXPORTED)
        else registerReceiver(receiver, filter)
    }
}
