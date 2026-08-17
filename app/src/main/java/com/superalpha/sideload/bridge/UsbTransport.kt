package com.superalpha.sideload.bridge

import android.hardware.usb.*
import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * UsbTransport — Quản lý kết nối USB thô với iPhone/iPad qua Android USB Host API.
 *
 * ════════════════════════════════════════════════════════════════════
 * KIẾN TRÚC HỌC TỪ termux-usbmuxd + termux-api (UsbAPI.java)
 * ════════════════════════════════════════════════════════════════════
 *
 * termux-usbmuxd dùng lệnh:
 *   termux-usb -E -e "usbmuxd_proxy <args>" /dev/bus/usb/XXX/YYY
 *
 * termux-api (UsbAPI.java) thực hiện:
 *   connection = usbManager.openDevice(device)   ← CHỈ open, KHÔNG claim interface
 *   fd = connection.getFileDescriptor()
 *   openDevices.put(fd, connection)              ← giữ connection để fd không bị close
 *   return fd                                    ← gửi fd cho usbmuxd qua TERMUX_USB_FD
 *
 * usbmuxd thật nhận fd qua env LIBUSB_FD:
 *   libusb_wrap_sys_device(ctx, fd, &handle)     ← libusb quản lý fd
 *   libusb_claim_interface(handle, iface_num)    ← libusb tự claim (không phải Android)
 *   → Endpoint ở trạng thái SẠCH → version exchange thành công
 *
 * ════════════════════════════════════════════════════════════════════
 * VẤN ĐỀ CŨ CỦA SIDELOADTOOL (trước fix này)
 * ════════════════════════════════════════════════════════════════════
 *
 * open() cũ gọi claimInterface() TRƯỚC KHI truyền fd cho libusb. Điều này:
 *   1. Đặt endpoint vào trạng thái mà Android kernel USB stack quản lý
 *   2. Khi libusb wrap fd và dùng endpoint đó → LIBUSB_ERROR_PIPE (STALL)
 *   3. Version exchange thất bại ngay cả sau nhiều lần clear_halt() và retry
 *
 * ════════════════════════════════════════════════════════════════════
 * FIX v27: Theo đúng mô hình UsbAPI.java của termux-api
 * ════════════════════════════════════════════════════════════════════
 *
 * Mode 1 (libimobiledevice + libusb):
 *   open() → getFileDescriptor() → nativeSetUsbFd()
 *   → libusb_wrap_sys_device(fd) → libusb tự claim interface
 *   → Version exchange thành công (endpoint sạch, không STALL)
 *   KHÔNG gọi claimInterface() — giống hệt UsbAPI.java
 *
 * Mode 2/3 (fallback custom protocol — UsbDeviceConnection.bulkTransfer()):
 *   prepareForBulkTransfers() → claimInterface() → endpointIn/Out sẵn sàng
 *   → nativeBulkWrite/nativeBulkRead hoạt động bình thường
 *   (chỉ gọi khi Mode 1 thất bại và cần fallback)
 */
object UsbTransport {
    private const val TAG = "UsbTransport"

    const val VENDOR_ID_APPLE     = 0x05AC
    private const val IFACE_CLASS    = 0xFF   // Vendor Specific
    private const val IFACE_SUBCLASS = 0xFE   // Apple Mobile Device
    private const val IFACE_PROTOCOL = 0x02   // usbmux

    // USB CLEAR_FEATURE(ENDPOINT_HALT) constants — dùng sau claimInterface()
    private const val USB_DIR_OUT           = 0x00
    private const val USB_TYPE_STANDARD     = 0x00
    private const val USB_RECIP_ENDPOINT    = 0x02
    private const val USB_REQ_CLEAR_FEATURE = 0x01
    private const val USB_ENDPOINT_HALT     = 0x0000

    /*
     * FIX v40 (Critical): USB buffer sizes học từ usbmuxd upstream.
     *
     * usbmuxd upstream dùng USB_MRU=16384 (16KB) cho read buffer và
     * USB_MTU=49152 (48KB) cho write buffer. Code cũ dùng buf size động
     * (do caller truyền vào) → có thể quá nhỏ → LIBUSB_ERROR_OVERFLOW
     * hoặc iPhone response bị cắt.
     *
     * Chuẩn hóa theo upstream:
     *   - Read buffer: 16384 bytes (USB_MRU)
     *   - Write buffer: caller-controlled (VERSION = 20 bytes, etc.)
     */
    private const val USB_MRU = 16384  // Max Receive Unit (usbmuxd upstream)

