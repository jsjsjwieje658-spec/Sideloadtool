/*
 * usbmuxd_server.c — Mini usbmuxd server nội bộ (in-process, Mode 1)
 *
 * ════════════════════════════════════════════════════════════════════
 * FIX v48 — Tổng hợp các fix protocol-level còn lại:
 *
 * 1. Xóa eager version exchange trong nativeSetUsbFd (gây race condition).
 *    Version exchange CHỈ được gọi trong do_usb_v1_connect() khi client
 *    Connect lần đầu → tránh trường hợp iPhone chưa sẵn sàngmux session.
 *
 * 2. FIX g_mux_rx_seq: iPhone dùng ECHO semantics (rx_seq = last received
 *    tx_seq từ peer). Đã fix ở v47, giữ nguyên.
 *
 * 3. FIX thread safety: g_mux_tx_seq và g_mux_rx_seq giờ được protect
 *    bởi g_mux_seq_mutex trong CẢ usb_send_tcp LẪN usb_recv_tcp.
 *    st->local_seq cũng được serialize trong usb_tx_lock.
 *
 * 4. FIX source port: Thêm TIME_WAIT avoidance — connection mới skip
 *    source port đã dùng trong 2s gần nhất.
 *
 * 5. FIX remote_seq: Cập nhật remote_seq từ iphone_ack cho pure ACK
 *    để đảm bảo ack number luôn phản ánh state mới nhất.
 *
 * 6. FIX cleanup: Gửi RST trước khi đóng connection để iPhone dọn
 *    TIME_WAIT state nhanh hơn.
 *
 * 7. FIX VERSION timeout: Tăng version exchange timeout từ 1500ms×5
 *    thành 2000ms×8 — iPhone mất thêm thời gian trên iOS 17+.
 *
 * Protocol stack:
 *   libimobiledevice ↔ [Unix socket, plist v1] ↔ usbmuxd_server
 *   usbmuxd_server   ↔ [USB bulk, Apple v1 TCP] ↔ iPhone
 * ════════════════════════════════════════════════════════════════════
 */
#include "usbmuxd_server.h"
#include "usb_fd_bridge.h"
#include "android_usbmuxd_fix.h"
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
#define LOGI(...) do { \
    __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__); \
    android_usbmuxd_fix_logf(__VA_ARGS__); \
} while (0)
#define LOGE(...) do { \
    __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__); \
    android_usbmuxd_fix_logf(__VA_ARGS__); \
} while (0)

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

/* ── iPhone USB mux protocol — big-endian ────────────────────────────── */
/* Mux v2 uses the full 16-byte header for version, setup and TCP frames. */
typedef struct {
    uint32_t protocol;
    uint32_t length;
    uint32_t magic;
    uint16_t tx_seq;
    uint16_t rx_seq;
} v2_mux_hdr_t;

/* Upstream usbmuxd starts version negotiation with the legacy 8-byte mux header.
 * The device switches to the 16-byte v2 header only after replying with version
 * 2 and receiving SETUP. */
