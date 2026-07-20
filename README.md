# SideloadTool Complete Fix v34

## Các lỗi đã fix

### 1. usbmuxd_server.c — Rewrite hoàn toàn
- **Packet reassembly**: Handle USB fragmented packets
- **Connection state machine**: CONNECTING → CONNECTED → CLOSING → CLOSED
- **Device state machine**: INIT → ACTIVE → DEAD
- **Drain non-TCP packets**: Trước khi gửi SYN
- **TCP header chuẩn**: Dùng `struct tcphdr` từ `<netinet/tcp.h>`
- **Window management**: Dynamic rx_win/tx_win
- **Proper error handling**: Mọi lỗi path đều được xử lý

### 2. jni_bridge_imd.c — Fix lockdown err=-8
- Check đúng `LOCKDOWN_E_MUX_ERROR` (-8) và `LOCKDOWN_E_INVALID_HOST_ID` (-21)
- Tự động pair khi cần
- Retry logic cho idevice_new_with_options

### 3. usb_fd_bridge.c — Fix USB communication
- Không retry claim khi BUSY (Android đã claim)
- OVERFLOW handling với drain + retry
- Flush helpers

## Build
```bash
cp *.c *.h app/src/main/cpp/
./gradlew assembleDebug
```

## Log kỳ vọng
```
[imd] ✅ iPhone UDID: ...
[lockdown] err=-8 (MUX_ERROR) — thử pair mới
[lockdown] Bắt đầu pairing flow...
[lockdown] ✅ lockdownd_client_new OK (no-TLS)
[lockdown] ⏳ Chờ bấm 'Tin cậy'... (1/20)
[lockdown] ✅ Pair thành công!
[lockdown] ✅ lockdownd OK (sau pair mới)
```
