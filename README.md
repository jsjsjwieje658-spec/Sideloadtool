# SideloadTool Critical Fix v33

## Các lỗi đã fix

### Fix 1: do_usb_v1_connect() - Drain non-TCP packets trước SYN
**File:** `usbmuxd_server.c`

iPhone gửi CONTROL packets (type 7 = info) sau SETUP. Nếu không drain,
usb_recv_tcp() đọc CONTROL thay vì SYN+ACK → kết nối thất bại.

### Fix 2: jni_bridge_imd.c - Check đúng LOCKDOWN_E_MUX_ERROR (-8)
**File:** `jni_bridge_imd.c`

err=-8 là LOCKDOWN_E_MUX_ERROR (kết nối usbmuxd thất bại), không phải
INVALID_HOST_ID (-21). Thêm check MUX_ERROR vào nhánh pair.

## Build
```bash
cp *.c *.h app/src/main/cpp/
./gradlew assembleDebug
```

## Log kỳ vọng
```
[imd] ✅ iPhone UDID: ...
[lockdown] lockdownd_client_new_with_handshake() err=-8 (MUX_ERROR) — thử pair mới
[lockdown] Bắt đầu pairing flow...
[lockdown] ✅ lockdownd_client_new() OK (no-TLS)
[lockdown] ⏳ Chờ bấm 'Tin cậy' trên iPhone... (1/20)
[lockdown] ✅ Pair thành công!
[lockdown] ✅ lockdownd session OK (sau pair mới)
```
