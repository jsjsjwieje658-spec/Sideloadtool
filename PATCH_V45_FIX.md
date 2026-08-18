# SideloadTool Patch v45 — Fix "Trust popup không hiện ra trên iPhone"

## TÓM TẮT

Bản vá này sửa lỗi **popup "Tin cậy máy tính này" không bao giờ xuất hiện trên iPhone** khi kết nối qua app SideloadTool.

---

## NGUYÊN NHÂN GỐC RỄ

Phân tích trace USB (file `1.txt` user gửi) cho thấy một **loop vô hạn ACK↔RST** giữa Android và iPhone:

```
Android TX:  ACK  (sport=1, dport=62078, seq=338, ack=51535, len=0)
iPhone  RX:  RST  (sport=62078, dport=1, seq=51535, ack=338, len=38)
                  payload = "\x01 handleMuxTCPInput no matching socket for socket N"
```

### 3 lỗi gốc rễ

| # | Lỗi | Hậu quả |
|---|-----|---------|
| **1** | `usb_recv_tcp()` **không nhận diện RST flag (0x04)** từ iPhone | Payload của RST (error message dạng `\x01 handleMuxTCPInput no matching socket...`) bị xử lý như TCP data thật, viết thẳng vào socket libimobiledevice → **làm hỏng lockdown protocol** |
| **2** | `thread_usb_to_sock()` gửi ACK lại ngay sau khi nhận RST | iPhone nhận ACK trên connection không tồn tại → lại gửi RST khác → **loop vô hạn ACK↔RST** |
| **3** | `do_usb_v1_connect()` **hardcode `st->sport = 1`** cho mọi connection | Connection mới tái sử dụng source port 1, xung đột với TIME_WAIT state trên iPhone usbmuxd → iPhone reject bằng RST "no matching socket" |

### Vì sao popup Trust không hiện?

1. App SideloadTool mở TCP connection tới lockdownd (port 62078) qua USB mux
2. TCP connection bị RST (vì sport conflict hoặc connection cũ chưa dọn)
3. Code cũ **không xử lý RST** → viết error message vào socket libimobiledevice
4. libimobiledevice parse error → `lockdownd_pair()` trả `LOCKDOWN_E_MUX_ERROR (-8)`
5. Code cũ `default: return JNI_FALSE` ngay lập tức → **Pair request chưa kịp gửi**
6. iPhone chưa nhận Pair request → **không hiện popup Trust**

---

## CÁC FILE ĐÃ SỬA

| File | Hàm / vị trí | Mô tả |
|------|--------------|------|
| `app/src/main/cpp/usbmuxd_server.c` | `tcp_state_t` struct | Thêm field `rst_received` để track connection đã bị RST |
| `app/src/main/cpp/usbmuxd_server.c` | `g_next_sport` + `alloc_source_port()` | Dynamic source port allocator (port 1, 2, 3, ... wraparound tại 0xFFFF→1) |
| `app/src/main/cpp/usbmuxd_server.c` | `usb_recv_tcp()` | Detect RST flag, drain error payload, set `rst_received=1`, return `-2` (special code) |
| `app/src/main/cpp/usbmuxd_server.c` | `thread_usb_to_sock()` | Handle return `-2` (RST): **không gửi ACK** (ngắt loop), **shutdown socket** để thread_sock_to_usb cũng exit |
| `app/src/main/cpp/usbmuxd_server.c` | `do_usb_v1_connect()` | Dùng `alloc_source_port()` thay vì hardcode 1; retry SYN handshake tối đa 3 lần khi iPhone gửi RST thay vì SYN+ACK |
| `app/src/main/cpp/usbmuxd_server.c` | `handle_client()` | Skip FIN khi `rst_received=1` (gửi FIN trên dead connection chỉ tạo thêm RST) |
| `app/src/main/cpp/usbmuxd_server.c` | `usbmuxd_server_reset_version_state()` | Reset source port allocator về 1 cho USB session mới |
| `app/src/main/cpp/jni_bridge_imd.c` | `nativePair()` switch | Thêm case `LOCKDOWN_E_MUX_ERROR`/`RECEIVE_TIMEOUT`/`SSL_ERROR`: re-establish lockdown client (TCP connection mới với source port mới), retry `lockdownd_pair()` thay vì return false |

---

## CHI TIẾT TỪNG FIX

### Fix 1: RST detection trong `usb_recv_tcp`

```c
if (thdr.flags & TH_RST) {
    LOGE("usb_recv_tcp: ⚠️ RST received from iPhone — connection reset by peer");
    st->rst_received = 1;
    /* Drain payload (error message) để giữ USB stream aligned */
    if (data_len_raw > 0) {
        uint8_t drain_buf[256];
        uint32_t to_drain = data_len_raw;
        while (to_drain > 0) {
            int chunk = (int)(to_drain < sizeof(drain_buf) ? to_drain : sizeof(drain_buf));
            int got = usb_read_exact(drain_buf, chunk, 1500);
            if (got < chunk) break;
            to_drain -= (uint32_t)got;
        }
    }
    return -2;  /* Special: RST received */
}
```

### Fix 2: Thread_usb_to_sock ngắt loop khi nhận RST

```c
if (n == -2) {
    LOGE("usb_to_sock: ⚠️ iPhone đã gửi RST — Ngắt tunnel NGAY");
    shutdown(sock, SHUT_RDWR);  /* Force thread_sock_to_usb exit */
    break;                       /* Không gửi ACK → ngắt ACK/RST loop */
}
```

