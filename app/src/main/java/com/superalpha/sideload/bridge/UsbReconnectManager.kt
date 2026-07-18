package com.superalpha.sideload.bridge

import android.content.Context
import android.hardware.usb.UsbManager
import android.util.Log
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * UsbReconnectManager — Tự động phát hiện và xử lý ngắt kết nối USB.
 *
 * Học từ termux-usbmuxd (usbmuxd_proxy.c + termux-usbmuxd script):
 * - Poll USB state mỗi 1 giây
 * - Khi phát hiện iPhone bị ngắt: đánh dấu disconnected
 * - Khi iPhone cắm lại: tự mở lại + setUsbFd + connect với EXPONENTIAL BACKOFF
 * - Giống cách termux-usbmuxd kiên nhẫn thử lại khi usbmuxd chưa sẵn sàng
 * - manualReconnect(): gọi từ UI button "Kết nối lại"
 * - doctor(): kiểm tra trạng thái tất cả thành phần (học từ lệnh "doctor" của termux-usbmuxd)
 */
object UsbReconnectManager {
    private const val TAG = "UsbReconnectManager"

    /** Backoff constants — học từ termux-usbmuxd retry loop kiên nhẫn */
    private const val BACKOFF_INITIAL_MS  = 1_000L
    private const val BACKOFF_MAX_MS      = 30_000L
    private const val BACKOFF_MULTIPLIER  = 2.0
    private const val MAX_RECONNECT_TRIES = 10

    enum class State { IDLE, CONNECTED, DISCONNECTED, RECONNECTING }

    private val _state = MutableStateFlow(State.IDLE)
    val state: StateFlow<State> = _state

    private val _reconnectCount = MutableStateFlow(0)
    val reconnectCount: StateFlow<Int> = _reconnectCount

    private val _lastError = MutableStateFlow<String?>(null)
    val lastError: StateFlow<String?> = _lastError

    private var scope: CoroutineScope? = null
    private var pollJob: Job? = null

    @Volatile private var appContext: Context? = null
    @Volatile private var bridge: NativeBridge? = null

    fun start(context: Context, nativeBridge: NativeBridge) {
        appContext = context.applicationContext
        bridge = nativeBridge
        scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
        startPolling()
    }

    fun stop() {
        pollJob?.cancel()
        scope?.cancel()
        scope = null
    }

    fun notifyConnected() {
        _state.value = State.CONNECTED
        _reconnectCount.value = 0
        _lastError.value = null
    }

    fun notifyDisconnected() {
        if (_state.value == State.CONNECTED || _state.value == State.RECONNECTING) {
            Log.i(TAG, "USB disconnected — bắt đầu theo dõi để reconnect")
            _state.value = State.DISCONNECTED
            startPolling()
        }
    }

    /**
     * manualReconnect — gọi từ UI "Kết nối lại".
     * Reset backoff và thử ngay lập tức.
     */
    suspend fun manualReconnect(): Boolean {
        _reconnectCount.value = 0
        _lastError.value = null
        return attemptReconnect()
    }

    private fun startPolling() {
        val sc = scope ?: return
        pollJob?.cancel()
        pollJob = sc.launch {
            val ctx = appContext ?: return@launch
            val usbManager = ctx.getSystemService(Context.USB_SERVICE) as UsbManager

            var backoffMs = BACKOFF_INITIAL_MS
            var tries = 0

            while (isActive) {
                val appleDevice = UsbTransport.findAppleDevice(usbManager)

                when (_state.value) {
                    State.CONNECTED -> {
                        // Kiểm tra xem kết nối vẫn còn không
                        if (appleDevice == null || !UsbTransport.isConnected()) {
                            Log.w(TAG, "Phát hiện ngắt kết nối — chuyển DISCONNECTED")
                            NativeLog.emit("[reconnect] ⚠️ iPhone ngắt kết nối USB.")
                            _state.value = State.DISCONNECTED
                            backoffMs = BACKOFF_INITIAL_MS
                            tries = 0
                        }
                        delay(1_000)
                    }

                    State.DISCONNECTED, State.RECONNECTING -> {
                        if (appleDevice == null) {
                            // iPhone chưa cắm lại — chờ không backoff
                            delay(1_000)
                            continue
                        }

                        // iPhone đã cắm lại — thử kết nối với backoff
                        if (tries >= MAX_RECONNECT_TRIES) {
                            NativeLog.emit("[reconnect] ❌ Đã thử $tries lần, bỏ cuộc. Thử lại thủ công.")
                            _lastError.value = "Đã thử $tries lần không thành công"
                            _state.value = State.DISCONNECTED
                            delay(5_000)
                            tries = 0
                            backoffMs = BACKOFF_INITIAL_MS
                            continue
                        }

                        _state.value = State.RECONNECTING
                        NativeLog.emit("[reconnect] Lần thử ${tries + 1}/$MAX_RECONNECT_TRIES (chờ ${backoffMs}ms)...")
                        delay(backoffMs)

                        val ok = attemptReconnect()
                        if (ok) {
                            _state.value = State.CONNECTED
                            _reconnectCount.value = _reconnectCount.value + 1
                            _lastError.value = null
                            NativeLog.emit("[reconnect] ✅ Kết nối lại thành công sau ${tries + 1} lần thử.")
                            backoffMs = BACKOFF_INITIAL_MS
                            tries = 0
                        } else {
                            tries++
                            // Exponential backoff — giống termux-usbmuxd retry loop
                            backoffMs = minOf((backoffMs * BACKOFF_MULTIPLIER).toLong(), BACKOFF_MAX_MS)
                            _lastError.value = "Kết nối thất bại (lần $tries)"
                        }
                    }

                    State.IDLE -> delay(2_000)
                }
            }
        }
    }

