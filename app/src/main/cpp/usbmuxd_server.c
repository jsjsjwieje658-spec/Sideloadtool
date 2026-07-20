/*
 * usbmuxd_server.c — Mini usbmuxd server nội bộ (in-process, Mode 1)
 *
 * ════════════════════════════════════════════════════════════════════
 * FIX v22 — Ba lỗi gây err=-3 (IDEVICE_E_NO_DEVICE):
 *
 * Bug A: Sau "Listen" OK, server phải gửi ngay "Attached" event.
 *        libusbmuxd gọi usbmuxd_get_device_list() → gửi Listen → đợi
 *        Attached events (có timeout). Nếu không có event nào →
 *        device list rỗng → IDEVICE_E_NO_DEVICE. ← ĐÂY LÀ LỖI ĐẦU TIÊN.
 *
 * Bug B: Tunnel data framing sai hoàn toàn.
 *        thread_sock_to_usb cố đọc usbmux plist header từ socket, nhưng
 *        sau Connect handshake, socket là raw TCP stream (không có header).
 *        thread_usb_to_sock wrap USB data trong plist header trước khi
 *        gửi vào socket, nhưng libimobiledevice expect raw TCP bytes.
 *
 * Bug C: iPhone hiện đại (iOS 7+) dùng v1 TCP-like protocol qua USB,
 *        không phải v0 binary CONNECT/DATA. Phải thực hiện:
 *          1. Version exchange
 *          2. TCP-like SYN → SYN+ACK → ACK handshake
 *          3. DATA packets với sequence/ack numbers
 *        Nếu gửi v0 CONNECT lên iPhone hiện đại → iPhone bỏ qua hoàn toàn.
 *
 * Protocol stack:
 *   libimobiledevice ↔ [Unix socket, plist v1] ↔ usbmuxd_server
 *   usbmuxd_server   ↔ [USB bulk, Apple v1 TCP] ↔ iPhone
 * ════════════════════════════════════════════════════════════════════
 *
 * ════════════════════════════════════════════════════════════════════
 * FIX v23 — Bốn lỗi phát hiện thêm:
 *
 * Bug A (Critical, hang 150s): usb_read_exact() dùng `static int retry`
 *        dùng chung giữa mọi cuộc gọi/thread. Fix: local + max_retries
 *        giảm 50 → 12.
 *
 * Bug B (Critical, confuse iPhone): version exchange bị lặp lại mỗi lần
 *        TCP "Connect". Fix: tách thành usbmux_version_exchange(), gọi
 *        đúng 1 lần trong nativeSetUsbFd() trước usbmuxd_server_start();
 *        idempotent qua g_version_done.
 *
 * Bug C (High, block claim — xem usb_fd_bridge.c): bỏ
 *        libusb_detach_kernel_driver() (không tồn tại trên Android).
 *
 * Bug D (Medium): usb_recv_version() đọc header trước, validate, rồi
 *        drain body riêng; timeout giảm 3000ms → 1500ms.
 * ════════════════════════════════════════════════════════════════════
 *
 * ════════════════════════════════════════════════════════════════════
 * FIX v30 — ROOT CAUSE THẬT SỰ của "version exchange thất bại 100%
 * nhất quán" xuyên suốt v20-v29 (đối chiếu trực tiếp với mã nguồn thật
 * github.com/libimobiledevice/usbmuxd, src/device.c + src/usb.c):
 *
 * Bug E (CRITICAL — nguyên nhân gốc): usb_send_version()/usb_recv_version()
 *        dùng SAI layout packet. Packet VERSION thật chỉ có header 8-byte
 *        (protocol+length, KHÔNG có magic/tx_seq/rx_seq) + body 12-byte
 *        (major/minor/padding), tổng 20 byte, major=2. Code cũ (v20-v28)
 *        gửi packet 32-byte với magic=0xfeedface + major=1 — một layout
 *        không hề tồn tại trong protocol thật. iPhone nhận packet dị dạng,
 *        không phản hồi → mọi retry/timeout/USB-reinit đều vô ích vì lỗi
 *        nằm ở tầng framing ứng dụng, không phải USB. Fix: viết lại theo
 *        đúng device.c thật (xem v1_mux_hdr_short_t + v1_version_body_t
 *        12-byte mới).
 *
 * Bug F (CRITICAL): thiếu hoàn toàn packet MUX_PROTO_SETUP — bắt buộc
 *        phải gửi ngay sau khi version response xác nhận major>=2 (xem
 *        device_version_input() thật). Thiếu packet này, device không
 *        bao giờ chuyển sang MUXDEV_ACTIVE. Fix: thêm usb_send_setup().
 *
 * Bug G (CRITICAL): V1_PROTO_TCP định nghĩa SAI = 1. Giá trị 1 thật ra là
 *        MUX_PROTO_CONTROL; giá trị đúng cho TCP là IPPROTO_TCP = 6 (xem
 *        "enum mux_protocol" thật trong device.c). Gửi SYN với protocol=1
 *        khiến device hiểu nhầm là control message và bỏ qua.
 *
 * Bug H (Medium): tx_seq/rx_seq ở tầng mux (KHÁC với seq/ack của riêng
 *        từng TCP connection) trước đây hardcode 0/0 cho mọi packet TCP
 *        gửi đi. Fix: theo dõi đúng theo device.c thật — tx_seq tăng dần
 *        sau mỗi packet gửi, rx_seq echo lại giá trị nhận được gần nhất
 *        từ device (xem g_mux_tx_seq/g_mux_rx_seq).
 * ════════════════════════════════════════════════════════════════════
 *
 * ════════════════════════════════════════════════════════════════════
 * FIX v31 — theo bảng báo cáo lỗi (Kimi AI review): "Attached" event
 * không được broadcast lại cho mọi client đang Listen khi UDID thật
 * chỉ được biết SAU khi client đó đã Listen xong.
 *
 * Bug I (HIGH): make_attached_event()/make_device_list() vốn đã có đầy đủ
 *        DeviceID, Properties dict, ConnectionType=USB, LocationID,
 *        ProductID, SerialNumber — không đổi ở fix này (đã đúng từ v22).
 *
 * Bug J (HIGH — MỚI): "Attached" trước đây chỉ gửi MỘT LẦN, ngay trong
 *        thread xử lý client vừa gửi "Listen", dùng UDID tại thời điểm
 *        đó (thường vẫn là placeholder). Không có cơ chế broadcast lại
 *        cho các client Listen khác, và không có cách nào gửi lại khi
 *        UDID thay đổi. Fix: thêm registry (register_listener()/
 *        unregister_listener()) + broadcast_attached() — gửi "Attached"
 *        tới TOÀN BỘ client đang Listen, không chỉ một client.
 *
 * Bug K (HIGH — MỚI): usbmuxd_server_update_udid() (gọi từ
 *        jni_bridge_imd.c ngay sau idevice_get_udid()) trước đây CHỈ
 *        ghi đè g_udid trong bộ nhớ — không thông báo lại cho bất kỳ
 *        client nào đang Listen. Fix: update_udid() giờ tự gọi
 *        broadcast_attached() sau khi cập nhật; đồng thời export public
 *        usbmuxd_server_broadcast_attached() để jni_bridge_imd.c gọi
 *        tường minh ngay sau update_udid() như một safety-net rõ ràng.
 * ════════════════════════════════════════════════════════════════════
 */
#include "usbmuxd_server.h"
#include "usb_fd_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <android/log.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * Dual-socket TCP support — học từ termux-usbmuxd/usbmuxd_proxy.c.
 *
 * Bên cạnh Unix socket (cho libimobiledevice C tools), server cũng mở một TCP
 * listener trên USBMUXD_TCP_PORT. Mỗi kết nối TCP được proxy in-process tới
 * Unix socket server nội bộ — giống socat trong termux-usbmuxd nhưng không
 * cần cài thêm bất kỳ gói nào.
 *
 * USBMUXD_SOCKET_ADDRESS được set về dạng "127.0.0.1:PORT" thay vì đường dẫn
 * Unix thuần tuý. Học từ termux-usbmuxd fix_shell_rc(): dạng host:port được
 * cả C tools (libimobiledevice) lẫn Rust tools (idevice-tools) hiểu đúng;
 * đường dẫn Unix thuần tuý gây crash Rust tools với AddrParseError(Socket).
 * ──────────────────────────────────────────────────────────────────────────── */
#ifndef USBMUXD_TCP_PORT
#define USBMUXD_TCP_PORT 27015
#endif
#include <time.h>

#define TAG "usbmuxd_srv"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* ── Unix socket protocol constants ────────────────────────────────────── */
#define USBMUX_PROTO_PLIST  1
#define USBMUX_TYPE_PLIST   8
#define TUNNEL_BUFSIZE      65536

/* ── usbmux socket header (LE, 16 bytes) ────────────────────────────────── */
#pragma pack(push,1)
typedef struct {
    uint32_t length;
    uint32_t version;
    uint32_t type;
    uint32_t tag;
} umux_hdr_t;

/* ── iPhone USB v1 protocol — big-endian ──────────────────────────────── */
/*
 * FIX v30 (ROOT CAUSE THẬT SỰ — thay thế mọi giả định sai từ v20-v29):
 *
 * Đối chiếu trực tiếp với mã nguồn thật của usbmuxd
 * (https://github.com/libimobiledevice/usbmuxd — src/device.c, hàm
 * send_packet() và device_data_input()), phát hiện KHUNG GÓI TIN của
 * riêng packet VERSION (protocol=0) — tức là packet ĐẦU TIÊN được gửi
 * trên mỗi USB session — hoàn toàn KHÔNG giống packet TCP/SETUP:
 *
 *   int mux_header_size = ((dev->version < 2) ? 8 : sizeof(struct mux_header));
 *
 * `dev->version` bắt đầu bằng 0 (chưa negotiate) → mux_header_size = 8,
 * nghĩa là packet VERSION chỉ có header 8 byte (protocol + length),
 * KHÔNG CÓ magic/tx_seq/rx_seq nào cả — 3 trường đó chỉ tồn tại trong
 * packet MỘT KHI dev->version >= 2 (tức là SAU khi version exchange
 * xong và đã gửi SETUP). Toàn bộ code v20-v28 (usb_send_version dùng
 * v1_mux_hdr_t 16-byte với magic=0xfeedface, rx_seq=0xffff) gửi một
 * packet VERSION có layout SAI HOÀN TOÀN — 32 byte thay vì 20 byte
 * đúng, với "magic" nằm chồng lên đúng vị trí byte mà iPhone mong đợi
 * đọc "major". iPhone thấy packet dị dạng → không phản hồi hoặc
 * silently drop → "version exchange (N retry) thất bại" LẶP LẠI
 * 100% NHẤT QUÁN qua mọi lần retry, mọi timeout, mọi USB re-init —
 * đúng như log cho thấy — vì đây là lỗi tất định ở tầng framing của
 * ứng dụng, không phải lỗi tạm thời ở tầng USB/libusb.
 *
 * Bằng chứng thêm (device_add() trong device.c thật):
 *   struct version_header vh;
 *   vh.major = htonl(2);   ← gửi major=2, KHÔNG PHẢI 1 (fix v28 "phải là 1" SAI)
 *   vh.minor = htonl(0);
 *   vh.padding = 0;        ← chỉ MỘT padding (12 byte body), không phải 2 (16 byte)
 *
 * Và sau khi nhận version response hợp lệ (major==2), phải gửi NGAY một
 * packet MUX_PROTO_SETUP (protocol=2, payload 1 byte 0x07) — dùng
 * header ĐẦY ĐỦ 16-byte (vì lúc này dev->version đã =2) — trước khi
 * device coi kết nối là "active" và chấp nhận TCP SYN. Packet SETUP
 * này HOÀN TOÀN VẮNG MẶT trong code trước đây (xem usb_send_setup()
 * mới thêm bên dưới).
 *
 * FIX: tách hai loại header — v1_mux_hdr_short_t (8 byte, chỉ cho
 * VERSION) và v1_mux_hdr_t (16 byte, cho SETUP + TCP, dùng SAU khi
 * version đã negotiate). usb_send_version()/usb_recv_version() viết
 * lại hoàn toàn theo layout 8-byte + version_header 12-byte (20 byte
 * tổng — khớp chính xác "length < 8) VÀ header (8-byte)"; usb_recv_version()
 * đọc lại. */
