package com.superalpha.sideload.bridge

import android.content.Context
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.withContext
import java.io.File

/**
 * NativeBridge — Kotlin wrapper cho libsideloadnative.so.
 *
 * ┌─ Mode 1: libimobiledevice thật ───────────────────────────────────────────┐
 * │  jni_bridge_imd.c → libimobiledevice-1.0.a + libusbmuxd-2.0.a           │
 * │  Flow: setUsbFd(fd) → connect() → pair() → sideload()                    │
 * └───────────────────────────────────────────────────────────────────────────┘
 * ┌─ Mode 2/3: Custom protocol layer (fallback) ───────────────────────────────┐
 * │  jni_bridge.c → usbmux.c/lockdown.c/pairing.c/afc.c/install_proxy.c     │
 * │  Flow: prepareForBulkTransfers() → connect() → pair() → sideload()       │
 * └───────────────────────────────────────────────────────────────────────────┘
 *
 * ════════════════════════════════════════════════════════════════════
 * FIX v27 — Kiến trúc học từ termux-usbmuxd + termux-api (UsbAPI.java)
 * ════════════════════════════════════════════════════════════════════
 *
 * termux-usbmuxd flow:
 *   termux-usb -E -e "usbmuxd_proxy ..." /dev/bus/usb/XXX/YYY
 *   → UsbAPI.java: openDevice() ONLY → fd → TERMUX_USB_FD
 *   → usbmuxd: libusb_wrap_sys_device(fd) → claim interface tự do
 *   → Endpoint SẠCH → version exchange thành công
 *
 * Sideloadtool fix:
 *   UsbTransport.open() không còn claimInterface() trước.
 *   connect() gọi nativeSetUsbFd() với fd sạch (chưa claim interface).
 *   libusb tự claim interface → Endpoint sạch → version exchange OK.
 *   Nếu Mode 1 thất bại: gọi prepareForBulkTransfers() → Mode 2/3.
 */
class NativeBridge(private val context: Context) {

    companion object {
        private const val TAG = "NativeBridge"

        init {
            System.loadLibrary("sideloadnative")
        }

        private val _trustRequired = MutableStateFlow(false)
        val trustRequired: StateFlow<Boolean> = _trustRequired

        @JvmStatic
        fun onTrustRequired() {
            Log.i(TAG, "Trust popup required")
            _trustRequired.value = true
            UiPrompt.showTrustBanner("⚠️  Bấm \"Tin cậy\" (Trust This Computer) trên màn hình iPhone!")
        }

        @JvmStatic
        fun dismissTrust() {
            _trustRequired.value = false
            UiPrompt.dismissTrustBanner()
        }

        @JvmStatic
        fun onNativeLog(line: String) {
            NativeLog.emit(line)
        }
    }

    fun init() {
        nativeInit(context.filesDir.absolutePath)
    }

    // ── setUsbFd — truyền Android USB fd vào libusb (Mode 1 only) ─────────────
    suspend fun setUsbFd(fd: Int, vendorId: Int, productId: Int): Boolean =
        withContext(Dispatchers.IO) {
            try {
                NativeLog.emit("[bridge] libusb_wrap_sys_device(fd=$fd vid=0x${vendorId.toString(16)})...")
                val udid = UsbTransport.getSerialNumber()
                val ok = nativeSetUsbFd(fd, vendorId, productId, udid)
                if (ok) NativeLog.emit("[bridge] ✅ libusb sẵn sàng")
                else    NativeLog.emit("[bridge] ❌ nativeSetUsbFd thất bại")
                ok
            } catch (_: UnsatisfiedLinkError) {
                NativeLog.emit("[bridge] ℹ️  Mode fallback — setUsbFd bỏ qua (OK)")
                true
            } catch (e: Exception) {
                NativeLog.emit("[bridge] ❌ setUsbFd: ${e.message}")
                false
            }
        }

    // ── Connection state ───────────────────────────────────────────────────────
    fun isNativeConnected(): Boolean = try { nativeIsConnected() } catch (_: Exception) { false }
    fun connectionState(): Int       = try { nativeGetConnectionState() } catch (_: Exception) { 0 }