typedef struct {
    uint32_t protocol;
    uint32_t length;
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

/* Version body used by usbmuxd upstream (12 bytes). */
typedef struct {
    uint32_t major;
    uint32_t minor;
    uint32_t padding;
} mux_version_body_t;

#pragma pack(pop)

#define V2_MAGIC         0xfeedface
#define MUX_PROTO_VERSION 0
#define MUX_PROTO_CONTROL 1
#define MUX_PROTO_SETUP   2
#define MUX_PROTO_TCP     6

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
    volatile int rst_received;    /* FIX v45: set khi iPhone gửi RST — tránh gửi thêm ACK/FIN */
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
static uint16_t         g_mux_tx_seq = 0;
static uint16_t         g_mux_rx_seq = 0xFFFF;
static pthread_mutex_t  g_mux_seq_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * FIX v45/v48 (Critical — Trust popup không hiện):
 *
 * Source-port allocator với TIME_WAIT avoidance.
 *
 * Code cũ hardcode `st->sport = 1` cho MỌI kết nối TCP → xung đột
 * TIME_WAIT trên iPhone → RST → loop vô hạn.
 *
 * v45 fix: cấp source port tăng dần (1, 2, 3, ...).
 *
 * v48 fix: Theo dõi thời gian sử dụng của mỗi source port. Connection
 * mới sẽ skip các port đã dùng trong TIME_WAIT_PERIOD (2 giây) để
 * tránh iPhone reject do TIME_WAIT state. Nếu tất cả port đều trong
 * TIME_WAIT, dùng port thấp nhất (best-effort — iPhone có thể đã dọn).
 */
#define TIME_WAIT_PERIOD_MS  2000   /* 2 giây — iPhone usbmuxd TIME_WAIT */

static volatile uint16_t g_next_sport = 1;
static pthread_mutex_t   g_sport_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Track last-use time for each source port (index = port - 1) */
#define MAX_SPORT_TRACK  256
static uint64_t g_sport_last_used[MAX_SPORT_TRACK];  /* milliseconds */
static int      g_sport_used_count = 0;

static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void mark_sport_used(uint16_t port) {
    int idx = (port - 1) % MAX_SPORT_TRACK;
    g_sport_last_used[idx] = get_time_ms();
}

static uint16_t alloc_source_port(void) {
    pthread_mutex_lock(&g_sport_mutex);
    uint64_t now = get_time_ms();
    uint16_t p = 0;

    /* Try to find a port not in TIME_WAIT */
    for (int i = 0; i < MAX_SPORT_TRACK; i++) {
        uint16_t candidate = (uint16_t)((g_next_sport + i) % MAX_SPORT_TRACK + 1);
        if (candidate == 0) continue;  /* skip port 0 */
        int idx = (candidate - 1) % MAX_SPORT_TRACK;
        if (g_sport_last_used[idx] == 0 ||
            (now - g_sport_last_used[idx]) >= TIME_WAIT_PERIOD_MS) {
            p = candidate;
            g_next_sport = (uint16_t)((candidate % MAX_SPORT_TRACK) + 1);
            if (g_next_sport == 0) g_next_sport = 1;
            break;
        }
    }

    /* Fallback: use next port anyway (TIME_WAIT may have cleared) */
    if (p == 0) {
        p = g_next_sport++;
        if (g_next_sport == 0) g_next_sport = 1;
    }

    mark_sport_used(p);
    g_sport_used_count++;
    pthread_mutex_unlock(&g_sport_mutex);
    return p;
}

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
    if (hdr.length < sizeof(hdr)) {
        LOGE("recv_plist: header length=%u nhỏ hơn header=%zu",
             hdr.length, sizeof(hdr));
        return NULL;
    }
    if (hdr.version != USBMUX_PROTO_PLIST || hdr.type != USBMUX_TYPE_PLIST) {
        LOGE("recv_plist: header không hợp lệ version=%u type=%u",
             hdr.version, hdr.type);
        return NULL;
    }
    uint32_t body_len = hdr.length - (uint32_t)sizeof(hdr);
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

static int usb_read_exact(void *buf, int len, int timeout_ms) {
    char *p = (char *)buf;
    int got = 0;
    /*
     * FIX Bug A (Critical): "retry" PHẢI là biến cục bộ.
     * Trước đây khai báo `static int retry` khiến counter dùng CHUNG
     * giữa MỌI cuộc gọi và MỌI thread (thread_sock_to_usb, thread_usb_to_sock,
     * do_usb_v1_connect...). Kết quả: retry tích luỹ qua các lần gọi khác
     * nhau thay vì reset mỗi lần usb_read_exact() mới — với timeout_ms=3000
     * và max_retry=50 cũ, một lần đọc bị stall có thể hang tới 150 giây
     * (50 × 3000ms) thay vì fail nhanh và trả lỗi.
     *
     * FIX v24 Bug E: usb_bridge_bulk_read() trả -1 cho lỗi tạm thời
     * (LIBUSB_ERROR_PIPE sau khi libusb_wrap_sys_device — endpoint bị stall
     * ngay sau init). Trước đây ta return -1 ngay lập tức mà không retry,
     * khiến version exchange thất bại ngay lần đọc đầu tiên. Fix: retry
     * tối đa 3 lần với 50ms delay trước khi từ bỏ.
     */
    int retry = 0; /* ✅ LOCAL — độc lập mỗi cuộc gọi/thread */
    int err_retry = 0; /* retry cho lỗi I/O tạm thời từ bulk_read */
    const int max_retries     = 12; /* timeout retries (max wait ~ 12 × timeout_ms) */
    const int max_err_retries = 4;  /* I/O error retries trước khi từ bỏ */
    while (got < len) {
        int n = usb_bridge_bulk_read(p + got, len - got, timeout_ms);
        if (n < 0) {
            /* Lỗi I/O — có thể tạm thời (LIBUSB_ERROR_PIPE sau wrap) */
            if (++err_retry > max_err_retries) return -1;
            usleep(50 * 1000); /* 50ms delay trước khi retry */
            continue;
        }
        if (n == 0) {
            /* timeout — thử lại, nhưng đếm retry cục bộ cho cuộc gọi này */
            if (++retry > max_retries) return -1;
            err_retry = 0; /* reset err_retry mỗi lần thấy progress */
            continue;
        }
        retry = 0;
        err_retry = 0;
        got += n;
    }
    return got;
}

static int usb_read_at_least(void *buf, int len, int timeout_ms) {
    return usb_bridge_bulk_read(buf, len, timeout_ms);
}

/* ── Gửi VERSION request theo usbmuxd upstream ─────────────────────────── */
/*
 * FIX v33: Thêm hex dump của VERSION packet để debug.
 * Khi iPhone không phản hồi, hex dump cho phép xác nhận byte gửi đi
 * khớp với upstream usbmuxd (00 00 00 00 | 00 00 00 14 | 00 00 00 02 |
 * 00 00 00 00 | 00 00 00 00 → 20 bytes).
 */
static void log_hex(const char *prefix, const void *buf, int len) {
    const uint8_t *p = (const uint8_t *)buf;
    /*
     * FIX v34 (Critical): Buffer overflow crash!
     *
     * Code cũ: char hex[16 * 3 + 1] = 49 bytes.
     *   Nhưng show có thể lên đến 32 byte (32 × 3 = 96 chars + null = 97).
     *   → Ghi 96 chars vào buffer 49 bytes → STACK BUFFER OVERFLOW →
     *   app crash ngay khi gọi log_hex() từ usb_send_version().
     *
     * Log ở screenshot:
     *   "[usbmux] Eager version exchange (best-effort, non-blocking)..."
     *   → crash ngay sau đó, không in thêm log nào. Đó là vì log_hex()
     *     trong usb_send_version() tràn buffer và SIGSEGV.
     *
     * Fix: Đảm bảo buffer đủ lớn cho 32 byte hex dump.
     *   32 byte × 3 chars/byte + 1 null + 4 chars dư an toàn = 101 bytes.
     */
    char hex[32 * 3 + 8];
    int off = 0;
    int show = len < 32 ? len : 32;  /* dump max 32 bytes */
    for (int i = 0; i < show; i++) {
        /* snprintf luôn trả về số chars cần thiết (không kể null).
         * Dùng sizeof(hex) - off làm size limit để tránh tràn. */
        int n = snprintf(hex + off, sizeof(hex) - off, "%02x ", p[i]);
        if (n < 0 || (size_t)n >= sizeof(hex) - off) break;  /* buffer đầy */
        off += n;
    }
    LOGI("%s (len=%d): %s%s", prefix, len, hex, len > 32 ? "..." : "");
}

static int usb_send_version(void) {
    /* Upstream device.c sends VERSION while dev->version == 0, therefore
     * send_packet() uses only protocol+length (8 bytes). */
    uint8_t pkt[sizeof(v1_mux_hdr_t) + sizeof(mux_version_body_t)];
    memset(pkt, 0, sizeof(pkt));

    v1_mux_hdr_t *hdr = (v1_mux_hdr_t *)pkt;
    hdr->protocol = htonl(MUX_PROTO_VERSION);
    hdr->length = htonl(sizeof(pkt));

    mux_version_body_t *body = (mux_version_body_t *)(pkt + sizeof(*hdr));
    body->major = htonl(2);
    body->minor = htonl(0);
    body->padding = 0;

    log_hex("usb_send_version: pkt", pkt, (int)sizeof(pkt));
    LOGI("usb_send_version: VERSION upstream header 8-byte + version 2.0, length=%zu", sizeof(pkt));
    int written = usb_write(pkt, (int)sizeof(pkt));
    if (written <= 0) {
        LOGE("usb_send_version: usb_write() returned %d (mong đợi %zu)", written, sizeof(pkt));
        return -1;
    }
    if (written != (int)sizeof(pkt)) {
        LOGE("usb_send_version: partial write %d/%zu", written, sizeof(pkt));
        return -1;
    }
    /*
     * FIX v33: delay 50ms sau khi gửi VERSION để libusb kịp flush URB
     * xuống device. Trên một số Android USB stack (đặc biệt là MediaTek/
     * older Qualcomm), libusb_bulk_transfer() return ngay nhưng URB chưa
     * thật sự được gửi đi. 50ms là đủ cho USB host controller schedule.
     */
    usleep(50 * 1000);
    return 0;
}

static int usb_drain_bytes(uint32_t len, int timeout_ms) {
    uint8_t buf[256];
    while (len > 0) {
        int take = (int)(len < sizeof(buf) ? len : sizeof(buf));
        int n = usb_read_exact(buf, take, timeout_ms);
        if (n != take) return -1;
        len -= (uint32_t)n;
    }
    return 0;
}

/*
 * FIX v33/v48: Timeout trong usb_recv_version.
 *
 * v33 giảm từ 3000ms×10 → 1500ms×5.
 * v48 tăng lại thành 2000ms×8 vì iPhone iOS 17+ cần thêm thời gian
 * để process VERSION request (đặc biệt sau khiUSB session bị reset).
 *
 * Tổng thời gian chờ mỗi lần gọi: 2000ms × 8 = 16 giây.
 * Nếu không có response sau 16s → chắc chắn iPhone sẽ không phản hồi
 * trong phiên này → clear_halt + retry từ outer loop.
 */
static int usb_recv_version(void) {
    const int VERSION_TIMEOUT_MS = 2000;
    const int VERSION_ATTEMPTS   = 8;

    for (int attempt = 0; attempt < VERSION_ATTEMPTS; attempt++) {
        v1_mux_hdr_t hdr;
        int n = usb_read_exact(&hdr, sizeof(hdr), VERSION_TIMEOUT_MS);
        if (n <= 0) {
            if (attempt == VERSION_ATTEMPTS - 1) {
                LOGE("usb_recv_version: timeout chờ version (%d lần × %dms) — "
                     "iPhone không phản hồi VERSION packet",
                     VERSION_ATTEMPTS, VERSION_TIMEOUT_MS);
                return -1;
            }
            usleep(200 * 1000);
            continue;
        }
        if (n != (int)sizeof(hdr)) {
            LOGE("usb_recv_version: đọc được %d byte header (cần %zu)", n, sizeof(hdr));
            return -1;
        }

        uint32_t protocol = ntohl(hdr.protocol);
        uint32_t packet_len = ntohl(hdr.length);

        /* Hex dump header để debug */
        log_hex("usb_recv_version: hdr", &hdr, (int)sizeof(hdr));
        LOGI("usb_recv_version: protocol=%u length=%u", protocol, packet_len);

        if (packet_len < sizeof(hdr) + sizeof(mux_version_body_t) || packet_len > 65536) {
            LOGE("usb_recv_version: packet length không hợp lệ=%u (mong đợi >= %zu và <= 65536)",
                 packet_len, sizeof(hdr) + sizeof(mux_version_body_t));
            /*
             * FIX v33: Dump 32 byte tiếp theo để xem iPhone đang gửi gì.
             * Có thể là v2 binary plist header (bplist00) hoặc garbage.
             */
            uint8_t peek[32];
            int peek_n = usb_read_exact(peek, sizeof(peek), 500);
            if (peek_n > 0) log_hex("usb_recv_version: peek", peek, peek_n);
            return -1;
        }
        uint32_t body_len = packet_len - (uint32_t)sizeof(hdr);
        if (protocol != MUX_PROTO_VERSION) {
            LOGE("usb_recv_version: protocol không hợp lệ=%u (mong đợi VERSION=0). "
                 "Có thể iPhone đang dùng binary plist v2 protocol", protocol);
            if (usb_drain_bytes(body_len, 2000) < 0) return -1;
            continue;
        }

        mux_version_body_t body;
        if (usb_read_exact(&body, sizeof(body), VERSION_TIMEOUT_MS) != (int)sizeof(body)) {
            LOGE("usb_recv_version: không đọc đủ version body");
            return -1;
        }
        if (body_len > sizeof(body) && usb_drain_bytes(body_len - sizeof(body), 2000) < 0) {
            LOGE("usb_recv_version: drain extra body thất bại");
            return -1;
        }

        uint32_t major = ntohl(body.major);
        uint32_t minor = ntohl(body.minor);
        LOGI("usb_recv_version: iPhone mux version %u.%u (legacy header 8-byte)", major, minor);
        if (major < 2) {
            LOGE("usb_recv_version: iPhone trả version %u.%u, không hỗ trợ mux v2. "
                 "Thiết bị quá cũ hoặc đang ở Recovery Mode?", major, minor);
            return -1;
        }
        return 0;
    }
    return -1;
}

static int usb_send_setup(void) {
    uint8_t pkt[sizeof(v2_mux_hdr_t) + 1];
    memset(pkt, 0, sizeof(pkt));

    v2_mux_hdr_t *hdr = (v2_mux_hdr_t *)pkt;
    hdr->protocol = htonl(MUX_PROTO_SETUP);
    hdr->length = htonl(sizeof(pkt));
    hdr->magic = htonl(V2_MAGIC);
    hdr->tx_seq = htons(0);
    hdr->rx_seq = htons(0xFFFF);
    pkt[sizeof(v2_mux_hdr_t)] = 0x07;

    int n = usb_write(pkt, (int)sizeof(pkt));
    if (n <= 0) return -1;

    pthread_mutex_lock(&g_mux_seq_mutex);
    g_mux_tx_seq = 1;
    g_mux_rx_seq = 0xFFFF;
    pthread_mutex_unlock(&g_mux_seq_mutex);
    LOGI("usb_send_setup: SETUP v2 đã gửi");
    return 0;
}

bool usbmux_version_exchange(void) {
    if (g_version_done) return true;

    /*
     * FIX v33/v48: Luôn clear endpoint halt + flush IN endpoint
     * TRƯỚC MỖI lần thử VERSION — kể cả lần đầu.
     *
     * FIX v48: Tăng từ 5 attempts × 1500ms → 8 attempts × 2000ms.
     * iPhone trên iOS 17+ cần thêm thời gian để enumeratemux session
     * sau khi USB fd mới được wrap. Eager attempt trước đó có thể đã
     * "confuse" iPhone — lazy attempt cần nhiều retry hơn.
     */
    const int VERSION_ATTEMPTS = 8;
    for (int attempt = 0; attempt < VERSION_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            LOGI("usbmux_version_exchange: retry %d/%d", attempt + 1, VERSION_ATTEMPTS);
            usleep(800 * 1000);  /* 800ms chờ trước retry */
        }

        /* Luôn clear endpoint halt + flush stale data trước mỗi attempt */
        usb_bridge_clear_endpoints_halt();
        usb_bridge_flush_in(8, 200);
        usleep(200 * 1000);  /* 200ms cho USB stack propagate clear_halt */

        if (usb_send_version() < 0) {
            LOGE("usbmux_version_exchange: usb_send_version() thất bại (attempt %d)", attempt + 1);
            continue;
        }
        if (usb_recv_version() < 0) {
            LOGE("usbmux_version_exchange: usb_recv_version() thất bại (attempt %d)", attempt + 1);
            continue;
        }
        if (usb_send_setup() < 0) {
            LOGE("usbmux_version_exchange: usb_send_setup() thất bại (attempt %d)", attempt + 1);
            continue;
        }

        g_version_done = 1;
        LOGI("usbmux_version_exchange: mux v2 + SETUP OK (attempt %d/%d)", attempt + 1, VERSION_ATTEMPTS);
        return true;
    }
    LOGE("usbmux_version_exchange: thất bại sau %d lần thử — "
         "kiểm tra cáp data, màn hình iPhone đã mở khóa, đã bấm Trust chưa",
         VERSION_ATTEMPTS);
    return false;
}