    @Volatile private var connection:       UsbDeviceConnection? = null
    @Volatile private var usbInterface:     UsbInterface?        = null
    @Volatile private var endpointIn:       UsbEndpoint?         = null
    @Volatile private var endpointOut:      UsbEndpoint?         = null
    @Volatile private var currentDevice:    UsbDevice?           = null
    @Volatile private var interfaceClaimed: Boolean              = false

    private val _connected = MutableStateFlow(false)
    val connected = _connected.asStateFlow()

    @Volatile private var _lastError: String? = null
    @JvmStatic fun lastError(): String? = _lastError

    fun isConnected() = _connected.value

    /*
     * FIX v40: Public accessor để NativeBridge.onNativeBulkWrite/Read có thể
     * check xem interface đã claim chưa trước khi gọi bulkTransfer().
     */
    fun isInterfaceClaimed(): Boolean = interfaceClaimed

    // Mode 1: fd cho libusb_wrap_sys_device()
    fun getFileDescriptor(): Int = connection?.fileDescriptor ?: -1
    fun getVendorId():  Int = currentDevice?.vendorId  ?: 0
    fun getProductId(): Int = currentDevice?.productId ?: 0
    /** FIX UDID: Lấy UDID thật từ Android UsbDevice.serialNumber */
    fun getSerialNumber(): String? = currentDevice?.serialNumber

    /*
     * FIX v37: Expose endpoint addresses + interface number cho native layer.
     *
     * Trước đây, native layer discover_apple_endpoints() tự tìm interface
     * Apple AMDI qua libusb_get_active_config_descriptor(). Nhưng khi dùng
     * libusb_wrap_sys_device(fd) với Android fd, descriptor access không
     * đáng tin — discovery thường fail → fallback endpoint mặc định
     * (0x85/0x04) → libusb_claim_interface() KHÔNG được gọi →
     * LIBUSB_ERROR_IO trên mọi bulk_transfer.
     *
     * Kotlin đã có sẵn thông tin này từ findUsbmuxIface() → truyền thẳng
     * xuống native để bypass discovery.
     */
    /** Trả về endpoint IN address (vd: 0x85) hoặc 0 nếu chưa open */
    fun getEndpointInAddress(): Int = endpointIn?.address?.toInt() ?: 0
    /** Trả về endpoint OUT address (vd: 0x04) hoặc 0 nếu chưa open */
    fun getEndpointOutAddress(): Int = endpointOut?.address?.toInt() ?: 0
    /** Trả về interface number (vd: 0) hoặc -1 nếu chưa open */
    fun getInterfaceId(): Int = usbInterface?.id ?: -1

    // ── Tìm thiết bị Apple ────────────────────────────────────────────────────
    fun findAppleDevice(usbManager: UsbManager): UsbDevice? =
        usbManager.deviceList.values.firstOrNull { it.vendorId == VENDOR_ID_APPLE }

    private data class FoundIface(
        val config: UsbConfiguration,
        val iface: UsbInterface,
        val epIn: UsbEndpoint,
        val epOut: UsbEndpoint
    )

    private fun findUsbmuxIface(device: UsbDevice): FoundIface? {
        for (ci in 0 until device.configurationCount) {
            val cfg = device.getConfiguration(ci)
            for (ii in 0 until cfg.interfaceCount) {
                val iface = cfg.getInterface(ii)
                if (iface.interfaceClass    != IFACE_CLASS    ||
                    iface.interfaceSubclass != IFACE_SUBCLASS ||
                    iface.interfaceProtocol != IFACE_PROTOCOL) continue
                var epIn: UsbEndpoint? = null
                var epOut: UsbEndpoint? = null
                for (ei in 0 until iface.endpointCount) {
                    val ep = iface.getEndpoint(ei)
                    if (ep.type != UsbConstants.USB_ENDPOINT_XFER_BULK) continue
                    if (ep.direction == UsbConstants.USB_DIR_IN  && epIn  == null) epIn  = ep
                    if (ep.direction == UsbConstants.USB_DIR_OUT && epOut == null) epOut = ep
                }
                if (epIn != null && epOut != null)
                    return FoundIface(cfg, iface, epIn, epOut)
            }
        }
        return null
    }

    fun findUsbmuxInterface(device: UsbDevice): UsbInterface? = findUsbmuxIface(device)?.iface