typedef struct {
    uint32_t protocol;   /* BE: 0=version */
    uint32_t length;     /* BE: total length including this 8-byte header */
} v1_mux_hdr_short_t;

/* Mux header (16 bytes) — CHỈ dùng cho SETUP + TCP, sau khi version>=2 */
typedef struct {
    uint32_t protocol;   /* BE: 2=setup, 6=tcp (IPPROTO_TCP) */
    uint32_t length;     /* BE: total length including all headers */
    uint32_t magic;      /* BE: 0xfeedface */
    uint16_t tx_seq;     /* BE: our outgoing mux-level frame counter */
    uint16_t rx_seq;     /* BE: last mux-level frame counter seen from iPhone */
} v1_mux_hdr_t;

/* TCP header (20 bytes) */
typedef struct {
    uint16_t sport;      /* BE: source port (our ephemeral port) */
    uint16_t dport;      /* BE: destination port (iPhone service port) */
    uint32_t seq;        /* BE: sequence number */
    uint32_t ack;        /* BE: acknowledgement number */
    uint8_t  off;        /* data offset in 32-bit words; 0x50=20 bytes */
    uint8_t  flags;      /* TCP flags: SYN=0x02, ACK=0x10, FIN=0x01, RST=0x04 */
    uint16_t window;     /* BE: receive window */
    uint16_t cksum;      /* checksum (0 = skip) */
    uint16_t urgp;       /* urgent pointer (0) */
} v1_tcp_hdr_t;

/*
 * Version packet body (12 bytes — KHÔNG PHẢI 16).
 * Khớp struct version_header thật trong usbmuxd/src/device.c: đúng
 * MỘT trường padding, không phải hai.
 */
typedef struct {
    uint32_t major;      /* BE — gửi đi PHẢI là 2 (xem device_add() thật) */
    uint32_t minor;      /* BE */
    uint32_t padding;
} v1_version_body_t;

#pragma pack(pop)

#define V1_MAGIC        0xfeedface
#define V1_PROTO_VER     0   /* MUX_PROTO_VERSION */
#define V1_PROTO_CONTROL 1   /* MUX_PROTO_CONTROL — KHÔNG PHẢI TCP! */
#define V1_PROTO_SETUP   2   /* MUX_PROTO_SETUP — bắt buộc gửi sau version OK */
/*
 * FIX v30 (CRITICAL): V1_PROTO_TCP trước đây = 1 — đó là giá trị của
 * MUX_PROTO_CONTROL trong protocol thật, KHÔNG PHẢI TCP! usbmuxd thật
 * định nghĩa MUX_PROTO_TCP = IPPROTO_TCP = 6 (usbmuxd/src/device.c:
 * "enum mux_protocol { ... MUX_PROTO_TCP = IPPROTO_TCP }"). Gửi SYN
 * với protocol=1 khiến iPhone hiểu nhầm gói TCP SYN là một gói CONTROL
 * (dùng cho log/error message nội bộ) và bỏ qua — kết nối TCP không
 * bao giờ được thiết lập dù version exchange có thành công.
 */
#define V1_PROTO_TCP     6   /* MUX_PROTO_TCP = IPPROTO_TCP (KHÔNG PHẢI 1) */

#define TH_FIN  0x01
#define TH_SYN  0x02
#define TH_RST  0x04
#define TH_PUSH 0x08
#define TH_ACK  0x10

/* ── TCP connection state for v1 tunnel ─────────────────────────────────── */
typedef struct {
    uint16_t sport;         /* our source port (ephemeral) */
    uint16_t dport;         /* iPhone destination port */
    uint32_t local_seq;     /* our next sequence number to send */
    uint32_t remote_seq;    /* iPhone's next expected seq (our ACK value) */
    pthread_mutex_t usb_tx_lock;  /* serialize USB writes from both threads */
} tcp_state_t;

/* ── Server state ────────────────────────────────────────────────────────── */
static volatile int    g_running    = 0;
static int             g_server_fd  = -1;
static pthread_t       g_srv_thread;
static char            g_sock_path[512];
static char            g_udid[64];
static int             g_product_id = 0;
static int             g_device_id  = 1;

/* ── TCP dual-socket state (học từ termux-usbmuxd/usbmuxd_proxy.c) ─────── */
static volatile int    g_tcp_running = 0;
static int             g_tcp_fd      = -1;
static pthread_t       g_tcp_thread;
static pthread_mutex_t g_udid_mutex = PTHREAD_MUTEX_INITIALIZER;
/*
 * FIX Bug B: version exchange chỉ được phép xảy ra MỘT LẦN cho mỗi USB
 * session (không phải mỗi lần TCP "Connect"). Flag này đảm bảo
 * usbmux_version_exchange() là idempotent.
 */
static volatile int    g_version_done = 0;

/*
 * FIX v30: mux-level tx_seq/rx_seq — KHÔNG phải seq/ack của riêng từng
 * TCP connection (đó là tcp_state_t.local_seq/remote_seq, không đổi).
 * Đây là counter framing dùng CHUNG cho toàn bộ USB link, đúng như
 * dev->tx_seq/dev->rx_seq trong usbmuxd/src/device.c thật:
 *   - mỗi packet gửi đi (SETUP hoặc TCP) mang tx_seq hiện tại rồi tăng lên 1
 *   - rx_seq gửi đi = giá trị rx_seq gần nhất mà iPhone gửi cho ta
 *   - riêng packet SETUP reset tx_seq=0, rx_seq=0xFFFF trước khi gửi
 * Có thể có nhiều TCP tunnel chạy song song (lockdownd, AFC, instproxy),
 * mỗi cái có usb_tx_lock RIÊNG trong tcp_state_t — nhưng tx_seq/rx_seq
 * là state CHUNG của cả link vật lý nên cần mutex riêng ở đây.
 */
static pthread_mutex_t g_mux_seq_lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t         g_mux_tx_seq   = 0;
static uint16_t         g_mux_rx_seq   = 0xFFFF;

/* ════════════════════════════════════════════════════════════════════════
 * Socket I/O helpers
 * ════════════════════════════════════════════════════════════════════════ */

static int sock_write_all(int fd, const void *buf, int len) {
    const char *p = (const char *)buf;
    int total = 0;
    while (total < len) {
        int n = (int)write(fd, p + total, len - total);
        if (n <= 0) return -1;
        total += n;
    }
    return total;
}

static int sock_read_all(int fd, void *buf, int len) {
    char *p = (char *)buf;
    int total = 0;
    while (total < len) {
        int n = (int)read(fd, p + total, len - total);
        if (n <= 0) return -1;
        total += n;
    }
    return total;
}

/* ════════════════════════════════════════════════════════════════════════
 * Unix socket plist protocol (libimobiledevice ↔ our server)
 * ════════════════════════════════════════════════════════════════════════ */

static int send_plist(int fd, uint32_t tag, const char *plist_xml) {
    uint32_t xml_len = (uint32_t)strlen(plist_xml);
    umux_hdr_t hdr;
    hdr.length  = sizeof(hdr) + xml_len;
    hdr.version = USBMUX_PROTO_PLIST;
    hdr.type    = USBMUX_TYPE_PLIST;
    hdr.tag     = tag;
    if (sock_write_all(fd, &hdr, sizeof(hdr)) < 0) return -1;
    if (sock_write_all(fd, plist_xml, xml_len) < 0) return -1;
    return 0;
}

static char *recv_plist(int fd, umux_hdr_t *hdr_out) {
    umux_hdr_t hdr;
    if (sock_read_all(fd, &hdr, sizeof(hdr)) < 0) return NULL;
    if (hdr_out) *hdr_out = hdr;
    uint32_t body_len = hdr.length - sizeof(hdr);
    if (body_len > 2*1024*1024) { LOGE("recv_plist: body_len=%u quá lớn", body_len); return NULL; }
    char *xml = malloc(body_len + 1);
    if (!xml) return NULL;
    if (body_len > 0 && sock_read_all(fd, xml, body_len) < 0) { free(xml); return NULL; }
    xml[body_len] = '\0';
    return xml;
}

/* ── Simple plist field extractors ─────────────────────────────────────── */

static const char *extract_str(const char *xml, const char *key) {
    /*
     * FIX v28: `static char buf[256]` → `static __thread char buf[256]`
     *
     * BUG: usbmuxd_server chạy nhiều thread đồng thời (mỗi client kết nối
     * tạo một thread riêng). `static char buf` toàn cục bị CHIA SẺ giữa
     * các thread → race condition: thread A đang đọc buf, thread B ghi đè →
     * corrupt kết quả (sai MessageType, UDID bị cắt, v.v.) → server gửi
     * sai plist response → libimobiledevice disconnect hoặc IDEVICE_E_NO_DEVICE.
     *
     * Fix: `__thread` (thread-local storage) → mỗi thread có buf riêng.
     * Supported trên Android NDK (GCC/Clang với bionic libc).
     */
    static __thread char buf[256];
    char tag_open[128];
    snprintf(tag_open, sizeof(tag_open), "<key>%s</key>", key);
    const char *p = strstr(xml, tag_open);
    if (!p) return NULL;
    p += strlen(tag_open);
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (strncmp(p, "<string>", 8) != 0) return NULL;
    p += 8;
    const char *e = strstr(p, "</string>");
    if (!e) return NULL;
    size_t len = (size_t)(e - p);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, p, len);
    buf[len] = '\0';
    return buf;
}

static long extract_int(const char *xml, const char *key) {
    char tag_open[128];
    snprintf(tag_open, sizeof(tag_open), "<key>%s</key>", key);
    const char *p = strstr(xml, tag_open);
    if (!p) return -1;
    p += strlen(tag_open);
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (strncmp(p, "<integer>", 9) != 0) return -1;
    p += 9;
    return atol(p);
}

/* ── Plist response builders ─────────────────────────────────────────────── */

static char *make_result_ok(void) {
    char *out = NULL;
    asprintf(&out,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\""
        " \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
        "<plist version=\"1.0\"><dict>"
        "<key>MessageType</key><string>Result</string>"
        "<key>Number</key><integer>0</integer>"
        "</dict></plist>");
    return out;
}

/*
 * FIX v22 Bug A: make_attached_event — gửi ngay sau khi phản hồi "Listen" OK.
 * libusbmuxd gọi usbmuxd_get_device_list() → Listen → đợi Attached events.
 * Nếu không gửi event này, device list rỗng → IDEVICE_E_NO_DEVICE=-3.
 */
