package com.superalpha.sideload.bridge

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicLong

/**
 * UsbPermissionManager — Xử lý luồng xin quyền USB 3 bước của Android.
 *
 * ═══════════════════════════════════════════════════════════════════
 * FIX v21 (Bug 3 — callback không trả UsbDevice về caller):
 *
 * Trước đây: callback onResult là (Boolean, String) → Unit.
 * Caller (MainActivity, PairingScreen) không nhận được UsbDevice sau khi
 * quyền được cấp → không thể gọi viewModel.onUsbDeviceGranted(device, mgr)
 * → setUsbFd() không bao giờ được gọi → nativeConnect() luôn thất bại.
 *
 * Fix: Thay đổi callback thành (Boolean, String, UsbDevice?) → Unit.
 * UsbDevice được trả về khi ok=true, null khi thất bại. Vẫn tương thích
 * với caller cũ qua default lambda.
 *
 * FIX v20 (giữ nguyên): Cooldown 8 giây sau thất bại để phá vòng lặp
 * ATTACHED → claimInterface fail → re-enumerate → ATTACHED → ...
 * ═══════════════════════════════════════════════════════════════════
 */
object UsbPermissionManager {
    private const val ACTION_USB_PERMISSION = "com.superalpha.sideload.USB_PERMISSION"

    private const val AUTO_CONNECT_COOLDOWN_MS = 8_000L

    private val lastFailTimestampMs = AtomicLong(0L)

    private val ioExecutor = Executors.newSingleThreadExecutor()

    @Volatile private var requestInFlight = false
    @Volatile private var pendingReceiver: BroadcastReceiver? = null

    private fun publishUdid(device: UsbDevice) {
        val serial = try { device.serialNumber } catch (_: Exception) { null }
        if (!serial.isNullOrBlank()) {
            try { AppConfig.lastUdid = serial } catch (_: Exception) {}
        }
    }

    private fun openFailureMessage(): String {
        val detail = UsbTransport.lastError()
        return if (detail.isNullOrBlank()) "Mở kết nối USB thất bại."
        else "Mở kết nối USB thất bại: $detail"
    }

    /**
     * Tìm thiết bị Apple, xin quyền (hoặc mở ngay nếu đã có quyền), gọi [onResult].
     *
     * FIX v21: [onResult] nhận (Boolean, String, UsbDevice?) thay vì (Boolean, String).
     * UsbDevice được trả khi ok=true để caller có thể gọi onUsbDeviceGranted().
     *
     * @param fromAutoAttach true → áp dụng cooldown 8s sau thất bại.
     *                       false → người dùng bấm thủ công, bỏ qua cooldown.
     * @param onResult (success, message, device?) — device != null khi success=true.
     */
    fun requestAndOpen(
        context: Context,
        fromAutoAttach: Boolean = false,
        onResult: (Boolean, String, UsbDevice?) -> Unit = { _, _, _ -> }
    ) {
        // ── Cooldown check ─────────────────────────────────────────────────────
        if (fromAutoAttach) {
            val elapsed = System.currentTimeMillis() - lastFailTimestampMs.get()
            if (elapsed < AUTO_CONNECT_COOLDOWN_MS) {
                NativeLog.emit(
                    "[usb] Bỏ qua auto-connect: cooldown ${AUTO_CONNECT_COOLDOWN_MS / 1000}s " +
                    "sau thất bại (còn ${(AUTO_CONNECT_COOLDOWN_MS - elapsed) / 1000}s)."
                )
                return
            }
        }

        if (requestInFlight) {
            if (!fromAutoAttach) {
                onResult(false, "Đang xử lý yêu cầu kết nối trước đó — vui lòng đợi rồi thử lại.", null)
            }
            return
        }
        requestInFlight = true

        val mainHandler = android.os.Handler(android.os.Looper.getMainLooper())

        fun finish(ok: Boolean, msg: String, device: UsbDevice?) {
            if (!ok) {
                lastFailTimestampMs.set(System.currentTimeMillis())
            }
            requestInFlight = false
            mainHandler.post { onResult(ok, msg, device) }
        }

        val usbManager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        val device = UsbTransport.findAppleDevice(usbManager)
        if (device == null) {
            finish(false, "Không tìm thấy iPhone/iPad nào đang cắm qua USB.", null)
            return
        }

        if (usbManager.hasPermission(device)) {
            ioExecutor.execute {
                val ok = UsbTransport.open(device, usbManager)
                if (ok) publishUdid(device)
                finish(ok, if (ok) "Đã kết nối USB." else openFailureMessage(), if (ok) device else null)
            }
            return
        }

        // ── Xin quyền USB (system dialog) ────────────────────────────────────
        pendingReceiver?.let {
            try { context.unregisterReceiver(it) } catch (_: Exception) {}
            pendingReceiver = null
        }

        // Theo Termux:API, cần FLAG_MUTABLE để EXTRA_PERMISSION_GRANTED được trả về.
        // Ref: https://developer.android.com/about/versions/12/behavior-changes-12#pending-intent-mutability
        val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) PendingIntent.FLAG_MUTABLE else 0
        val permissionIntent = PendingIntent.getBroadcast(
            context, 0, Intent(ACTION_USB_PERMISSION).setPackage(context.packageName), flags
        )

        val receiver = object : BroadcastReceiver() {
            override fun onReceive(ctx: Context, intent: Intent) {
                if (intent.action != ACTION_USB_PERMISSION) return
                try { ctx.unregisterReceiver(this) } catch (_: Exception) {}
                pendingReceiver = null
                val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                if (!granted) {
                    finish(false, "Người dùng từ chối quyền truy cập USB.", null)
                    return
                }
                val grantedDevice: UsbDevice? = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                if (grantedDevice == null) {
                    finish(false, "Không nhận được thiết bị sau khi cấp quyền.", null)
                    return
                }
                ioExecutor.execute {
                    val ok = UsbTransport.open(grantedDevice, usbManager)
                    if (ok) publishUdid(grantedDevice)
                    finish(ok, if (ok) "Đã kết nối USB." else openFailureMessage(), if (ok) grantedDevice else null)
                }
            }
        }
        pendingReceiver = receiver

        // Đối với Android 14 (API 34) trở lên, nếu targetSdkVersion >= 34, cần thêm
        // Context.RECEIVER_NOT_EXPORTED khi đăng ký receiver runtime.
        // Ref: https://developer.android.com/about/versions/14/behavior-changes-14#runtime-receivers-exported
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            context.registerReceiver(receiver, IntentFilter(ACTION_USB_PERMISSION),
                Context.RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            context.registerReceiver(receiver, IntentFilter(ACTION_USB_PERMISSION))
        }
        usbManager.requestPermission(device, permissionIntent)
    }
}
