# SideloadTool Critical Fix v32
## Patch cho lỗi lockdown_client_new_with_handshake() err=-8 và giao tiếp iPhone

---

## 📋 TÓM TẮT CÁC LỖI ĐÃ FIX

### 🔴 LỖI 1: lockdown_client_new_with_handshake() err=-8 (LOCKDOWN_E_INVALID_HOST_ID)
**File:** `jni_bridge_imd.c` — Hàm `nativeConnect()`

**Nguyên nhân gốc:**
- Code cũ: Xóa pairing record cũ → gọi `lockdownd_client_new_with_handshake()` ngay
- `lockdownd_client_new_with_handshake()` **KHÔNG tự động pair**. Nó chỉ kiểm tra pair record hiện có.
- Không có pair record → `INVALID_HOST_ID` (-8)

**Fix:**
```
Flow mới (đúng theo libimobiledevice):
  1. Thử lockdownd_client_new_with_handshake() — nếu đã pair trước đó
  2. Nếu INVALID_HOST_ID hoặc PASSWORD_PROTECTED:
     2a. lockdownd_client_new() — kết nối KHÔNG TLS
     2b. lockdownd_pair() — thực hiện pairing (hiện Trust popup)
     2c. lockdownd_client_free() — đóng session không TLS
     2d. lockdownd_client_new_with_handshake() — kết nối CÓ TLS
```

---

### 🔴 LỖI 2: VERSION packet layout sai (20-byte vs 32-byte)
**File:** `usbmuxd_server.c` — Hàm `usb_send_version()`

**Nguyên nhân:**
- Code cũ gửi packet VERSION 32-byte với magic=0xfeedface
- Packet VERSION thật chỉ có 20-byte: 8-byte header (KHÔNG magic) + 12-byte body
- iPhone nhận packet dị dạng → không phản hồi → "version exchange thất bại 100%"

**Fix:** Đã sửa trong code gốc v30 — giữ nguyên.

---

### 🔴 LỖI 3: Thiếu MUX_PROTO_SETUP packet
**File:** `usbmuxd_server.c` — Hàm `usb_send_setup()`

**Nguyên nhân:**
- Sau version exchange OK (major>=2), phải gửi packet SETUP (protocol=2, payload 0x07)
- Thiếu packet này → iPhone không chuyển sang MUXDEV_ACTIVE → từ chối mọi TCP SYN

**Fix:** Đã thêm trong code gốc v30 — giữ nguyên.

---

### 🔴 LỖI 4: V1_PROTO_TCP sai giá trị (1 thay vì 6)
**File:** `usbmuxd_server.c`

**Nguyên nhân:**
- `#define V1_PROTO_TCP 1` — giá trị 1 là MUX_PROTO_CONTROL
- Giá trị đúng: MUX_PROTO_TCP = IPPROTO_TCP = 6
- iPhone hiểu nhầm SYN packet là control message → bỏ qua

**Fix:** Đã sửa trong code gốc v30 — giữ nguyên.

---

### 🔴 LỖI 5: USB read overflow
**File:** `usbmuxd_server.c` — Hàm `usb_read_exact()`

**Nguyên nhân:**
- Đọc 16-byte header trong khi iPhone gửi cả message lớn trong một URB
- `LIBUSB_ERROR_OVERFLOW` (-8) → dữ liệu bị mất 100%

**Fix:** Đã thêm buffered read 64KB trong code gốc v29 — giữ nguyên.

---

### 🔴 Lỗi 6: libusb claim interface trên Android
**File:** `usb_fd_bridge.c` — Hàm `discover_apple_endpoints()`

**Nguyên nhân:**
- Android UsbDeviceConnection đã claim interface → libusb_claim_interface() trả BUSY
- Code cũ retry claim → vô ích vì Android giữ interface

**Fix:** Không retry claim khi BUSY — dùng shared fd với Android.

---

### 🔴 Lỗi 7: Detached event gửi vào fd đã đóng
**File:** `usbmuxd_server.c` — Hàm `handle_client()`

**Nguyên nhân:**
- Gửi Detached event vào `client_fd` sau khi đã `close(client_fd)`
- Gây SIGPIPE/EPIPE không cần thiết

**Fix:** Đóng fd trước, broadcast Detached cho các client khác đang Listen.

---

## 📁 DANH SÁCH FILE TRONG PATCH

| File | Mô tả thay đổi |
|------|----------------|
| `jni_bridge_imd.c` | **FIX CHÍNH**: Sửa nativeConnect() để tự động pair khi err=-8 |
| `usbmuxd_server.c` | Sửa Detached event handling |
| `usb_fd_bridge.c` | Sửa claim interface và OVERFLOW handling |
| `lockdown.c` | Giữ nguyên (đã có plist_get_data fix) |
| `pairing.c` | Giữ nguyên |
| `usbmux.c` | Giữ nguyên |
| `usbmux.h` | Giữ nguyên |
| `jni_bridge.c` | Giữ nguyên |
| `install_proxy.c` | Giữ nguyên |
| `afc.c` | Giữ nguyên |
| `plist_util.c` | Giữ nguyên |
| `android_usbmuxd_fix.c` | Giữ nguyên |
| `android_usbmuxd_fix.h` | Giữ nguyên |
| `usbmuxd_server.h` | Giữ nguyên |
| `usb_fd_bridge.h` | Giữ nguyên |
| `lockdown.h` | Giữ nguyên |
| `pairing.h` | Giữ nguyên |

---

## 🚀 HƯỚNG DẪN BUILD

```bash
# 1. Copy các file patch vào thư mục app/src/main/cpp/
cp *.c *.h /path/to/Sideloadtool/app/src/main/cpp/

# 2. Build APK
./gradlew assembleDebug

# 3. Cài đặt
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

---

## 📝 LOG KỲ VỌNG SAU FIX

```
[imd] ✅ iPhone UDID: 102e03e0d56583407853e9518f945642c72298d3
[lockdown] Mở lockdownd session...
[lockdown] lockdownd_client_new_with_handshake() err=-8 — cần pair mới
[lockdown] Bắt đầu pairing flow...
[lockdown] ✅ lockdownd_client_new() OK (no-TLS)
[lockdown] ⏳ Chờ bấm 'Tin cậy' trên iPhone... (1/20)
[lockdown] ✅ Pair thành công!
[lockdown] Mở lại lockdownd với TLS sau pair...
[lockdown] ✅ lockdownd session OK (sau pair mới)
```

---

## ⚠️ LƯU Ý QUAN TRỌNG

1. **iPhone PHẢI được mở khoá** trước khi kết nối
2. **Bấm "Tin cậy"** trên iPhone khi popup xuất hiện
3. **Dùng cáp USB hỗ trợ data** (không phải cáp sạc-only)
4. **Giữ cáp USB ổn định** — không rút ra trong quá trình pair

---

*Patch version: v32*
*Date: 2026-07-19*
