package com.superalpha.sideload.ui

import android.app.Application
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.superalpha.sideload.bridge.AppConfig
import com.superalpha.sideload.bridge.DeviceNative
import com.superalpha.sideload.bridge.NativeBridge
import com.superalpha.sideload.bridge.NativeLog
import com.superalpha.sideload.bridge.UsbReconnectManager
import com.superalpha.sideload.bridge.UsbTransport
import com.superalpha.sideload.python.PythonBridge
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

/**
 * HomeViewModel — central state holder cho toàn bộ luồng USB + sideload.
 *
 * ═══════════════════════════════════════════════════════════════════
 * FIX v21 (Bug 1 — Dual NativeBridge):
 *
 * Trước đây: HomeViewModel tạo NativeBridge(app) riêng, trong khi
 * SuperAlphaApp.onCreate() → DeviceNative.init() cũng tạo một instance khác.
 * Hai lần nativeInit() → C global state bị reset (g_mux, g_ld, g_rec, g_udid
 * đều về zero) → kết nối thiết lập bởi DeviceNative không còn hiệu lực.
 *
 * Fix: Dùng DeviceNative.getBridge(app) — trả về CÙNG singleton đã init.
 * Chỉ một NativeBridge instance tồn tại trong toàn bộ process.
 *
 * FIX v21 (Bug 4 — connectAndPair() bỏ qua connect()):
 *
 * Trước đây: connectAndPair() chỉ gọi pair() mà không gọi connect() trước.
 * Nếu người dùng bấm "Bắt đầu Ghép nối" mà chưa kết nối → pair() thất bại
 * mà không có thông báo rõ ràng.
 *
 * Fix: connectAndPair() gọi connect() trước nếu chưa kết nối, với log rõ ràng.
 * ═══════════════════════════════════════════════════════════════════
 *
 * Luồng đầy đủ với libimobiledevice (Mode 1):
 * 1. MainActivity nhận USB_DEVICE_ATTACHED → UsbPermissionManager.requestAndOpen()
 * 2. Callback trả (ok, msg, device) → onUsbDeviceGranted(device, usbManager)
 * 3. UsbTransport.open(device, usbManager)  → claim USB interface
 * 4. nativeBridge.connect()                 → tự gọi setUsbFd + idevice_new + lockdownd
 * 5. [PairingScreen] connectAndPair()       → connect() (nếu cần) + pair()
 * 6. [SideloadScreen] nativeBridge.sideload → AFC push + instproxy
 */
class HomeViewModel(app: Application) : AndroidViewModel(app) {

    // FIX v21: Dùng DeviceNative.getBridge() thay vì NativeBridge(app) mới
    val nativeBridge: NativeBridge = DeviceNative.getBridge(app)

    private val _log = MutableStateFlow<List<String>>(emptyList())
    val log: StateFlow<List<String>> = _log

    private val _usbConnected = MutableStateFlow(false)
    val usbConnected: StateFlow<Boolean> = _usbConnected

    private val _busy = MutableStateFlow(false)
    val busy: StateFlow<Boolean> = _busy

    private val _savedAppleId     = MutableStateFlow(AppConfig.appleId)
    val savedAppleId: StateFlow<String> = _savedAppleId

    private val _savedAnisetteUrl = MutableStateFlow(AppConfig.anisetteUrl)
    val savedAnisetteUrl: StateFlow<String> = _savedAnisetteUrl

    private val _isPaired = MutableStateFlow(false)
    val isPaired: StateFlow<Boolean> = _isPaired

    private val _anisetteServers = MutableStateFlow<List<PythonBridge.AnisetteServer>>(emptyList())
    val anisetteServers: StateFlow<List<PythonBridge.AnisetteServer>> = _anisetteServers

    private val _anisetteServersLoading = MutableStateFlow(false)
    val anisetteServersLoading: StateFlow<Boolean> = _anisetteServersLoading

    val trustRequired  = NativeBridge.trustRequired
    val reconnectState = UsbReconnectManager.state
    val reconnectCount = UsbReconnectManager.reconnectCount

    init {
        // Không gọi nativeBridge.init() ở đây — DeviceNative.init() đã gọi rồi
        viewModelScope.launch {
            NativeLog.lines.collect { line ->
                _log.value = (_log.value + line).takeLast(500)
            }
        }

        viewModelScope.launch {
            UsbTransport.connected.collect { connected ->
                _usbConnected.value = connected
                if (!connected) UsbReconnectManager.notifyDisconnected()
            }
        }

        UsbReconnectManager.start(getApplication(), nativeBridge)
    }

    fun setBusy(v: Boolean)        { _busy.value = v }
    fun clearLog()                 { _log.value = emptyList() }
    fun saveAppleId(v: String)     { _savedAppleId.value = v; AppConfig.appleId = v }
    fun saveAnisetteUrl(v: String) { _savedAnisetteUrl.value = v; AppConfig.anisetteUrl = v }
    fun dismissTrust()             = NativeBridge.dismissTrust()
    fun emitLog(line: String)      = NativeLog.emit(line)