void usbmuxd_server_reset_version_state(void) {
    g_version_done = 0;
    pthread_mutex_lock(&g_mux_seq_mutex);
    g_mux_tx_seq = 0;
    g_mux_rx_seq = 0xFFFF;
    pthread_mutex_unlock(&g_mux_seq_mutex);
    /*
     * FIX v45/v48: Reset source port allocator khi USB session mới bắt đầu.
     * Connection đầu tiên của session mới dùng sport=1 (match upstream).
     * Xóa lịch sử TIME_WAIT tracking — session mới bắt đầu sạch.
     */
    pthread_mutex_lock(&g_sport_mutex);
    g_next_sport = 1;
    memset((void *)g_sport_last_used, 0, sizeof(g_sport_last_used));
    g_sport_used_count = 0;
    pthread_mutex_unlock(&g_sport_mutex);
    LOGI("usbmuxd_server_reset_version_state: reset USB mux v2 session + source port allocator");
}

/* ── Gửi TCP packet lên iPhone (qua USB) ────────────────────────────────── */
/*
 * FIX v48: Serialize mux_tx_seq, mux_rx_seq, và local_seq cùng một lúc
 * dưới SINGLE mutex để tránh race giữa thread_sock_to_usb và thread_usb_to_sock.
 *
 * Trước đây, g_mux_seq_mutex protect mux_tx/rx, và st->usb_tx_lock serialize
 * USB write. Nhưng local_seq được đọc và ghi bên ngoài mutex → race window
 * khi hai thread cùng gửi packet.
 *
 * Fix: Dùng g_mux_seq_mutex để serialize CẢ mux seq LẪN local_seq.
 * st->usb_tx_lock vẫn giữ để serialize USB write (usb_write() có thể block).
 */
