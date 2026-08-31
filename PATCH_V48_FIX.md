# SideloadTool Patch v48 — Comprehensive protocol fix

## TÓM TẮT

Bản vá v48 sửa **6 lỗi protocol-level** còn lại trong mini-usbmuxd server,
từ v47 (đã fix rx_seq echo semantics) nhưng vẫn chưa hoạt động ổn định.

---

## NGUYÊN NHÂN GỐC RẼ (Từ phân tích v20-v47)

### Tại sao termux-usbmuxd hoạt động mà SideloadTool không?

| | termux-usbmuxd | SideloadTool |
|---|---|---|
| USB mux daemon | upstream usbmuxd binary (battle-tested) | Tự viết 1700 dòng |
| Version exchange | upstream code | Tự viết (sửa 3 lần rx_seq) |
| TCP-like handshake | upstream code | Tự viết (sửa 4 lần) |
| **Kết quả** | **100% hoạt động** | **27+ patches, vẫn bug** |

v48 fixes **6 lỗi cụ thể** trong custom protocol:

---

## 6 LỖI ĐÃ SỬA

### Lỗi #1 (CRITICAL): Eager version exchange gây race condition

**Vấn đề:**
```
nativeSetUsbFd():
  1. usb_bridge_init_from_fd2()     → libusb init
  2. usbmux_version_exchange()       → EAGER: VERSION + SETUP gửi ngay
  3. usbmuxd_server_start()          → Server threads start
  4. ...time passes...
  5. nativeConnect() → idevice_new() → Connect request → do_usb_v1_connect()
  6. usbmux_version_exchange() → returns true (g_version_done=1) → KHÔNG retry
  7. iPhone đã "forget" mux session từ step 2 → SYN bị reject
```

**Fix:** XÓA hoàn toàn eager version exchange. Version exchange CHỈ xảy ra
trong `do_usb_v1_connect()` khi client Connect lần đầu — đúng thời điểm
iPhone mux daemon đang ở trạng thái "accepting".

**File:** `jni_bridge_imd.c` — `nativeSetUsbFd()`

### Lỗi #2 (CRITICAL): Thread safety — g_mux_tx_seq và local_seq race

**Vấn đề:**
```
thread_sock_to_usb:        thread_usb_to_sock:
  read g_mux_tx_seq          read g_mux_tx_seq
  write to packet            write to packet
  increment g_mux_tx_seq     increment g_mux_tx_seq
  (KHÔNG protect local_seq)
```

Hai thread cùng increment `g_mux_tx_seq` mà KHÔNG được serialize đầy đủ.
`st->local_seq` cũng bị race khi hai thread cùng đọc/ghi.

**Fix:** Serialize mux_tx_seq, mux_rx_seq, và local_seq cùng một lúc
dưới `st->usb_tx_lock` mutex trong `usb_send_tcp()`.

**File:** `usbmuxd_server.c` — `usb_send_tcp()`

### Lỗi #3 (HIGH): Source port TIME_WAIT conflict

**Vấn đề:**
```
Connection 1: sport=1, dport=62078 → RST → iPhone giữ TIME_WAIT
Connection 2: sport=1 (reuse!) → iPhone reject → RST → loop
```

v45 đã fix dynamic source port, nhưng KHÔNG track TIME_WAIT.
Port 1 có thể bị reuse quá nhanh trước khi iPhone dọn socket entry.

**Fix:** Theo dõi thời gian sử dụng của mỗi source port. Connection mới
skip các port đã dùng trong `TIME_WAIT_PERIOD` (2 giây).

**File:** `usbmuxd_server.c` — `alloc_source_port()`

### Lỗi #4 (HIGH): Lockdown retry không reset version state

**Vấn đề:**
```
nativePair():
  err = MUX_ERROR → RST
  → usbmuxd_server_reset_version_state() KHÔNG được gọi
  → lockdownd_client_new() → Connect → do_usb_v1_connect()
  → usbmux_version_exchange() returns true (g_version_done=1)
  → iPhone đã reset mux session → SYN fail
```

**Fix:** Gọi `usbmuxd_server_reset_version_state()` + `usb_bridge_clear_endpoints_halt()`
trước khi re-create lockdown client. Version exchange sẽ được thực hiện lại.

**File:** `jni_bridge_imd.c` — `nativePair()`

### Lỗi #5 (MEDIUM): Version exchange timeout quá ngắn

**Vấn đề:**
```
usb_recv_version(): 1500ms × 5 attempts = 7.5s total
iPhone iOS 17+ cần 2-3s để process VERSION request
→ Timeout quá ngắn → version exchange fail thường xuyên
```

**Fix:**
- Tăng timeout: 1500ms → 2000ms
- Tăng attempts: 5 → 8
- Tổng thời gian: 7.5s → 16s

**File:** `usbmuxd_server.c` — `usb_recv_version()`, `usbmux_version_exchange()`

### Lỗi #6 (MEDIUM): nativeConnect lockdown retry不足

**Vấn đề:**
```
ld_attempts = 3, delay = 300ms
iPhone iOS 17+ cần 500ms+ để dọn TIME_WAIT
→ Retry quá nhanh → fail
```

**Fix:**
- Tăng ld_attempts: 3 → 5
- Tăng delay: 300ms → 500ms
- Thêm clear_halt trước mỗi retry

**File:** `jni_bridge_imd.c` — `nativeConnect()`

---

## CÁC FILE ĐÃ SỬA