static char *make_attached_event(void) {
    pthread_mutex_lock(&g_udid_mutex);
    char udid_copy[64];
    strncpy(udid_copy, g_udid, sizeof(udid_copy)-1);
    udid_copy[63] = '\0';
    pthread_mutex_unlock(&g_udid_mutex);

    char *out = NULL;
    asprintf(&out,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\""
        " \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
        "<plist version=\"1.0\"><dict>"
        "<key>MessageType</key><string>Attached</string>"
        "<key>DeviceID</key><integer>%d</integer>"
        "<key>Properties</key><dict>"
        "<key>ConnectionType</key><string>USB</string>"
        "<key>DeviceID</key><integer>%d</integer>"
        "<key>LocationID</key><integer>0</integer>"
        "<key>ProductID</key><integer>%d</integer>"
        "<key>SerialNumber</key><string>%s</string>"
        "<key>UDID</key><string>%s</string>"
        "</dict>"
        "</dict></plist>",
        g_device_id, g_device_id, g_product_id,
        udid_copy, udid_copy);
    return out;
}

static char *make_detached_event(void) {
    pthread_mutex_lock(&g_udid_mutex);
    char udid_copy[64];
    strncpy(udid_copy, g_udid, sizeof(udid_copy)-1);
    udid_copy[63] = '\0';
    pthread_mutex_unlock(&g_udid_mutex);

    char *out = NULL;
    asprintf(&out,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\""
        " \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
        "<plist version=\"1.0\"><dict>"
        "<key>MessageType</key><string>Detached</string>"
        "<key>DeviceID</key><integer>%d</integer>"
        "<key>Properties</key><dict>"
        "<key>ConnectionType</key><string>USB</string>"
        "<key>DeviceID</key><integer>%d</integer>"
        "<key>LocationID</key><integer>0</integer>"
        "<key>ProductID</key><integer>%d</integer>"
        "<key>SerialNumber</key><string>%s</string>"
        "<key>UDID</key><string>%s</string>"
        "</dict>"
        "</dict></plist>",
        g_device_id, g_device_id, g_product_id,
        udid_copy, udid_copy);
    return out;
}

static char *make_device_list(void) {
    pthread_mutex_lock(&g_udid_mutex);
    char udid_copy[64];
    strncpy(udid_copy, g_udid, sizeof(udid_copy)-1);
    udid_copy[63] = '\0';
    pthread_mutex_unlock(&g_udid_mutex);

    char *out = NULL;
    asprintf(&out,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\""
        " \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
        "<plist version=\"1.0\"><dict>"
        "<key>MessageType</key><string>Result</string>"
        "<key>Number</key><integer>0</integer>"
        "<key>DeviceList</key><array><dict>"
        "<key>DeviceID</key><integer>%d</integer>"
        "<key>MessageType</key><string>Attached</string>"
        "<key>Properties</key><dict>"
        "<key>ConnectionType</key><string>USB</string>"
        "<key>DeviceID</key><integer>%d</integer>"
        "<key>LocationID</key><integer>0</integer>"
        "<key>ProductID</key><integer>%d</integer>"
        "<key>SerialNumber</key><string>%s</string>"
        "<key>UDID</key><string>%s</string>"
        "</dict></dict></array>"
        "</dict></plist>",
        g_device_id, g_device_id, g_product_id,
        udid_copy, udid_copy);
    return out;
}


static char *make_empty_device_list(void) {
    char *out = NULL;
    asprintf(&out,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\""
        " \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
        "<plist version=\"1.0\"><dict>"
        "<key>MessageType</key><string>Result</string>"
        "<key>Number</key><integer>0</integer>"
        "<key>DeviceList</key><array/>"
        "</dict></plist>");
    return out;
}
static char *make_connect_result(uint32_t code) {
    char *out = NULL;
    asprintf(&out,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\""
        " \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
        "<plist version=\"1.0\"><dict>"
        "<key>MessageType</key><string>Result</string>"
        "<key>Number</key><integer>%u</integer>"
        "</dict></plist>",
        (unsigned)code);
    return out;
}

/* ════════════════════════════════════════════════════════════════════════
 * iPhone USB v1 protocol — TCP-like state machine
 *
 * iPhone hiện đại (iOS 7+) CHỈ hỗ trợ v1 protocol. Phải:
 *   1. Version exchange
 *   2. SYN → SYN+ACK → ACK handshake
 *   3. DATA với sequence/ack numbers + ACK phản hồi
 *   4. FIN khi đóng kết nối
 * ════════════════════════════════════════════════════════════════════════ */

/* USB raw I/O (sử dụng libusb qua usb_fd_bridge) */
static int usb_write(const void *buf, int len) {
    return usb_bridge_bulk_write(buf, len, 5000);
}

/* ════════════════════════════════════════════════════════════════════════
 * FIX v29 (ROOT CAUSE — thay thế toàn bộ v20-v28): buffered USB read layer
 * ════════════════════════════════════════════════════════════════════════
 *
 * NGUYÊN NHÂN GỐC của mọi lần "version exchange thất bại" / "SYN+ACK không
 * nhận được" xuyên suốt các bản v20-v28 (đã thử timeout dài hơn, nhiều retry
 * hơn, clear_halt, flush, full re-init — KHÔNG bản nào sửa được vì không
 * bản nào đụng đến nguyên nhân thật):
 *
 *   usb_read_exact() cũ gọi usb_bridge_bulk_read() TRỰC TIẾP với độ dài
 *   CHÍNH XÁC nhỏ — ví dụ đọc 16 byte mux-header trước, rồi mới đọc riêng
 *   20 byte TCP-header, rồi mới đọc riêng phần data (xem usb_recv_version()
 *   và usb_recv_tcp()).
 *
 *   Nhưng iPhone không gửi từng phần nhỏ như vậy — nó ghi CẢ message
 *   (mux header + tcp header + data, hoặc mux header + version body) trong
 *   MỘT lần write()/URB duy nhất phía thiết bị. Khi ta yêu cầu libusb đọc
 *   chỉ 16 byte trong khi iPhone đã đẩy 32+ byte vào CÙNG một gói USB,
 *   libusb/kernel trả về LIBUSB_ERROR_OVERFLOW (-8) — đây là lỗi "device
 *   gửi nhiều hơn buffer host yêu cầu", KHÔNG PHẢI lỗi tạm thời như PIPE
 *   hay timeout (xem libusb docs: "Packets and overflows"). Dữ liệu bị mất,
 *   usb_bridge_bulk_read() không retry lỗi này (chỉ retry PIPE) → thất bại
 *   NHẤT QUÁN 100% mỗi lần, đúng như log cho thấy (5/5 attempt đều thất bại
 *   giống hệt nhau — không phải flaky, mà là lỗi tất định).
 *
 * GIẢI PHÁP (giống hệt buf_read()/rxbuf đã dùng ĐÚNG trong usbmux.c —
 * Mode 2/3 chưa từng bị lỗi này vì nó vốn đã đọc theo kiểu buffer lớn):
 *
 *   Đọc vào MỘT buffer nội bộ LỚN (64KB — bội số của max packet size nên
 *   theo đúng khuyến cáo libusb, KHÔNG BAO GIỜ overflow: "you will never
 *   see an overflow if your transfer buffer size is a multiple of the
 *   endpoint's packet size") bằng MỘT lệnh usb_bridge_bulk_read() duy nhất,
 *   rồi phục vụ mọi usb_read_exact() từ buffer đó — chỉ gọi bulk_read() mới
 *   khi buffer đã cạn. Mutex bảo vệ buffer dùng chung giữa nhiều thread
 *   (thread_sock_to_usb / thread_usb_to_sock / do_usb_v1_connect).
 * ════════════════════════════════════════════════════════════════════════ */
#define USB_RXBUF_SIZE 65536
static pthread_mutex_t g_usb_rx_lock    = PTHREAD_MUTEX_INITIALIZER;
static uint8_t          g_usb_rxbuf[USB_RXBUF_SIZE];
static int               g_usb_rxbuf_pos   = 0;
static int               g_usb_rxbuf_avail = 0;

/*
 * usb_rx_buf_reset — xoá sạch buffer nội bộ. PHẢI gọi mỗi khi bắt đầu một
 * USB session mới (fd mới / sau full re-init) để tránh byte "rác" còn sót
 * từ session/lần thử trước bị hiểu nhầm là dữ liệu của session mới.
 * Gọi từ usbmuxd_server_reset_version_state() — nơi vốn đã được gọi đúng
 * tại mọi điểm bắt đầu session mới trong jni_bridge_imd.c.
 */
static void usb_rx_buf_reset(void) {
    pthread_mutex_lock(&g_usb_rx_lock);
    g_usb_rxbuf_pos   = 0;
    g_usb_rxbuf_avail = 0;
    pthread_mutex_unlock(&g_usb_rx_lock);
}

static int usb_read_exact(void *buf, int len, int timeout_ms) {
    char *p = (char *)buf;
    int got = 0;

    /*
     * FIX Bug A (giữ nguyên): các counter retry PHẢI là biến cục bộ —
     * không dùng `static` dùng chung giữa nhiều thread.
     */
    int retry     = 0; /* timeout retries */
    int err_retry = 0; /* lỗi I/O thật sự (không phải overflow — overflow
                           giờ không còn xảy ra nhờ buffer 64KB ở trên) */
    const int max_retries     = 12;
    const int max_err_retries = 4;

    pthread_mutex_lock(&g_usb_rx_lock);
    while (got < len) {
        /* Phục vụ từ buffer nội bộ trước, nếu còn dữ liệu */
        if (g_usb_rxbuf_avail > 0) {
            int take = g_usb_rxbuf_avail < (len - got) ? g_usb_rxbuf_avail : (len - got);
            memcpy(p + got, g_usb_rxbuf + g_usb_rxbuf_pos, (size_t)take);
            g_usb_rxbuf_pos   += take;
            g_usb_rxbuf_avail -= take;
            got += take;
            retry = 0;
            err_retry = 0;
            continue;
        }

        /*
         * Buffer rỗng — nạp lại bằng MỘT lệnh đọc LỚN (64KB). Đây là điểm
         * mấu chốt của fix: yêu cầu buffer luôn ≥ bất kỳ gói tin đơn lẻ nào
         * iPhone có thể gửi → không bao giờ LIBUSB_ERROR_OVERFLOW nữa.
         */
        g_usb_rxbuf_pos = 0;
        int n = usb_bridge_bulk_read(g_usb_rxbuf, USB_RXBUF_SIZE, timeout_ms);
        if (n < 0) {
            /* Lỗi I/O thật (VD: PIPE sau khi đã retry nội bộ hết) */
            if (++err_retry > max_err_retries) {
                pthread_mutex_unlock(&g_usb_rx_lock);
                return -1;
            }
            usleep(50 * 1000);
            continue;
        }
        if (n == 0) {
            /* timeout thật sự — không có dữ liệu nào để đọc */
            if (++retry > max_retries) {
                pthread_mutex_unlock(&g_usb_rx_lock);
                return -1;
            }
            continue;
        }
        g_usb_rxbuf_avail = n;
    }
    pthread_mutex_unlock(&g_usb_rx_lock);
    return got;
}

static int usb_read_at_least(void *buf, int len, int timeout_ms) {
    return usb_read_exact(buf, len, timeout_ms);
}

/* ── Gửi v1 version packet ─────────────────────────────────────────────── */
/*
 * FIX v30 (ROOT CAUSE THẬT SỰ — xem giải thích đầy đủ ở khối comment lớn
 * phía trên struct v1_mux_hdr_short_t).
 *
 * Toàn bộ v20-v28 gửi SAI layout cho packet VERSION: dùng header 16-byte
 * (có magic + tx_seq + rx_seq) trong khi packet VERSION thật — theo
 * chính mã nguồn usbmuxd (device.c: send_packet(), device_add()) — chỉ
 * có header 8-byte (protocol + length), KHÔNG có magic/seq nào, vì
 * "version" của kết nối lúc này vẫn là 0 (chưa negotiate). Gửi đúng:
 *
 *   [4B protocol=0 BE][4B length=20 BE][4B major=2 BE][4B minor=0 BE][4B padding=0]
 *
 * Tổng cộng đúng 20 byte — không phải 32 byte như trước.
 */