    // ── connect — mở lockdownd session ─────────────────────────────────────────
    /**
     * FIX v27: Học từ termux-usbmuxd.
     *
     * Flow mới:
     *   1. UsbTransport.open() đã KHÔNG claim interface (fd sạch cho libusb)
     *   2. nativeSetUsbFd() với fd sạch → libusb claim interface tự do
     *      → Endpoint sạch → version exchange thành công
     *   3. Nếu Mode 1 thất bại, đóng session hiện tại; requestAndOpen()
     *      sẽ mở UsbDeviceConnection/fd mới thay vì re-wrap cùng fd.
     *
     * Retry ở mức mux/native chỉ giữ nguyên handle và fd của session; không
     * reset USB bus, không đóng/re-wrap descriptor giữa các lần thử.
     */
    suspend fun connect(): Boolean = withContext(Dispatchers.IO) {
        try {
            // ── FIX v27: Auto-call setUsbFd với fd SẠCH (không claim interface) ──
            if (UsbTransport.isConnected()) {
                val fd  = UsbTransport.getFileDescriptor()
                val vid = UsbTransport.getVendorId()
                val pid = UsbTransport.getProductId()
                if (fd > 0) {
                    try {
                        /*
                         * FIX v27 (termux-usbmuxd pattern):
                         *
                         * UsbTransport.open() giờ KHÔNG claim interface, giống
                         * UsbAPI.java của termux-api. fd được truyền trực tiếp
                         * cho libusb mà không có Android interface ownership.
                         *
                         * libusb có thể:
                         *   1. Claim interface thành công (LIBUSB_SUCCESS) → sạch hoàn toàn
                         *   2. Claim trả LIBUSB_ERROR_BUSY → vẫn hoạt động (handled trong C)
                         * Cả hai trường hợp đều tốt hơn trường hợp cũ (Android pre-claim
                         * gây STALL trên endpoints).
                         *
                         * Mỗi Android fd chỉ được wrap một lần trong một USB session;
                         * retry attach sẽ tạo session/fd mới thay vì close/re-wrap cùng fd.
                         */
                        val udid = UsbTransport.getSerialNumber()
                        /* Một Android fd chỉ được wrap một lần trong một USB session.
                         * termux-usbmuxd giữ nguyên descriptor và daemon; không retry
                         * bằng cách close/re-wrap cùng fd. */
                        val fdOk = nativeSetUsbFd(fd, vid, pid, udid)
                        if (!fdOk) {
                            NativeLog.emit("[bridge] ❌ libusb/usbmux attach thất bại trên USB fd hiện tại")
                            NativeLog.emit("[bridge] 💡 Không claim interface fallback; mở lại USB session rồi thử lại")
                            UsbTransport.close()
                            return@withContext false
                        }
                        NativeLog.emit("[bridge] ✅ libusb bridge ready — fd sạch (termux-api pattern)")
                    } catch (_: UnsatisfiedLinkError) {
                        // Mode 2/3 — nativeSetUsbFd không tồn tại (symbol không được link)
                        // Cần claim interface cho bulk transfers
                        NativeLog.emit("[bridge] ℹ️  Mode 2/3 — chuẩn bị bulk transfers...")
                        UsbTransport.prepareForBulkTransfers()
                    }
                }
            } else {
                NativeLog.emit("[bridge] ⚠️ UsbTransport chưa kết nối khi gọi connect()")
            }

            NativeLog.emit("[bridge] nativeConnect()...")
            val ok = nativeConnect()
            NativeLog.emit(if (ok) "[bridge] ✅ Kết nối thành công" else "[bridge] ❌ Kết nối thất bại")
            ok
        } catch (e: Exception) {
            NativeLog.emit("[bridge] ❌ connect() exception: ${e.message}")
            false
        }
    }

    // ── pair — ghép nối + TLS ──────────────────────────────────────────────────
    suspend fun pair(): Boolean = withContext(Dispatchers.IO) {
        try {
            NativeLog.emit("[bridge] nativePair()...")
            val ok = nativePair()
            NativeLog.emit(if (ok) "[bridge] ✅ Ghép nối thành công" else "[bridge] ❌ Ghép nối thất bại")
            ok
        } catch (e: Exception) {
            NativeLog.emit("[bridge] ❌ pair() exception: ${e.message}")
            false
        }
    }

