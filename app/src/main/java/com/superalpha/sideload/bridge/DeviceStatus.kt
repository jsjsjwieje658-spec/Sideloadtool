package com.superalpha.sideload.bridge

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/*
 * ════════════════════════════════════════════════════════════════════════
 *  v50 — DeviceStatus: snapshot trạng thái iPhone cho thẻ thiết bị.
 *
 *  Sau khi bỏ tab "Ghép nối" + "Đăng ký UDID", mọi thông tin trạng thái
 *  được gom về MỘT thẻ trên màn chính:
 *    - UsbTransport.isConnected()  → cáp đã mở fd chưa
 *    - NativeBridge.connectionState() → 0 chưa connect, 1 lockdown chưa mở,
 *      2 đã kết nối nhưng CHƯA ghép nối (cần Trust), 3 đã ghép nối + SSL OK
 *    - NativeBridge.isPaired()
 *    - AppConfig.lastUdid (được cập nhật sau mỗi lần connect thành công)
 *
 *  Cách lấy: poll 1 giây/lần trên Dispatchers.Default (các JNI getter này
 *  chỉ đọc biến toàn cục — giá như nativeIsConnected() vốn đã được gọi kiểu
 *  này từ UsbReconnectManager.doctor()), gộp vào MỘT StateFlow duy nhất để
 *  UI chỉ recompose đúng 1 nhánh nhỏ của màn hình khi trạng thái đổi.
 * ════════════════════════════════════════════════════════════════════════
 */
object DeviceStatus {

    data class Snapshot(
        val usbConnected: Boolean,
        val nativeState: Int,   // 0..3 từ nativeGetConnectionState()
        val paired: Boolean,    // nativeIsPaired()
        val udid: String,       // AppConfig.lastUdid (rỗng nếu chưa từng biết)
    ) {
        /** iPhone đã sẵn sàng cho việc ký & cài (đã ghép nối xong). */
        val ready: Boolean get() = usbConnected && nativeState >= 3
        /** Đã cắm + kết nối nhưng chưa ghép nối — chờ người dùng bấm Trust. */
        val needsTrust: Boolean get() = usbConnected && nativeState == 2
    }

    private val _snapshot = MutableStateFlow(Snapshot(false, 0, false, ""))
    val snapshot: StateFlow<Snapshot> = _snapshot.asStateFlow()

    private var scope: CoroutineScope? = null

    fun start() {
        if (scope != null) return
        scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        scope!!.launch {
            while (isActive) {
                val bridge = try { DeviceNative.getBridge() } catch (_: Exception) { null }
                val next = Snapshot(
                    usbConnected = UsbTransport.isConnected(),
                    nativeState = if (bridge != null) {
                        try { bridge.connectionState() } catch (_: Exception) { 0 }
                    } else 0,
                    paired = if (bridge != null) {
                        try { bridge.isPaired() } catch (_: Exception) { false }
                    } else false,
                    udid = AppConfig.lastUdid,
                )
                /*
                 * v53: CHỈ phát khi giá trị thật sự đổi. Trước đây mỗi giây
                 * tạo Snapshot mới (tham chiếu mới) → StateFlow phát → toàn
                 * bộ màn chính recompose 1 lần/giây ngay cả khi chẳng có gì
                 * thay đổi — đúng nhịp "dật dật" từng giây khi dùng app.
                 * Data class equals so sánh theo giá trị → idle = 0 recompose.
                 */
                if (next != _snapshot.value) _snapshot.value = next
                delay(1_000)
            }
        }
    }
}