static int usb_send_version(void) {
    uint8_t pkt[sizeof(v1_mux_hdr_short_t) + sizeof(v1_version_body_t)];
    memset(pkt, 0, sizeof(pkt));

    v1_mux_hdr_short_t *hdr = (v1_mux_hdr_short_t *)pkt;
    hdr->protocol = htonl(V1_PROTO_VER);
    hdr->length   = htonl(sizeof(pkt));
    /* KHÔNG có magic/tx_seq/rx_seq — packet VERSION không mang các
     * trường này (dev->version == 0 tại thời điểm gửi, xem usbmuxd
     * thật: mux_header_size = (dev->version < 2) ? 8 : 16). */

    v1_version_body_t *body =
        (v1_version_body_t *)(pkt + sizeof(v1_mux_hdr_short_t));
    /*
     * major PHẢI là 2 — khớp device_add() thật trong usbmuxd/src/device.c:
     *   struct version_header vh; vh.major = htonl(2); vh.minor = htonl(0);
     * major=2 là "v2 protocol" (TCP-like framing, dùng bởi mọi iPhone
     * iOS 7 trở lên). device_version_input() phía nhận chấp nhận cả
     * major==1 lẫn major==2, nhưng CHỈ upgrade lên header dài (và gửi
     * SETUP) nếu major>=2 — nên host phải chủ động đề nghị 2, không
     * phải 1 (fix v28 "phải là 1" dựa trên hiểu nhầm proto version với
     * "v1 protocol" nói chung — đây là lỗi thuật ngữ, không phải giá trị
     * thật của trường major trên wire).
     */
    body->major   = htonl(2);
    body->minor   = htonl(0);
    body->padding = 0;

    return usb_write(pkt, sizeof(pkt)) > 0 ? 0 : -1;
}

/* ── Gửi MUX_PROTO_SETUP packet — bắt buộc ngay sau version OK ─────────── */
/*
 * FIX v30 (mới — trước đây KHÔNG TỒN TẠI trong code):
 *
 * Theo device_version_input() thật (usbmuxd/src/device.c):
 *   dev->version = vh->major;                 // = 2
 *   if (dev->version >= 2)
 *       send_packet(dev, MUX_PROTO_SETUP, NULL, "\x07", 1);
 *   dev->state = MUXDEV_ACTIVE;               // sẵn sàng nhận TCP
 *
 * Nếu không gửi packet SETUP này, phía device không bao giờ được thông
 * báo "host đã sẵn sàng" — mọi TCP SYN gửi sau đó (lockdownd, AFC,
 * instproxy...) có thể bị bỏ qua vì thiết bị coi kết nối vẫn ở trạng
 * thái init. Dùng header ĐẦY ĐỦ 16-byte (magic + tx_seq + rx_seq) vì
 * tại thời điểm này "version" của kết nối đã là 2. Theo đúng spec,
 * gửi SETUP phải reset tx_seq=0 / rx_seq=0xFFFF trước khi ghi header,
 * rồi tăng tx_seq lên 1 cho các packet kế tiếp (SYN, ACK, DATA...).
 */
static int usb_send_setup(void) {
    uint8_t pkt[sizeof(v1_mux_hdr_t) + 1];
    memset(pkt, 0, sizeof(pkt));

    pthread_mutex_lock(&g_mux_seq_lock);
    g_mux_tx_seq = 0;
    g_mux_rx_seq = 0xFFFF;

    v1_mux_hdr_t *hdr = (v1_mux_hdr_t *)pkt;
    hdr->protocol = htonl(V1_PROTO_SETUP);
    hdr->length   = htonl(sizeof(pkt));
    hdr->magic    = htonl(V1_MAGIC);
    hdr->tx_seq   = htons(g_mux_tx_seq);
    hdr->rx_seq   = htons(g_mux_rx_seq);
    g_mux_tx_seq++;
    pthread_mutex_unlock(&g_mux_seq_lock);

    pkt[sizeof(v1_mux_hdr_t)] = 0x07;  /* payload byte thật trong usbmuxd (device.c) */

    int r = usb_write(pkt, sizeof(pkt));
    LOGI("usb_send_setup: MUX_PROTO_SETUP gửi %s", r > 0 ? "OK" : "THẤT BẠI");
    return r > 0 ? 0 : -1;
}

/* ── Nhận và kiểm tra version response từ iPhone ───────────────────────── */
/*
 * FIX v30 (ROOT CAUSE — thay thế hoàn toàn logic đọc 16-byte header cũ):
 *
 * Packet VERSION KHÔNG có trường magic (xem giải thích đầy đủ ở khối
 * comment lớn tại struct v1_mux_hdr_short_t phía trên). Đọc đúng: 8-byte
 * header (protocol + length) rồi 12-byte body (major + minor + padding)
 * — KHÔNG đọc/validate magic vì trường đó không tồn tại trong packet
 * này. Đây chính là lý do version exchange trước đây thất bại 100% nhất
 * quán: code cũ đọc 16 byte làm "header" trong khi byte thứ 9-12 thật sự
 * đã là "major" của body — validate magic trên đúng 4 byte đó luôn thất
 * bại, mọi retry/timeout/reinit đều vô ích vì lỗi nằm ở tầng framing chứ
 * không phải USB.
 *
 * out_major: nhận giá trị major mà iPhone trả về (1 hoặc 2), dùng để
 * quyết định có cần gửi thêm packet SETUP hay không (chỉ khi major>=2).
 */
static int usb_recv_version(uint32_t *out_major) {
    const int VERSION_TIMEOUT_MS      = 3000;  /* ms mỗi lần đọc — ngắn hơn để retry nhanh hơn */
    const int MAX_TIMEOUT_TRIES       = 10;    /* tối đa 10 lần timeout (30 giây tổng) */
    const int MAX_SKIP                = 30;    /* tối đa 30 bad packet bị skip */
    v1_mux_hdr_short_t hdr;

    int timeout_tries = 0;
    for (int skip = 0; skip < MAX_SKIP; ) {
        /* ── Đọc 8-byte short header (protocol + length, KHÔNG có magic) ── */
        int n = usb_read_exact(&hdr, sizeof(hdr), VERSION_TIMEOUT_MS);
        if (n <= 0) {
            timeout_tries++;
            if (timeout_tries >= MAX_TIMEOUT_TRIES) {
                LOGI("usb_recv_version: %d timeout liên tiếp — dừng tìm version packet",
                     MAX_TIMEOUT_TRIES);
                break;
            }
            LOGI("usb_recv_version: header timeout n=%d (timeout %d/%d), tiếp tục chờ...",
                 n, timeout_tries, MAX_TIMEOUT_TRIES);
            usleep(200 * 1000); /* 200ms */
            continue;  /* KHÔNG tăng skip */
        }
        /* Nhận được dữ liệu — reset timeout counter */
        timeout_tries = 0;

        if (n < (int)sizeof(hdr)) {
            LOGI("usb_recv_version: short header n=%d (skip=%d/%d), retry",
                 n, skip+1, MAX_SKIP);
            skip++;
            continue;
        }

        uint32_t protocol = ntohl(hdr.protocol);
        uint32_t pkt_len  = ntohl(hdr.length);

        /* ── Sanity-check length trước khi tin bất kỳ thứ gì ── */
        if (pkt_len < sizeof(hdr) || pkt_len > 65536) {
            LOGI("usb_recv_version: length=%u vô lý (skip=%d/%d) — có thể lệch byte, retry",
                 pkt_len, skip+1, MAX_SKIP);
            skip++;
            continue;
        }

        /* ── Kiểm tra protocol ── */
        if (protocol != V1_PROTO_VER) {
            uint32_t body_len = pkt_len - sizeof(hdr);
            if (body_len > 0 && body_len < 65536) {
                uint8_t *drain = malloc(body_len);
                if (drain) {
                    usb_read_exact(drain, (int)body_len, 2000);
                    free(drain);
                }
            }
            LOGI("usb_recv_version: protocol=%u (not version), drain & retry (skip=%d/%d)",
                 protocol, skip+1, MAX_SKIP);
            skip++;
            continue;
        }

        /* ── Version packet hợp lệ — đọc body (12 byte: major/minor/padding) ── */
        uint32_t body_len = pkt_len - sizeof(hdr);
        if (body_len > sizeof(v1_version_body_t)) body_len = sizeof(v1_version_body_t);
        uint32_t major = 0, minor = 0;
        if (body_len > 0) {
            uint8_t body[sizeof(v1_version_body_t)];
            memset(body, 0, sizeof(body));
            int r = usb_read_exact(body, (int)body_len, VERSION_TIMEOUT_MS);
            if (r < (int)body_len) {
                LOGI("usb_recv_version: short body read r=%d/%u, retry", r, body_len);
                continue; /* retry — có thể đọc được ở lần sau */
            }
            v1_version_body_t *vb = (v1_version_body_t *)body;
            major = ntohl(vb->major);
            minor = ntohl(vb->minor);
            LOGI("usb_recv_version: iPhone version major=%u minor=%u", major, minor);
        }

        /*
         * Khớp device_version_input() thật: chỉ chấp nhận major==1 hoặc
         * major==2, từ chối mọi giá trị khác (thường là dấu hiệu đọc lệch
         * byte / dữ liệu rác chứ không phải version hợp lệ).
         */
        if (major != 1 && major != 2) {
            LOGI("usb_recv_version: major=%u không hợp lệ (skip=%d/%d), retry",
                 major, skip+1, MAX_SKIP);
            skip++;
            continue;
        }

        LOGI("usb_recv_version: \u2705 iPhone version confirmed major=%u minor=%u (skip=%d)",
             major, minor, skip);
        if (out_major) *out_major = major;
        return 0;
    } /* end for(skip) */

    LOGE("usb_recv_version: \u274c thất bại — skip=%d/%d timeout=%d/%d",
         MAX_SKIP, MAX_SKIP, timeout_tries, MAX_TIMEOUT_TRIES);
    return -1;
}

/*
 * usbmux_version_exchange (public, khai báo trong usbmuxd_server.h)
 *
 * FIX Bug B (Critical — confuse iPhone): iPhone v1 mux protocol chỉ chấp
 * nhận MỘT version exchange per USB session. Code cũ gọi
 * usb_send_version()+usb_recv_version() bên trong do_usb_v1_connect(), tức
 * là mỗi lần libusbmuxd gửi "Connect" (lockdownd, AFC, instproxy, ...) đều
 * lặp lại version exchange. Từ lần kết nối thứ 2 trở đi, iPhone nhận được
 * một packet version không mong đợi trong lúc nó đang chờ SYN → reject
 * connection.
 *
 * Fix: tách thành hàm public này, gọi đúng MỘT LẦN — từ nativeSetUsbFd()
 * ngay sau khi libusb init xong và TRƯỚC KHI usbmuxd_server_start(). Flag
 * g_version_done đảm bảo idempotent nếu hàm bị gọi lại (an toàn khi
 * do_usb_v1_connect() cũng gọi lại như một safety check).
 */
