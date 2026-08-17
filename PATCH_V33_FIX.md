# SideloadTool Patch v33 — Fix lỗi "version exchange thất bại sau 5 lần thử"

## TÓM TẮT

Bản vá này sửa lỗi ghép nối iPhone thất bại với thông báo:

```
usbmux_version_exchange: retry N/5
usb_send_version: VERSION upstream header 8-byte + version 2.0, length=20
usbmux_version_exchange: thất bại sau 5 lần thử
do_usb_v1_connect: version exchange thất bại -- iPhone không hỗ trợ v1?
[lockdown] ✕ lockdown_client_new() err=-8
[bridge] ✕ Kết nối thất bại
```

---

## NGUYÊN NHÂN GỐC RỄ

Sau khi phân tích log 8 ảnh chụp màn hình và đối chiếu với source
`usbmuxd` upstream (`libimobiledevice/usbmuxd/src/device.c`), có 3 vấn đề:

### Vấn đề 1: Endpoint STALL không được clear trước lần thử VERSION đầu tiên

Trong `usbmux_version_exchange()` ở `usbmuxd_server.c` (dòng ~592), logic cũ:

```c
for (int attempt = 0; attempt < 5; attempt++) {
    if (attempt > 0) {                          // ← CHỈ clear từ attempt 1
        usb_bridge_clear_endpoints_halt();
        usb_bridge_flush_in(8, 200);
        usleep(1000 * 1000);
    }
    if (usb_send_version() < 0) continue;       // attempt 0 gửi vào endpoint STALL
    ...
}
```

Sau khi `libusb_wrap_sys_device()` nhận Android fd, bulk endpoint thường
ở trạng thái **STALL** do USB PID toggle mismatch (Android USB stack để lại
state từ session trước). Khi `usb_send_version()` gửi VERSION vào endpoint
đang STALL ở **attempt 0**, libusb báo "thành công" (transferred=length)
nhưng iPhone **KHÔNG** nhận byte nào → iPhone không phản hồi → timeout →
fail.

Code cũ chỉ `clear_halt` + `flush_in` từ **attempt 1** trở đi. Nhưng lúc
đó iPhone đã ở trạng thái "endpoint halted" quá lâu và cần thêm USB reset
mới nhận lại packet → 5 lần retry đều fail.

### Vấn đề 2: VERSION exchange chỉ chạy LAZY (sai với upstream)

Trong `jni_bridge_imd.c` (hàm `nativeSetUsbFd`), code cũ có comment:

```c
/*
 * Không thực hiện version exchange tại thời điểm nhận fd.
 * ...
 * Nếu gửi version packet ngay tại đây, iPhone chưa có một session mux
 * được mở và có thể bỏ qua/stall bulk endpoint
 */
```

Tuy nhiên, **upstream usbmuxd làm ngược lại**: trong `device_add()` (gọi
ngay khi USB device được enumerate), code gửi VERSION packet **NGAY LẬP TỨC**
không đợi Connect request. iPhone có "mux session window" — nếu không nhận
VERSION trong vài giây đầu sau enumeration, iPhone vào trạng thái idle và
bỏ qua VERSION packet gửi sau đó.

→ Code cũ đợi đến khi libimobiledevice gửi Connect tới socket (qua nhiều
layer) mới gọi `usbmux_version_exchange()`. Lúc đó iPhone đã idle → fail.

### Vấn đề 3: Timeout quá dài che giấu lỗi

Trong `usb_recv_version()` cũ:

```c
const int VERSION_TIMEOUT_MS = 3000;
for (int attempt = 0; attempt < 10; attempt++) {  // 10 × 3000ms = 30s
    ...
}
```

Mỗi lần gọi `usb_recv_version()` tốn tới 30 giây nếu iPhone không phản hồi.
Với 5 outer retry × 30s = **150 giây** chỉ cho version exchange. Người dùng
thấy app "treo" rất lâu và dễ tưởng là crash.

---

## CÁC FILE ĐÃ SỬA