    // ── sideload — AFC push + cài đặt IPA ─────────────────────────────────────
    suspend fun sideload(ipaPath: String): Boolean = withContext(Dispatchers.IO) {
        try {
            NativeLog.emit("[bridge] nativeSideload($ipaPath)")
            val ok = nativeSideload(ipaPath)
            NativeLog.emit(if (ok) "[bridge] ✅ Cài đặt xong" else "[bridge] ❌ Cài đặt thất bại")
            ok
        } catch (e: Exception) {
            NativeLog.emit("[bridge] ❌ sideload() exception: ${e.message}")
            false
        }
    }

    // ── Getters ────────────────────────────────────────────────────────────────
    suspend fun getUdid(): String? = withContext(Dispatchers.IO) {
        try { nativeGetUdid() } catch (_: Exception) { null }
    }

    fun isPaired(): Boolean = try { nativeIsPaired() } catch (_: Exception) { false }

    // ── tryReconnect — gọi bởi UsbReconnectManager ────────────────────────────
    suspend fun tryReconnect(): Boolean = withContext(Dispatchers.IO) {
        NativeLog.emit("[bridge] Thử kết nối lại...")
        if (!UsbTransport.isConnected()) {
            NativeLog.emit("[bridge] ❌ USB vẫn chưa kết nối")
            return@withContext false
        }
        try {
            val ok = nativeConnect()
            NativeLog.emit(if (ok) "[bridge] ✅ Kết nối lại thành công" else "[bridge] ❌ Kết nối lại thất bại")
            ok
        } catch (e: Exception) {
            NativeLog.emit("[bridge] ❌ tryReconnect exception: ${e.message}")
            false
        }
    }

    // ── Export pair record ─────────────────────────────────────────────────────
    suspend fun exportPairingFile(): File? = withContext(Dispatchers.IO) {
        try {
            val xml  = nativeGetPairingPlist() ?: return@withContext null
            val udid = nativeGetUdid() ?: "unknown"
            val file = File(context.filesDir, "pair_$udid.plist")
            file.writeText(xml)
            NativeLog.emit("[bridge] Đã xuất pair record: ${file.name}")
            file
        } catch (e: Exception) { null }
    }

    // ── Reset ──────────────────────────────────────────────────────────────────
    /**
     * Dọn cả native usbmuxd/libusb và Android UsbDeviceConnection.
     * Phải gọi native trước để các tunnel không còn dùng fd mà Java sắp đóng.
     */
    fun reset() {
        try { nativeReset() } catch (_: Exception) {}
        UsbTransport.close()
    }

    // ── listInstalledApps ──────────────────────────────────────────────────────
    suspend fun listInstalledApps(): List<String> = withContext(Dispatchers.IO) {
        try {
            val xml = nativeListInstalledApps() ?: return@withContext emptyList()
            val regex = Regex("<string>([A-Za-z0-9._-]+\\.[A-Za-z0-9._-]+)</string>")
            regex.findAll(xml).map { it.groupValues[1] }.toList()
        } catch (e: UnsatisfiedLinkError) {
            NativeLog.emit("[bridge] ℹ️  listInstalledApps: Mode fallback — trả [] (OK)")
            emptyList()
        } catch (e: Exception) {
            NativeLog.emit("[bridge] ⚠️  listInstalledApps: ${e.message}")
            emptyList()
        }
    }

    // ── diagnostics ────────────────────────────────────────────────────────────
    fun diagnostics(): String = try {
        nativeDiagnostics() ?: "(diagnostics không khả dụng ở mode này)"
    } catch (_: UnsatisfiedLinkError) {
        "Mode 2/3 (custom protocol). UsbTransport.isConnected()=${UsbTransport.isConnected()}"
    } catch (e: Exception) {
        "Lỗi diagnostics: ${e.message}"
    }

    // ── JNI declarations ───────────────────────────────────────────────────────
    private external fun nativeInit(filesDir: String)
    private external fun nativeSetUsbFd(fd: Int, vendorId: Int, productId: Int, udid: String?): Boolean
    private external fun nativeConnect(): Boolean
    private external fun nativePair(): Boolean
    private external fun nativeSideload(ipaPath: String): Boolean
    private external fun nativeGetUdid(): String?
    private external fun nativeIsPaired(): Boolean
    private external fun nativeGetPairingPlist(): String?
    private external fun nativeReset()
    private external fun nativeIsConnected(): Boolean
    private external fun nativeGetConnectionState(): Int
    private external fun nativeListInstalledApps(): String?
    private external fun nativeDiagnostics(): String?
}