bool usbmux_version_exchange(void) {
    if (g_version_done) {
        LOGI("usbmux_version_exchange: đã thực hiện trước đó cho session này — bỏ qua (idempotent)");
        return true;
    }
    LOGI("usbmux_version_exchange: bắt đầu version exchange (đúng 1 lần/session)");

    /*
     * FIX v24 Bug H: Thêm retry (3 lần, khoảng cách 1 giây).
     * Trước đây một lần gửi+nhận thất bại là hết — không có cơ hội phục hồi.
     * Sau khi libusb wrap fd, endpoint có thể bị stall trong 1-2 giây đầu
     * (đặc biệt trên iOS 16+ và một số dòng Android). Retry cho phép
     * version exchange thành công ở lần thứ 2 hoặc 3.
     */
    /*
     * FIX v27 (học từ usbmuxd_proxy.c của termux-usbmuxd):
     *
     * Tăng từ 3 → 5 lần retry. Giữa mỗi lần retry:
     *   1. usb_bridge_clear_endpoints_halt() — xóa STALL trên cả ep_out và ep_in
     *   2. usb_bridge_flush_in() — drain tất cả stale packet còn trong buffer
     *   3. Delay 1.5 giây — cho iPhone đủ thời gian reset MUX endpoint
     *
     * Lý do 5 lần (thay vì 3): Trên một số dòng iPhone (iOS 16+) và một số
     * Android OEM (MediaTek SoC), endpoint STALL kéo dài đến 3-4 giây sau
     * khi libusb wrap fd. 5 retry × 1.5s = 7.5s tổng thời gian chờ.
     *
     * termux-usbmuxd giải quyết vấn đề này bằng cách dùng fd SẠCH (không
     * claim interface từ Android). Với fix v27 (UsbTransport.open() không
     * claim interface nữa), số lần retry thực tế cần ít hơn, nhưng giữ 5
     * để đảm bảo backward compat với các thiết bị khó chịu.
     */
    for (int attempt = 0; attempt < 5; attempt++) {
        if (attempt > 0) {
            LOGI("usbmux_version_exchange: retry lần %d/5 — clear_halt + flush + 1.5s...", attempt + 1);
            usb_bridge_clear_endpoints_halt();
            usb_bridge_flush_in(8, 200);
            usleep(1500 * 1000); /* 1.5 giây giữa các lần retry */
        }

        if (usb_send_version() < 0) {
            LOGE("usbmux_version_exchange attempt %d: gửi VERSION thất bại", attempt + 1);
            continue; /* thử lại */
        }

        uint32_t major = 0;
        if (usb_recv_version(&major) == 0) {
            /*
             * FIX v30: bắt buộc gửi MUX_PROTO_SETUP ngay khi major>=2 —
             * xem usb_send_setup() và device_version_input() thật. Không
             * gửi SETUP thì device không chuyển sang MUXDEV_ACTIVE và sẽ
             * không phản hồi bất kỳ TCP SYN nào gửi sau đó.
             */
            if (major >= 2) {
                if (usb_send_setup() < 0) {
                    LOGE("usbmux_version_exchange attempt %d: gửi SETUP thất bại", attempt + 1);
                    continue; /* thử lại toàn bộ exchange */
                }
            } else {
                LOGI("usbmux_version_exchange: thiết bị dùng major=1 (protocol cũ) — bỏ qua SETUP");
            }
            g_version_done = 1;
            LOGI("usbmux_version_exchange: ✅ version exchange OK (attempt %d, major=%u)",
                 attempt + 1, major);
            return true;
        }

        LOGE("usbmux_version_exchange attempt %d: nhận VERSION thất bại", attempt + 1);
    }

    LOGE("usbmux_version_exchange: ❌ thất bại sau 5 lần thử — iPhone không phản hồi v1 protocol");
    return false;
}

/*
 * usbmuxd_server_reset_version_state — xem giải thích trong .h.
 * Gọi khi có USB fd/session mới, TRƯỚC usbmux_version_exchange(), để
 * flag idempotent không "ăn" mất version exchange thật sự cần thiết
 * cho session mới (VD: rút/cắm lại cáp mà không restart app process).
 */
void usbmuxd_server_reset_version_state(void) {
    g_version_done = 0;
    /*
     * FIX v29: reset luôn buffer đọc nội bộ (xem usb_rx_buf_reset() ở trên).
     * Bắt buộc — nếu không, byte còn sót trong buffer từ lần thử/USB fd
     * trước có thể bị hiểu nhầm là dữ liệu hợp lệ của session mới.
     */
    usb_rx_buf_reset();
    /*
     * FIX v30: reset luôn mux-level tx_seq/rx_seq — đây là state của
     * PHIÊN USB, không phải của riêng packet SETUP. Session mới (fd mới)
     * phải bắt đầu lại từ tx_seq=0 / rx_seq=0xFFFF, đúng như device.c
     * thật làm mỗi khi gửi packet MUX_PROTO_SETUP đầu phiên.
     */
    pthread_mutex_lock(&g_mux_seq_lock);
    g_mux_tx_seq = 0;
    g_mux_rx_seq = 0xFFFF;
    pthread_mutex_unlock(&g_mux_seq_lock);
    LOGI("usbmuxd_server_reset_version_state: reset — session mới sẽ version exchange lại");
}

/* ── Gửi TCP packet lên iPhone (qua USB) ────────────────────────────────── */
static int usb_send_tcp(tcp_state_t *st, uint8_t flags,
                         const void *data, uint32_t data_len) {
    uint32_t total = sizeof(v1_mux_hdr_t) + sizeof(v1_tcp_hdr_t) + data_len;
    uint8_t *pkt = malloc(total);
    if (!pkt) return -1;
    memset(pkt, 0, total);

    v1_mux_hdr_t *mhdr = (v1_mux_hdr_t *)pkt;
    mhdr->protocol = htonl(V1_PROTO_TCP);
    mhdr->length   = htonl(total);
    mhdr->magic    = htonl(V1_MAGIC);
    /*
     * FIX v30: dùng mux-level tx_seq/rx_seq CHUNG cho cả phiên USB (khớp
     * dev->tx_seq/dev->rx_seq trong device.c thật), không hardcode 0/0.
     * tx_seq tăng dần sau MỖI packet gửi đi (SETUP hoặc TCP); rx_seq gửi
     * đi luôn là giá trị rx_seq mới nhất mà ta nhận được từ iPhone (xem
     * usb_recv_tcp() — nơi cập nhật g_mux_rx_seq).
     */
    pthread_mutex_lock(&g_mux_seq_lock);
    mhdr->tx_seq = htons(g_mux_tx_seq);
    mhdr->rx_seq = htons(g_mux_rx_seq);
    g_mux_tx_seq++;
    pthread_mutex_unlock(&g_mux_seq_lock);

    v1_tcp_hdr_t *thdr = (v1_tcp_hdr_t *)(pkt + sizeof(v1_mux_hdr_t));
    thdr->sport  = htons(st->sport);
    thdr->dport  = htons(st->dport);
    thdr->seq    = htonl(st->local_seq);
    thdr->ack    = htonl(st->remote_seq);
    thdr->off    = 0x50;  /* 20 bytes / 4 = 5 → 0x50 */
    thdr->flags  = flags;
    /*
     * FIX (Bug 5 — HIGH): TCP receive window 512 bytes (0x0200) is far too small.
     * The lockdownd TLS certificate exchange payload regularly exceeds 512 bytes.
     * When the iPhone's TCP stack sees window=512 it throttles/fragments the
     * response into many tiny segments which stalls the connection before the
     * Trust prompt is ever shown.  Use the maximum 16-bit window (64 KB).
     */
    thdr->window = htons(0xFFFF);  /* FIX: 65535 bytes — full 16-bit window */
    thdr->cksum  = 0;
    thdr->urgp   = 0;

    if (data && data_len > 0)
        memcpy(pkt + sizeof(v1_mux_hdr_t) + sizeof(v1_tcp_hdr_t), data, data_len);

    int r;
    pthread_mutex_lock(&st->usb_tx_lock);
    r = usb_write(pkt, (int)total);
    pthread_mutex_unlock(&st->usb_tx_lock);
    free(pkt);
    return r > 0 ? 0 : -1;
}

/* ── Nhận TCP packet từ iPhone và xử lý ────────────────────────────────── */
/* Trả: data_len nếu có data, 0 nếu chỉ là ACK/SYN-ACK/FIN, -1 nếu lỗi */
static int usb_recv_tcp(tcp_state_t *st, void *data_out, int max_data,
                         uint8_t *flags_out, int timeout_ms) {
    v1_mux_hdr_t mhdr;
    int n = usb_read_exact(&mhdr, sizeof(mhdr), timeout_ms);
    if (n < (int)sizeof(mhdr)) return -1;

    if (ntohl(mhdr.magic) != V1_MAGIC) {
        LOGE("usb_recv_tcp: bad magic=0x%08x", ntohl(mhdr.magic));
        return -1;
    }

    /*
     * FIX v30: cập nhật mux-level rx_seq từ MỌI packet nhận được (khớp
     * device_data_input() thật: "if (dev->version >= 2) dev->rx_seq =
     * ntohs(mhdr->rx_seq)"). Giá trị này được echo lại trong header của
     * packet KẾ TIẾP mà ta gửi đi (xem usb_send_tcp()).
     */
    pthread_mutex_lock(&g_mux_seq_lock);
    g_mux_rx_seq = ntohs(mhdr.rx_seq);
    pthread_mutex_unlock(&g_mux_seq_lock);

    if (ntohl(mhdr.protocol) != V1_PROTO_TCP) {
        /* FIX v30: không phải TCP — thường là MUX_PROTO_CONTROL (log/error
         * message nội bộ từ device, xem device_control_input() thật).
         * An toàn để bỏ qua: drain phần body rồi tiếp tục. */
        uint32_t body_len = ntohl(mhdr.length) - sizeof(mhdr);
        if (body_len > 0 && body_len < 4096) {
            uint8_t *drain = malloc(body_len);
            if (drain) { usb_read_exact(drain, body_len, 2000); free(drain); }
        }
        return 0;
    }

    v1_tcp_hdr_t thdr;
    if (usb_read_exact(&thdr, sizeof(thdr), 2000) < (int)sizeof(thdr)) return -1;

    uint32_t total_len = ntohl(mhdr.length);
    uint32_t tcp_hdr_len = (thdr.off >> 4) * 4;
    uint32_t data_len_raw = total_len - sizeof(v1_mux_hdr_t) - tcp_hdr_len;

    /* Đọc TCP options nếu có (tcp_hdr_len > 20) */
    if (tcp_hdr_len > sizeof(thdr)) {
        uint32_t opts_len = tcp_hdr_len - sizeof(thdr);
        if (opts_len > 40) opts_len = 40;
        uint8_t opts_buf[40];
        if (usb_read_exact(opts_buf, opts_len, 2000) < (int)opts_len) return -1;
    }

    if (flags_out) *flags_out = thdr.flags;

    /* Cập nhật remote_seq từ ack + seq fields */
    uint32_t iphone_seq = ntohl(thdr.seq);

    /* Đọc data nếu có */
    int data_read = 0;
    if (data_len_raw > 0) {
        int to_read = (int)(data_len_raw < (uint32_t)max_data ? data_len_raw : (uint32_t)max_data);
        int n2 = usb_read_exact(data_out, to_read, 5000);
        if (n2 < to_read) return -1;
        data_read = n2;
        /* Drain extra nếu data_len_raw > max_data */
        if (data_len_raw > (uint32_t)max_data) {
            int extra = (int)(data_len_raw - (uint32_t)max_data);
            uint8_t *drain = malloc(extra);
            if (drain) { usb_read_exact(drain, extra, 2000); free(drain); }
        }
        st->remote_seq = iphone_seq + data_read;  /* ACK đến cuối data */
    } else {
        /* Pure ACK/SYN+ACK/FIN — cập nhật remote_seq từ seq */
        if (thdr.flags & TH_SYN) {
            /* iPhone's seq là ISN của nó, ACK sau SYN+ACK phải là ISN+1 */
            st->remote_seq = iphone_seq + 1;
        }
    }

    return data_read;
}