### Fix 3: Dynamic source port

```c
static volatile uint16_t g_next_sport = 1;
static pthread_mutex_t   g_sport_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint16_t alloc_source_port(void) {
    pthread_mutex_lock(&g_sport_mutex);
    uint16_t p = g_next_sport++;
    if (g_next_sport == 0) g_next_sport = 1;  /* wraparound, skip 0 */
    pthread_mutex_unlock(&g_sport_mutex);
    return p;
}

/* Trong do_usb_v1_connect: */
st->sport = alloc_source_port();  /* thay vì hardcode 1 */
```

### Fix 4: SYN handshake retry trên RST

```c
for (int syn_attempt = 0; syn_attempt < 3; syn_attempt++) {
    if (syn_attempt > 0) {
        usleep(300 * 1000);
        st->sport = alloc_source_port();  /* sport mới */
        /* ... */
    }
    /* Gửi SYN, nhận SYN+ACK */
    int n = usb_recv_tcp(st, dummy, sizeof(dummy), &flags, 5000);
    if (n == -2) {
        LOGE("iPhone gửi RST thay vì SYN+ACK — retry với sport mới");
        continue;
    }
    /* ... */
}
```

### Fix 5: nativePair retry trên transport error

```c
case LOCKDOWN_E_MUX_ERROR:
case LOCKDOWN_E_RECEIVE_TIMEOUT:
case LOCKDOWN_E_SSL_ERROR: {
    /* Free old lockdown client (dead transport) */
    if (g_lockdown) { lockdownd_client_free(g_lockdown); g_lockdown = NULL; }
    /* Re-create → triggers new TCP connection với source port mới */
    lockdownd_client_new(g_device, &g_lockdown, "sideloadtool");
    usleep(300 * 1000);
    continue;  /* retry lockdownd_pair */
}
```

---

## LOG KỲ VỌNG SAU FIX

```
[usbmux] ✅ Eager version exchange OK
[usbmuxd_srv] ✅ Server listening
[imd] ✅ idevice OK
[imd] USBMUXD_SOCKET_ADDRESS=/data/.../usbmuxd.sock
[lockdown] Mở lockdownd client (no-TLS, chờ Pair)...
do_usb_v1_connect: port=62078
do_usb_v1_connect: allocated sport=1 for dport=62078
usb_send_tcp: SYN gửi (sport=1 dport=62078 seq=0)
usb_recv_tcp: sport=62078 dport=1 flags=0x12 seq=0 ack=1  ← SYN+ACK
do_usb_v1_connect: ✅ kết nối TCP port=62078 OK (sport=1)
[lockdown] ✅ lockdownd client OK
[pair] Bắt đầu ghép nối...
[pair] ⏳ Chờ "Tin cậy" trên iPhone... (1/20)  ← POPUP TRUST HIỆN
[pair] ✅ Ghép nối thành công!
```

Nếu iPhone gửi RST (vd. connection conflict):

```
do_usb_v1_connect: SYN retry 2/3 với sport mới
do_usb_v1_connect: new sport=2
usb_send_tcp: SYN gửi (sport=2 dport=62078 seq=0)
usb_recv_tcp: sport=62078 dport=2 flags=0x12 seq=0 ack=1  ← SYN+ACK OK
do_usb_v1_connect: ✅ kết nối TCP port=62078 OK (sport=2)
```

Nếu tunnel đang chạy mà iPhone gửi RST:

```
usb_recv_tcp: ⚠️ RST received from iPhone — connection reset by peer
usb_recv_tcp: RST payload drained 38 bytes (likely 'handleMuxTCPInput...')
usb_to_sock: ⚠️ iPhone đã gửi RST — Ngắt tunnel NGAY
client fd=N: skip FIN — iPhone đã RST, connection đã chết
[pair] ⚠️ Transport err=-8 (RST?) — re-establish lockdown client (loop 3/20)
[pair] ✅ Re-established lockdown client — retry pair
[pair] ⏳ Chờ "Tin cậy" trên iPhone... (4/20)
```

---

## CÁCH BUILD

```bash
git clone https://github.com/jsjsjwieje658-spec/Sideloadtool.git
cd Sideloadtool
git checkout main
git pull
# Build APK bằng GitHub Actions (tự động khi push) hoặc local:
./gradlew assembleDebug
```

---

## LƯU Ý QUAN TRỌNG

1. **iPhone PHẢI được mở khoá** trước khi bấm "Ghép nối"
2. **Cáp USB phải là cáp data** (không phải cáp sạc-only)
3. **Bấm "Tin cậy"** trên iPhone khi popup Trust xuất hiện
4. Sau khi cài APK mới, **gỡ APK cũ trước** để tránh dùng binary cũ
5. Nếu vẫn không hiện Trust popup sau v45, gửi kèm log mới — đặc biệt các dòng:
   - `do_usb_v1_connect: SYN retry N/3` — chỉ ra iPhone reject SYN
   - `usb_recv_tcp: ⚠️ RST received` — chỉ ra iPhone reset connection
   - `RST payload drained N bytes` — log error message từ iPhone

---

*Patch version: v45*
*Date: 2026-08-18*
*Solved issue: Trust popup không hiện — ACK/RST infinite loop do code không handle RST flag*
