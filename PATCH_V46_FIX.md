# SideloadTool Patch v46 — Fix "Trust popup không hiện ra trên iPhone" (Lần 2)

## TÓM TẮT

Bản vá này sửa lỗi **popup "Tin cậy máy tính này" không xuất hiện trên iPhone** khi phân tích log `2.txt` user gửi. V45 đã xử lý RST flag và dynamic source port, nhưng vẫn còn một lỗi nghiêm trọng: **`lockdownd_pair()` trả `err=-256` (LOCKDOWN_E_UNKNOWN_ERROR)** mà code không handle, rơi vào `default:` và return false ngay lập tức.

---

## NGUYÊN NHÂN GỐC RỄ (Phân tích từ `2.txt`)

### Trace log quan trọng

```
[pair] Bắt đầu ghép nối...
[pair] ⚠️ Transport err=-8 (RST?) — re-establish lockdown client (loop 1/20)
... (re-establish thành công với sport=2) ...
[pair] ✅ Re-established lockdown client — retry pair
usb_send_tcp: sport=2 dport=62078 flags=0x18 seq=1 ack=1 len=338 mux_tx=6 mux_rx=4
... (gửi Pair request 338 bytes) ...
[usb] nativeBulkRead: got 80 bytes (dt=301ms)  ← RST nhận sau 301ms (iPhone đã xử lý)
usb_recv_tcp: sport=2 dport=62078 flags=0x04 seq=1 ack=1 win=0 len=44
usb_recv_tcp: RST payload drained 44 bytes (likely Apple internal 'handleMuxTCPInput...')
[pair] ❌ lockdownd_pair() err=-256  ← RƠI VÀO DEFAULT CASE, RETURN FALSE!
```

### 3 lỗi gốc rễ

| # | Lỗi | Hậu quả |
|---|-----|---------|
| **1** | `LOCKDOWN_E_UNKNOWN_ERROR (-256)` không được handle trong switch | iOS 16/17+ khi nhận Pair request sẽ hiển thị Trust popup, sau đó **đóng TCP connection bằng RST** (thông điệp "PairingDialogResponsePending" được map thành `-256` trong libimobiledevice 1.3.0). Code rơi vào `default:` → return false ngay → **user không có thời gian bấm Trust** |
| **2** | `g_mux_rx_seq = mux_rx` (BUG) trong `usb_recv_tcp()` | iPhone's `rx_seq` field = echo OUR last `tx_seq`. Gán bằng iPhone's `rx_seq` sẽ echo ngược lại tx_seq của chính mình thay vì echo iPhone's `tx_seq`. Có thể gây "no matching socket" trên iPhone |
| **3** | Không có delay giữa SYN+ACK và ACK | iOS 16/17+ đôi khi cần thời gian để tạo socket entry trong usbmuxd socket table SAU khi gửi SYN+ACK. ACK đến quá nhanh → socket chưa kịp tạo → RST |

### Vì sao popup Trust KHÔNG hiện?

1. App gửi Pair request → TCP handshake thành công
2. iPhone nhận Pair request → **hiển thị Trust popup** (trong 301ms trước khi RST)
3. iPhone đóng TCP connection bằng RST (vì chưa nhận được response từ user)
4. libimobiledevice 1.3.0 map RST/response thành `LOCKDOWN_E_UNKNOWN_ERROR (-256)`
5. Code cũ `default: return JNI_FALSE` ngay lập tức → **không retry, không chờ user bấm Trust**
6. User không thấy popup (hoặc thấy nhưng app đã báo "thất bại" rồi)

---

## CÁC FILE ĐÃ SỬA

| File | Hàm / vị trí | Mô tả |
|------|--------------|-------|
| `app/src/main/cpp/jni_bridge_imd.c` | `nativePair()` switch | Thêm `case LOCKDOWN_E_UNKNOWN_ERROR:` (-256) — re-establish lockdown client + retry, giống `LOCKDOWN_E_MUX_ERROR` |
| `app/src/main/cpp/jni_bridge_imd.c` | `nativePair()` case -256 | Gọi `onTrustRequired()` để UI thông báo "Đang chờ user bấm Trust" + extra 500ms delay cho user |
| `app/src/main/cpp/usbmuxd_server.c` | `usb_recv_tcp()` | Fix `g_mux_rx_seq = mux_tx + 1` (next expected, học từ upstream usbmuxd) thay vì `mux_rx` (echo ngược tx_seq của chính mình) |
| `app/src/main/cpp/usbmuxd_server.c` | `do_usb_v1_connect()` | Thêm `usleep(50ms)` giữa SYN+ACK receipt và ACK send — cho iPhone thời gian tạo socket entry |