/*
 * do_usb_v1_connect — Thiết lập kết nối TCP đến `port` trên iPhone
 * qua USB v1 protocol: SYN → SYN+ACK → ACK.
 *
 * FIX v22 Bug C: Thay thế hoàn toàn v0 binary CONNECT bằng v1 TCP handshake
 * vì iPhone hiện đại (iOS 7+) chỉ hỗ trợ v1 protocol.
 *
 * LƯU Ý (fix mới, Bug B): version exchange KHÔNG còn nằm trong hàm này.
 * Nó chỉ chạy một lần/session, xem usbmux_version_exchange().
 */
static bool do_usb_v1_connect(tcp_state_t *st, int port) {
    LOGI("do_usb_v1_connect: port=%d", port);

    /* Bước 0: Init TCP state */
    srand((unsigned)time(NULL));
    st->sport      = (uint16_t)(49152 + (rand() % 16383));
    st->dport      = (uint16_t)port;
    st->local_seq  = (uint32_t)(rand());
    st->remote_seq = 0;
    pthread_mutex_init(&st->usb_tx_lock, NULL);

    /* FIX v33: Đảm bảo version exchange OK */
    if (!usbmux_version_exchange()) {
        LOGE("do_usb_v1_connect: ❌ version exchange thất bại");
        return false;
    }
    LOGI("do_usb_v1_connect: ✅ version exchange OK");

    /* FIX v33 (CRITICAL): Drain non-TCP packets trước SYN */
    LOGI("do_usb_v1_connect: Drain non-TCP packets...");
    int drained = 0;
    for (int i = 0; i < 10; i++) {
        v1_mux_hdr_t mhdr;
        int n = usb_read_exact(&mhdr, sizeof(mhdr), 300);
        if (n < (int)sizeof(mhdr)) break;

        if (ntohl(mhdr.magic) != V1_MAGIC) {
            LOGW("do_usb_v1_connect: drain bad magic, stop");
            break;
        }

        uint32_t proto = ntohl(mhdr.protocol);
        uint32_t body_len = ntohl(mhdr.length) - sizeof(mhdr);

        if (proto == V1_PROTO_TCP) {
            LOGW("do_usb_v1_connect: drain: unexpected TCP, skip");
            if (body_len > 0 && body_len < 65536) {
                uint8_t *db = malloc(body_len);
                if (db) { usb_read_exact(db, body_len, 1000); free(db); }
            }
            drained++; continue;
        }

        if (body_len > 0 && body_len < 65536) {
            uint8_t *db = malloc(body_len);
            if (db) {
                int r = usb_read_exact(db, body_len, 1000);
                if (r > 0 && proto == V1_PROTO_CONTROL) {
                    LOGI("do_usb_v1_connect: drain CONTROL type=%d", db[0]);
                }
                free(db);
            }
        }
        drained++;
    }
    if (drained > 0) LOGI("do_usb_v1_connect: drained %d packets", drained);

    /* Bước 1: SYN */
    uint32_t isn = st->local_seq;
    LOGI("do_usb_v1_connect: Gửi SYN port=%d sport=%d...", port, st->sport);
    if (usb_send_tcp(st, TH_SYN, NULL, 0) < 0) {
        LOGE("do_usb_v1_connect: ❌ SYN failed");
        return false;
    }
    LOGI("do_usb_v1_connect: ✅ SYN sent seq=%u", isn);

    /* Bước 2: Nhận SYN+ACK */
    int synack_timeout = 20000;
    uint8_t flags = 0;
    uint8_t dummy[1];
    int n = usb_recv_tcp(st, dummy, sizeof(dummy), &flags, synack_timeout);
    if (n < 0) {
        LOGE("do_usb_v1_connect: ❌ SYN+ACK timeout (%dms)", synack_timeout);
        return false;
    }
    if (!(flags & TH_SYN) || !(flags & TH_ACK)) {
        if (flags & TH_RST) {
            LOGE("do_usb_v1_connect: ❌ RST received — port %d refused", port);
        } else {
            LOGE("do_usb_v1_connect: ❌ flags=0x%02x (not SYN+ACK)", flags);
        }
        return false;
    }
    LOGI("do_usb_v1_connect: ✅ SYN+ACK remote_seq=%u", st->remote_seq);

    /* Bước 3: ACK */
    st->local_seq = isn + 1;
    if (usb_send_tcp(st, TH_ACK, NULL, 0) < 0) {
        LOGE("do_usb_v1_connect: ❌ ACK failed");
        return false;
    }
    LOGI("do_usb_v1_connect: ✅ TCP port=%d OK", port);
    return true;
}

/* ════════════════════════════════════════════════════════════════════════
 * Tunnel threads — sau khi v1 TCP kết nối thành công
 *
 * FIX v22 Bug B: Tunnel threads đã được viết lại hoàn toàn.
 *
 * socket → USB (sock_to_usb):
 *   - Đọc raw bytes từ socket (libimobiledevice gửi raw TCP data)
 *   - Wrap trong v1 TCP DATA packet (mux_hdr + tcp_hdr + raw_data)
 *   - Gửi lên iPhone qua USB
 *
 * USB → socket (usb_to_sock):
 *   - Nhận v1 TCP DATA packet từ iPhone
 *   - Strip mux_hdr + tcp_hdr headers
 *   - Gửi raw bytes vào socket (libimobiledevice đọc raw)
 *   - Gửi ACK phản hồi lên iPhone
 * ════════════════════════════════════════════════════════════════════════ */

typedef struct {
    int            sock_fd;
    tcp_state_t   *st;
    volatile int  *running;
} tunnel_arg_t;

/*
 * thread_sock_to_usb (FIX v22 Bug B):
 * Đọc raw bytes từ Unix socket, wrap trong v1 TCP DATA, gửi lên iPhone.
 */
static void *thread_sock_to_usb(void *arg) {
    tunnel_arg_t *ta = (tunnel_arg_t *)arg;
    int sock = ta->sock_fd;
    tcp_state_t *st = ta->st;
    uint8_t *buf = malloc(TUNNEL_BUFSIZE);
    if (!buf) { *(ta->running) = 0; free(ta); return NULL; }

    while (*(ta->running)) {
        /* Đọc raw bytes từ socket (libimobiledevice gửi raw lockdown/AFC data) */
        int n = (int)read(sock, buf, TUNNEL_BUFSIZE);
        if (n <= 0) {
            LOGI("sock_to_usb: socket đóng (n=%d errno=%d)", n, errno);
            break;
        }

        /* Gửi data dưới dạng TCP PSH+ACK đến iPhone qua USB */
        if (usb_send_tcp(st, TH_PUSH | TH_ACK, buf, (uint32_t)n) < 0) {
            LOGE("sock_to_usb: USB write thất bại");
            break;
        }
        st->local_seq += (uint32_t)n;  /* Advance sequence number */
    }

    free(buf);
    *(ta->running) = 0;
    free(ta);
    return NULL;
}

/*
 * thread_usb_to_sock (FIX v22 Bug B):
 * Nhận v1 TCP DATA từ iPhone, strip headers, gửi raw vào socket.
 * Gửi ACK phản hồi sau mỗi data packet.
 */
