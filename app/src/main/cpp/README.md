# SideloadTool No-Root Fix v35

## Thay đổi chính

### 1. Bỏ hoàn toàn lockdownd_client_new_with_handshake()
- Dùng thẳng `lockdownd_client_new()` + `lockdownd_pair()`
- Không cần TLS certificate (không cần root)
- Sideload chỉ cần kết nối no-TLS để cài app

### 2. Fix usbmuxd_server.c
- Packet reassembly cho USB fragmented packets
- Connection state machine
- Drain non-TCP packets trước SYN
- TCP header chuẩn (struct tcphdr)
- Proper tunnel với ACK handling

### 3. Fix usb_fd_bridge.c
- Không retry claim khi BUSY (Android đã claim)
- OVERFLOW handling
- Flush helpers

## Build
```bash
cp *.c *.h app/src/main/cpp/
./gradlew assembleDebug
```

## Log kỳ vọng
```
[imd] ✅ iPhone UDID: ...
[lockdown] Kết nối lockdownd (no-TLS)...
[lockdown] ✅ lockdownd_client_new OK (no-TLS)
[lockdown] Bắt đầu pair...
[lockdown] ⏳ Chờ bấm 'Tin cậy'... (1/20)
[lockdown] ✅ Pair thành công!
[lockdown] ✅ Kết nối OK (no-TLS, đã pair)
```

## Lưu ý
- iPhone phải mở khoá
- Bấm "Tin cậy" khi popup xuất hiện
- Dùng cáp USB hỗ trợ data
