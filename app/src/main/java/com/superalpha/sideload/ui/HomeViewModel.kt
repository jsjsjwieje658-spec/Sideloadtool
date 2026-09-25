package com.superalpha.sideload.ui

import android.app.Application
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.superalpha.sideload.bridge.AppConfig
import com.superalpha.sideload.bridge.DeviceNative
import com.superalpha.sideload.bridge.DeviceStatus
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
 * v50 (UI redesign):
 *   - BỎ StateFlow<List<String>> `log`: LogConsole giờ đọc trực tiếp
 *     LogBuffer.snapshot (ring buffer, xuất theo lô 100 ms) — không còn
 *     copy list + recompose toàn màn hình cho từng dòng log.
 *   - BỎ connectAndPair()/onUsbReady()/exportPairingFile(): tab "Ghép nối"
 *     và "Đăng ký UDID" đã bị xoá; do_sideload() (Python) tự connectAndPair
 *     + đăng ký UDID ngay trong Bước 0/5, không cần thao tác tay riêng.
 *   - THÊM deviceStatus (DeviceStatus.snapshot) cho thẻ trạng thái iPhone
 *     trên màn chính, và busyText để nút hành động mô tả bước đang chạy.
 *
 * Luồng đầy đủ với libimobiledevice (Mode 1) — KHÔNG đổi:
 * 1. MainActivity nhận USB_DEVICE_ATTACHED → UsbPermissionManager.requestAndOpen()
 * 2. Callback trả (ok, msg, device) → onUsbDeviceGranted(device, usbManager)
 * 3. UsbTransport.open(device, usbManager)  → claim USB interface
 * 4. nativeBridge.connect()                 → tự gọi setUsbFd + idevice_new + lockdownd
 * 5. [SideloadScreen] PythonBridge.sideload → DeviceNative.connectAndPair() + Apple API
 * 6. nativeSideload → AFC push + instproxy
 */
class HomeViewModel(app: Application) : AndroidViewModel(app) {

    val nativeBridge: NativeBridge = DeviceNative.getBridge(app)

    private val _usbConnected = MutableStateFlow(false)
    val usbConnected: StateFlow<Boolean> = _usbConnected

    private val _busy = MutableStateFlow(false)
    val busy: StateFlow<Boolean> = _busy

    private val _busyText = MutableStateFlow("")
    val busyText: StateFlow<String> = _busyText

    private val _savedAppleId     = MutableStateFlow(AppConfig.appleId)
    val savedAppleId: StateFlow<String> = _savedAppleId

    /** v51: mật khẩu đã lưu (người dùng yêu cầu lưu để khỏi nhập lại). */
    private val _savedPassword = MutableStateFlow(AppConfig.applePassword)
    val savedPassword: StateFlow<String> = _savedPassword

    /**
     * v51: cổng đăng nhập — false khi chưa có Apple ID + mật khẩu đã lưu
     * (lần đầu cài app, hoặc sau khi Đăng xuất). MainActivity dựa vào đây
     * để hiện LoginScreen thay vì app chính.
     */
    private val _signedIn = MutableStateFlow(AppConfig.hasAppleAccount())
    val signedIn: StateFlow<Boolean> = _signedIn

    private val _savedAnisetteUrl = MutableStateFlow(AppConfig.anisetteUrl)
    val savedAnisetteUrl: StateFlow<String> = _savedAnisetteUrl

    private val _anisetteServers = MutableStateFlow<List<PythonBridge.AnisetteServer>>(emptyList())
    val anisetteServers: StateFlow<List<PythonBridge.AnisetteServer>> = _anisetteServers

    private val _anisetteServersLoading = MutableStateFlow(false)
    val anisetteServersLoading: StateFlow<Boolean> = _anisetteServersLoading

    val deviceStatus  = DeviceStatus.snapshot
    val trustRequired = NativeBridge.trustRequired
    val reconnectState = UsbReconnectManager.state
    val reconnectCount = UsbReconnectManager.reconnectCount