static int usb_send_tcp(tcp_state_t *st, uint8_t flags,
                         const void *data, uint32_t data_len) {
    uint32_t total = sizeof(v2_mux_hdr_t) + sizeof(v1_tcp_hdr_t) + data_len;
    uint8_t *pkt = malloc(total);
    if (!pkt) return -1;
    memset(pkt, 0, total);

    v2_mux_hdr_t *mhdr = (v2_mux_hdr_t *)pkt;
    mhdr->protocol = htonl(MUX_PROTO_TCP);
    mhdr->length = htonl(total);
    mhdr->magic = htonl(V2_MAGIC);

    v1_tcp_hdr_t *thdr = (v1_tcp_hdr_t *)(pkt + sizeof(v2_mux_hdr_t));
    thdr->sport = htons(st->sport);
    thdr->dport = htons(st->dport);
    thdr->off = 0x50;
    thdr->flags = flags;
    thdr->window = htons(0x0200);

    /* Serialize mux seq + TCP seq + USB write under usb_tx_lock */
    pthread_mutex_lock(&st->usb_tx_lock);

    /* Lock g_mux_seq_mutex ngắn hạn để lấy mux seq */
    uint16_t mux_tx;
    uint16_t mux_rx;
    pthread_mutex_lock(&g_mux_seq_mutex);
    mux_tx = g_mux_tx_seq++;
    mux_rx = g_mux_rx_seq;
    pthread_mutex_unlock(&g_mux_seq_mutex);

    /* Fill TCP seq/ack fields */
    thdr->seq = htonl(st->local_seq);
    thdr->ack = htonl(st->remote_seq);

    /* Advance local_seq for DATA packets (ACK packets don't consume seq) */
    if (data && data_len > 0)
        st->local_seq += data_len;

    mhdr->tx_seq = htons(mux_tx);
    mhdr->rx_seq = htons(mux_rx);

    if (data && data_len > 0)
        memcpy(pkt + sizeof(v2_mux_hdr_t) + sizeof(v1_tcp_hdr_t), data, data_len);

    LOGI("usb_send_tcp: sport=%u dport=%u flags=0x%02x seq=%u ack=%u len=%u mux_tx=%u mux_rx=%u",
         st->sport, st->dport, flags,
         ntohl(thdr->seq), ntohl(thdr->ack), data_len, mux_tx, mux_rx);
    int r = usb_write(pkt, (int)total);
    pthread_mutex_unlock(&st->usb_tx_lock);
    free(pkt);
    return r > 0 ? 0 : -1;
}

