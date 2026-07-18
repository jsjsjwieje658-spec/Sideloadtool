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
                val ok = nativeSetUsbFd(fd, vendorId, productId)
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
     *   3. Nếu Mode 1 thất bại hoàn toàn (5 lần retry với mỗi lần 5 internal retry):
     *      → prepareForBulkTransfers() → claim interface cho Mode 2/3
     *      → nativeConnect() sẽ dùng Mode 2/3 fallback
     *
     * Số lần retry Kotlin-level giảm xuống (5 lần, delay ngắn hơn vì
     * mỗi lần nativeSetUsbFd() đã có 5 internal retry × 12s = ~60s).
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
                         * Số lần retry: 5 lần Kotlin-level với delay 3s/5s/5s/8s/10s.
                         * Mỗi lần gọi nativeSetUsbFd() đã có 5 internal retry trong C.
                         * Tổng: 5 × 5 = 25 lần thử với đủ clear_halt + flush + delay.
                         */
                        val retryDelays = longArrayOf(3_000L, 5_000L, 5_000L, 8_000L, 10_000L)
                        var fdOk = nativeSetUsbFd(fd, vid, pid)
                        if (fdOk) {
                            NativeLog.emit("[bridge] ✅ libusb bridge ready — fd sạch (termux-api pattern)")
                        } else {
                            var attempt = 1
                            for (delay in retryDelays) {
                                NativeLog.emit("[bridge] ⚠️ nativeSetUsbFd lần $attempt thất bại — thử lại sau ${delay/1000}s...")
                                NativeLog.emit("[bridge] 💡 Giữ cáp USB, không rút ra. iPhone đã unlock + bấm Trust chưa?")
                                Thread.sleep(delay)
                                fdOk = nativeSetUsbFd(fd, vid, pid)
                                if (fdOk) {
                                    NativeLog.emit("[bridge] ✅ libusb bridge ready (lần thử ${attempt+1})")
                                    break
                                }
                                attempt++
                            }
                            if (!fdOk) {
                                NativeLog.emit("[bridge] ⚠️ Mode 1 (libimobiledevice) thất bại sau ${retryDelays.size+1} lần")
                                NativeLog.emit("[bridge] 🔄 Chuẩn bị Mode 2/3 (custom protocol fallback)...")
                                /*
                                 * FIX v27: Fallback sang Mode 2/3.
                                 *
                                 * Khi Mode 1 thất bại hoàn toàn, claim interface để
                                 * Mode 2/3 có thể dùng UsbDeviceConnection.bulkTransfer().
                                 * prepareForBulkTransfers() claim interface + clear endpoint halt.
                                 */
                                val bulkReady = UsbTransport.prepareForBulkTransfers()
                                if (bulkReady) {
                                    NativeLog.emit("[bridge] ✅ Mode 2/3 ready (interface claimed)")
                                } else {
                                    NativeLog.emit("[bridge] ❌ Không thể claim interface cho Mode 2/3")
                                    NativeLog.emit("[bridge] 💡 Thử: Rút cáp USB 5s rồi cắm lại và nhấn 'Kết nối'")
                                }
                                // Tiếp tục gọi nativeConnect() bất kể — để libimobiledevice
                                // thử tất cả các mode có sẵn và báo lỗi rõ ràng hơn
                            }
                        }
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
    fun reset() { try { nativeReset() } catch (_: Exception) {} }

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
    private external fun nativeSetUsbFd(fd: Int, vendorId: Int, productId: Int): Boolean
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
