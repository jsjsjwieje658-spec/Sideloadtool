package com.superalpha.sideload.bridge

import android.content.Context
import kotlinx.coroutines.runBlocking

/**
 * DeviceNative — Synchronous (blocking) wrapper around NativeBridge for Chaquopy
 * Python code to call via Java interop.
 *
 * ═══════════════════════════════════════════════════════════════════
 * FIX v21 (Bug 1 — Dual NativeBridge instance):
 *
 * Trước đây: SuperAlphaApp.onCreate() gọi DeviceNative.init() tạo bridge A,
 * HomeViewModel tạo bridge B = NativeBridge(app). C JNI bị gọi nativeInit()
 * HAI LẦN → global state của C bị reset lần 2 (g_mux, g_ld, g_rec, g_udid
 * đều về zero) ngay khi HomeViewModel khởi tạo.
 *
 * Fix: DeviceNative giữ bridge singleton và expose getBridge() để
 * HomeViewModel dùng CÙNG instance. Chỉ một lần nativeInit() được gọi.
 * ═══════════════════════════════════════════════════════════════════
 */
object DeviceNative {
    @Volatile private var bridge: NativeBridge? = null

    /**
     * Gọi từ SuperAlphaApp.onCreate() — khởi tạo bridge singleton.
     * Idempotent: gọi nhiều lần không tạo thêm instance.
     */
    fun init(context: Context) {
        if (bridge == null) {
            synchronized(this) {
                if (bridge == null) {
                    bridge = NativeBridge(context.applicationContext)
                    bridge!!.init()
                }
            }
        }
    }

    /**
     * FIX v21: Expose bridge cho HomeViewModel để dùng chung.
     * Nếu chưa init (không nên xảy ra), tạo mới với applicationContext.
     */
    fun getBridge(context: Context? = null): NativeBridge {
        return bridge ?: run {
            val ctx = context?.applicationContext
                ?: throw IllegalStateException("DeviceNative chưa được init — gọi init(context) trước")
            NativeBridge(ctx).also {
                it.init()
                bridge = it
            }
        }
    }

    /**
     * Kết nối USB mux + thực hiện lockdown pairing.
     * Gọi từ device_link.py: DeviceNative.connectAndPair()
     * Trả về True nếu thành công.
     */
    @JvmStatic
    fun connectAndPair(): Boolean = runBlocking {
        val b = bridge ?: run {
            NativeLog.emit("[DeviceNative] ❌ Chưa init — gọi DeviceNative.init() trước.")
            return@runBlocking false
        }

        if (!UsbTransport.isConnected()) {
            NativeLog.emit(
                "[DeviceNative] ❌ USB chưa kết nối — vui lòng cắm cáp và bấm \"Kết nối\" trước."
            )
            return@runBlocking false
        }

        val connected = b.connect()
        if (!connected) {
            NativeLog.emit("[DeviceNative] ❌ connect() thất bại — kiểm tra cáp USB và thiết bị đã mở khoá.")
            return@runBlocking false
        }

        val paired = b.pair()
        if (!paired) {
            NativeLog.emit("[DeviceNative] ❌ pair() thất bại — nếu iPhone hỏi \"Tin cậy?\" hãy bấm Tin cậy.")
        }
        paired
    }

    @JvmStatic
    fun getUdid(): String? = runBlocking {
        bridge?.getUdid() ?: AppConfig.lastUdid.ifBlank { null }
    }

    @JvmStatic
    fun sideloadIpa(localIpaPath: String): Boolean = runBlocking {
        val b = bridge ?: run {
            NativeLog.emit("[DeviceNative] ❌ Chưa init.")
            return@runBlocking false
        }
        if (!UsbTransport.isConnected()) {
            NativeLog.emit("[DeviceNative] ❌ USB đã ngắt trong quá trình cài đặt — thử lại từ đầu.")
            return@runBlocking false
        }
        b.sideload(localIpaPath)
    }

    @JvmStatic
    fun reset() {
        bridge?.reset()
    }

    /**
     * listInstalledApps — Trả về danh sách bundle ID User apps từ iPhone.
     * Gọi từ device_link.py để sideload_core.py tránh tạo App ID trùng.
     */
    @JvmStatic
    fun listInstalledApps(): List<String> = runBlocking {
        bridge?.listInstalledApps() ?: emptyList()
    }

    /**
     * diagnostics — Chẩn đoán trạng thái kết nối iPhone.
     * Học từ lệnh "termux-usbmuxd doctor" — trả về báo cáo để hiển thị UI.
     */
    @JvmStatic
    fun diagnostics(): String {
        val nativeDiag = bridge?.diagnostics() ?: "(bridge chưa khởi tạo)"
        val usbStatus  = if (UsbTransport.isConnected()) "✅ USB connected" else "❌ USB not connected"
        return "$usbStatus\n$nativeDiag"
    }
}