    // ── Bước 2-4: USB vừa được cấp quyền và mở thành công ───────────────────
    /**
     * onUsbDeviceGranted — gọi sau khi UsbPermissionManager.requestAndOpen()
     * thành công. Chỉ cần gọi connect() vì connect() đã tự gọi setUsbFd().
     *
     * FIX v21: connect() tích hợp sẵn setUsbFd() → không cần gọi riêng nữa.
     */
    fun onUsbDeviceGranted(device: UsbDevice, usbManager: UsbManager) {
        if (_busy.value) return
        _busy.value = true
        viewModelScope.launch {
            // USB đã được mở bởi UsbPermissionManager (UsbTransport.open() đã chạy)
            // connect() sẽ tự gọi setUsbFd() từ UsbTransport state
            NativeLog.emit("[usb] ✅ USB open (fd=${UsbTransport.getFileDescriptor()})")

            if (nativeBridge.connect()) {
                nativeBridge.getUdid()?.let { AppConfig.lastUdid = it }
                UsbReconnectManager.notifyConnected()
            } else {
                NativeLog.emit("[device] ❌ Kiểm tra màn hình iPhone đã mở khoá chưa")
            }
            _busy.value = false
        }
    }

    /** Backward-compat: khi chỉ có intent, không có UsbDevice ref */
    fun onUsbReady() {
        if (_busy.value) return
        _busy.value = true
        viewModelScope.launch {
            if (nativeBridge.connect()) {
                nativeBridge.getUdid()?.let { AppConfig.lastUdid = it }
                UsbReconnectManager.notifyConnected()
            }
            _busy.value = false
        }
    }

    // ── Bước 5: Ghép nối (PairingScreen) ─────────────────────────────────────
    /**
     * connectAndPair — gọi từ PairingScreen "Bắt đầu Ghép nối" button.
     *
     * FIX v21: Gọi connect() trước nếu chưa kết nối, sau đó mới pair().
     * Trước đây chỉ gọi pair() → thất bại nếu connect() chưa chạy.
     */
    fun connectAndPair() {
        if (_busy.value) return
        _busy.value = true
        viewModelScope.launch {
            // Bước 1: Kết nối nếu chưa kết nối
            if (!nativeBridge.isNativeConnected()) {
                NativeLog.emit("[pairing] Chưa kết nối — thực hiện connect() trước...")
                val connected = nativeBridge.connect()
                if (!connected) {
                    NativeLog.emit("❌ Ghép nối thất bại — kiểm tra nhật ký phía trên để biết chi tiết.")
                    _busy.value = false
                    return@launch
                }
                nativeBridge.getUdid()?.let { AppConfig.lastUdid = it }
            }

            // Bước 2: Ghép nối
            val ok = nativeBridge.pair()
            _isPaired.value = ok
            if (ok) {
                NativeLog.emit("✅ Ghép nối thành công!")
                UsbReconnectManager.notifyConnected()
            } else {
                NativeLog.emit("❌ Ghép nối thất bại — kiểm tra nhật ký phía trên để biết chi tiết.")
            }
            _busy.value = false
        }
    }

    // ── Bước 6: Cài đặt IPA (SideloadScreen) ─────────────────────────────────
    fun sideload(ipaPath: String, onResult: (Boolean) -> Unit) {
        if (_busy.value) return
        _busy.value = true
        viewModelScope.launch {
            onResult(nativeBridge.sideload(ipaPath))
            _busy.value = false
        }
    }

    // ── Kết nối lại thủ công ──────────────────────────────────────────────────
    fun manualReconnect() {
        if (_busy.value) return
        _busy.value = true
        viewModelScope.launch {
            val ok = UsbReconnectManager.manualReconnect()
            if (ok) nativeBridge.getUdid()?.let { AppConfig.lastUdid = it }
            _busy.value = false
        }
    }

    // ── Export pair record ────────────────────────────────────────────────────
    fun exportPairingFile(onResult: (java.io.File?) -> Unit) {
        viewModelScope.launch { onResult(nativeBridge.exportPairingFile()) }
    }

    // ── Anisette servers ──────────────────────────────────────────────────────
    fun loadAnisetteServersIfNeeded() {
        if (_anisetteServers.value.isNotEmpty() || _anisetteServersLoading.value) return
        reloadAnisetteServers()
    }

    fun reloadAnisetteServers() {
        _anisetteServersLoading.value = true
        viewModelScope.launch {
            _anisetteServers.value = PythonBridge.listAnisetteServers()
            _anisetteServersLoading.value = false
        }
    }

    override fun onCleared() {
        super.onCleared()
        UsbReconnectManager.stop()
        nativeBridge.reset()
    }
}
