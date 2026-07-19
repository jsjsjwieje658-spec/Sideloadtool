# SideloadTool Critical Fixes — Patch Set

## Tổng quan
Đây là bộ 5 patch fix các lỗi cốt lõi khiến SideloadTool không nhận diện được UDID 
và không giao tiếp được với iPhone qua USB.

## Các lỗi đã fix

### 1. UDID Placeholder Propagation (CRITICAL)
- **File:** `usb_fd_bridge.c`, `usbmuxd_server.c`, `jni_bridge_imd.c`
- **Mô tả:** usbmuxd_server_start() được gọi với UDID placeholder.
- **Fix:** Đọc UDID thật từ USB descriptor (iSerialNumber).

### 2. USBMUXD_SOCKET_ADDRESS Overwrite (CRITICAL)
- **File:** `jni_bridge_imd.c`
- **Mô tả:** nativeConnect() overwrite giá trị TCP đúng bằng Unix path sai.
- **Fix:** Xóa setenv() trong nativeConnect().

### 3. Socket Path Parsing (CRITICAL)
- **File:** `android_usbmuxd_fix.c`
- **Mô tả:** Không xử lý absolute path không có "unix:" prefix.
- **Fix:** Thêm điều kiện `addr_env[0] == '/'`.

### 4. lockdownd err=-8 (INVALID_SERVICE)
- **File:** `jni_bridge_imd.c`
- **Mô tả:** Tunnel hỏng do lỗi 2 & 3 → handshake thất bại.
- **Fix:** Cải thiện error message.

### 5. UDID Update không broadcast (HIGH)
- **File:** `usbmuxd_server.c`, `jni_bridge_imd.c`
- **Mô tả:** UDID placeholder không bao giờ được cập nhật thành thật.
- **Fix:** Đọc UDID thật từ USB descriptor từ đầu.

## Hướng dẫn apply

```bash
cd /path/to/Sideloadtool-main
git apply patches/0001-usb_fd_bridge-Add-get_udid-declaration.patch
git apply patches/0002-usb_fd_bridge-Read-UDID-from-USB-descriptor.patch
git apply patches/0003-jni_bridge-Fix-socket-path-and-UDID-handling.patch
git apply patches/0004-android_usbmuxd_fix-Fix-socket-path-parsing.patch
git apply patches/0005-usbmuxd_server-Fix-UDID-propagation.patch