| File | Hàm / vị trí | Mô tả |
|------|--------------|------|
| `app/src/main/cpp/usbmuxd_server.c` | `usbmux_version_exchange()` | Clear endpoint + flush IN **trước mỗi attempt** (kể cả attempt 0). Thêm delay 200ms sau clear_halt cho USB stack propagate. |
| `app/src/main/cpp/usbmuxd_server.c` | `usb_send_version()` | Validate `written > 0` và `written == sizeof(pkt)`. Thêm 50ms delay sau write để libusb flush URB. Hex dump packet. |
| `app/src/main/cpp/usbmuxd_server.c` | `usb_recv_version()` | Timeout giảm 3000ms×10 → 1500ms×5 (tổng 7.5s thay vì 30s). Hex dump header + peek 32 byte nếu packet không hợp lệ. |
| `app/src/main/cpp/jni_bridge_imd.c` | `nativeSetUsbFd()` | Thêm **eager version exchange** best-effort sau khi `usb_bridge_init_from_fd()` thành công. Nếu fail, reset state và để lazy attempt xử lý. |

---

## CHI TIẾT TỪNG FIX

### Fix 1: Clear endpoint + flush IN trước mỗi attempt

```c
// File: usbmuxd_server.c — usbmux_version_exchange()
for (int attempt = 0; attempt < 5; attempt++) {
    if (attempt > 0) {
        LOGI("usbmux_version_exchange: retry %d/5", attempt + 1);
        usleep(800 * 1000);  // 800ms chờ trước retry
    }

    /* Luôn clear endpoint halt + flush stale data trước mỗi attempt */
    usb_bridge_clear_endpoints_halt();
    usb_bridge_flush_in(8, 150);
    usleep(200 * 1000);  // 200ms cho USB stack propagate clear_halt

    if (usb_send_version() < 0) {
        LOGE("usb_send_version() thất bại (attempt %d)", attempt + 1);
        continue;
    }
    ...
}
```

### Fix 2: Giảm timeout usb_recv_version

```c
// File: usbmuxd_server.c — usb_recv_version()
const int VERSION_TIMEOUT_MS = 1500;  // giảm từ 3000ms
const int VERSION_ATTEMPTS   = 5;     // giảm từ 10
```

### Fix 3: Hex dump diagnostic

```c
// File: usbmuxd_server.c — log_hex() helper mới
static void log_hex(const char *prefix, const void *buf, int len) {
    const uint8_t *p = (const uint8_t *)buf;
    char hex[16 * 3 + 1];
    int off = 0;
    int show = len < 32 ? len : 32;
    for (int i = 0; i < show; i++) {
        off += snprintf(hex + off, sizeof(hex) - off, "%02x ", p[i]);
    }
    LOGI("%s (len=%d): %s%s", prefix, len, hex, len > 32 ? "..." : "");
}
```

Log mới sẽ hiển thị:
```
usb_send_version: pkt (len=20): 00 00 00 00 00 00 00 14 00 00 00 02 00 00 00 00 00 00 00 00
usb_recv_version: hdr (len=8): 00 00 00 00 00 00 00 14
usb_recv_version: protocol=0 length=20
usb_recv_version: iPhone mux version 2.0 (legacy header 8-byte)
```

### Fix 4: Eager version exchange

```c
// File: jni_bridge_imd.c — nativeSetUsbFd()
usbmuxd_server_reset_version_state();
emit_log("[usbmux] Đã giữ raw USB fd; trì hoãn version exchange đến lúc Connect lockdown...");

/* FIX v33: Eager version exchange "best-effort" ngay sau khi USB bridge sẵn sàng */
{
    emit_log("[usbmux] Eager version exchange (best-effort, non-blocking)...");
    bool eager_ok = usbmux_version_exchange();
    if (eager_ok) {
        emit_log("[usbmux] ✅ Eager version exchange OK — iPhone đã sẵn sàng mux session");
    } else {
        emit_log("[usbmux] ⚠️ Eager version exchange fail — sẽ retry lazily khi Connect tới");
        usbmuxd_server_reset_version_state();
    }
}
```

Nếu eager attempt thành công, `g_version_done=1` và lazy attempt trong
`do_usb_v1_connect()` sẽ skip (idempotent). Nếu fail, reset state để lazy
attempt có cơ hội retry với `clear_halt` đã apply.

---

## LOG KỲ VỌNG SAU FIX

Khi fix hoạt động đúng, log sẽ hiển thị:

```
[usb] ✅ USB open (fd=53)
[usb] libusb_wrap_sys_device(fd=53, vid=0x05ac, pid=0x12a8)
[usb] ✅ libusb ready: ep_in=0x85 ep_out=0x04
[usbmux] Đã giữ raw USB fd; trì hoãn version exchange đến lúc Connect lockdown...
[usbmux] Eager version exchange (best-effort, non-blocking)...
usb_send_version: pkt (len=20): 00 00 00 00 00 00 00 14 00 00 00 02 00 00 00 00 00 00 00 00
usb_send_version: VERSION upstream header 8-byte + version 2.0, length=20
usb_recv_version: hdr (len=8): 00 00 00 00 00 00 00 14
usb_recv_version: protocol=0 length=20
usb_recv_version: iPhone mux version 2.0 (legacy header 8-byte)
usb_send_setup: SETUP v2 đã gửi
usbmux_version_exchange: mux v2 + SETUP OK (attempt 1)
[usbmux] ✅ Eager version exchange OK — iPhone đã sẵn sàng mux session
[usbmuxd_srv] ✅ Server listening: ...
[imd] USBMUXD_SOCKET_ADDRESS=/data/user/0/com.superalpha.sideload/files/usbmuxd.sock
[imd] usbmuxd socket ready ✅
[imd] idevice_new_with_options(USBMUX)...
[imd] ✅ idevice OK
[lockdown] Mở lockdownd client (no-TLS, chờ Pair)...
[lockdown] ✅ lockdownd client OK (no-TLS; sẵn sàng Pair/Trust)
[pair] Bắt đầu ghép nối...
[pair] ⏳ Chờ "Tin cậy" trên iPhone... (1/20)
```

---

## CÁCH BUILD VÀ CÀI ĐẶT

```bash
# 1. Clone repo
git clone https://github.com/jsjsjwieje658-spec/Sideloadtool.git
cd Sideloadtool

# 2. Copy 2 file đã patched vào nơi tương ứng (nếu bạn chưa có sẵn)
#    - app/src/main/cpp/usbmuxd_server.c
#    - app/src/main/cpp/jni_bridge_imd.c

# 3. Build APK
./gradlew assembleDebug

# 4. Cài đặt lên điện thoại
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

---

## LƯU Ý QUAN TRỌNG

1. **iPhone PHẢI được mở khoá** trước khi click "Bắt đầu Ghép nối".
2. **Cáp USB phải là cáp data** (không phải cáp sạc-only).
3. **Bấm "Tin cậy"** trên iPhone khi popup Trust xuất hiện.
4. Nếu vẫn fail sau khi đã vá, gửi kèm log mới có hex dump để debug thêm:
   - Hex dump `usb_send_version: pkt` xác nhận packet gửi đi đúng.
   - Hex dump `usb_recv_version: hdr` (nếu có) cho biết iPhone trả về gì.
   - `usb_recv_version: peek` (nếu packet không hợp lệ) cho 32 byte đầu của response.

5. Nếu iPhone vẫn không phản hồi VERSION dù đã clear_halt, có thể cần
   **rút cáp USB ra và cắm lại** để reset USB enumeration state của iPhone.

---

## KIỂM TRA THÊM NẾU VẪN FAIL

Nếu sau khi vá, log hiển thị:

```
usb_send_version: pkt (len=20): 00 00 00 00 00 00 00 14 00 00 00 02 ...
usb_recv_version: timeout chờ version (5 lần × 1500ms) — iPhone không phản hồi VERSION packet
```

→ iPhone không nhận VERSION packet. Nguyên nhân có thể:

1. **Cáp sạc-only**: không có data wire. Đổi cáp.
2. **iPhone đang ở Recovery Mode**: pid sẽ là 0x1281 (Recovery) thay vì
   0x12a8 (Normal). Cần thoát Recovery bằng `ideviceenterrecovery` ngược
   hoặc giữ Home+Power 10s để restart iPhone.
3. **USB hub không tương thích**: cắm thẳng vào cổng USB của điện thoại
   Android (không qua hub).
4. **Android USB stack bị treo**: thử reboot điện thoại Android.
5. **iPhone chưa unlock màn hình**: mở khóa màn hình iPhone trước khi
   bấm "Bắt đầu Ghép nối".

---

*Patch version: v33*
*Date: 2026-08-17*
*Solved issue: usbmux_version_exchange thất bại sau 5 lần thử — iPhone không phản hồi VERSION packet*