    init {
        DeviceStatus.start()
        viewModelScope.launch {
            UsbTransport.connected.collect { connected ->
                _usbConnected.value = connected
                if (!connected) UsbReconnectManager.notifyDisconnected()
            }
        }
        UsbReconnectManager.start(getApplication(), nativeBridge)
    }

    fun setBusy(v: Boolean) {
        _busy.value = v
        if (!v) _busyText.value = ""
    }

    fun setBusyText(t: String) { _busyText.value = t }

    fun saveAppleId(v: String)     { _savedAppleId.value = v; AppConfig.appleId = v }
    fun saveAnisetteUrl(v: String) { _savedAnisetteUrl.value = v; AppConfig.anisetteUrl = v }
    fun dismissTrust()             = NativeBridge.dismissTrust()

    // ── v51: Đăng nhập / Đăng xuất Apple ID ───────────────────────────────────

    /**
     * Đăng nhập lần đầu (LoginScreen): XÁC THỰC với Apple qua Python
     * (do_login — 2FA hiện dialog nếu Apple hỏi). Thành công → lưu thông tin
     * và vào app chính.
     */
    fun signIn(appleId: String, password: String, onResult: (Boolean) -> Unit = {}) {
        if (_busy.value) return
        _busy.value = true
        _busyText.value = "Đang đăng nhập Apple ID…"
        viewModelScope.launch {
            val outcome = PythonBridge.login(
                appleId.trim(), password, _savedAnisetteUrl.value.ifBlank { null }
            )
            if (outcome.success) {
                saveCredentials(appleId, password)
            } else {
                NativeLog.emit("❌ Đăng nhập không thành công — xem nhật ký phía dưới.")
            }
            _busy.value = false
            _busyText.value = ""
            onResult(outcome.success)
        }
    }

    /** Lưu thông tin đăng nhập KHÔNG xác thực (dùng khi mạng/Anisette lỗi). */
    fun saveCredentials(appleId: String, password: String) {
        AppConfig.appleId = appleId.trim()
        AppConfig.applePassword = password
        _savedAppleId.value = appleId.trim()
        _savedPassword.value = password
        _signedIn.value = AppConfig.hasAppleAccount()
    }

    /** Đăng xuất (SettingsScreen) — lần vào app tiếp theo phải đăng nhập lại. */
    fun signOut() {
        AppConfig.clearAppleAccount()
        _savedAppleId.value = ""
        _savedPassword.value = ""
        _signedIn.value = false
        NativeLog.emit("[app] Đã đăng xuất Apple ID — cần đăng nhập lại để tiếp tục dùng app.")
    }

    // ── USB vừa được cấp quyền và mở thành công ──────────────────────────────
    /** Gọi từ MainActivity / nút "Kết nối". connect() tự setUsbFd(). */
    fun onUsbDeviceGranted(device: UsbDevice, usbManager: UsbManager) {
        if (_busy.value) return
        _busy.value = true
        _busyText.value = "Đang kết nối iPhone…"
        viewModelScope.launch {
            NativeLog.emit("[usb] ✅ USB open — ${device.productName ?: "iPhone"} (fd=${UsbTransport.getFileDescriptor()})")

            if (nativeBridge.connect()) {
                nativeBridge.getUdid()?.let { AppConfig.lastUdid = it }
                UsbReconnectManager.notifyConnected()
            } else {
                NativeLog.emit("[device] ❌ Kiểm tra màn hình iPhone đã mở khoá chưa")
            }
            _busy.value = false
            _busyText.value = ""
        }
    }

    /** Kết nối lại thủ công (dùng khi auto-reconnect bỏ cuộc). */
    fun manualReconnect() {
        if (_busy.value) return
        _busy.value = true
        _busyText.value = "Đang kết nối lại…"
        viewModelScope.launch {
            val ok = UsbReconnectManager.manualReconnect()
            if (ok) nativeBridge.getUdid()?.let { AppConfig.lastUdid = it }
            _busy.value = false
            _busyText.value = ""
        }
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
