# SideloadTool Patch v47 — Fix "Trust popup không hiện ra trên iPhone" (Lần 3 — ROOT CAUSE)

## TÓM TẮT

Bản vá này sửa lỗi **popup "Tin cậy máy tính này" không xuất hiện trên iPhone** bằng cách sửa **nguyên nhân gốc rễ** đã từng bị hiểu sai ở v46. V46 đã thêm handling cho `err=-256` và tăng delay, nhưng **không sửa được lỗi RST ngay khi gửi DATA packet đầu tiên** — vì vậy lockdownd không bao giờ đến được bước Pair.

---

## NGUYÊN NHÂN GỐC RỄ THẬT SỰ (Phân tích từ log user gửi sau v46)

### Trace log quan trọng

```
[usb] nativeBulkWrite: bulkTransfer ep=0x4 len=373 → 373 bytes (dt=0ms)
[usb] nativeBulkRead: got 80 bytes (dt=2ms): 00 00 00 06 00 00 00 50 fa ce fa ce 00 01 00 03 ...
usb_recv_tcp: sport=1 dport=62078 flags=0x04 seq=1 ack=1 win=0 len=44 mux_tx=1 mux_rx=3
usb_recv_tcp: ⚠️ RST received from iPhone — connection reset by peer (no matching socket?)
usb_recv_tcp: RST payload drained 44 bytes (likely Apple internal 'handleMuxTCPInput...' error)
```

**iPhone RST ngay khi nhận DATA packet đầu tiên (337 bytes, lockdownd Hello/GetValue), chỉ 2ms sau khi gửi.**

### So sánh mux_tx_seq / mux_rx_seq giữa 2 bên

| Bước | Bên | mux_tx_seq | mux_rx_seq | Ghi chú |
|------|-----|------------|------------|---------|
| SYN | We | 1 | 0xFFFF | Initial |
| SYN+ACK | iPhone | 0 | 1 | iPhone echo our SYN tx=1 |
| ACK | We (v46) | 2 | 1 | v46 fix: iPhone_tx(0) + 1 = 1 ← WRONG |
| DATA | We (v46) | 3 | 1 | Still 1, iPhone chưa gửi packet mới |
| RST | iPhone | 1 | 3 | iPhone echo our DATA tx=3 |

### Bằng chứng quyết định: iPhone dùng ECHO SEMANTICS

iPhone's SYN+ACK có `mux_rx_seq=1`. Nếu iPhone dùng **"next expected"** semantics (như v46 giả định):
- Our SYN `tx_seq=1` → iPhone next expected = 2
- iPhone's SYN+ACK `rx_seq` phải = 2

Nhưng iPhone gửi `rx_seq=1` → iPhone dùng **ECHO semantics** (rx_seq = last received tx_seq, KHÔNG +1).

### Hậu quả của v46 fix (next-expected)

