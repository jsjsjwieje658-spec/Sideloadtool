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
/* Mux header (16 bytes) */
typedef struct {
    uint32_t protocol;   /* BE: 0=version, 1=TCP */
    uint32_t length;     /* BE: total length including all headers */
    uint32_t magic;      /* BE: 0xfeedface */
    uint16_t tx_seq;     /* BE: device frame sequence (we use 0) */
    uint16_t rx_seq;     /* BE: last received frame (we use 0xffff) */
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

/* Version packet body (16 bytes) */
typedef struct {
    uint32_t major;      /* BE */
    uint32_t minor;      /* BE */
    uint32_t padding;
    uint32_t padding2;
} v1_version_body_t;

#pragma pack(pop)

#define V1_MAGIC     0xfeedface
#define V1_PROTO_VER 0
#define V1_PROTO_TCP 1

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

/* ── Gửi v1 version packet ─────────────────────────────────────────────── */
static int usb_send_version(void) {
    uint8_t pkt[sizeof(v1_mux_hdr_t) + sizeof(v1_version_body_t)];
    memset(pkt, 0, sizeof(pkt));

    v1_mux_hdr_t *hdr = (v1_mux_hdr_t *)pkt;
    hdr->protocol = htonl(V1_PROTO_VER);
    hdr->length   = htonl(sizeof(pkt));
    hdr->magic    = htonl(V1_MAGIC);
    hdr->tx_seq   = htons(0);
    /*
     * FIX v28 (CRITICAL): rx_seq phải là 0xFFFF, không phải 0x0000.
     *
     * 0xFFFF = "chưa nhận frame nào" — đây là giá trị khởi tạo đúng theo
     * spec usbmuxd thật (xem libimobiledevice/tools/iproxy.c và
     * usbmuxd/src/usb.c: hdr.rx_seq = 0xffff trong version request).
     *
     * Với 0x0000, iPhone (iOS 7+) đọc "rx_seq=0" nghĩa là "đã nhận frame 0"
     * → trạng thái protocol không nhất quán → iPhone bỏ qua hoặc từ chối
     * version packet → version exchange thất bại từ lần đầu.
     *
     * FIX v26 (đã sai): "0x0000 đúng với spec" — KHÔNG ĐÚNG.
     * Spec thật: https://github.com/libimobiledevice/usbmuxd/blob/master/src/usb.c
     * version_request.header.rx_seq = htons(0xffff);
     */
    hdr->rx_seq   = htons(0xFFFF);

    v1_version_body_t *body = (v1_version_body_t *)(pkt + sizeof(v1_mux_hdr_t));
    /*
     * FIX v28 (CRITICAL): major phải là 1, không phải 2.
     *
     * iPhone iOS 7+ chỉ hỗ trợ v1 protocol (major=1, minor=0).
     * Gửi major=2 khiến iPhone không nhận ra version packet và không
     * phản hồi → version exchange thất bại → Trust popup không xuất hiện.
     *
     * Tham khảo real usbmuxd source (usb.c):
     *   version_request.body.major = htonl(1);
     *   version_request.body.minor = htonl(0);
     */
    body->major   = htonl(1);   /* FIX: v1 protocol — iPhone iOS 7+ chỉ accept major=1 */
    body->minor   = htonl(0);
    body->padding = 0;
    body->padding2= 0;

    return usb_write(pkt, sizeof(pkt)) > 0 ? 0 : -1;
}

/* ── Nhận và kiểm tra version response từ iPhone ───────────────────────── */
/*
 * FIX Bug D (Medium): đọc header (16 bytes) TRƯỚC, validate magic +
 * protocol ngay, rồi mới đọc/drain phần body (version_body) riêng.
 *
 * Trước đây đọc gộp header+body trong một lần usb_read_exact(sizeof(pkt)),
 * chờ đủ toàn bộ 32 bytes mới validate — nếu iPhone đã đẩy version packet
 * (hoặc byte đầu của gói kế tiếp) vào buffer USB trước khi ta kịp đọc,
 * việc đọc gộp dễ lẫn dữ liệu và validate trễ. Đọc header trước cho phép
 * fail-fast ngay khi magic sai, và timeout mỗi lần đọc giảm 3000ms → 1500ms
 * để không giữ pipe quá lâu khi iPhone không phản hồi.
 */
static int usb_recv_version(void) {
    /*
     * FIX v25 (Critical — version exchange thất bại):
     *
     * Ba vấn đề được sửa trong lần này:
     *
     * 1. BAD MAGIC → return -1 ngay (WRONG): iPhone đôi khi gửi dữ liệu
     *    USB cũ (từ lần kết nối trước, buffered trong kernel) có magic khác.
     *    Fix: khi magic sai nhưng length có vẻ hợp lệ (8-65535), drain body
     *    và TIẾP TỤC vòng lặp thay vì return -1. Tăng số lần skip từ 5 → 12.
     *
     * 2. SHORT HEADER READ → return -1 ngay (WRONG): khi usb_read_exact()
     *    trả về 0 (timeout) hoặc số byte nhỏ, không nên bỏ ngay mà phải
     *    retry lại toàn bộ header read.
     *
     * 3. TIMEOUT quá ngắn: tăng từ 5000ms → 8000ms để phù hợp với iOS 17+
     *    trên các máy Android có USB controller chậm (MediaTek).
     */
    /*
     * FIX v28: Tách riêng timeout retries khỏi bad-packet skip budget.
     *
     * VẤN ĐỀ CŨ (v27): Timeout và bad packet đều dùng chung biến `skip`
     * (MAX_SKIP=20). Nếu iPhone không phản hồi ngay (phổ biến với USB
     * controller chậm), 20 lần timeout cạn kiệt toàn bộ skip budget mà
     * chưa nhận được một packet rác nào → hàm trả -1 quá sớm.
     *
     * FIX: Hai counter riêng biệt:
     *   - timeout_tries: đếm số lần đọc trả về ≤0 (timeout/error)
     *   - skip: chỉ tăng khi nhận được packet thực sự nhưng sai (bad magic/protocol)
     *
     * Timeout tối đa: MAX_TIMEOUT_TRIES × VERSION_TIMEOUT_MS = 10 × 3000ms = 30 giây
     * Bad packet skip: MAX_SKIP = 30 (tăng từ 20 để xử lý thiết bị ồn ào hơn)
     */
    const int VERSION_TIMEOUT_MS      = 3000;  /* ms mỗi lần đọc — ngắn hơn để retry nhanh hơn */
    const int MAX_TIMEOUT_TRIES       = 10;    /* tối đa 10 lần timeout (30 giây tổng) */
    const int MAX_SKIP                = 30;    /* tối đa 30 bad packet bị skip */
    v1_mux_hdr_t hdr;

    int timeout_tries = 0;
    for (int skip = 0; skip < MAX_SKIP; ) {
        /* ── Đọc 16-byte header ── */
        int n = usb_read_exact(&hdr, sizeof(hdr), VERSION_TIMEOUT_MS);
        if (n <= 0) {
            /*
             * FIX v28: Timeout KHÔNG tăng skip — dùng counter riêng.
             * Giúp hàm tiếp tục chờ ngay cả khi iPhone khởi tạo endpoint chậm.
             */
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

        uint32_t magic    = ntohl(hdr.magic);
        uint32_t protocol = ntohl(hdr.protocol);
        uint32_t pkt_len  = ntohl(hdr.length);

        /* ── Kiểm tra magic ── */
        if (magic != V1_MAGIC) {
            LOGI("usb_recv_version: bad magic=0x%08x (skip=%d/%d) — drain & retry",
                 magic, skip+1, MAX_SKIP);
            if (pkt_len > sizeof(hdr) && pkt_len < 65536) {
                uint32_t body_len = pkt_len - sizeof(hdr);
                uint8_t *drain = malloc(body_len);
                if (drain) {
                    usb_read_exact(drain, (int)body_len, 2000);
                    free(drain);
                } else {
                    uint8_t tmp[256];
                    uint32_t remaining = body_len;
                    while (remaining > 0) {
                        int take = (int)(remaining < sizeof(tmp) ? remaining : sizeof(tmp));
                        int r = usb_read_exact(tmp, take, 1000);
                        if (r <= 0) break;
                        remaining -= (uint32_t)r;
                    }
                }
            }
            skip++;  /* FIX v28: tăng skip chỉ khi có packet rác thật sự */
            continue;
        }

        /* ── Magic hợp lệ — kiểm tra protocol ── */
        if (protocol != V1_PROTO_VER) {
            uint32_t body_len = pkt_len > sizeof(hdr) ? pkt_len - sizeof(hdr) : 0;
            if (body_len > 0 && body_len < 65536) {
                uint8_t *drain = malloc(body_len);
                if (drain) {
                    usb_read_exact(drain, (int)body_len, 2000);
                    free(drain);
                }
            }
            LOGI("usb_recv_version: protocol=%u (not version), drain & retry (skip=%d/%d)",
                 protocol, skip+1, MAX_SKIP);
            skip++;  /* FIX v28: tăng skip chỉ khi có packet rác thật sự */
            continue;
        }

        /* ── Version packet hợp lệ — đọc/drain body ── */
        uint32_t body_len = pkt_len > sizeof(hdr) ? pkt_len - sizeof(hdr) : 0;
        if (body_len > sizeof(v1_version_body_t)) body_len = sizeof(v1_version_body_t);
        if (body_len > 0) {
            uint8_t body[sizeof(v1_version_body_t)];
            int r = usb_read_exact(body, (int)body_len, VERSION_TIMEOUT_MS);
            if (r < (int)body_len) {
                LOGI("usb_recv_version: short body read r=%d/%u, retry", r, body_len);
                continue; /* retry — có thể đọc được ở lần sau */
            }
            v1_version_body_t *vb = (v1_version_body_t *)body;
            LOGI("usb_recv_version: iPhone version major=%u minor=%u",
                 ntohl(vb->major), ntohl(vb->minor));
        }

        LOGI("usb_recv_version: ✅ iPhone v1 protocol confirmed (skip=%d)", skip);
        return 0;
    } /* end for(skip) */

    LOGE("usb_recv_version: ❌ thất bại — skip=%d/%d timeout=%d/%d",
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

        if (usb_recv_version() == 0) {
            g_version_done = 1;
            LOGI("usbmux_version_exchange: ✅ version exchange OK (attempt %d)", attempt + 1);
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
    mhdr->tx_seq   = htons(0);
    mhdr->rx_seq   = htons(0);

    v1_tcp_hdr_t *thdr = (v1_tcp_hdr_t *)(pkt + sizeof(v1_mux_hdr_t));
    thdr->sport  = htons(st->sport);
    thdr->dport  = htons(st->dport);
    thdr->seq    = htonl(st->local_seq);
    thdr->ack    = htonl(st->remote_seq);
    thdr->off    = 0x50;  /* 20 bytes / 4 = 5 → 0x50 */
    thdr->flags  = flags;
    thdr->window = htons(0x0200);  /* 512 — conservative */
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
    if (ntohl(mhdr.protocol) != V1_PROTO_TCP) {
        /* Bỏ qua version packets từ iPhone */
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
    /* Sport: dùng port ngẫu nhiên trong range 49152-65535 */
    srand((unsigned)time(NULL));
    st->sport      = (uint16_t)(49152 + (rand() % 16383));
    st->dport      = (uint16_t)port;
    st->local_seq  = (uint32_t)(rand());  /* Initial sequence number */
    st->remote_seq = 0;
    pthread_mutex_init(&st->usb_tx_lock, NULL);

    /*
     * FIX Bug B: KHÔNG lặp lại version exchange ở đây nữa. iPhone v1 mux
     * chỉ chấp nhận một version exchange per USB session; version exchange
     * thật sự đã được thực hiện một lần trong nativeSetUsbFd() (ngay sau
     * libusb init, trước usbmuxd_server_start()). Gọi lại usbmux_version_exchange()
     * ở đây chỉ là safety check — idempotent nhờ g_version_done, sẽ no-op
     * nếu đã làm rồi, và chỉ thực hiện thật nếu vì lý do nào đó chưa làm.
     */
    if (!usbmux_version_exchange()) {
        LOGE("do_usb_v1_connect: version exchange thất bại — iPhone không hỗ trợ v1?");
        return false;
    }

    /* Bước 1: SYN */
    uint32_t isn = st->local_seq;
    if (usb_send_tcp(st, TH_SYN, NULL, 0) < 0) {
        LOGE("do_usb_v1_connect: gửi SYN thất bại");
        return false;
    }
    LOGI("do_usb_v1_connect: SYN gửi (sport=%u dport=%u seq=%u)",
         st->sport, st->dport, st->local_seq);

    /* Bước 2: Nhận SYN+ACK từ iPhone */
    uint8_t flags = 0;
    uint8_t dummy[1];
    int n = usb_recv_tcp(st, dummy, sizeof(dummy), &flags, 5000);
    if (n < 0) {
        LOGE("do_usb_v1_connect: nhận SYN+ACK thất bại");
        return false;
    }
    if (!(flags & TH_SYN) || !(flags & TH_ACK)) {
        LOGE("do_usb_v1_connect: nhận flags=0x%02x (không phải SYN+ACK)", flags);
        return false;
    }
    LOGI("do_usb_v1_connect: SYN+ACK nhận, remote_seq=%u", st->remote_seq);

    /* Bước 3: ACK */
    st->local_seq = isn + 1;   /* SYN tiêu thụ 1 sequence number */
    if (usb_send_tcp(st, TH_ACK, NULL, 0) < 0) {
        LOGE("do_usb_v1_connect: gửi ACK thất bại");
        return false;
    }
    LOGI("do_usb_v1_connect: ✅ kết nối TCP port=%d OK (local_seq=%u remote_seq=%u)",
         port, st->local_seq, st->remote_seq);
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
    strncpy(g_udid, udid ? udid : "00000000-0000-0000-0000-000000000000",
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
    LOGI("usbmuxd_server_update_udid: %s", g_udid);
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