    // ── Open ─────────────────────────────────────────────────────────────────
    /**
     * open() — Mở kết nối USB theo mô hình UsbAPI.java của termux-api.
     *
     * ══════════════════════════════════════════════════════════════════
     * QUAN TRỌNG: KHÔNG gọi claimInterface() ở đây.
     * ══════════════════════════════════════════════════════════════════
     *
     * Lý do (học từ termux-usbmuxd/UsbAPI.java):
     *   Khi Android claimInterface() trước, USB kernel state manager của
     *   Android "owns" các endpoint. Khi libusb sau đó dùng cùng fd
     *   (qua libusb_wrap_sys_device), kernel thấy endpoint đang bị owned
     *   bởi Android stack → LIBUSB_ERROR_PIPE (STALL) trên mọi bulk transfer.
     *
     *   UsbAPI.java chỉ làm:
     *     connection = usbManager.openDevice(device)
     *     fd = connection.getFileDescriptor()
     *     openDevices.put(fd, connection)  ← giữ alive
     *     return fd
     *   Không gì thêm. libusb sau đó tự claim interface qua fd.
     *
     * Việc lưu `connection` object đảm bảo fd không bị đóng (fd chỉ hợp lệ
     * khi UsbDeviceConnection còn sống — giống cách UsbAPI.java dùng
     * openDevices static map để giữ connection).
     *
     * Mode 2/3 (bulkTransfer): gọi prepareForBulkTransfers() riêng sau khi
     * Mode 1 thất bại — hàm đó mới claim interface.
     */
    @Synchronized
    fun open(device: UsbDevice, usbManager: UsbManager): Boolean {
        close()
        _lastError = null

        // Verify Apple AMDI interface tồn tại (chỉ để kiểm tra, không claim)
        val found = findUsbmuxIface(device) ?: run {
            _lastError = "Không tìm thấy usbmux interface (class=0xFF sub=0xFE proto=0x02)"
            Log.e(TAG, _lastError!!); return false
        }

        val conn = usbManager.openDevice(device) ?: run {
            _lastError = "openDevice() thất bại — thiếu quyền USB"
            Log.e(TAG, _lastError!!); return false
        }

        /*
         * FIX v27: KHÔNG gọi claimInterface() ở đây.
         *
         * Lưu reference đến interface và endpoint để dùng khi cần prepareForBulkTransfers().
         * Nhưng KHÔNG claim bây giờ — để libusb tự claim sau khi nhận fd.
         *
         * Tương tự UsbAPI.java: openDevices.put(fd, connection) giữ connection alive.
         */
        connection      = conn
        usbInterface    = found.iface
        endpointIn      = found.epIn
        endpointOut     = found.epOut
        currentDevice   = device
        interfaceClaimed = false
        _connected.value = true

        Log.i(TAG, "✅ USB open (libusb mode — no interface claim): " +
                   "${device.productName ?: device.deviceName}" +
                   " | fd=${conn.fileDescriptor}" +
                   " | iface=${found.iface.id}" +
                   " | in=0x${found.epIn.address.toString(16)}" +
                   " | out=0x${found.epOut.address.toString(16)}")
        return true
    }