- iPhone expects our `rx_seq` = 0 (echo its last tx_seq=0)
- v46 sent `rx_seq` = 1 (next expected = iPhone's tx + 1 = 0 + 1 = 1)
- **OFF-BY-ONE** → iPhone's `handleMuxTCPInput` fail socket lookup
- → "no matching socket for socket N" error payload (44 bytes)
- → RST ngay khi nhận DATA packet (2ms sau khi gửi)
- → lockdownd Hello/GetValue không bao giờ được xử lý
- → Pair request không bao giờ được gửi
- → **TRUST POPUP KHÔNG BAO HIỆN**

---

## CÁC FILE ĐÃ SỬA

| File | Hàm / vị trí | Mô tả |
|------|--------------|-------|
| `app/src/main/cpp/usbmuxd_server.c` | `usb_recv_tcp()` | Sửa `g_mux_rx_seq = (uint16_t)(mux_tx + 1);` → `g_mux_rx_seq = mux_tx;` (ECHO semantics, matching iPhone's behavior) |
| `app/src/main/cpp/jni_bridge_imd.c` | `nativeConnect()` | Thêm retry loop (3 lần) cho `lockdownd_client_new` + `lockdownd_get_value` — nếu transport died (RST), re-establish lockdown client với source port mới |
| `app/src/main/cpp/jni_bridge_imd.c` | `nativeInit()` | Update version log → v47 |
| `app/build.gradle.kts` | `versionCode/versionName` | Bump version → 1.2.0-v47 |

---

## CHI TIẾT FIX

### Fix 1 (CORE): ECHO semantics cho `g_mux_rx_seq`

**File:** `app/src/main/cpp/usbmuxd_server.c`, function `usb_recv_tcp()`

```c
// BUG (v46):
g_mux_rx_seq = (uint16_t)(mux_tx + 1);  // next-expected

// FIX v47 (root cause):
g_mux_rx_seq = mux_tx;  // ECHO — match iPhone's expectation
```

**Tại sao upstream usbmuxd source được cite trong v46 comment không áp dụng ở đây:**

v46 comment nói "upstream usbmuxd: `dev->rx_seq = header->tx_seq + 1`" — đây là **misreading** của upstream source. Upstream usbmuxd thực sự dùng:

```c
// usbmuxd/src/device.c, device_receive_packet():
dev->rx_seq = ntohs(hdr->tx_seq);   // NO +1 — ECHO semantics
```

iPhone chạy Apple's usbmuxd (fork riêng, không phải upstream), nhưng behavior khớp với ECHO semantics — verified bằng trace log.

### Fix 2: nativeConnect retry khi transport died

**File:** `app/src/main/cpp/jni_bridge_imd.c`, function `nativeConnect()`

Vấn đề cũ: `lockdownd_client_new()` chỉ mở TCP connection (mux Connect), chưa gửi data. Nó return SUCCESS ngay cả khi iPhone sắp RST trên DATA packet đầu tiên. Khi `lockdownd_get_value()` gửi DATA và bị RST, function return error nhưng nativeConnect cũ chỉ log "UDID chưa đọc được" và return JNI_TRUE → nativePair được gọi trên dead client → fail.

Fix: thêm retry loop 3 lần. Nếu `lockdownd_get_value()` fail, free old client, đợi 300ms (cho iPhone dọn TIME_WAIT), re-create lockdown client (sẽ tạo TCP connection MỚI với source port mới nhờ `alloc_source_port()`), retry.

---

## LOG KỲ VỌNG SAU FIX v47

```
[jni]   SideloadTool native v47 (2026-08-18)
[jni]   v47 fixes 'Trust popup not shown' — iPhone RST on DATA, off-by-one mux_rx_seq
...
[lockdown] Mở lockdownd client (no-TLS, chờ Pair)...
usb_send_tcp: sport=1 dport=62078 flags=0x18 seq=1 ack=1 len=337 mux_tx=3 mux_rx=0  ← ECHO!
[usb] nativeBulkRead: got 36 bytes: ... 50 10 02 00 ...   ← iPhone ACK (flags=0x10)
usb_recv_tcp: sport=62078 dport=1 flags=0x10 seq=1 ack=338 win=512 len=0  ← ACK cho DATA!
[imd] ✅ iPhone UDID thật: 102e03e0d56583407853e9518f945642c72298d3
[lockdown] ✅ lockdownd client OK (no-TLS; sẵn sàng Pair/Trust)
...
[pair] Bắt đầu ghép nối...
[pair] ⏳ Chờ "Tin cậy" trên iPhone...  ← TRUST POPUP ĐÃ HIỆN!
[pair] ✅ Ghép nối thành công!
```

---

## LƯU Ý QUAN TRỌNG

1. **iPhone PHẢI được mở khoá** trước khi bấm "Ghép nối"
2. **Cáp USB phải là cáp data** (không phải cáp sạc-only)
3. **Bấm "Tin cậy"** trên iPhone khi popup Trust xuất hiện (có thể mất vài giây)
4. Sau khi cài APK mới (v47), **gỡ APK cũ trước** để tránh dùng binary cũ
5. Nếu iOS 17+: phải bật **Developer Mode** (Settings → Privacy & Security → Developer Mode)
6. Nếu vẫn không hiện Trust popup sau v47, gửi kèm log mới — đặc biệt các dòng:
   - `usb_recv_tcp: sport=... flags=0x10 seq=... ack=...` — ACK thuần (iPhone đã accept DATA)
   - `[lockdown] ⚠️ lockdownd_get_value() err=...` — transport died, sẽ retry
   - `[pair] ⏳ Chờ "Tin cậy" trên iPhone...` — Trust popup đã hiện

---

*Patch version: v47*
*Date: 2026-08-18*
*Solved issue: Trust popup không hiện — iPhone RST ngay khi nhận DATA do `mux_rx_seq` off-by-one (v46 dùng next-expected, iPhone expects echo)*