static void *thread_usb_to_sock(void *arg) {
    tunnel_arg_t *ta = (tunnel_arg_t *)arg;
    int sock = ta->sock_fd;
    tcp_state_t *st = ta->st;
    uint8_t *buf = malloc(TUNNEL_BUFSIZE);
    if (!buf) { *(ta->running) = 0; free(ta); return NULL; }

    while (*(ta->running)) {
        uint8_t flags = 0;
        int n = usb_recv_tcp(st, buf, TUNNEL_BUFSIZE, &flags, 2000);

        if (n < 0) {
            LOGE("usb_to_sock: USB read thất bại");
            break;
        }

        /* Kiểm tra FIN từ iPhone — kết nối đóng */
        if (flags & TH_FIN) {
            LOGI("usb_to_sock: nhận FIN từ iPhone — đóng tunnel");
            /* Gửi FIN+ACK phản hồi */
            st->remote_seq++;  /* FIN tiêu thụ 1 sequence */
            usb_send_tcp(st, TH_FIN | TH_ACK, NULL, 0);
            break;
        }

        if (n > 0) {
            /* Ghi raw bytes vào socket (libimobiledevice expect raw data) */
            if (sock_write_all(sock, buf, n) < 0) {
                LOGI("usb_to_sock: socket write thất bại — kết thúc");
                break;
            }

            /* Gửi ACK phản hồi lên iPhone */
            usb_send_tcp(st, TH_ACK, NULL, 0);
        }
        /* n=0 → pure ACK hoặc timeout — tiếp tục */
    }

    free(buf);
    *(ta->running) = 0;
    free(ta);
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════
 * TCP dual-socket proxy — học từ termux-usbmuxd/usbmuxd_proxy.c + socat.
 *
 * Mỗi kết nối TCP được bridged vào Unix socket server nội bộ bằng hai
 * thread proxy (một chiều mỗi thread), giống cách socat hoạt động trong
 * termux-usbmuxd nhưng không cần cài thêm gói nào trên Android.
 * ════════════════════════════════════════════════════════════════════════ */

typedef struct { int src; int dst; } proxy_half_args_t;

static void *proxy_half(void *arg) {
    proxy_half_args_t *p = (proxy_half_args_t *)arg;
    int src = p->src, dst = p->dst;
    free(p);
    signal(SIGPIPE, SIG_IGN); /* bỏ qua broken pipe — học từ usbmuxd_proxy.c */
    char buf[4096];
    while (1) {
        int n = (int)read(src, buf, sizeof(buf));
        if (n <= 0) break;
        int sent = 0;
        while (sent < n) {
            int w = (int)write(dst, buf + sent, n - sent);
            if (w <= 0) goto done_half;
            sent += w;
        }
    }
done_half:
    shutdown(src, SHUT_RDWR);
    shutdown(dst, SHUT_RDWR);
    return NULL;
}

static void *handle_tcp_client(void *arg) {
    int tcp_fd = *(int *)arg;
    free(arg);
    signal(SIGPIPE, SIG_IGN);

    /* Kết nối đến Unix socket server nội bộ */
    int unix_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (unix_fd < 0) { close(tcp_fd); return NULL; }

    struct sockaddr_un ua;
    memset(&ua, 0, sizeof(ua));
    ua.sun_family = AF_UNIX;
    strncpy(ua.sun_path, g_sock_path, sizeof(ua.sun_path) - 1);

    /* Chờ Unix socket sẵn sàng — giống poll loop trong usbmuxd_proxy.c */
    int waited_ms = 0;
    while (connect(unix_fd, (struct sockaddr *)&ua, sizeof(ua)) < 0) {
        if (!g_running || waited_ms >= 3000) {
            LOGE("tcp_client: Unix socket không ready sau %dms", waited_ms);
            close(unix_fd); close(tcp_fd); return NULL;
        }
        usleep(100 * 1000);
        waited_ms += 100;
    }

    /* Hai thread proxy bidirectional (TCP ↔ Unix) */
    proxy_half_args_t *p1 = malloc(sizeof(proxy_half_args_t));
    proxy_half_args_t *p2 = malloc(sizeof(proxy_half_args_t));
    if (!p1 || !p2) { free(p1); free(p2); close(unix_fd); close(tcp_fd); return NULL; }
    p1->src = tcp_fd;  p1->dst = unix_fd;
    p2->src = unix_fd; p2->dst = tcp_fd;

    pthread_t t1, t2;
    pthread_create(&t1, NULL, proxy_half, p1);
    pthread_create(&t2, NULL, proxy_half, p2);
    pthread_detach(t1);
    pthread_join(t2, NULL);

    close(unix_fd);
    close(tcp_fd);
    return NULL;
}

static void *tcp_server_thread(void *arg) {
    (void)arg;
    /* Học từ usbmuxd_proxy.c: ignore SIGPIPE để tránh crash khi iPhone
     * ngắt kết nối giữa chừng (EPIPE = broken pipe). */
    signal(SIGPIPE, SIG_IGN);
    LOGI("tcp_server_thread: listening trên 127.0.0.1:%d", USBMUXD_TCP_PORT);

    while (g_tcp_running) {
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(g_tcp_fd, &rset);
        int r = select(g_tcp_fd + 1, &rset, NULL, NULL, &tv);
        if (r < 0) { if (errno == EINTR) continue; LOGE("tcp_server: select err=%d", errno); break; }
        if (r == 0) continue;
        int cfd = accept(g_tcp_fd, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR || errno == EAGAIN) continue; continue; }
        LOGI("tcp_server: client kết nối fd=%d", cfd);
        int *fa = malloc(sizeof(int));
        if (!fa) { close(cfd); continue; }
        *fa = cfd;
        pthread_t t;
        if (pthread_create(&t, NULL, handle_tcp_client, fa) == 0) pthread_detach(t);
        else { close(cfd); free(fa); }
    }
    LOGI("tcp_server_thread: kết thúc");
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════
 * FIX (báo cáo lỗi #2/#3/#7) — Broadcast "Attached" tới mọi client Listen
 * ════════════════════════════════════════════════════════════════════════
 *
 * TRƯỚC ĐÂY: "Attached" event chỉ được gửi MỘT LẦN, ngay trong cùng thread
 * xử lý client vừa gửi "Listen" (xem nhánh msg_type=="Listen" bên dưới).
 * Điều này gây hai vấn đề:
 *
 *   (a) Nếu client đó đã disconnect trước khi kịp nhận event, hoặc
 *       libusbmuxd tạo một kết nối "Listen" khác song song, server không
 *       có cách nào gửi lại "Attached" cho các client Listen khác.
 *
 *   (b) UDID thật thường chỉ được biết SAU khi client đầu tiên đã Listen
 *       xong (luồng thật: nativeSetUsbFd() → usbmuxd_server_start() với
 *       UDID placeholder → nativeConnect() → idevice_new_with_options()
 *       gửi Listen/ListDevices → device_get_udid() → update_udid()).
 *       Client đã Listen từ bước 1 chỉ thấy UDID placeholder mãi mãi vì
 *       không có cơ chế thông báo lại.
 *
 * FIX: giữ một danh sách mọi client fd đang ở trạng thái Listen
 * (register_listener() khi nhận "Listen", unregister_listener() khi
 * client đóng kết nối hoặc chuyển sang tunnel dữ liệu qua "Connect").
 * broadcast_attached() gửi "Attached" event (dùng UDID mới nhất) tới toàn
 * bộ danh sách này — được gọi từ usbmuxd_server_update_udid() và có thể
 * gọi tường minh qua usbmuxd_server_broadcast_attached().
 * ════════════════════════════════════════════════════════════════════════ */
#define MAX_LISTEN_CLIENTS 16
static pthread_mutex_t g_listen_lock            = PTHREAD_MUTEX_INITIALIZER;
static int             g_listen_fds[MAX_LISTEN_CLIENTS];
static int             g_listen_count           = 0;

static void register_listener(int fd) {
    pthread_mutex_lock(&g_listen_lock);
    for (int i = 0; i < g_listen_count; i++) {
        if (g_listen_fds[i] == fd) { pthread_mutex_unlock(&g_listen_lock); return; }
    }
    if (g_listen_count < MAX_LISTEN_CLIENTS) {
        g_listen_fds[g_listen_count++] = fd;
        LOGI("register_listener: fd=%d — tổng %d client đang Listen", fd, g_listen_count);
    } else {
        LOGE("register_listener: danh sách đầy (%d) — bỏ qua fd=%d", MAX_LISTEN_CLIENTS, fd);
    }
    pthread_mutex_unlock(&g_listen_lock);
}

static void unregister_listener(int fd) {
    pthread_mutex_lock(&g_listen_lock);
    for (int i = 0; i < g_listen_count; i++) {
        if (g_listen_fds[i] == fd) {
            g_listen_fds[i] = g_listen_fds[g_listen_count - 1];
            g_listen_count--;
            break;
        }
    }
    pthread_mutex_unlock(&g_listen_lock);
}

static void broadcast_attached(void) {
    int fds_copy[MAX_LISTEN_CLIENTS];
    int count;

    pthread_mutex_lock(&g_listen_lock);
    count = g_listen_count;
    memcpy(fds_copy, g_listen_fds, sizeof(int) * (size_t)count);
    pthread_mutex_unlock(&g_listen_lock);

    if (count == 0) return;

    char *event = make_attached_event();
    if (!event) return;

    int dead[MAX_LISTEN_CLIENTS];
    int dead_count = 0;
    for (int i = 0; i < count; i++) {
        if (send_plist(fds_copy[i], 0, event) < 0) {
            LOGE("broadcast_attached: gửi thất bại fd=%d — client có thể đã đóng", fds_copy[i]);
            dead[dead_count++] = fds_copy[i];
        }
    }
    free(event);

    /* Dọn các fd đã chết khỏi danh sách (tránh cố gửi lại lần sau) */
    if (dead_count > 0) {
        pthread_mutex_lock(&g_listen_lock);
        for (int i = 0; i < dead_count; i++) {
            for (int j = 0; j < g_listen_count; j++) {
                if (g_listen_fds[j] == dead[i]) {
                    g_listen_fds[j] = g_listen_fds[g_listen_count - 1];
                    g_listen_count--;
                    break;
                }
            }
        }
        pthread_mutex_unlock(&g_listen_lock);
    }

    LOGI("broadcast_attached: đã gửi Attached event (UDID mới) tới %d client Listen", count);
}

void usbmuxd_server_broadcast_attached(void) {
    broadcast_attached();
}

/* ════════════════════════════════════════════════════════════════════════
 * Client handler — xử lý một kết nối từ libimobiledevice
 * ════════════════════════════════════════════════════════════════════════ */

static void *handle_client(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);
    signal(SIGPIPE, SIG_IGN); /* bỏ qua SIGPIPE — học từ usbmuxd_proxy.c */

    LOGI("client thread: fd=%d", client_fd);
    volatile int tunnel_running = 1;

    while (1) {
        umux_hdr_t hdr;
        char *xml = recv_plist(client_fd, &hdr);
        if (!xml) {
            LOGI("client fd=%d: đọc request thất bại hoặc kết nối đóng", client_fd);
            break;
        }

        const char *msg_type = extract_str(xml, "MessageType");
        LOGI("client fd=%d: MessageType=%s", client_fd, msg_type ? msg_type : "(null)");

        if (!msg_type) { free(xml); break; }

        if (strcmp(msg_type, "Hello") == 0) {
            /* Version check — phản hồi OK */
            char *resp = make_result_ok();
            if (resp) { send_plist(client_fd, hdr.tag, resp); free(resp); }
            free(xml);

        } else if (strcmp(msg_type, "Listen") == 0) {
            LOGI("client_thread: nhận Listen");
            char *resp = make_result_ok();
            if (resp) {
                send_plist(client_fd, hdr.tag, resp);
                free(resp);
            }
            /*
             * FIX (Bug 6 — HIGH): free(xml) was MISSING here.
             * Every Listen request body was leaked.  Non-fatal individually but
             * accumulates over many reconnects / device list polls.
             */
            free(xml);  /* FIX: release the request plist body */

            /*
             * FIX (báo cáo lỗi #2/#3): trước đây chỉ gửi "Attached" một lần
             * ngay tại đây cho riêng client này rồi thôi — không track lại
             * client_fd ở đâu cả, nên nếu UDID cập nhật sau (real UDID từ
             * idevice_get_udid()) thì client này (và mọi client Listen
             * khác) không bao giờ nhận được event mới.
             *
             * Fix: đăng ký client_fd vào danh sách "đang Listen" rồi dùng
             * broadcast_attached() — gửi ngay lập tức (đúng hành vi cũ,
             * client mới Listen vẫn cần Attached ngay) NHƯNG cũng tự động
             * gửi lại cho MỌI client khác mỗi khi UDID được cập nhật qua
             * usbmuxd_server_update_udid()/usbmuxd_server_broadcast_attached().
             */
            register_listener(client_fd);
            /* FIX: Only broadcast Attached when real UDID is present. */
            pthread_mutex_lock(&g_udid_mutex);
            int has_udid = (g_udid[0] != '\0');
            pthread_mutex_unlock(&g_udid_mutex);
            if (has_udid) {
                broadcast_attached();
            } else {
                LOGI("handle_listen: fd=%d registered, UDID not ready — will auto-broadcast later", client_fd);
            }
        } else if (strcmp(msg_type, "ListDevices") == 0) {
            /* FIX: Return empty list when UDID not ready. */
            pthread_mutex_lock(&g_udid_mutex);
            int has_udid = (g_udid[0] != '\0');
            pthread_mutex_unlock(&g_udid_mutex);
            char *resp = has_udid ? make_device_list() : make_empty_device_list();
            if (resp) { send_plist(client_fd, hdr.tag, resp); free(resp); }
            free(xml);

        } else if (strcmp(msg_type, "ReadBUID") == 0) {
            char *resp = NULL;
            asprintf(&resp,
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                "<plist version=\"1.0\"><dict>"
                "<key>MessageType</key><string>Result</string>"
                "<key>Number</key><integer>0</integer>"
                "<key>BUID</key><string>00000000-0000-0000-0000-000000000000</string>"
                "</dict></plist>");
            if (resp) { send_plist(client_fd, hdr.tag, resp); free(resp); }
            free(xml);

        } else if (strcmp(msg_type, "ReadPairRecord") == 0) {
            /* Trả kết quả rỗng — libimobiledevice sẽ tự pair */
            char *resp = NULL;
            asprintf(&resp,
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                "<plist version=\"1.0\"><dict>"
                "<key>MessageType</key><string>Result</string>"
                "<key>Number</key><integer>0</integer>"
                "</dict></plist>");
            if (resp) { send_plist(client_fd, hdr.tag, resp); free(resp); }
            free(xml);

        } else if (strcmp(msg_type, "Connect") == 0) {
            long port_be = extract_int(xml, "PortNumber");
            /* PortNumber trong plist là big-endian */
            int port = (int)ntohs((uint16_t)(port_be & 0xFFFF));
            free(xml);

            LOGI("client fd=%d: Connect port_be=%ld → port=%d", client_fd, port_be, port);

            /*
             * FIX (an toàn cho broadcast_attached()): nếu client_fd này
             * từng gửi "Listen" trước đó trên cùng kết nối (không phải
             * pattern thường gặp, nhưng phải phòng thủ), gỡ nó khỏi danh
             * sách broadcast NGAY trước khi chuyển fd sang chế độ tunnel
             * dữ liệu thô. Nếu không, một lần usbmuxd_server_update_udid()
             * xảy ra giữa lúc tunnel đang chạy sẽ ghi đè plist "Attached"
             * chồng lên luồng byte TCP thô → hỏng dữ liệu tunnel.
             */
            unregister_listener(client_fd);

            /*
             * FIX v22 Bug B+C: Dùng v1 TCP handshake thay vì v0 binary CONNECT
             */
            tcp_state_t *st = malloc(sizeof(tcp_state_t));
            bool ok = false;
            if (st) {
                ok = do_usb_v1_connect(st, port);
            }

            /* Báo kết quả cho libimobiledevice */
            char *resp = make_connect_result(ok ? 0 : 3);  /* 3 = ECONNREFUSED */
            if (resp) { send_plist(client_fd, hdr.tag, resp); free(resp); }

            if (!ok) {
                LOGE("client fd=%d: v1 connect thất bại cho port=%d", client_fd, port);
                if (st) { pthread_mutex_destroy(&st->usb_tx_lock); free(st); }
                break;
            }

            LOGI("client fd=%d: bắt đầu tunnel v1 port=%d", client_fd, port);

            /* Bắt đầu 2 tunnel threads */
            tunnel_arg_t *ta1 = malloc(sizeof(tunnel_arg_t));
            tunnel_arg_t *ta2 = malloc(sizeof(tunnel_arg_t));
            if (!ta1 || !ta2) {
                free(ta1); free(ta2);
                pthread_mutex_destroy(&st->usb_tx_lock);
                free(st);
                break;
            }

            ta1->sock_fd = client_fd;  ta1->st = st;  ta1->running = &tunnel_running;
            ta2->sock_fd = client_fd;  ta2->st = st;  ta2->running = &tunnel_running;

            pthread_t t1, t2;
            pthread_create(&t1, NULL, thread_sock_to_usb, ta1);
            pthread_create(&t2, NULL, thread_usb_to_sock, ta2);
            pthread_detach(t1);
            pthread_detach(t2);

            /* Chờ đến khi một trong hai tunnel dừng */
            while (tunnel_running) usleep(100000);
            LOGI("client fd=%d: tunnel kết thúc", client_fd);

            /* Gửi FIN để đóng TCP connection phía iPhone */
            usb_send_tcp(st, TH_FIN | TH_ACK, NULL, 0);
            usleep(100000);  /* Cho iPhone xử lý FIN */

            pthread_mutex_destroy(&st->usb_tx_lock);
            free(st);
            break;

        } else {
            LOGI("client fd=%d: MessageType=%s không xử lý — gửi OK", client_fd, msg_type);
            free(xml);
            char *resp = make_result_ok();
            if (resp) { send_plist(client_fd, hdr.tag, resp); free(resp); }
        }
    }

    /*
     * FIX (báo cáo lỗi #2/#3): gỡ client_fd khỏi danh sách broadcast khi
     * đóng kết nối — tránh broadcast_attached() cố gửi vào fd đã đóng ở
     * lần cập nhật UDID kế tiếp (dead_count cleanup trong broadcast_attached()
     * cũng xử lý việc này, nhưng gỡ ngay tại đây tránh cửa sổ race và log
     * lỗi gửi thất bại không cần thiết).
     */
    unregister_listener(client_fd);

    // Gửi Detached event khi client đóng kết nối
    char *detached_event = make_detached_event();
    if (detached_event) {
        send_plist(client_fd, 0, detached_event); // tag=0 cho events
        free(detached_event);
    }

    close(client_fd);
    LOGI("client fd=%d: đóng kết nối", client_fd);
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════
 * Server accept loop
 * ════════════════════════════════════════════════════════════════════════ */

static void *server_thread(void *arg) {
    (void)arg;
    /* Học từ usbmuxd_proxy.c: bỏ qua SIGPIPE, SIGHUP để không bị kill
     * khi iPhone ngắt kết nối hoặc process bị detach khỏi terminal. */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP,  SIG_IGN);
    LOGI("server_thread: listen trên %s", g_sock_path);

    while (g_running) {
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(g_server_fd, &rset);
        int r = select(g_server_fd + 1, &rset, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            LOGE("server_thread: select() err=%d", errno);
            break;
        }
        if (r == 0) continue;

        int client_fd = accept(g_server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            LOGE("server_thread: accept() err=%d", errno);
            continue;
        }

        LOGI("server_thread: client kết nối fd=%d", client_fd);

        int *fd_arg = malloc(sizeof(int));
        if (!fd_arg) { close(client_fd); continue; }
        *fd_arg = client_fd;

        pthread_t t;
        if (pthread_create(&t, NULL, handle_client, fd_arg) != 0) {
            LOGE("server_thread: pthread_create thất bại");
            close(client_fd);
            free(fd_arg);
        } else {
            pthread_detach(t);
        }
    }
    LOGI("server_thread: kết thúc");
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════════════════════════════ */

bool usbmuxd_server_start(const char *files_dir, const char *udid, int product_id) {
    if (g_running) {
        LOGI("usbmuxd_server_start: đã chạy — dừng trước");
        usbmuxd_server_stop();
    }

    pthread_mutex_lock(&g_udid_mutex);
    strncpy(g_udid, (udid && udid[0]) ? udid : "00000000-0000000000000000",
            sizeof(g_udid) - 1);
    g_udid[63] = '\0';
    pthread_mutex_unlock(&g_udid_mutex);
    g_product_id = product_id;

    snprintf(g_sock_path, sizeof(g_sock_path), "%s/usbmuxd.sock",
             files_dir ? files_dir : "/tmp");
    unlink(g_sock_path);

    g_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        LOGE("usbmuxd_server_start: socket() err=%d", errno);
        return false;
    }

    int opt = 1;
    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_sock_path, sizeof(addr.sun_path) - 1);

    if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("usbmuxd_server_start: bind(%s) err=%d", g_sock_path, errno);
        close(g_server_fd); g_server_fd = -1;
        return false;
    }

    if (listen(g_server_fd, 8) < 0) {
        LOGE("usbmuxd_server_start: listen() err=%d", errno);
        close(g_server_fd); g_server_fd = -1;
        unlink(g_sock_path);
        return false;
    }

    /* ── Unix socket sẵn sàng — khởi động Unix listener thread ───────────── */
    g_running = 1;
    if (pthread_create(&g_srv_thread, NULL, server_thread, NULL) != 0) {
        LOGE("usbmuxd_server_start: pthread_create thất bại");
        g_running = 0;
        close(g_server_fd); g_server_fd = -1;
        unlink(g_sock_path);
        return false;
    }
    pthread_detach(g_srv_thread);
    LOGI("usbmuxd_server_start: ✅ Unix socket: %s", g_sock_path);

    /* ── Dual-socket: khởi động TCP listener (học từ termux-usbmuxd) ────────
     * Bên cạnh Unix socket, tạo thêm TCP listener trên 127.0.0.1:27015.
     * USBMUXD_SOCKET_ADDRESS được ưu tiên set về TCP form (host:port) vì
     * cả C tools lẫn Rust tools đều hiểu TCP, nhưng Rust tools (idevice-tools)
     * KHÔNG hiểu đường dẫn Unix socket thuần tuý — sẽ crash với:
     *   AddrParseError(Socket) (học từ termux-usbmuxd fix_shell_rc() comment).
     * Nếu TCP bind thất bại → fallback dùng Unix socket path (chỉ C tools). */
    /* FIX TCP: Thử nhiều port nếu 27015 bị chiếm */
    g_tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_tcp_fd >= 0) {
        int tcp_opt = 1;
        setsockopt(g_tcp_fd, SOL_SOCKET, SO_REUSEADDR, &tcp_opt, sizeof(tcp_opt));
        struct sockaddr_in tcp_addr;
        memset(&tcp_addr, 0, sizeof(tcp_addr));
        tcp_addr.sin_family      = AF_INET;
        tcp_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        int ports[] = {USBMUXD_TCP_PORT, 27016, 27017, 27018, 27019};
        for (int p = 0; p < 5; p++) {
            tcp_addr.sin_port = htons(ports[p]);
            if (bind(g_tcp_fd, (struct sockaddr *)&tcp_addr, sizeof(tcp_addr)) == 0
                && listen(g_tcp_fd, 8) == 0) {
                char tcp_sock_str[32];
                snprintf(tcp_sock_str, sizeof(tcp_sock_str), "127.0.0.1:%d", ports[p]);
                setenv("USBMUXD_SOCKET_ADDRESS", tcp_sock_str, 1);
                g_tcp_running = 1;
                if (pthread_create(&g_tcp_thread, NULL, tcp_server_thread, NULL) == 0) {
                    pthread_detach(g_tcp_thread);
                    LOGI("usbmuxd_server_start: ✅ TCP dual-socket: %s (udid=%s)", tcp_sock_str, g_udid);
                    break;
                } else {
                    LOGE("usbmuxd_server_start: TCP pthread_create thất bại port %d", ports[p]);
                    g_tcp_running = 0;
                    close(g_tcp_fd); g_tcp_fd = -1;
                }
            } else {
                if (p == 4) {
                    LOGE("usbmuxd_server_start: Tất cả TCP port đều bị chiếm — fallback Unix");
                    close(g_tcp_fd); g_tcp_fd = -1;
                    setenv("USBMUXD_SOCKET_ADDRESS", g_sock_path, 1);
                }
            }
        }
    } else {
        setenv("USBMUXD_SOCKET_ADDRESS", g_sock_path, 1);
    }

    LOGI("usbmuxd_server_start: ✅ USBMUXD_SOCKET_ADDRESS=%s", getenv("USBMUXD_SOCKET_ADDRESS") ?: "?");
    return true;
}