    /**
     * prepareForBulkTransfers() — Claim interface để dùng UsbDeviceConnection.bulkTransfer().
     *
     * Chỉ gọi hàm này khi Mode 1 (libimobiledevice/libusb) thất bại và cần dùng
     * Mode 2/3 (custom protocol với JNI callbacks nativeBulkWrite/nativeBulkRead).
     *
     * UsbDeviceConnection.bulkTransfer() yêu cầu interface đã được claim.
     * Sau claim, gửi CLEAR_FEATURE(ENDPOINT_HALT) để đảm bảo endpoint sạch.
     *
     * @return true nếu claim thành công, false nếu thất bại
     */
    @Synchronized
    fun prepareForBulkTransfers(): Boolean {
        val conn = connection ?: return false
        val iface = usbInterface ?: return false
        if (interfaceClaimed) return true  // đã claim rồi

        Log.i(TAG, "prepareForBulkTransfers: claiming interface ${iface.id}...")

        /*
         * FIX (Bug 7 — HIGH): setConfiguration() must be called BEFORE claimInterface().
         *
         * Android does not guarantee that the active USB configuration is set when
         * openDevice() returns.  On some OEM devices (MediaTek SoCs, some Samsung
         * variants) the interface remains in an unconfigured state → claimInterface()
         * fails with false even though the interface exists.
         *
         * The correct sequence (per Android USB Host API docs) is:
         *   1. openDevice()
         *   2. setConfiguration(cfg)   ← activate the configuration
         *   3. claimInterface(iface)   ← now reliably succeeds
         *
         * We find the UsbConfiguration for the target interface via findUsbmuxIface()
         * and call setConfiguration() before each claimInterface() attempt.
         */
        val found = findUsbmuxIface(currentDevice ?: run {
            Log.e(TAG, "prepareForBulkTransfers: no currentDevice"); return false
        }) ?: run {
            Log.e(TAG, "prepareForBulkTransfers: usbmux iface not found"); return false
        }
        try {
            conn.setConfiguration(found.config)
            Log.i(TAG, "prepareForBulkTransfers: setConfiguration(${found.config.id}) OK")
        } catch (e: Exception) {
            Log.w(TAG, "prepareForBulkTransfers: setConfiguration exception (non-fatal): $e")
        }

        // Claim với retry (8 lần, exponential backoff)
        var claimed = false
        val delays = longArrayOf(0, 150, 300, 500, 800, 1200, 1800, 2500)
        for (i in 0 until 8) {
            if (i > 0) Thread.sleep(delays.getOrElse(i) { 2500L })
            if (conn.claimInterface(iface, true)) { claimed = true; break }
            Log.w(TAG, "prepareForBulkTransfers: claimInterface retry $i")
        }

        if (!claimed) {
            Log.e(TAG, "prepareForBulkTransfers: claimInterface thất bại sau 8 lần")
            return false
        }

        try { conn.setInterface(iface) } catch (_: Exception) {}
        interfaceClaimed = true

        // Clear endpoint halts sau khi claim
        clearEndpointHaltAndroid(endpointOut)
        clearEndpointHaltAndroid(endpointIn)

        Log.i(TAG, "✅ prepareForBulkTransfers: interface claimed, endpoints ready")
        return true
    }

    /**
     * clearEndpointHaltAndroid — gửi USB CLEAR_FEATURE(ENDPOINT_HALT) qua controlTransfer().
     *
     * Cần gọi sau claimInterface() để đảm bảo endpoint không ở trạng thái STALL.
     * Đây là cách đúng cho Android — đi thẳng vào USB Host driver, không qua libusb.
     */
    private fun clearEndpointHaltAndroid(endpoint: UsbEndpoint?) {
        endpoint ?: return
        val conn = connection ?: return
        val result = conn.controlTransfer(
            USB_DIR_OUT or USB_TYPE_STANDARD or USB_RECIP_ENDPOINT,
            USB_REQ_CLEAR_FEATURE,
            USB_ENDPOINT_HALT,
            endpoint.address,
            null, 0, 1000
        )
        Log.d(TAG, "clearEndpointHalt ep=0x${endpoint.address.toString(16)} result=$result")
    }

    @Synchronized
    fun close() {
        if (interfaceClaimed) {
            try { connection?.releaseInterface(usbInterface) } catch (_: Exception) {}
        }
        try { connection?.close() } catch (_: Exception) {}
        connection      = null
        usbInterface    = null
        endpointIn      = null
        endpointOut     = null
        currentDevice   = null
        interfaceClaimed = false
        _connected.value = false
    }

    // ── JNI callbacks — gọi từ C jni_bridge.c trong Mode 2/3 ─────────────────
    // LƯU Ý: Chỉ hoạt động SAU KHI prepareForBulkTransfers() đã claim interface!