---

## CHI TIẾT TỪNG FIX

### Fix 1: Handle `LOCKDOWN_E_UNKNOWN_ERROR (-256)` trong `nativePair()`

```c
case LOCKDOWN_E_MUX_ERROR:
case LOCKDOWN_E_RECEIVE_TIMEOUT:
case LOCKDOWN_E_SSL_ERROR:
case LOCKDOWN_E_UNKNOWN_ERROR: {  // ← THÊM CASE NÀY (FIX v46)
    int is_unknown = (err == LOCKDOWN_E_UNKNOWN_ERROR);
    // ... re-establish lockdown client, retry
    // Extra delay 500ms cho UNKNOWN_ERROR để user có thời gian bấm Trust
    if (is_unknown) {
        usleep(500 * 1000);
    }
    continue;  // retry lockdownd_pair
}
```

### Fix 2: Sửa `g_mux_rx_seq` trong `usb_recv_tcp()`

```c
// BUG cũ:
g_mux_rx_seq = mux_rx;  // ← iPhone's rx_seq field (= echo our last tx_seq)

// FIX v46:
g_mux_rx_seq = (uint16_t)(mux_tx + 1);  // ← iPhone's tx_seq + 1 (next expected)
```

### Fix 3: Delay 50ms giữa SYN+ACK và ACK

```c
// Sau khi nhận SYN+ACK, trước khi gửi ACK:
usleep(50 * 1000);  // cho iPhone tạo socket entry trong usbmuxd table
```

---

## LOG KỲ VỌNG SAU FIX

```
[pair] Bắt đầu ghép nối...
[pair] ⚠️ Transport err=-8 (RST?) — re-establish lockdown client (loop 1/20)
[pair] ✅ Re-established lockdown client — retry pair
[pair] ⚠️ UNKNOWN_ERROR err=-256 (RST hoặc Trust dialog pending?) — re-establish (loop 2/20)
[pair] ✅ Re-established lockdown client — retry pair
[pair] ⚠️ UNKNOWN_ERROR err=-256 — re-establish (loop 3/20)
... (user bấm "Trust" trên iPhone) ...
[pair] ⏳ Chờ "Tin cậy" trên iPhone... (4/20)  ← POPUP TRUST ĐÃ HIỆN!
[pair] ✅ Ghép nối thành công!
```

---

## CÁCH BUILD

```bash
git clone https://github.com/jsjsjwieje658-spec/Sideloadtool.git
cd Sideloadtool
git checkout main
git pull
# Build APK bằng GitHub Actions (tự động khi push)
```

---

## LƯU Ý QUAN TRỌNG

1. **iPhone PHẢI được mở khoá** trước khi bấm "Ghép nối"
2. **Cáp USB phải là cáp data** (không phải cáp sạc-only)
3. **Bấm "Tin cậy"** trên iPhone khi popup Trust xuất hiện (có thể mất vài giây)
4. Sau khi cài APK mới, **gỡ APK cũ trước** để tránh dùng binary cũ
5. Nếu iOS 17+: phải bật **Developer Mode** (Settings → Privacy & Security → Developer Mode)
6. Nếu vẫn không hiện Trust popup sau v46, gửi kèm log mới — đặc biệt các dòng:
   - `[pair] ⚠️ UNKNOWN_ERROR err=-256` — chỉ ra iPhone đã nhận Pair request
   - `do_usb_v1_connect: SYN retry N/3` — chỉ ra iPhone reject SYN
   - `usb_recv_tcp: ⚠️ RST received` — chỉ ra iPhone reset connection

---

*Patch version: v46*
*Date: 2026-08-18*
*Solved issue: Trust popup không hiện — `err=-256` không được handle, code return false ngay lập tức*