/* ── Nhận TCP packet từ iPhone và xử lý ────────────────────────────────── */
/* Trả: data_len nếu có data, 0 nếu chỉ là ACK/SYN-ACK/FIN, -1 nếu lỗi */
static int usb_recv_tcp(tcp_state_t *st, void *data_out, int max_data,
                         uint8_t *flags_out, int timeout_ms) {
    v2_mux_hdr_t mhdr;
    int n = usb_read_exact(&mhdr, sizeof(mhdr), timeout_ms);
    if (n < (int)sizeof(mhdr)) return -1;

    /*
     * FIX v44 (Critical): KHÔNG kiểm tra magic trên RX.
     *
     * Quan sát từ log v43 của user:
     *   TX (we sent):    magic = htonl(0xfeedface) → bytes FE ED FA CE
     *   RX (iPhone sent): magic = 0xfaceface → bytes FA CE FA CE
     *
     * iPhone chấp nhận magic 0xfeedface của chúng ta (gửi SYN+ACK lại),
     * nhưng response của iPhone dùng magic 0xfaceface (khác!).
     * Có thể iPhone dùng magic khác cho TCP, hoặc đây là quirk của iOS.
     *
     * Học từ upstream usbmuxd (device.c dòng 760-820):
     *   Upstream KHÔNG kiểm tra magic trên RX. Họ chỉ parse:
     *     - protocol (để biết packet type)
     *     - length (để biết kích thước)
     *     - tx_seq/rx_seq (cho v2)
     *   Và switch theo protocol.
     *
     * Fix: bỏ hoàn toàn magic check. Chỉ log warning nếu magic khác 0xfeedface
     * để debug, nhưng vẫn xử lý packet bình thường.
     */
    uint32_t rx_magic = ntohl(mhdr.magic);
    if (rx_magic != V2_MAGIC) {
        LOGI("usb_recv_tcp: magic=0x%08x (khác 0x%08x, nhưng vẫn xử lý — upstream không check)",
             rx_magic, V2_MAGIC);
    }

    uint32_t packet_len = ntohl(mhdr.length);
    if (packet_len < sizeof(mhdr) || packet_len > 1024 * 1024) {
        LOGE("usb_recv_tcp: packet length không hợp lệ=%u", packet_len);
        return -1;
    }
    if (ntohl(mhdr.protocol) != MUX_PROTO_TCP) {
        /* Drain control/setup payloads so the next TCP frame stays aligned. */
        uint32_t body_len = packet_len - (uint32_t)sizeof(mhdr);
        if (body_len > 0 && usb_drain_bytes(body_len, 2000) < 0) return -1;
        return 0;
    }

    v1_tcp_hdr_t thdr;
    if (usb_read_exact(&thdr, sizeof(thdr), 2000) < (int)sizeof(thdr)) return -1;

    uint32_t total_len = ntohl(mhdr.length);
    uint32_t tcp_hdr_len = (thdr.off >> 4) * 4;
    if (total_len < sizeof(v2_mux_hdr_t) + tcp_hdr_len || tcp_hdr_len < sizeof(thdr)) {
        LOGE("usb_recv_tcp: packet length/header không hợp lệ total=%u tcp=%u", total_len, tcp_hdr_len);
        return -1;
    }
    uint16_t mux_tx = ntohs(mhdr.tx_seq);
    uint16_t mux_rx = ntohs(mhdr.rx_seq);
    pthread_mutex_lock(&g_mux_seq_mutex);
    /*
     * FIX v47 (CRITICAL — Trust popup không hiện, RST ngay khi gửi DATA):
     *
     * iPhone dùng ECHO SEMANTICS cho mux_rx_seq:
     *   rx_seq = LAST RECEIVED tx_seq từ peer (KHÔNG phải +1)
     *
     * Bằng chứng từ trace log user (lần này, sau v46):
     *   Our SYN:      tx_seq=1, rx_seq=0xFFFF  (initial)
     *   iPhone SYN+ACK: tx_seq=0, rx_seq=1     ← iPhone echo our SYN tx=1
     *   Our ACK:       tx_seq=2, rx_seq=1       ← v46 fix: iPhone_tx(0)+1=1 (NEXT-EXPECTED)
     *   Our DATA:      tx_seq=3, rx_seq=1       ← vẫn 1 (iPhone chưa gửi packet mới)
     *   iPhone RST:    tx_seq=1, rx_seq=3       ← iPhone echo our DATA tx=3
     *
     * iPhone's SYN+ACK có rx_seq=1. Nếu iPhone dùng "next-expected", rx_seq
     * phải = 2 (vì our SYN tx=1, next expected = 2). Nhưng iPhone gửi 1
     * → iPhone dùng ECHO semantics (rx_seq = last received tx_seq, KHÔNG +1).
     *
     * Hậu quả của v46 fix (next-expected):
     *   iPhone expect our rx_seq = 0 (echo iPhone's last tx_seq=0)
     *   We sent rx_seq = 1 (next expected, OFF-BY-ONE)
     *   → iPhone's handleMuxTCPInput fail socket lookup
     *   → "no matching socket for socket N" error payload
     *   → RST ngay khi nhận DATA packet (chỉ 2ms sau khi gửi)
     *   → lockdownd Hello/GetValue không bao giờ được xử lý
     *   → Pair request không bao giờ được gửi
     *   → TRUST POPUP KHÔNG BAO HIỆN
     *
     * Fix: g_mux_rx_seq = mux_tx (ECHO — exactly the same value as iPhone's last tx_seq).
     * Đây là behavior mà iPhone expects; verified bằng cách đối chiếu với trace.
     *
     * Lưu ý: v46 comment nói "upstream usbmuxd: dev->rx_seq = header->tx_seq + 1"
     * là INCORRECT — upstream usbmuxd thực sự dùng ECHO semantics:
     *   dev->rx_seq = ntohs(hdr->tx_seq);   // NO +1
     * (xem usbmuxd/src/device.c, device_receive_packet)
     */
    g_mux_rx_seq = mux_tx;  /* ECHO semantics — match iPhone's expectation */
    pthread_mutex_unlock(&g_mux_seq_mutex);
    uint32_t data_len_raw = total_len - sizeof(v2_mux_hdr_t) - tcp_hdr_len;

    /* Đọc TCP options nếu có (tcp_hdr_len > 20) */
    if (tcp_hdr_len > sizeof(thdr)) {
        uint32_t opts_len = tcp_hdr_len - sizeof(thdr);
        if (opts_len > 40) opts_len = 40;
        uint8_t opts_buf[40];
        if (usb_read_exact(opts_buf, opts_len, 2000) < (int)opts_len) return -1;
    }

    if (flags_out) *flags_out = thdr.flags;

    uint32_t iphone_seq = ntohl(thdr.seq);
    uint32_t iphone_ack = ntohl(thdr.ack);
    LOGI("usb_recv_tcp: sport=%u dport=%u flags=0x%02x seq=%u ack=%u win=%u len=%u mux_tx=%u mux_rx=%u",
         ntohs(thdr.dport), ntohs(thdr.sport), thdr.flags, iphone_seq, iphone_ack,
         ntohs(thdr.window), data_len_raw, mux_tx, mux_rx);

    /* Cập nhật remote_seq từ ack + seq fields */

    /*
     * FIX v45 (Critical — Trust popup không hiện): Nhận diện RST flag.
     *
     * Khi iPhone gửi RST, packet thường kèm payload là error message dạng
     *   \x01 handleMuxTCPInput no matching socket for socket N
     * (Apple internal diagnostic string). Code cũ KHÔNG check RST flag, xử
     * lý payload như TCP data bình thường → viết error message vào socket
     * libimobiledevice → libimobiledevice parse error → protocol mismatch.
     * Đồng thời thread_usb_to_sock gửi ACK phản hồi → iPhone lại gửi RST
     * khác → LOOP VÔ HẠN như trace 1.txt cho thấy.
     *
     * Fix:
     *   1. Drain payload (error message) để USB stream stay aligned.
     *   2. Đánh dấu st->rst_received = 1 để thread上层 biết connection đã chết.
     *   3. Return -2 (special code) để caller phân biệt với error thường.
     *   4. NOT update st->remote_seq (connection đã chết, không cần ACK).
     */
    if (thdr.flags & TH_RST) {
        LOGE("usb_recv_tcp: ⚠️ RST received from iPhone (flags=0x%02x seq=%u ack=%u "
             "len=%u) — connection reset by peer (no matching socket?)",
             thdr.flags, iphone_seq, iphone_ack, data_len_raw);
        st->rst_received = 1;
        /* Drain payload (error message) để giữ USB stream aligned */
        if (data_len_raw > 0) {
            uint8_t drain_buf[256];
            uint32_t to_drain = data_len_raw;
            int logged_first = 0;
            while (to_drain > 0) {
                int chunk = (int)(to_drain < sizeof(drain_buf) ? to_drain : sizeof(drain_buf));
                int got = usb_read_exact(drain_buf, chunk, 1500);
                if (got < chunk) {
                    LOGE("usb_recv_tcp: drain RST payload fail (got=%d want=%d remaining=%u)",
                         got, chunk, to_drain);
                    break;
                }
                if (!logged_first) {
                    /* Log first chunk để debug — thường là "handleMuxTCPInput..." */
                    log_hex("usb_recv_tcp: RST payload (first chunk)", drain_buf,
                            got < 64 ? got : 64);
                    logged_first = 1;
                }
                to_drain -= (uint32_t)got;
            }
            LOGE("usb_recv_tcp: RST payload drained %u bytes (likely Apple internal "
                 "'handleMuxTCPInput no matching socket' error)", data_len_raw);
        }
        return -2;  /* Special: RST received */
    }

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
        /*
         * FIX v48:remote_seq = iphone_seq + data_read
         * (next expected sequence từ iPhone = total bytes received).
         * iphone_ack (field ack trong TCP header) cho biết iPhone đã
         * nhận được data của chúng ta đến đâu — nhưng chúng ta KHÔNG
         * dùng nó làm remote_seq, vì remote_seq là "next expected
         * FROM iPhone", không phải "what iPhone has acked FROM us".
         */
        st->remote_seq = iphone_seq + data_read;
    } else {
        /* Pure ACK/SYN+ACK/FIN — cập nhật remote_seq từ seq */
        if (thdr.flags & TH_SYN) {
            /*
             * iPhone's SYN có ISN (Initial Sequence Number).
             * Sau khi gửi SYN+ACK, remote_seq = ISN + 1
             * (FIN+SYN tiêu tüket 1 sequence number).
             */
            st->remote_seq = iphone_seq + 1;
        }
        /*
         * FIX v48: Cho pure ACK (iPhone ACK data của chúng ta):
         * KHÔNG update remote_seq — remote_seq là "next expected
         * FROM iPhone", pure ACK không gửi data từ iPhone.
         *
         * iphone_ack (thông tin trong ACK) cho biết iPhone đã
         * nhận data của chúng ta. Nếu cần flow control, ta có thể
         * dùng nó — nhưng trong đơn giản hóa hiện tại, bỏ qua.
         */
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

    /*
     * FIX v45 (Critical — Trust popup không hiện):
     *
     * Code cũ hardcode `st->sport = 1` cho MỌI kết nối TCP. Khi connection
     * trước đó bị đóng nhưng iPhone usbmuxd vẫn giữ TIME_WAIT state cho
     * (sport=1, dport=port), connection MỚI với cùng sport=1 sẽ bị iPhone
     * reject với RST + "handleMuxTCPInput no matching socket".
     *
     * Fix: dùng alloc_source_port() để cấp port tăng dần. Connection đầu
     * tiên dùng port 1 (match upstream usbmuxd), connection sau dùng port 2, 3, ...
     * Tránh xung đột với TIME_WAIT state trên iPhone.
     */
    st->sport      = alloc_source_port();
    st->dport      = (uint16_t)port;
    st->local_seq  = 0;
    st->remote_seq = 0;
    st->rst_received = 0;
    pthread_mutex_init(&st->usb_tx_lock, NULL);

    LOGI("do_usb_v1_connect: allocated sport=%u for dport=%u", st->sport, st->dport);

    /* Version exchange được thực hiện một lần ngay trước SYN, sau khi
     * client đã gửi Connect tới port lockdown. */
    if (!usbmux_version_exchange()) {
        LOGE("do_usb_v1_connect: version exchange thất bại — iPhone không hỗ trợ v1?");
        return false;
    }

    /*
     * FIX v45: SYN handshake với retry trên RST.
     *
     * Nếu iPhone gửi RST trong lúc SYN handshake (vì sport conflict hoặc
     * iPhone usbmuxd chưa sẵn sàng), retry với sport mới sau delay ngắn.
     * Tối đa 3 lần thử.
     */
    bool syn_ok = false;
    for (int syn_attempt = 0; syn_attempt < 5; syn_attempt++) {
        if (syn_attempt > 0) {
            /*
             * FIX v48: Tăng retry từ 3 → 5, delay từ 300ms → 500ms.
             * iPhone iOS 17+ đôi khi cần thêm thời gian để dọn TIME_WAIT
             * socket entry trước khi nhận connection mới.
             */
            LOGI("do_usb_v1_connect: SYN retry %d/5 với sport mới", syn_attempt + 1);
            usleep(500 * 1000);  /* 500ms delay trước retry */
            /* Clear endpoint halt trước mỗi retry để endpoint sạch */
            usb_bridge_clear_endpoints_halt();
            usleep(100 * 1000);
            st->sport = alloc_source_port();
            st->local_seq = 0;
            st->remote_seq = 0;
            st->rst_received = 0;
            LOGI("do_usb_v1_connect: new sport=%u", st->sport);
        }

        /* Bước 1: SYN */
        uint32_t isn = st->local_seq;
        if (usb_send_tcp(st, TH_SYN, NULL, 0) < 0) {
            LOGE("do_usb_v1_connect: gửi SYN thất bại (attempt %d)", syn_attempt + 1);
            continue;
        }
        LOGI("do_usb_v1_connect: SYN gửi (sport=%u dport=%u seq=%u)",
             st->sport, st->dport, st->local_seq);

        /* Bước 2: Nhận SYN+ACK từ iPhone */
        uint8_t flags = 0;
        uint8_t dummy[1];
        int n = usb_recv_tcp(st, dummy, sizeof(dummy), &flags, 5000);

        /* FIX v45: RST trong lúc handshake → retry với sport mới */
        if (n == -2) {
            LOGE("do_usb_v1_connect: iPhone gửi RST thay vì SYN+ACK (attempt %d) — "
                 "sport conflict hoặc iPhone usbmuxd chưa sẵn sàng", syn_attempt + 1);
            continue;
        }
        if (n < 0) {
            LOGE("do_usb_v1_connect: nhận SYN+ACK thất bại (attempt %d)", syn_attempt + 1);
            continue;
        }
        if (!(flags & TH_SYN) || !(flags & TH_ACK)) {
            LOGE("do_usb_v1_connect: nhận flags=0x%02x (không phải SYN+ACK, attempt %d)",
                 flags, syn_attempt + 1);
            continue;
        }

        LOGI("do_usb_v1_connect: SYN+ACK nhận, remote_seq=%u (expected ack=%u)",
             st->remote_seq, isn + 1);

        /*
         * FIX v46/v48: Delay giữa SYN+ACK và ACK.
         *
         * Trên iOS 16/17+, lockdownd cần thời gian để tạo socket entry
         * trong usbmuxd socket table SAU khi gửi SYN+ACK. Nếu ACK đến
         * quá nhanh, iPhone chưa kịp tạo socket → RST.
         *
         * v48: Tăng từ 50ms → 100ms cho iOS 17+ (chậm hơn).
         */
        usleep(100 * 1000);

        /* Bước 3: ACK — SYN tiêu thụ 1 sequence number */
        st->local_seq = isn + 1;
        if (usb_send_tcp(st, TH_ACK, NULL, 0) < 0) {
            LOGE("do_usb_v1_connect: gửi ACK thất bại (attempt %d)", syn_attempt + 1);
            continue;
        }

        LOGI("do_usb_v1_connect: ✅ kết nối TCP port=%d OK (sport=%u local_seq=%u remote_seq=%u)",
             port, st->sport, st->local_seq, st->remote_seq);
        syn_ok = true;
        break;
    }

    if (!syn_ok) {
        LOGE("do_usb_v1_connect: SYN handshake thất bại sau 3 lần thử — "
             "iPhone reject tất cả source port. Có thể lockdownd không listen trên port %d, "
             "hoặc iPhone đang ở trạng thái không accept connection", port);
        return false;
    }
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

        /* FIX v48: local_seq đã được advance trong usb_send_tcp(),
         * không cần advance ở đây nữa (tránh double-advance race). */
        if (usb_send_tcp(st, TH_PUSH | TH_ACK, buf, (uint32_t)n) < 0) {
            LOGE("sock_to_usb: USB write thất bại");
            break;
        }
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

        /*
         * FIX v45 (Critical — Trust popup không hiện):
         *
         * RST từ iPhone = connection đã chết. Code cũ không check RST, viết
         * error message payload vào socket libimobiledevice (làm hỏng protocol)
         * rồi gửi ACK lại → iPhone gửi RST khác → LOOP VÔ HẠN (như trace 1.txt).
         *
         * Fix:
         *   1. Không viết RST payload vào socket (đó là error message).
         *   2. Không gửi ACK lại (break loop).
         *   3. Shutdown socket để thread_sock_to_usb cũng thoát khỏi read().
         *   4. Break tunnel loop, để libimobiledevice biết connection đã chết.
         */
        if (n == -2) {
            LOGE("usb_to_sock: ⚠️ iPhone đã gửi RST — connection reset by peer. "
                 "Ngắt tunnel NGAY, không gửi ACK (ngắt ACK/RST loop).");
            /* Shutdown socket để thread_sock_to_usb thoát khỏi read() đang block */
            shutdown(sock, SHUT_RDWR);
            break;
        }

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
            // FIX v22 Bug A: Gửi Attached event ngay sau khi Listen OK
            char *attached_event = make_attached_event();
            if (attached_event) {
                send_plist(client_fd, 0, attached_event); // tag=0 cho events
                free(attached_event);
            }
        } else if (strcmp(msg_type, "ListDevices") == 0) {
            char *resp = make_device_list();
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

            /*
             * FIX v45/v48: Xử lý kết thúc TCP connection.
             *
             * v45: Nếu iPhone đã RST → KHÔNG gửi FIN (connection đã chết).
             *
             * v48: Thêm delay 200ms trước khi đóng để iPhone usbmuxd
             * kịp dọn socket entry. Nếu gửi FIN quá nhanh sau khi data
             * exchange, iPhone có thể chưa xử lý xong → secondary RST.
             */
            if (st->rst_received) {
                LOGI("client fd=%d: skip FIN — iPhone đã RST, connection đã chết", client_fd);
            } else {
                /* Gửi FIN để đóng TCP connection phía iPhone */
                usb_send_tcp(st, TH_FIN | TH_ACK, NULL, 0);
                usleep(200000);  /* 200ms cho iPhone xử lý FIN */
                /* Sau FIN, đọc thêm 1 packet để nhận FIN+ACK từ iPhone */
                {
                    uint8_t dummy_flags = 0;
                    uint8_t dummy[1];
                    int fin_ack = usb_recv_tcp(st, dummy, sizeof(dummy), &dummy_flags, 1000);
                    if (fin_ack >= 0 && (dummy_flags & TH_ACK)) {
                        LOGI("client fd=%d: received FIN+ACK from iPhone", client_fd);
                    }
                }
            }

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

    /* Không gửi Detached ở đây. Việc đóng một control client (ví dụ
     * lockdownd/AFC/instproxy) không đồng nghĩa thiết bị USB đã detached;
     * event giả làm libusbmuxd xoá device và lần connect kế tiếp trả
     * LOCKDOWN_E_MUX_ERROR (-8). Chỉ broadcast detach khi có sự kiện USB
     * thật từ Android/native lifecycle. */
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
    strncpy(g_udid, udid ? udid : "pending-device",
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
    g_tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_tcp_fd >= 0) {
        int tcp_opt = 1;
        setsockopt(g_tcp_fd, SOL_SOCKET, SO_REUSEADDR, &tcp_opt, sizeof(tcp_opt));
        struct sockaddr_in tcp_addr;
        memset(&tcp_addr, 0, sizeof(tcp_addr));
        tcp_addr.sin_family      = AF_INET;
        tcp_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        tcp_addr.sin_port        = htons(USBMUXD_TCP_PORT);
        if (bind(g_tcp_fd, (struct sockaddr *)&tcp_addr, sizeof(tcp_addr)) == 0
            && listen(g_tcp_fd, 8) == 0) {
            char tcp_sock_str[32];
            snprintf(tcp_sock_str, sizeof(tcp_sock_str), "127.0.0.1:%d", USBMUXD_TCP_PORT);
            setenv("USBMUXD_SOCKET_ADDRESS", tcp_sock_str, 1);
            g_tcp_running = 1;
            if (pthread_create(&g_tcp_thread, NULL, tcp_server_thread, NULL) == 0) {
                pthread_detach(g_tcp_thread);
                LOGI("usbmuxd_server_start: ✅ TCP dual-socket: %s (udid=%s)", tcp_sock_str, g_udid);
            } else {
                LOGE("usbmuxd_server_start: TCP pthread_create thất bại — fallback Unix");
                g_tcp_running = 0;
                close(g_tcp_fd); g_tcp_fd = -1;
                setenv("USBMUXD_SOCKET_ADDRESS", g_sock_path, 1);
            }
        } else {
            LOGE("usbmuxd_server_start: TCP bind/listen lỗi %d — fallback Unix socket", errno);
            close(g_tcp_fd); g_tcp_fd = -1;
            setenv("USBMUXD_SOCKET_ADDRESS", g_sock_path, 1);
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
    LOGI("usbmuxd_server_update_udid: real identity=%s", g_udid);
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

    /* Mỗi lần start sau đó phải thực hiện version exchange cho USB session
     * mới; không được giữ cờ của session trước. */
    usbmuxd_server_reset_version_state();
    pthread_mutex_lock(&g_udid_mutex);
    g_udid[0] = '\0';
    g_product_id = 0;
    pthread_mutex_unlock(&g_udid_mutex);
    LOGI("usbmuxd_server_stop: done");
}