    @JvmStatic
    fun nativeBulkWrite(data: ByteArray, timeoutMs: Int): Int {
        val ep = endpointOut ?: run {
            Log.e(TAG, "nativeBulkWrite: endpointOut NULL")
            return -1
        }
        val c  = connection ?: run {
            Log.e(TAG, "nativeBulkWrite: connection NULL")
            return -1
        }
        if (!interfaceClaimed) {
            Log.w(TAG, "nativeBulkWrite: interface chưa claim — gọi prepareForBulkTransfers() trước!")
            return -1
        }
        /*
         * FIX v40 (Critical — học từ usbmuxd upstream usb.c:176-201):
         *
         * usbmuxd gửi Zero-Length Packet (ZLP) sau mỗi packet có
         * length % wMaxPacketSize == 0. iPhone yêu cầu ZLP để biết
         * packet đã kết thúc — nếu không có ZLP, iPhone sẽ chờ thêm
         * data và không phản hồi.
         *
         * Cụ thể cho iPhone USB Full-Speed (wMaxPacketSize=64):
         *   - VERSION packet = 20 bytes → 20 < 64, không cần ZLP (short packet)
         *   - TCP packet 512 bytes → 512 % 64 == 0 → CẦN ZLP
         *
         * Tuy nhiên, libusb trên Linux/Mac tự thêm ZLP khi cần.
         * Android UsbDeviceConnection.bulkTransfer() có thể KHÔNG tự thêm.
         *
         * Quan trọng hơn: usbmuxd upstream cũng xử lý trường hợp
         * length % wMaxPacketSize == 0 bằng cách gửi ZLP riêng.
         * Chúng ta cần tự gửi ZLP trong trường hợp này.
         */
        val t0 = System.currentTimeMillis()
        val result = c.bulkTransfer(ep, data, data.size, timeoutMs)
        val dt = System.currentTimeMillis() - t0
        if (result < 0) {
            Log.e(TAG, "nativeBulkWrite: bulkTransfer ep=0x${ep.address.toString(16)} " +
                       "len=${data.size} timeout=${timeoutMs}ms → $result (dt=${dt}ms)")
            return result
        }
        Log.i(TAG, "nativeBulkWrite: bulkTransfer ep=0x${ep.address.toString(16)} " +
                   "len=${data.size} → $result bytes (dt=${dt}ms)")

        /*
         * Gửi ZLP nếu length là bội của wMaxPacketSize (64 cho Full-Speed,
         * 512 cho High-Speed). usbmuxd upstream kiểm tra:
         *   if (length % dev->wMaxPacketSize == 0) send ZLP;
         *
         * Lấy wMaxPacketSize từ endpoint descriptor (Android UsbEndpoint).
         */
        val wMaxPacketSize = ep.maxPacketSize
        if (wMaxPacketSize > 0 && data.size % wMaxPacketSize == 0) {
            Log.i(TAG, "nativeBulkWrite: gửi ZLP (len=${data.size} % wMaxPacketSize=$wMaxPacketSize == 0)")
            val zlpResult = c.bulkTransfer(ep, ByteArray(0), 0, 1000)
            Log.i(TAG, "nativeBulkWrite: ZLP result=$zlpResult")
        }

        return result
    }

    @JvmStatic
    fun nativeBulkRead(buf: ByteArray, timeoutMs: Int): Int {
        val ep = endpointIn ?: run {
            Log.e(TAG, "nativeBulkRead: endpointIn NULL")
            return -1
        }
        val c  = connection ?: run {
            Log.e(TAG, "nativeBulkRead: connection NULL")
            return -1
        }
        if (!interfaceClaimed) {
            Log.w(TAG, "nativeBulkRead: interface chưa claim — gọi prepareForBulkTransfers() trước!")
            return -1
        }
        /*
         * FIX v40 (Critical — học từ usbmuxd upstream usb.c:253-268):
         *
         * usbmuxd upstream dùng USB_MRU=16384 (16KB) cho read buffer.
         * Code cũ nhận buffer do caller truyền vào, có thể quá nhỏ
         * (vd: 8-byte header) → iPhone response bị cắt → parse fail.
         *
         * Chuẩn hóa theo upstream:
         *   - Caller luôn truyền buf size = USB_MRU (16384) hoặc lớn hơn
         *   - Ta chỉ đọc tối đa buf.size bytes
         *   - Return số byte thực đọc
         *
         * Quan trọng: UsbDeviceConnection.bulkTransfer() sẽ block cho đến
         * khi nhận được data hoặc timeout. Nếu iPhone không phản hồi trong
         * timeoutMs, return -1. Nếu nhận được short packet (< wMaxPacketSize),
         * Android sẽ return ngay lập tức với số byte thực nhận.
         */
        val t0 = System.currentTimeMillis()
        val result = c.bulkTransfer(ep, buf, buf.size, timeoutMs)
        val dt = System.currentTimeMillis() - t0
        if (result < 0) {
            Log.e(TAG, "nativeBulkRead: bulkTransfer ep=0x${ep.address.toString(16)} " +
                       "len=${buf.size} timeout=${timeoutMs}ms → $result (dt=${dt}ms)")
        } else if (result == 0) {
            Log.i(TAG, "nativeBulkRead: timeout, 0 bytes (dt=${dt}ms)")
        } else {
            Log.i(TAG, "nativeBulkRead: bulkTransfer ep=0x${ep.address.toString(16)} " +
                       "→ $result bytes (dt=${dt}ms)")
        }
        return result
    }
}