    private suspend fun attemptReconnect(): Boolean {
        val b = bridge ?: return false
        return try {
            b.tryReconnect()
        } catch (e: Exception) {
            Log.w(TAG, "attemptReconnect exception: ${e.message}")
            false
        }
    }

    /**
     * doctor — Kiểm tra trạng thái tất cả thành phần.
     * Học từ lệnh "termux-usbmuxd doctor" — tự chẩn đoán và báo cáo vấn đề.
     *
     * @return chuỗi báo cáo chẩn đoán để hiển thị trên UI / LogConsole
     */
    fun doctor(context: Context): String {
        val sb = StringBuilder()
        sb.appendLine("═══ CHẨN ĐOÁN KẾT NỐI iPHONE ═══")
        sb.appendLine("(học từ termux-usbmuxd doctor command)")
        sb.appendLine()

        // 1. Kiểm tra USB device
        val usbManager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        val appleDevice = UsbTransport.findAppleDevice(usbManager)
        if (appleDevice != null) {
            sb.appendLine("✅ USB: iPhone phát hiện (vid=0x${appleDevice.vendorId.toString(16)} pid=0x${appleDevice.productId.toString(16)})")
        } else {
            sb.appendLine("❌ USB: Không thấy iPhone — cắm cáp USB và thử lại")
        }

        // 2. Kiểm tra UsbTransport
        sb.appendLine(if (UsbTransport.isConnected()) "✅ UsbTransport: đã mở" else "⚠️  UsbTransport: chưa mở")

        // 3. Kiểm tra NativeBridge
        val b = bridge
        if (b != null) {
            val state = b.connectionState()
            val stateStr = when (state) {
                0 -> "chưa kết nối"
                1 -> "connected, chưa lockdown"
                2 -> "lockdown OK, chưa pair"
                3 -> "paired ✅"
                else -> "unknown=$state"
            }
            sb.appendLine("ℹ️  NativeBridge state: $stateStr")
            sb.appendLine(if (b.isPaired()) "✅ Pairing: đã ghép nối" else "⚠️  Pairing: chưa ghép nối")
        } else {
            sb.appendLine("❌ NativeBridge: chưa khởi tạo")
        }

        // 4. Kiểm tra reconnect state
        sb.appendLine("ℹ️  Reconnect state: ${_state.value}")
        sb.appendLine("ℹ️  Số lần reconnect: ${_reconnectCount.value}")
        _lastError.value?.let { sb.appendLine("⚠️  Lỗi gần nhất: $it") }

        // 5. Gợi ý
        sb.appendLine()
        sb.appendLine("─── Gợi ý ───")
        if (appleDevice == null) {
            sb.appendLine("→ Cắm cáp Lightning/USB-C vào điện thoại Android")
            sb.appendLine("→ Một số máy cần cáp OTG adapter")
        } else if (!UsbTransport.isConnected()) {
            sb.appendLine("→ Bấm nút 'Kết nối' và cấp quyền USB khi được hỏi")
        } else if (b != null && !b.isPaired()) {
            sb.appendLine("→ Bấm 'Ghép nối' và bấm 'Tin cậy' trên iPhone")
        } else {
            sb.appendLine("→ Thiết bị sẵn sàng để sideload IPA")
        }

        return sb.toString()
    }
}