| File | Vị trí | Mô tả |
|------|--------|-------|
| `usbmuxd_server.c` | `alloc_source_port()` | TIME_WAIT tracking — skip port < 2s |
| `usbmuxd_server.c` | `usbmux_version_exchange()` | Tăng timeout 1500ms→2000ms, attempts 5→8 |
| `usbmuxd_server.c` | `usb_recv_version()` | Tăng timeout 1500ms→2000ms, attempts 5→8 |
| `usbmuxd_server.c` | `usb_send_tcp()` | Serialize mux seq + local_seq dưới usb_tx_lock |
| `usbmuxd_server.c` | `thread_sock_to_usb()` | Bỏ local_seq advance (đã làm trong usb_send_tcp) |
| `usbmuxd_server.c` | `usb_recv_tcp()` | Comments rõ hơn về remote_seq update |
| `usbmuxd_server.c` | `do_usb_v1_connect()` | Tăng SYN retry 3→5, delay 300ms→500ms, clear_halt |
| `usbmuxd_server.c` | `do_usb_v1_connect()` | Delay SYN+ACK→ACK 50ms→100ms |
| `usbmuxd_server.c` | `handle_client()` | Thêm FIN+ACK read sau FIN send, delay 200ms |
| `usbmuxd_server.c` | `usbmuxd_server_reset_version_state()` | Reset sport tracker + used_count |
| `jni_bridge_imd.c` | `nativeSetUsbFd()` | XÓA hoàn toàn eager version exchange |
| `jni_bridge_imd.c` | `nativeConnect()` | Tăng ld_attempts 3→5, delay 300ms→500ms |
| `jni_bridge_imd.c` | `nativePair()` | Thêm version state reset + clear_halt trước re-establish |
| `jni_bridge_imd.c` | `nativePair()` | Re-establish fail → retry trong loop (không return false) |
| `usb_fd_bridge.c` | `usb_bridge_init_from_fd2()` | Claim fallback logging rõ hơn |
| `usb_fd_bridge.c` | `usb_bridge_init_from_fd2()` | Clear halt delay 80ms→120ms cho iOS 17+ |

---

## LOG KỲ VỌNG SAU FIX

### Flow bình thường:
```
[jni]   SideloadTool native v48
[usb]   libusb_wrap_sys_device OK: fd=X
[usb]   libusb_claim_interface(X) NOT_FOUND — JNI fallback sẽ activate
[usbmux] v48: KHÔNG eager version exchange — để do_usb_v1_connect() xử lý
[usbmuxd_srv] ✅ Server listening: /data/.../usbmuxd.sock
[imd]   idevice OK
[lockdown] Mở lockdownd client...
do_usb_v1_connect: port=62078
usbmux_version_exchange: clear_halt + flush...
usb_send_version: 20 bytes
usb_recv_version: protocol=0 length=20 → version 2.0
usb_send_setup: 17 bytes → SETUP v2
usbmux_version_exchange: mux v2 + SETUP OK (attempt 1/8)
do_usb_v1_connect: SYN gửi (sport=1 dport=62078)
usb_recv_tcp: SYN+ACK received
do_usb_v1_connect: ✅ kết nối TCP OK
[lockdown] ✅ lockdownd client OK
[pair] Bắt đầu ghép nối...
[pair] ⏳ Chờ "Tin cậy" trên iPhone...
[pair] ✅ Ghép nối thành công!
```

### Flow với RST + retry:
```
do_usb_v1_connect: SYN retry 2/5 với sport mới
usbmuxd_server_reset_version_state: reset + clear_halt
do_usb_v1_connect: sport=2
usbmux_version_exchange: mux v2 + SETUP OK (attempt 1/8)
do_usb_v1_connect: SYN+ACK OK
do_usb_v1_connect: ✅ kết nối TCP OK
```

### Flow nativePair re-establish:
```
[pair] ⚠️ UNKNOWN_ERROR err=-256 — re-establish (loop 2/20)
usbmuxd_server_reset_version_state: reset
usb_bridge_clear_endpoints_halt
[pair] ✅ Re-established lockdown client
[pair] ⏳ Chờ "Tin cậy"... (3/20)
[pair] ✅ Ghép nối thành công!
```

---

## CÁCH BUILD

```bash
git clone https://github.com/jsjsjwieje658-spec/Sideloadtool.git
cd Sideloadtool
git checkout main
git pull
# Build APK
./gradlew assembleDebug
# Hoặc: push lên GitHub để Actions build tự động
```

---

## LƯU Ý QUAN TRỌNG

1. **iPhone PHẢI được mở khoá** trước khi bấm "Ghép nối"
2. **Cáp USB phải là cáp data** (không phải cáp sạc-only)
3. **Bấm "Tin cậy"** trên iPhone khi popup Trust xuất hiện
4. Sau khi cài APK mới, **gỡ APK cũ trước** để tránh dùng binary cũ
5. Nếu iOS 17+: phải bật **Developer Mode** (Settings → Privacy & Security → Developer Mode)
6. **Phiên Android**: Đảm bảo app có quyền USB Host (USB_ACCESSORY permission)
7. Nếu vẫn không hoạt động, gửi log với các dòng:
   - `usbmux_version_exchange: mux v2 + SETUP OK` — version exchange thành công
   - `do_usb_v1_connect: ✅ kết nối TCP OK` — SYN handshake thành công
   - `[pair] ⏳ Chờ "Tin cậy"` — Trust popup đã hiện
   - `usb_recv_tcp: ⚠️ RST received` — iPhone RST (cần debug thêm)

---

*Patch version: v48*
*Date: 2026-08-21*
*Fixes: 6 protocol-level bugs — eager version race, thread safety, TIME_WAIT, retry improvements*