void usbmuxd_server_update_udid(const char *udid) {
    if (!udid) return;
    pthread_mutex_lock(&g_udid_mutex);
    strncpy(g_udid, udid, sizeof(g_udid) - 1);
    g_udid[63] = '\0';
    pthread_mutex_unlock(&g_udid_mutex);
    LOGI("usbmuxd_server_update_udid: %s", g_udid);

    /*
     * FIX (báo cáo lỗi #3): trước đây hàm này DỪNG LẠI Ở ĐÂY — chỉ cập
     * nhật g_udid trong bộ nhớ mà không thông báo cho bất kỳ client nào
     * đang Listen. Khi nativeConnect() gọi idevice_get_udid() rồi gọi hàm
     * này để cập nhật UDID thật (thay vì UDID placeholder lúc
     * usbmuxd_server_start()), mọi client đã Listen từ trước đó (VD:
     * chính idevice_new_with_options() vừa mới Listen để lấy device list
     * ban đầu) không bao giờ nhận được UDID mới → vẫn thấy UDID placeholder
     * trong DeviceList của họ.
     *
     * Fix: gửi lại "Attached" event (với UDID mới) cho MỌI client đang
     * Listen ngay sau khi cập nhật xong.
     */
    broadcast_attached();
}

const char *usbmuxd_server_socket_path(void) {
    if (!g_running || g_server_fd < 0) return NULL;
    return g_sock_path;
}

void usbmuxd_server_stop(void) {
    /* Dừng TCP dual-socket thread trước (học từ termux-usbmuxd stop flow) */
    g_tcp_running = 0;
    if (g_tcp_fd >= 0) {
        close(g_tcp_fd);
        g_tcp_fd = -1;
    }
    /* Dừng Unix socket server */
    g_running = 0;
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }
    if (g_sock_path[0]) {
        unlink(g_sock_path);
        LOGI("usbmuxd_server_stop: đã xóa socket %s", g_sock_path);
        g_sock_path[0] = '\0';
    }
    LOGI("usbmuxd_server_stop: done");
}

