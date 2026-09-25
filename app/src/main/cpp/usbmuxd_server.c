/*
 * usbmuxd_server.c — usbmuxd chạy trong tiến trình app (Android, không root)
 *
 * ════════════════════════════════════════════════════════════════════════
 * VIẾT LẠI (v49). Bản cũ (v20→v48, ~1800 dòng, 27+ bản vá) KHÔNG phải một
 * usbmuxd: mỗi kết nối TCP có RIÊNG một luồng tự đọc thẳng endpoint bulk-IN
 * dùng chung, không lọc theo cổng → khi libimobiledevice mở kết nối thứ hai
 * (lockdownd luôn còn mở khi StartService/AFC/installation_proxy chạy), gói
 * SYN+ACK/dữ liệu của kết nối này bị luồng của kết nối kia "ăn" mất → RST,
 * treo, "no matching socket"… Ngoài ra còn: gửi dữ liệu với cờ PSH|ACK
 * (upstream chỉ dùng ACK), rx_seq sai ngữ nghĩa (copy tx_seq thay vì rx_seq),
 * đóng kết nối bằng FIN rồi đọc USB từ luồng khác, không có flow-control,
 * payload tới 64 KiB (> USB_MTU), ReadBUID/ReadPairRecord trả "Result" nên
 * libusbmuxd vứt bỏ dữ liệu, SavePairRecord không lưu gì (pair xong vẫn như
 * chưa pair) …
 *
 * Bản này port ĐÚNG giao thức của usbmuxd upstream — thứ mà termux-usbmuxd
 * chạy nguyên bản nên "100% hoạt động":
 *   - libimobiledevice/usbmuxd src/device.c  (giao thức USB ↔ iPhone)
 *   - libimobiledevice/usbmuxd src/client.c  (giao thức plist ↔ libusbmuxd)
 *   - libimobiledevice/usbmuxd src/conf.c    (SystemBUID + pair record)
 *
 * Kiến trúc: 1 luồng đọc USB (chỉ đọc, đẩy từng transfer vào hàng đợi) +
 * 1 luồng lõi (poll()) sở hữu TOÀN BỘ trạng thái: thiết bị, các kết nối TCP,
 * các client libusbmuxd, và là nơi DUY NHẤT ghi USB. Không có race.
 *
 *   libimobiledevice ⇄ libusbmuxd ⇄ [Unix socket, plist] ⇄ luồng lõi
 *   luồng lõi ⇄ usb_fd_bridge (libusb trên fd của UsbDeviceConnection) ⇄ iPhone
 * ════════════════════════════════════════════════════════════════════════
 */
#include "usbmuxd_server.h"
#include "usb_fd_bridge.h"
#include "android_usbmuxd_fix.h"

#include <plist/plist.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

#ifdef __ANDROID__
#include <android/log.h>
#define ALOG(prio, ...) __android_log_print(prio, "usbmuxd_srv", __VA_ARGS__)
#else
#define ANDROID_LOG_DEBUG 3
#define ANDROID_LOG_INFO  4
#define ANDROID_LOG_ERROR 6
#define ALOG(prio, ...) do { if ((prio) >= ANDROID_LOG_INFO || getenv("MUXSRV_DEBUG")) { \
        fprintf(stderr, "[usbmuxd_srv] " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#endif

/* LOGI/LOGE: cả logcat lẫn màn hình log của app. LOGD: chỉ logcat (từng gói). */
#define LOGI(...) do { ALOG(ANDROID_LOG_INFO,  __VA_ARGS__); android_usbmuxd_fix_logf(__VA_ARGS__); } while (0)
#define LOGE(...) do { ALOG(ANDROID_LOG_ERROR, __VA_ARGS__); android_usbmuxd_fix_logf(__VA_ARGS__); } while (0)
#define LOGD(...) do { if (g_verbose) ALOG(ANDROID_LOG_DEBUG, __VA_ARGS__); } while (0)

/* ── Hằng số — giữ đúng giá trị của usbmuxd upstream ─────────────────────── */
#define MUX_PROTO_VERSION 0
#define MUX_PROTO_CONTROL 1
#define MUX_PROTO_SETUP   2
#define MUX_PROTO_TCP     6

#define USB_MRU          16384          /* kích thước mỗi lần đọc bulk-IN        */
#define USB_MTU          (3 * 16384)    /* gói mux tối đa khi GỬI                */
#define DEV_MRU          65536          /* gói mux tối đa khi NHẬN               */
#define CONN_INBUF_SIZE  262144
#define CONN_OUTBUF_SIZE 65536
#define TCP_TX_WINDOW    131072         /* quảng bá dạng (win >> 8) như upstream */
#define ACK_TIMEOUT_MS   30

#define TH_FIN  0x01
#define TH_SYN  0x02
#define TH_RST  0x04
#define TH_PUSH 0x08
#define TH_ACK  0x10

/* Mã kết quả của giao thức client (usbmuxd-proto.h) */
#define RESULT_OK          0
#define RESULT_BADCOMMAND  1
#define RESULT_BADDEV      2
#define RESULT_CONNREFUSED 3
#define RESULT_BADVERSION  6

#define MESSAGE_RESULT 1
#define MESSAGE_PLIST  8

#define MAX_CLIENTS      64
#define CLIENT_MAX_MSG   (1024 * 1024)
#define CONNECT_TIMEOUT_MS   4500   /* libusbmuxd chỉ chờ kết quả Connect 5 s   */
#define VERSION_RETRY_MS     1500
#define VERSION_MAX_TRIES    8
#define USB_WRITE_TIMEOUT_MS 5000

#define PLACEHOLDER_UDID "pending-device"

#pragma pack(push, 1)
struct mux_header {           /* big-endian; header v1 chỉ dùng 8 byte đầu */
    uint32_t protocol;
    uint32_t length;
    uint32_t magic;
    uint16_t tx_seq;
    uint16_t rx_seq;
};
struct version_header {
    uint32_t major;
    uint32_t minor;
    uint32_t padding;
};
struct mux_tcphdr {           /* bố cục struct tcphdr BSD, 20 byte */
    uint16_t sport;
    uint16_t dport;
    uint32_t seq;
    uint32_t ack;
    uint8_t  off;             /* 0x50 = 5 word, như upstream th_off = 5 */
    uint8_t  flags;
    uint16_t win;
    uint16_t sum;
    uint16_t urp;
};
struct umux_header {          /* little-endian — header socket của libusbmuxd */
    uint32_t length;
    uint32_t version;
    uint32_t message;
    uint32_t tag;
};
#pragma pack(pop)

enum dev_state  { DEV_STOPPED = 0, DEV_INIT, DEV_ACTIVE, DEV_DEAD };
enum conn_state { CONN_CONNECTING, CONN_CONNECTED, CONN_REFUSED, CONN_DYING, CONN_DEAD };
enum cl_state   { CL_COMMAND, CL_LISTEN, CL_CONNECTING, CL_CONNECTED, CL_DEAD };

struct mux_conn;

struct client {
    int fd;
    enum cl_state state;
    uint8_t *ib; uint32_t ib_size, ib_cap;   /* lệnh plist đang đọc          */
    uint8_t *ob; uint32_t ob_size, ob_cap;   /* phản hồi chưa ghi xong       */
    uint32_t connect_tag;
    struct mux_conn *conn;
};

struct mux_conn {
    struct client *client;
    enum conn_state state;
    uint16_t sport, dport;
    uint32_t tx_seq, tx_ack, tx_acked, tx_win;
    uint32_t rx_seq, rx_recvd, rx_ack, rx_win;
    uint32_t max_payload, sendable;
    int ack_pending;
    uint8_t *ib_buf; uint32_t ib_size, ib_cap;  /* dữ liệu iPhone → client */
    uint64_t last_ack_time;
    uint64_t connect_deadline;
    struct mux_conn *next;
};

struct usb_xfer {             /* một transfer bulk-IN, luồng đọc → luồng lõi */
    struct usb_xfer *next;
    uint32_t len;
    int completed;            /* 1 = transfer kết thúc (ngắn hơn bộ đệm) */
    int dead;                 /* 1 = luồng đọc báo thiết bị đã mất       */
    uint8_t data[];
};

/* ── Trạng thái toàn cục ─────────────────────────────────────────────────── */
static int g_verbose = 0;

static pthread_mutex_t g_state_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_state_cv  = PTHREAD_COND_INITIALIZER;
static volatile int    g_running   = 0;
static volatile enum dev_state g_dev_state = DEV_STOPPED;
static char   g_udid[128];
static int    g_product_id = 0;
static char   g_files_dir[512];
static char   g_sock_path[512];
static char   g_sock_addr[520];
static char   g_config_dir[600];
static int    g_listen_fd = -1;
static int    g_wake_pipe[2] = { -1, -1 };
static pthread_t g_core_thread;
static pthread_t g_reader_thread;
static int    g_core_started = 0, g_reader_started = 0;
static volatile int g_reader_run = 0;

static pthread_mutex_t g_q_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct usb_xfer *g_q_head = NULL, *g_q_tail = NULL;

/* Chỉ luồng lõi được đọc/ghi các biến dưới đây. */
static struct {
    int      id;
    int      version;
    uint16_t tx_seq, rx_seq;
    uint16_t next_sport;
    uint8_t  pktbuf[DEV_MRU + USB_MRU];
    uint32_t pktlen;
    uint64_t version_sent_at;
    int      version_tries;
} g_dev;

static struct client  *g_clients[MAX_CLIENTS];
static struct mux_conn *g_conns = NULL;
static uint8_t g_txbuf[USB_MTU];
static uint8_t g_obuf[CONN_OUTBUF_SIZE];

/* ── Tiện ích ─────────────────────────────────────────────────────────────── */
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void set_dev_state(enum dev_state st) {
    pthread_mutex_lock(&g_state_mtx);
    g_dev_state = st;
    pthread_cond_broadcast(&g_state_cv);
    pthread_mutex_unlock(&g_state_mtx);
}

static void wake_core(void) {
    if (g_wake_pipe[1] >= 0) {
        char c = 1;
        ssize_t r;
        do { r = write(g_wake_pipe[1], &c, 1); } while (r < 0 && errno == EINTR);
    }
}

static void get_udid_copy(char *out, size_t n) {
    pthread_mutex_lock(&g_state_mtx);
    snprintf(out, n, "%s", g_udid[0] ? g_udid : PLACEHOLDER_UDID);
    pthread_mutex_unlock(&g_state_mtx);
}

static uint32_t rd_be32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return ntohl(v); }

/* ════════════════════════════════════════════════════════════════════════
 * Cấu hình: SystemBUID + pair record (port src/conf.c của usbmuxd)
 * Lưu ở <filesDir>/lockdown/ — riêng tư của app, tồn tại qua các lần mở app
 * nên KHÔNG phải bấm "Tin cậy" lại mỗi lần.
 * ════════════════════════════════════════════════════════════════════════ */
static int ensure_config_dir(void) {
    struct stat st;
    if (stat(g_config_dir, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    if (mkdir(g_config_dir, 0700) == 0 || errno == EEXIST) return 0;
    LOGE("[usbmux] Không tạo được thư mục %s: %s", g_config_dir, strerror(errno));
    return -1;
}

static int read_file(const char *path, char **data, uint32_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 8 * 1024 * 1024) { fclose(f); return -1; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return -1; }
    fclose(f);
    buf[sz] = 0;
    *data = buf;
    *size = (uint32_t)sz;
    return 0;
}

static int write_file_atomic(const char *path, const char *data, uint32_t size) {
    char tmp[700];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, size, f) == size;
    if (fflush(f) != 0) ok = 0;
    fsync(fileno(f));
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

static plist_t plist_from_any(const char *data, uint32_t size) {
    plist_t p = NULL;
    if (!data || size < 8) return NULL;
    if (memcmp(data, "bplist00", 8) == 0) plist_from_bin(data, size, &p);
    else plist_from_xml(data, size, &p);
    return p;
}

/* Tên file an toàn: chỉ giữ [A-Za-z0-9._-] (UDID dạng 40 hex hoặc 8-16 hex). */
static int record_path(const char *record_id, char *out, size_t n) {
    char clean[160];
    size_t j = 0;
    if (!record_id || !record_id[0]) return -1;
    for (size_t i = 0; record_id[i] && j + 1 < sizeof(clean); i++) {
        char c = record_id[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.')
            clean[j++] = c;
    }
    clean[j] = 0;
    if (!j || strcmp(clean, ".") == 0 || strcmp(clean, "..") == 0) return -1;
    snprintf(out, n, "%s/%s.plist", g_config_dir, clean);
    return 0;
}

static void generate_uuid(char *out /* >= 37 */) {
    unsigned char b[16];
    int ok = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) { ok = read(fd, b, sizeof(b)) == (ssize_t)sizeof(b); close(fd); }
    if (!ok) {
        srand((unsigned)(time(NULL) ^ getpid()));
        for (int i = 0; i < 16; i++) b[i] = (unsigned char)(rand() & 0xff);
    }
    b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
    snprintf(out, 37, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

/* upstream config_get_system_buid(): đọc SystemBUID, chưa có thì sinh + lưu. */
static void config_get_system_buid(char *out, size_t n) {
    char path[700];
    snprintf(path, sizeof(path), "%s/SystemConfiguration.plist", g_config_dir);
    char *data = NULL; uint32_t size = 0;
    if (read_file(path, &data, &size) == 0) {
        plist_t p = plist_from_any(data, size);
        free(data);
        if (p) {
            plist_t node = plist_dict_get_item(p, "SystemBUID");
            char *s = NULL;
            if (node && plist_get_node_type(node) == PLIST_STRING) plist_get_string_val(node, &s);
            plist_free(p);
            if (s && strlen(s) >= 32) { snprintf(out, n, "%s", s); free(s); return; }
            free(s);
        }
    }
    char uuid[40];
    generate_uuid(uuid);
    snprintf(out, n, "%s", uuid);
    if (ensure_config_dir() == 0) {
        plist_t p = plist_new_dict();
        plist_dict_set_item(p, "SystemBUID", plist_new_string(uuid));
        char *xml = NULL; uint32_t xlen = 0;
        plist_to_xml(p, &xml, &xlen);
        plist_free(p);
        if (xml) {
            if (write_file_atomic(path, xml, xlen) == 0)
                LOGI("[usbmux] Đã tạo SystemBUID mới: %s", uuid);
            free(xml);
        }
    }
}

/* Pair record có thể bị lưu dưới UDID tạm (trước khi lockdown báo UDID thật):
 * đọc thử tên kia nếu tên được hỏi không có. */
static const char *alias_record_id(const char *record_id, char *buf, size_t n) {
    char udid[128];
    get_udid_copy(udid, sizeof(udid));
    if (strcmp(record_id, PLACEHOLDER_UDID) == 0 && strcmp(udid, PLACEHOLDER_UDID) != 0) {
        snprintf(buf, n, "%s", udid);
        return buf;
    }
    if (strcmp(record_id, udid) == 0) {
        snprintf(buf, n, "%s", PLACEHOLDER_UDID);
        return buf;
    }
    return NULL;
}

static int config_get_device_record(const char *record_id, char **data, uint64_t *size) {
    char path[700];
    char *raw = NULL; uint32_t rawsz = 0;
    if (record_path(record_id, path, sizeof(path)) != 0) return -EINVAL;
    if (read_file(path, &raw, &rawsz) != 0) {
        char alias[128];
        const char *a = alias_record_id(record_id, alias, sizeof(alias));
        if (!a || record_path(a, path, sizeof(path)) != 0 || read_file(path, &raw, &rawsz) != 0)
            return -ENOENT;
    }
    plist_t p = plist_from_any(raw, rawsz);
    free(raw);
    if (!p) return -ENOENT;
    char *xml = NULL; uint32_t xlen = 0;
    plist_to_xml(p, &xml, &xlen);
    plist_free(p);
    if (!xml) return -ENOENT;
    *data = xml;
    *size = xlen;
    return 0;
}

static int config_set_device_record(const char *record_id, const char *data, uint64_t size) {
    char path[700];
    if (!record_id || !data || size < 8) return -EINVAL;
    plist_t p = plist_from_any(data, (uint32_t)size);
    if (!p || plist_get_node_type(p) != PLIST_DICT) { if (p) plist_free(p); return -EINVAL; }
    if (ensure_config_dir() != 0 || record_path(record_id, path, sizeof(path)) != 0) {
        plist_free(p); return -EACCES;
    }
    char *xml = NULL; uint32_t xlen = 0;
    plist_to_xml(p, &xml, &xlen);
    plist_free(p);
    if (!xml) return -ENOMEM;
    int r = write_file_atomic(path, xml, xlen);
    free(xml);
    if (r != 0) { LOGE("[usbmux] Không ghi được pair record %s: %s", path, strerror(errno)); return -EACCES; }
    return 0;
}

static int config_remove_device_record(const char *record_id) {
    char path[700];
    if (record_path(record_id, path, sizeof(path)) != 0) return -EINVAL;
    if (unlink(path) != 0 && errno != ENOENT) return -errno;
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════
 * USB: gửi gói mux (port send_packet của device.c)
 * ════════════════════════════════════════════════════════════════════════ */
static void dev_mark_dead(const char *why);

static int dev_send_packet(uint32_t proto, const void *hdr, int hdrlen, const void *data, int len) {
    int mhs = (g_dev.version < 2) ? 8 : (int)sizeof(struct mux_header);
    int total = mhs + hdrlen + len;
    if (total > USB_MTU) {
        LOGE("[usbmux] Gói %d byte vượt USB_MTU — bỏ", total);
        return -1;
    }
    struct mux_header *mh = (struct mux_header *)g_txbuf;
    mh->protocol = htonl(proto);
    mh->length = htonl((uint32_t)total);
    if (g_dev.version >= 2) {
        mh->magic = htonl(0xfeedface);
        if (proto == MUX_PROTO_SETUP) {
            g_dev.tx_seq = 0;
            g_dev.rx_seq = 0xFFFF;
        }
        mh->tx_seq = htons(g_dev.tx_seq);
        mh->rx_seq = htons(g_dev.rx_seq);
        g_dev.tx_seq++;
    }
    if (hdrlen) memcpy(g_txbuf + mhs, hdr, (size_t)hdrlen);
    if (len && data) memcpy(g_txbuf + mhs + hdrlen, data, (size_t)len);
    int r = usb_bridge_bulk_write(g_txbuf, total, USB_WRITE_TIMEOUT_MS);
    if (r != total) {
        LOGE("[usbmux] Ghi USB thất bại (%d/%d byte)", r, total);
        dev_mark_dead("usb write");
        return -1;
    }
    return total;
}

static int send_version_packet(void) {
    struct version_header vh;
    vh.major = htonl(2);
    vh.minor = htonl(0);
    vh.padding = 0;
    g_dev.version_sent_at = now_ms();
    g_dev.version_tries++;
    LOGI("[usbmux] Gửi VERSION 2.0 tới iPhone (lần %d)", g_dev.version_tries);
    return dev_send_packet(MUX_PROTO_VERSION, &vh, sizeof(vh), NULL, 0);
}

/* ════════════════════════════════════════════════════════════════════════
 * Client (libusbmuxd) — ghi phản hồi plist
 * ════════════════════════════════════════════════════════════════════════ */
static void client_close(struct client *c);
static void conn_teardown(struct mux_conn *conn);

static int client_flush(struct client *c) {
    while (c->ob_size > 0) {
        ssize_t n = send(c->fd, c->ob, c->ob_size, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            memmove(c->ob, c->ob + n, c->ob_size - (uint32_t)n);
            c->ob_size -= (uint32_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        return -1;
    }
    return 0;
}

static int client_queue(struct client *c, const void *data, uint32_t len) {
    if (c->ob_size + len > c->ob_cap) {
        uint32_t cap = c->ob_cap ? c->ob_cap : 4096;
        while (cap < c->ob_size + len) cap *= 2;
        uint8_t *nb = realloc(c->ob, cap);
        if (!nb) return -1;
        c->ob = nb;
        c->ob_cap = cap;
    }
    memcpy(c->ob + c->ob_size, data, len);
    c->ob_size += len;
    return client_flush(c);
}

static int send_plist(struct client *c, uint32_t tag, plist_t dict) {
    char *xml = NULL;
    uint32_t xlen = 0;
    plist_to_xml(dict, &xml, &xlen);
    if (!xml) return -1;
    struct umux_header h;
    h.length = (uint32_t)sizeof(h) + xlen;
    h.version = 1;
    h.message = MESSAGE_PLIST;
    h.tag = tag;
    int r = client_queue(c, &h, sizeof(h));
    if (r == 0) r = client_queue(c, xml, xlen);
    free(xml);
    return r;
}

static int send_result(struct client *c, uint32_t tag, uint32_t result) {
    plist_t d = plist_new_dict();
    plist_dict_set_item(d, "MessageType", plist_new_string("Result"));
    plist_dict_set_item(d, "Number", plist_new_uint(result));
    int r = send_plist(c, tag, d);
    plist_free(d);
    return r;
}

static plist_t create_device_attached_plist(void) {
    char udid[128];
    get_udid_copy(udid, sizeof(udid));
    plist_t dict = plist_new_dict();
    plist_dict_set_item(dict, "MessageType", plist_new_string("Attached"));
    plist_dict_set_item(dict, "DeviceID", plist_new_uint((uint64_t)g_dev.id));
    plist_t props = plist_new_dict();
    plist_dict_set_item(props, "ConnectionSpeed", plist_new_uint(480000000));
    plist_dict_set_item(props, "ConnectionType", plist_new_string("USB"));
    plist_dict_set_item(props, "DeviceID", plist_new_uint((uint64_t)g_dev.id));
    plist_dict_set_item(props, "LocationID", plist_new_uint(0));
    plist_dict_set_item(props, "ProductID", plist_new_uint((uint64_t)(g_product_id & 0xffff)));
    plist_dict_set_item(props, "SerialNumber", plist_new_string(udid));
    plist_dict_set_item(dict, "Properties", props);
    return dict;
}

static void notify_listeners_attached(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = g_clients[i];
        if (c && c->state == CL_LISTEN) {
            plist_t d = create_device_attached_plist();
            if (send_plist(c, 0, d) < 0) c->state = CL_DEAD;
            plist_free(d);
        }
    }
}

static void notify_listeners_detached(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = g_clients[i];
        if (c && c->state == CL_LISTEN) {
            plist_t d = plist_new_dict();
            plist_dict_set_item(d, "MessageType", plist_new_string("Detached"));
            plist_dict_set_item(d, "DeviceID", plist_new_uint((uint64_t)g_dev.id));
            if (send_plist(c, 0, d) < 0) c->state = CL_DEAD;
            plist_free(d);
        }
    }
}

/* client_notify_connect() của upstream */
static int client_notify_connect(struct client *c, uint32_t result) {
    if (!c || c->state == CL_DEAD) return -1;
    if (c->state != CL_CONNECTING) return -1;
    if (send_result(c, c->connect_tag, result) < 0) return -1;
    if (result == RESULT_OK) {
        c->state = CL_CONNECTED;          /* từ đây socket là đường ống thô */
        free(c->ib); c->ib = NULL; c->ib_size = c->ib_cap = 0;
    } else {
        c->state = CL_COMMAND;
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════
 * Kết nối TCP-over-USB (port device.c)
 * ════════════════════════════════════════════════════════════════════════ */
static struct mux_conn *find_conn(uint16_t sport, uint16_t dport) {
    for (struct mux_conn *c = g_conns; c; c = c->next)
        if (c->sport == sport && c->dport == dport && c->state != CONN_DEAD) return c;
    return NULL;
}

static uint16_t find_sport(void) {
    for (int guard = 0; guard < 65536; guard++) {
        if (g_dev.next_sport == 0) g_dev.next_sport = 1;
        int used = 0;
        for (struct mux_conn *c = g_conns; c; c = c->next)
            if (c->sport == g_dev.next_sport) { used = 1; break; }
        if (!used) return g_dev.next_sport++;
        g_dev.next_sport++;
    }
    return 0;
}

static int send_tcp(struct mux_conn *conn, uint8_t flags, const uint8_t *data, int len) {
    struct mux_tcphdr th;
    memset(&th, 0, sizeof(th));
    th.sport = htons(conn->sport);
    th.dport = htons(conn->dport);
    th.seq   = htonl(conn->tx_seq);
    th.ack   = htonl(conn->tx_ack);
    th.flags = flags;
    th.off   = 0x50;
    th.win   = htons((uint16_t)(conn->tx_win >> 8));
    LOGD("[OUT] sport=%u dport=%u seq=%u ack=%u flags=0x%02x len=%d",
         conn->sport, conn->dport, conn->tx_seq, conn->tx_ack, flags, len);
    int r = dev_send_packet(MUX_PROTO_TCP, &th, sizeof(th), data, len);
    if (r >= 0) {
        conn->tx_acked = conn->tx_ack;
        conn->last_ack_time = now_ms();
        conn->ack_pending = 0;
    }
    return r;
}

static int send_anon_rst(uint16_t sport, uint16_t dport, uint32_t ack) {
    struct mux_tcphdr th;
    memset(&th, 0, sizeof(th));
    th.sport = htons(sport);
    th.dport = htons(dport);
    th.ack   = htonl(ack);
    th.flags = TH_RST;
    th.off   = 0x50;
    LOGD("[OUT] anon RST sport=%u dport=%u", sport, dport);
    return dev_send_packet(MUX_PROTO_TCP, &th, sizeof(th), NULL, 0);
}

static void update_connection(struct mux_conn *conn) {
    uint32_t sent = conn->tx_seq - conn->rx_ack;
    conn->sendable = (conn->rx_win > sent) ? conn->rx_win - sent : 0;
    if (conn->sendable > CONN_OUTBUF_SIZE) conn->sendable = CONN_OUTBUF_SIZE;
    if (conn->sendable > conn->max_payload) conn->sendable = conn->max_payload;
    conn->ack_pending = (conn->tx_acked != conn->tx_ack);
}

static void conn_unlink_free(struct mux_conn *conn) {
    struct mux_conn **pp = &g_conns;
    while (*pp) {
        if (*pp == conn) { *pp = conn->next; break; }
        pp = &(*pp)->next;
    }
    free(conn->ib_buf);
    free(conn);
}

/* Ghi dữ liệu iPhone→client không chặn; tx_ack chỉ tăng theo số byte ĐÃ giao
 * cho client (giống upstream master) — nên iPhone không bao giờ gửi vượt bộ
 * đệm 256 KiB của ta. Trả -1 nếu socket client lỗi. */
static int conn_flush_to_client(struct mux_conn *conn) {
    struct client *c = conn->client;
    if (!c || c->state != CL_CONNECTED || c->ob_size > 0) return 0;
    while (conn->ib_size > 0) {
        ssize_t n = send(c->fd, conn->ib_buf, conn->ib_size, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            conn->tx_ack += (uint32_t)n;
            memmove(conn->ib_buf, conn->ib_buf + n, conn->ib_size - (uint32_t)n);
            conn->ib_size -= (uint32_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        return -1;
    }
    update_connection(conn);
    return 0;
}

static void best_effort_flush(struct mux_conn *conn, int budget_ms) {
    struct client *c = conn->client;
    if (!c) return;
    uint64_t end = now_ms() + (uint64_t)budget_ms;
    while ((conn->ib_size > 0 || c->ob_size > 0) && now_ms() < end) {
        if (c->ob_size > 0 && client_flush(c) < 0) return;
        if (c->ob_size == 0 && conn_flush_to_client(conn) < 0) return;
        if (conn->ib_size == 0 && c->ob_size == 0) break;
        struct pollfd p = { .fd = c->fd, .events = POLLOUT, .revents = 0 };
        if (poll(&p, 1, 50) < 0 && errno != EINTR) return;
    }
}

/* connection_teardown() của upstream: RST tới iPhone (trừ khi iPhone đã RST),
 * báo client (ECONNREFUSED nếu chưa kết nối xong, hoặc đóng socket). */
static void conn_teardown(struct mux_conn *conn) {
    if (!conn || conn->state == CONN_DEAD) return;
    LOGD("teardown sport=%u dport=%u state=%d", conn->sport, conn->dport, conn->state);
    enum conn_state prev = conn->state;
    if (g_dev_state == DEV_ACTIVE && prev != CONN_DYING && prev != CONN_REFUSED)
        send_tcp(conn, TH_RST, NULL, 0);
    conn->state = CONN_DEAD;
    struct client *c = conn->client;
    conn->client = NULL;
    if (c) {
        c->conn = NULL;
        if (prev == CONN_REFUSED || prev == CONN_CONNECTING) {
            if (client_notify_connect(c, RESULT_CONNREFUSED) < 0) c->state = CL_DEAD;
        } else {
            conn->client = c;
            best_effort_flush(conn, 1000);
            conn->client = NULL;
            c->state = CL_DEAD;
        }
    }
    conn_unlink_free(conn);
}

static int device_start_connect(struct client *c, uint16_t dport) {
    if (g_dev_state != DEV_ACTIVE) return -RESULT_BADDEV;
    uint16_t sport = find_sport();
    if (!sport) return -RESULT_BADDEV;
    struct mux_conn *conn = calloc(1, sizeof(*conn));
    if (!conn) return -RESULT_BADDEV;
    conn->ib_buf = malloc(CONN_INBUF_SIZE);
    if (!conn->ib_buf) { free(conn); return -RESULT_BADDEV; }
    conn->ib_cap = CONN_INBUF_SIZE;
    conn->client = c;
    conn->state = CONN_CONNECTING;
    conn->sport = sport;
    conn->dport = dport;
    conn->tx_win = TCP_TX_WINDOW;
    conn->max_payload = USB_MTU - sizeof(struct mux_header) - sizeof(struct mux_tcphdr);
    conn->connect_deadline = now_ms() + CONNECT_TIMEOUT_MS;
    conn->next = g_conns;
    g_conns = conn;
    if (send_tcp(conn, TH_SYN, NULL, 0) < 0) {
        conn->client = NULL;
        conn_unlink_free(conn);
        return -RESULT_CONNREFUSED;
    }
    c->conn = conn;
    LOGD("SYN sport=%u → cổng iPhone %u", sport, dport);
    return 0;
}

static void conn_device_input(struct mux_conn *conn, const uint8_t *payload, uint32_t len) {
    if (conn->ib_size + len > conn->ib_cap) {
        LOGE("[usbmux] Tràn bộ đệm kết nối cổng %u (%u + %u) — đóng", conn->dport, conn->ib_size, len);
        conn_teardown(conn);
        return;
    }
    memcpy(conn->ib_buf + conn->ib_size, payload, len);
    conn->ib_size += len;
    conn->rx_recvd += len;
    if (conn_flush_to_client(conn) < 0) {
        conn_teardown(conn);
        return;
    }
    update_connection(conn);
}

/* device_tcp_input() của upstream */
static void dev_tcp_input(const struct mux_tcphdr *th, const uint8_t *payload, uint32_t plen) {
    uint16_t sport = ntohs(th->dport);   /* cổng của ta */
    uint16_t dport = ntohs(th->sport);   /* cổng dịch vụ trên iPhone */
    LOGD("[IN] sport=%u dport=%u seq=%u ack=%u flags=0x%02x win=%u len=%u",
         dport, sport, ntohl(th->seq), ntohl(th->ack), th->flags, (unsigned)ntohs(th->win) << 8, plen);

    if (g_dev_state != DEV_ACTIVE) return;   /* gói cũ của phiên trước */

    struct mux_conn *conn = find_conn(sport, dport);
    if (!conn) {
        if (!(th->flags & TH_RST)) send_anon_rst(sport, dport, ntohl(th->seq));
        return;
    }
    conn->rx_seq = ntohl(th->seq);
    conn->rx_ack = ntohl(th->ack);
    conn->rx_win = (uint32_t)ntohs(th->win) << 8;

    if (th->flags & TH_RST) {
        char reason[160];
        uint32_t n = plen < sizeof(reason) - 1 ? plen : (uint32_t)sizeof(reason) - 1;
        uint32_t j = 0;
        for (uint32_t i = 0; i < n; i++) {
            char ch = (char)payload[i];
            if (ch >= 32 && ch < 127) reason[j++] = ch;
        }
        reason[j] = 0;
        LOGD("RST từ iPhone (cổng %u): %s", dport, reason);
    }

    if (conn->state == CONN_CONNECTING) {
        if (th->flags != (TH_SYN | TH_ACK)) {
            if (th->flags & TH_RST) conn->state = CONN_REFUSED;
            LOGI("[usbmux] iPhone từ chối kết nối tới cổng %u", dport);
            conn_teardown(conn);
        } else {
            conn->tx_seq++;
            conn->tx_ack++;
            conn->rx_recvd = conn->rx_seq;
            if (send_tcp(conn, TH_ACK, NULL, 0) < 0) { conn_teardown(conn); return; }
            conn->state = CONN_CONNECTED;
            LOGD("kết nối cổng %u OK (sport=%u)", dport, sport);
            if (client_notify_connect(conn->client, RESULT_OK) < 0) {
                if (conn->client) conn->client->conn = NULL;
                conn->client = NULL;
                conn_teardown(conn);
                return;
            }
            update_connection(conn);
        }
    } else if (conn->state == CONN_CONNECTED) {
        /* Upstream: mọi cờ khác ACK = reset. Chấp nhận thêm PSH|ACK cho chắc. */
        if ((th->flags & (uint8_t)~TH_PUSH) != TH_ACK) {
            if (th->flags & TH_RST) conn->state = CONN_DYING;
            LOGD("iPhone đóng kết nối cổng %u (flags 0x%02x)", dport, th->flags);
            conn_teardown(conn);
        } else {
            if (plen) conn_device_input(conn, payload, plen);
            else update_connection(conn);
            if (conn->state == CONN_CONNECTED) {
                /* "Device likes it best when we are promptly ACKing data" */
                if (plen) send_tcp(conn, TH_ACK, NULL, 0);
            }
        }
    }
}

static void dev_control_input(const uint8_t *payload, uint32_t len) {
    if (!len) return;
    char msg[256];
    uint32_t n = (len - 1) < sizeof(msg) - 1 ? (len - 1) : (uint32_t)sizeof(msg) - 1;
    memcpy(msg, payload + 1, n);
    msg[n] = 0;
    switch (payload[0]) {
        case 3: LOGE("[usbmux] iPhone báo lỗi: %s", msg); break;
        case 5: LOGI("[usbmux] iPhone cảnh báo: %s", msg); break;
        case 7: LOGD("iPhone: %s", msg); break;
        default: LOGD("control %u: %s", payload[0], msg); break;
    }
}

static void dev_version_input(const struct version_header *vh) {
    if (g_dev_state != DEV_INIT) {
        LOGD("VERSION thừa từ iPhone — bỏ qua");
        return;
    }
    uint32_t major = ntohl(vh->major), minor = ntohl(vh->minor);
    if (major != 1 && major != 2) {
        LOGE("[usbmux] iPhone trả phiên bản mux lạ %u.%u", major, minor);
        dev_mark_dead("bad version");
        return;
    }
    g_dev.version = (int)major;
    if (g_dev.version >= 2) {
        if (dev_send_packet(MUX_PROTO_SETUP, NULL, 0, "\x07", 1) < 0) return;
    }
    char udid[128];
    get_udid_copy(udid, sizeof(udid));
    LOGI("[usbmux] ✅ Đã bắt tay mux v%u.%u với iPhone (UDID %s)", major, minor, udid);
    set_dev_state(DEV_ACTIVE);
    notify_listeners_attached();
}

/* Xử lý MỘT gói mux hoàn chỉnh (device_data_input của upstream, phần sau
 * khi đã gom đủ gói). */
static void dev_packet_input(const uint8_t *pkt, uint32_t len) {
    uint32_t proto = rd_be32(pkt);
    int mhs = (g_dev.version < 2) ? 8 : (int)sizeof(struct mux_header);

    if (proto == MUX_PROTO_VERSION) {
        /* Trả lời VERSION thường dùng header 8 byte; nếu iPhone còn ở chế độ
         * v2 của phiên trước thì có thể kèm header 16 byte (có magic). */
        uint32_t magic = len >= 12 ? rd_be32(pkt + 8) : 0;
        mhs = (len >= 16 + sizeof(struct version_header) &&
               (magic == 0xfeedface || magic == 0xfaceface)) ? 16 : 8;
        if (len < (uint32_t)mhs + sizeof(struct version_header)) {
            LOGE("[usbmux] Gói VERSION quá ngắn (%u)", len);
            return;
        }
        struct version_header vh;
        memcpy(&vh, pkt + mhs, sizeof(vh));
        dev_version_input(&vh);
        return;
    }
    if (g_dev.version >= 2 && len >= sizeof(struct mux_header)) {
        uint16_t rx;
        memcpy(&rx, pkt + 14, 2);
        g_dev.rx_seq = ntohs(rx);      /* upstream: dev->rx_seq = ntohs(mhdr->rx_seq) */
    }
    switch (proto) {
        case MUX_PROTO_CONTROL:
            if (len >= (uint32_t)mhs) dev_control_input(pkt + mhs, len - (uint32_t)mhs);
            break;
        case MUX_PROTO_TCP: {
            if (len < (uint32_t)mhs + sizeof(struct mux_tcphdr)) {
                LOGE("[usbmux] Gói TCP quá ngắn (%u)", len);
                return;
            }
            struct mux_tcphdr th;
            memcpy(&th, pkt + mhs, sizeof(th));
            const uint8_t *payload = pkt + mhs + sizeof(th);
            uint32_t plen = len - (uint32_t)mhs - (uint32_t)sizeof(th);
            dev_tcp_input(&th, payload, plen);
            break;
        }
        case MUX_PROTO_SETUP:
            break;
        default:
            LOGD("gói mux protocol lạ 0x%x (%u byte)", proto, len);
            break;
    }
}

/* Gom transfer thành gói mux. Transfer ngắn hơn bộ đệm = hết một transfer của
 * iPhone → phần dư chưa đủ gói là rác (upstream cũng bỏ). */
static void dev_data_input(const uint8_t *buf, uint32_t len, int completed) {
    if (len == 0) return;
    if (g_dev.pktlen + len > sizeof(g_dev.pktbuf)) {
        LOGE("[usbmux] Bộ đệm gom gói tràn — đồng bộ lại");
        g_dev.pktlen = 0;
        if (len > sizeof(g_dev.pktbuf)) return;
    }
    memcpy(g_dev.pktbuf + g_dev.pktlen, buf, len);
    g_dev.pktlen += len;

    uint32_t off = 0;
    while (g_dev.pktlen - off >= 8) {
        uint32_t plen = rd_be32(g_dev.pktbuf + off + 4);
        if (plen < 8 || plen > DEV_MRU) {
            LOGE("[usbmux] Độ dài gói mux không hợp lệ (%u) — bỏ %u byte, đồng bộ lại",
                 plen, g_dev.pktlen - off);
            g_dev.pktlen = 0;
            return;
        }
        if (g_dev.pktlen - off < plen) break;
        dev_packet_input(g_dev.pktbuf + off, plen);
        off += plen;
        if (g_dev_state == DEV_DEAD || !g_running) { g_dev.pktlen = 0; return; }
    }
    if (off > 0) {
        memmove(g_dev.pktbuf, g_dev.pktbuf + off, g_dev.pktlen - off);
        g_dev.pktlen -= off;
    }
    if (g_dev.pktlen > 0 && completed) {
        LOGD("bỏ %u byte gói mux dở dang (transfer đã kết thúc)", g_dev.pktlen);
        g_dev.pktlen = 0;
    }
}

static void dev_mark_dead(const char *why) {
    if (g_dev_state == DEV_DEAD) return;
    enum dev_state prev = g_dev_state;
    LOGE("[usbmux] ❌ Mất kết nối USB với iPhone (%s)", why);
    set_dev_state(DEV_DEAD);
    struct mux_conn *c = g_conns;
    while (c) {
        struct mux_conn *nx = c->next;
        conn_teardown(c);            /* không gửi RST khi DEV_DEAD */
        c = nx;
    }
    if (prev == DEV_ACTIVE) notify_listeners_detached();
}

/* ════════════════════════════════════════════════════════════════════════
 * Luồng đọc USB — CHỈ đọc; mọi xử lý nằm ở luồng lõi
 * ════════════════════════════════════════════════════════════════════════ */
static void queue_push(const uint8_t *data, uint32_t len, int completed, int dead) {
    struct usb_xfer *x = malloc(sizeof(*x) + len);
    if (!x) return;
    x->next = NULL;
    x->len = len;
    x->completed = completed;
    x->dead = dead;
    if (len) memcpy(x->data, data, len);
    pthread_mutex_lock(&g_q_mtx);
    if (g_q_tail) g_q_tail->next = x; else g_q_head = x;
    g_q_tail = x;
    pthread_mutex_unlock(&g_q_mtx);
    wake_core();
}

static struct usb_xfer *queue_take_all(void) {
    pthread_mutex_lock(&g_q_mtx);
    struct usb_xfer *h = g_q_head;
    g_q_head = g_q_tail = NULL;
    pthread_mutex_unlock(&g_q_mtx);
    return h;
}

static void *usb_reader_main(void *arg) {
    (void)arg;
    uint8_t *buf = malloc(USB_MRU);
    int errors = 0;
    if (!buf) return NULL;
    while (g_reader_run) {
        int completed = 1;
        int n = usb_bridge_bulk_read_ex(buf, USB_MRU, 500, &completed);
        if (n > 0) {
            errors = 0;
            queue_push(buf, (uint32_t)n, completed && n < USB_MRU, 0);
            continue;
        }
        if (n == 0) { errors = 0; continue; }      /* timeout hoặc ZLP */
        if (!g_reader_run) break;
        if (++errors >= 6) {
            queue_push(NULL, 0, 1, 1);
            break;
        }
        usleep(150 * 1000);
    }
    free(buf);
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════
 * Lệnh plist từ libusbmuxd (client_command() của upstream)
 * ════════════════════════════════════════════════════════════════════════ */
static char *dict_get_string(plist_t dict, const char *key) {
    plist_t n = plist_dict_get_item(dict, key);
    char *s = NULL;
    if (n && plist_get_node_type(n) == PLIST_STRING) plist_get_string_val(n, &s);
    return s;
}

static uint64_t dict_get_uint(plist_t dict, const char *key, uint64_t def) {
    plist_t n = plist_dict_get_item(dict, key);
    uint64_t v = def;
    if (n && plist_get_node_type(n) == PLIST_UINT) plist_get_uint_val(n, &v);
    return v;
}

static int handle_plist_command(struct client *c, uint32_t tag, plist_t dict) {
    char *msg = dict_get_string(dict, "MessageType");
    if (!msg) return send_result(c, tag, RESULT_BADCOMMAND);
    int r = 0;
    LOGD("client fd=%d: %s", c->fd, msg);

    if (!strcmp(msg, "Listen")) {
        r = send_result(c, tag, RESULT_OK);
        c->state = CL_LISTEN;
        if (r == 0 && g_dev_state == DEV_ACTIVE) {
            plist_t d = create_device_attached_plist();
            r = send_plist(c, 0, d);
            plist_free(d);
        }
    } else if (!strcmp(msg, "ListDevices")) {
        plist_t d = plist_new_dict();
        plist_t arr = plist_new_array();
        if (g_dev_state == DEV_ACTIVE) plist_array_append_item(arr, create_device_attached_plist());
        plist_dict_set_item(d, "DeviceList", arr);
        r = send_plist(c, tag, d);       /* KHÔNG có MessageType — như upstream */
        plist_free(d);
    } else if (!strcmp(msg, "ListListeners")) {
        plist_t d = plist_new_dict();
        plist_dict_set_item(d, "ListenerList", plist_new_array());
        r = send_plist(c, tag, d);
        plist_free(d);
    } else if (!strcmp(msg, "Connect")) {
        uint32_t device_id = (uint32_t)dict_get_uint(dict, "DeviceID", 0);
        uint16_t portnum = (uint16_t)dict_get_uint(dict, "PortNumber", 0);
        uint16_t port = ntohs(portnum);   /* libusbmuxd gửi htons(port) */
        int res;
        if ((int)device_id != g_dev.id) res = -RESULT_BADDEV;
        else res = device_start_connect(c, port);
        if (res < 0) {
            r = send_result(c, tag, (uint32_t)(-res));
        } else {
            c->connect_tag = tag;
            c->state = CL_CONNECTING;
        }
    } else if (!strcmp(msg, "ReadBUID")) {
        char buid[64];
        config_get_system_buid(buid, sizeof(buid));
        plist_t d = plist_new_dict();
        plist_dict_set_item(d, "BUID", plist_new_string(buid));
        r = send_plist(c, tag, d);       /* KHÔNG có MessageType — như upstream */
        plist_free(d);
    } else if (!strcmp(msg, "ReadPairRecord")) {
        char *rid = dict_get_string(dict, "PairRecordID");
        char *data = NULL;
        uint64_t size = 0;
        int res = rid ? config_get_device_record(rid, &data, &size) : -EINVAL;
        if (res == 0) {
            plist_t d = plist_new_dict();
            plist_dict_set_item(d, "PairRecordData", plist_new_data(data, size));
            r = send_plist(c, tag, d);
            plist_free(d);
        } else {
            r = send_result(c, tag, (uint32_t)(-res));   /* ENOENT = 2 → "chưa pair" */
        }
        LOGD("ReadPairRecord(%s) → %s", rid ? rid : "?", res == 0 ? "có" : "không có");
        free(data);
        free(rid);
    } else if (!strcmp(msg, "SavePairRecord")) {
        char *rid = dict_get_string(dict, "PairRecordID");
        plist_t rdata = plist_dict_get_item(dict, "PairRecordData");
        char *data = NULL;
        uint64_t size = 0;
        uint32_t rval = RESULT_OK;
        if (rdata && plist_get_node_type(rdata) == PLIST_DATA) plist_get_data_val(rdata, &data, &size);
        if (rid && data) {
            int res = config_set_device_record(rid, data, size);
            if (res < 0) rval = (uint32_t)(-res);
            else LOGI("[usbmux] ✅ Đã lưu pair record cho %s", rid);
        } else {
            rval = EINVAL;
        }
        r = send_result(c, tag, rval);
        free(data);
        free(rid);
    } else if (!strcmp(msg, "DeletePairRecord")) {
        char *rid = dict_get_string(dict, "PairRecordID");
        uint32_t rval = RESULT_OK;
        if (rid) {
            int res = config_remove_device_record(rid);
            if (res < 0) rval = (uint32_t)(-res);
            else LOGI("[usbmux] Đã xoá pair record của %s", rid);
        } else {
            rval = EINVAL;
        }
        r = send_result(c, tag, rval);
        free(rid);
    } else {
        LOGD("lệnh không hỗ trợ: %s", msg);
        r = send_result(c, tag, RESULT_BADCOMMAND);
    }
    free(msg);
    return r;
}

/* Xử lý các thông điệp đã đọc đủ trong c->ib. */
static int client_process_input(struct client *c) {
    while (c->state == CL_COMMAND || c->state == CL_LISTEN) {
        if (c->ib_size < sizeof(struct umux_header)) return 0;
        struct umux_header h;
        memcpy(&h, c->ib, sizeof(h));
        if (h.length < sizeof(h) || h.length > CLIENT_MAX_MSG) return -1;
        if (c->ib_size < h.length) return 0;
        uint32_t plen = h.length - (uint32_t)sizeof(h);
        int r;
        if (h.version != 1 || h.message != MESSAGE_PLIST) {
            /* chỉ hỗ trợ giao thức plist (libusbmuxd 2.x mặc định dùng plist) */
            r = send_result(c, h.tag, RESULT_BADCOMMAND);
        } else {
            plist_t dict = plist_from_any((const char *)c->ib + sizeof(h), plen);
            if (!dict || plist_get_node_type(dict) != PLIST_DICT) {
                if (dict) plist_free(dict);
                r = send_result(c, h.tag, RESULT_BADCOMMAND);
            } else {
                r = handle_plist_command(c, h.tag, dict);
                plist_free(dict);
            }
        }
        if (c->ib) {
            memmove(c->ib, c->ib + h.length, c->ib_size - h.length);
            c->ib_size -= h.length;
        }
        if (r < 0) return -1;
    }
    return 0;
}

static int client_read_commands(struct client *c) {
    if (c->ib_cap - c->ib_size < 4096) {
        uint32_t cap = c->ib_cap ? c->ib_cap * 2 : 8192;
        if (cap > CLIENT_MAX_MSG + 8192) cap = CLIENT_MAX_MSG + 8192;
        if (cap <= c->ib_size) return -1;
        uint8_t *nb = realloc(c->ib, cap);
        if (!nb) return -1;
        c->ib = nb;
        c->ib_cap = cap;
    }
    ssize_t n = recv(c->fd, c->ib + c->ib_size, c->ib_cap - c->ib_size, MSG_DONTWAIT);
    if (n == 0) return -1;
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    c->ib_size += (uint32_t)n;
    return client_process_input(c);
}

/* client → iPhone (device_client_process, nhánh POLLIN) */
static void client_data_to_device(struct client *c) {
    struct mux_conn *conn = c->conn;
    if (!conn || conn->state != CONN_CONNECTED) return;
    update_connection(conn);
    if (conn->sendable == 0) return;
    ssize_t n = recv(c->fd, g_obuf, conn->sendable, MSG_DONTWAIT);
    if (n == 0) {                         /* client đóng socket → RST */
        c->state = CL_DEAD;
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        c->state = CL_DEAD;
        return;
    }
    if (send_tcp(conn, TH_ACK, g_obuf, (int)n) < 0) {   /* dữ liệu: chỉ ACK, KHÔNG PSH */
        c->state = CL_DEAD;
        return;
    }
    conn->tx_seq += (uint32_t)n;
    update_connection(conn);
}

static void client_close(struct client *c) {
    if (!c) return;
    for (int i = 0; i < MAX_CLIENTS; i++) if (g_clients[i] == c) g_clients[i] = NULL;
    struct mux_conn *conn = c->conn;
    c->conn = NULL;
    if (conn) {
        conn->client = NULL;
        conn_teardown(conn);
    }
    if (c->fd >= 0) close(c->fd);
    free(c->ib);
    free(c->ob);
    free(c);
}

static void accept_clients(void) {
    for (;;) {
        int fd = accept(g_listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            return;
        }
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) if (!g_clients[i]) { slot = i; break; }
        struct client *c = slot >= 0 ? calloc(1, sizeof(*c)) : NULL;
        if (!c) { close(fd); LOGE("[usbmux] Quá nhiều client — từ chối"); continue; }
        c->fd = fd;
        c->state = CL_COMMAND;
        g_clients[slot] = c;
        LOGD("client mới fd=%d", fd);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 * Luồng lõi
 * ════════════════════════════════════════════════════════════════════════ */
static void process_usb_queue(void) {
    struct usb_xfer *x = queue_take_all();
    while (x) {
        struct usb_xfer *nx = x->next;
        if (x->dead) dev_mark_dead("đọc USB lỗi liên tục — rút cáp hoặc thiết bị khoá USB");
        else if (g_dev_state == DEV_INIT || g_dev_state == DEV_ACTIVE) dev_data_input(x->data, x->len, x->completed);
        free(x);
        x = nx;
    }
}

static void check_timers(void) {
    uint64_t t = now_ms();
    if (g_dev_state == DEV_INIT && t - g_dev.version_sent_at >= VERSION_RETRY_MS) {
        if (g_dev.version_tries >= VERSION_MAX_TRIES) {
            LOGE("[usbmux] ❌ iPhone không trả lời VERSION sau %d lần — kiểm tra: cáp có "
                 "truyền dữ liệu (không phải cáp chỉ sạc), iPhone đã mở khoá, cổng OTG", g_dev.version_tries);
            dev_mark_dead("no VERSION reply");
        } else {
            send_version_packet();
        }
    }
    struct mux_conn *c = g_conns;
    while (c) {
        struct mux_conn *nx = c->next;
        if (c->state == CONN_CONNECTING && t >= c->connect_deadline) {
            LOGI("[usbmux] Hết thời gian chờ SYN+ACK từ cổng %u", c->dport);
            conn_teardown(c);
        } else if (c->state == CONN_CONNECTED) {
            if (c->ib_size > 0) conn_flush_to_client(c);
            if (c->ack_pending && t - c->last_ack_time >= ACK_TIMEOUT_MS) send_tcp(c, TH_ACK, NULL, 0);
        }
        c = nx;
    }
}

static int compute_timeout(void) {
    int timeout = 1000;
    uint64_t t = now_ms();
    if (g_dev_state == DEV_INIT) {
        uint64_t due = g_dev.version_sent_at + VERSION_RETRY_MS;
        int d = due > t ? (int)(due - t) : 0;
        if (d < timeout) timeout = d;
    }
    for (struct mux_conn *c = g_conns; c; c = c->next) {
        if (c->state == CONN_CONNECTING) {
            int d = c->connect_deadline > t ? (int)(c->connect_deadline - t) : 0;
            if (d < timeout) timeout = d;
        } else if (c->state == CONN_CONNECTED && (c->ack_pending || c->ib_size > 0)) {
            if (timeout > ACK_TIMEOUT_MS) timeout = ACK_TIMEOUT_MS;
        }
    }
    return timeout;
}

static void *core_main(void *arg) {
    (void)arg;
    signal(SIGPIPE, SIG_IGN);
    struct pollfd pfds[2 + MAX_CLIENTS];
    struct client *pcl[2 + MAX_CLIENTS];

    if (send_version_packet() < 0) LOGE("[usbmux] Không gửi được VERSION — USB chưa sẵn sàng");

    while (g_running) {
        int n = 0;
        pfds[n].fd = g_listen_fd; pfds[n].events = POLLIN; pfds[n].revents = 0; pcl[n] = NULL; n++;
        pfds[n].fd = g_wake_pipe[0]; pfds[n].events = POLLIN; pfds[n].revents = 0; pcl[n] = NULL; n++;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            struct client *c = g_clients[i];
            if (!c) continue;
            short ev = 0;
            if (c->ob_size > 0) ev |= POLLOUT;
            switch (c->state) {
                case CL_COMMAND:
                case CL_LISTEN:
                    ev |= POLLIN;
                    break;
                case CL_CONNECTED:
                    if (c->conn && c->conn->state == CONN_CONNECTED) {
                        update_connection(c->conn);
                        if (c->conn->sendable > 0 && c->ob_size == 0) ev |= POLLIN;
                        if (c->conn->ib_size > 0) ev |= POLLOUT;
                    }
                    break;
                default:
                    break;
            }
            pfds[n].fd = c->fd; pfds[n].events = ev; pfds[n].revents = 0; pcl[n] = c; n++;
        }

        int pr = poll(pfds, (nfds_t)n, compute_timeout());
        if (!g_running) break;
        if (pr < 0 && errno != EINTR) {
            LOGE("[usbmux] poll() lỗi: %s", strerror(errno));
            usleep(10 * 1000);
        }

        if (pfds[1].revents & POLLIN) {
            char drain[64];
            while (read(g_wake_pipe[0], drain, sizeof(drain)) > 0) {}
        }
        process_usb_queue();

        if (pfds[0].revents & POLLIN) accept_clients();

        for (int i = 2; i < n; i++) {
            struct client *c = pcl[i];
            int alive = 0;
            for (int k = 0; k < MAX_CLIENTS; k++) if (g_clients[k] == c) { alive = 1; break; }
            if (!alive || c->state == CL_DEAD) continue;
            short re = pfds[i].revents;
            if (!re) continue;

            if ((re & POLLOUT) && c->ob_size > 0 && client_flush(c) < 0) { c->state = CL_DEAD; continue; }

            if (c->state == CL_CONNECTED) {
                if ((re & POLLOUT) && c->conn && c->conn->ib_size > 0) {
                    if (conn_flush_to_client(c->conn) < 0) { c->state = CL_DEAD; continue; }
                }
                if (re & POLLIN) client_data_to_device(c);
                if ((re & (POLLHUP | POLLERR | POLLNVAL)) && !(re & POLLIN)) c->state = CL_DEAD;
            } else if (c->state == CL_COMMAND || c->state == CL_LISTEN) {
                if (re & (POLLIN | POLLHUP | POLLERR)) {
                    if (client_read_commands(c) < 0) c->state = CL_DEAD;
                }
            } else if (c->state == CL_CONNECTING) {
                if (re & (POLLHUP | POLLERR | POLLNVAL)) c->state = CL_DEAD;
            }
        }

        check_timers();

        for (int i = 0; i < MAX_CLIENTS; i++) {
            struct client *c = g_clients[i];
            if (c && c->state == CL_DEAD) client_close(c);
        }
    }

    /* dọn dẹp */
    for (int i = 0; i < MAX_CLIENTS; i++) if (g_clients[i]) client_close(g_clients[i]);
    while (g_conns) {
        struct mux_conn *c = g_conns;
        if (g_dev_state == DEV_ACTIVE && c->state == CONN_CONNECTED) send_tcp(c, TH_RST, NULL, 0);
        c->client = NULL;
        conn_unlink_free(c);
    }
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════
 * API công khai
 * ════════════════════════════════════════════════════════════════════════ */
bool usbmuxd_server_start(const char *files_dir, const char *udid, int product_id) {
    if (g_running || g_core_started || g_reader_started) usbmuxd_server_stop();

    const char *v = getenv("SIDELOAD_MUX_VERBOSE");
    g_verbose = (v && v[0] == '1');

    snprintf(g_files_dir, sizeof(g_files_dir), "%s", files_dir ? files_dir : "/tmp");
    snprintf(g_config_dir, sizeof(g_config_dir), "%s/lockdown", g_files_dir);
    snprintf(g_sock_path, sizeof(g_sock_path), "%s/usbmuxd.sock", g_files_dir);
    snprintf(g_sock_addr, sizeof(g_sock_addr), "UNIX:%s", g_sock_path);
    ensure_config_dir();

    pthread_mutex_lock(&g_state_mtx);
    snprintf(g_udid, sizeof(g_udid), "%s", (udid && udid[0]) ? udid : PLACEHOLDER_UDID);
    pthread_mutex_unlock(&g_state_mtx);
    g_product_id = product_id;

    memset(&g_dev, 0, sizeof(g_dev));
    g_dev.id = 1;
    g_dev.version = 0;
    g_dev.next_sport = (uint16_t)(1 + (now_ms() % 2000));   /* tránh socket cũ phía iPhone */
    for (int i = 0; i < MAX_CLIENTS; i++) g_clients[i] = NULL;
    g_conns = NULL;

    struct sockaddr_un addr;
    if (strlen(g_sock_path) >= sizeof(addr.sun_path)) {
        LOGE("[usbmux] Đường dẫn socket quá dài: %s", g_sock_path);
        return false;
    }
    unlink(g_sock_path);
    g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen_fd < 0) { LOGE("[usbmux] socket(): %s", strerror(errno)); return false; }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_sock_path);
    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(g_listen_fd, 16) < 0) {
        LOGE("[usbmux] bind/listen %s: %s", g_sock_path, strerror(errno));
        close(g_listen_fd); g_listen_fd = -1;
        unlink(g_sock_path);
        return false;
    }
    chmod(g_sock_path, 0600);
    int fl = fcntl(g_listen_fd, F_GETFL, 0);
    if (fl >= 0) fcntl(g_listen_fd, F_SETFL, fl | O_NONBLOCK);
    fcntl(g_listen_fd, F_SETFD, FD_CLOEXEC);

    if (pipe(g_wake_pipe) != 0) {
        LOGE("[usbmux] pipe(): %s", strerror(errno));
        close(g_listen_fd); g_listen_fd = -1; unlink(g_sock_path);
        return false;
    }
    for (int i = 0; i < 2; i++) {
        int f = fcntl(g_wake_pipe[i], F_GETFL, 0);
        if (f >= 0) fcntl(g_wake_pipe[i], F_SETFL, f | O_NONBLOCK);
        fcntl(g_wake_pipe[i], F_SETFD, FD_CLOEXEC);
    }

    set_dev_state(DEV_INIT);
    g_running = 1;
    g_reader_run = 1;
    if (pthread_create(&g_reader_thread, NULL, usb_reader_main, NULL) != 0) {
        LOGE("[usbmux] Không tạo được luồng đọc USB");
        usbmuxd_server_stop();
        return false;
    }
    g_reader_started = 1;
    if (pthread_create(&g_core_thread, NULL, core_main, NULL) != 0) {
        LOGE("[usbmux] Không tạo được luồng lõi");
        usbmuxd_server_stop();
        return false;
    }
    g_core_started = 1;
    LOGI("[usbmux] usbmuxd nội bộ đang chạy: %s", g_sock_addr);
    return true;
}

bool usbmuxd_server_device_ready(int timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ns = (uint64_t)ts.tv_nsec + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000000ull;
    ts.tv_sec += (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    pthread_mutex_lock(&g_state_mtx);
    while (g_dev_state == DEV_INIT && timeout_ms > 0) {
        if (pthread_cond_timedwait(&g_state_cv, &g_state_mtx, &ts) == ETIMEDOUT) break;
    }
    bool ok = (g_dev_state == DEV_ACTIVE);
    pthread_mutex_unlock(&g_state_mtx);
    return ok;
}

int usbmuxd_server_device_state(void) {
    return (int)g_dev_state;
}

bool usbmux_version_exchange(void) {
    return usbmuxd_server_device_ready(15000);
}

void usbmuxd_server_reset_version_state(void) {
    /* Giữ để tương thích ABI. Phiên mux chỉ bắt tay lại khi có USB fd mới
     * (usbmuxd_server_start); reset giữa chừng sẽ giết các kết nối đang mở. */
    LOGD("reset_version_state: bỏ qua (không cần với usbmuxd chuẩn)");
}

void usbmuxd_server_update_udid(const char *udid) {
    if (!udid || !udid[0]) return;
    pthread_mutex_lock(&g_state_mtx);
    int changed = strcmp(g_udid, udid) != 0;
    snprintf(g_udid, sizeof(g_udid), "%s", udid);
    pthread_mutex_unlock(&g_state_mtx);
    if (changed) LOGI("[usbmux] UDID thiết bị: %s", udid);
}

const char *usbmuxd_server_socket_path(void) {
    return (g_running && g_listen_fd >= 0) ? g_sock_path : NULL;
}

const char *usbmuxd_server_socket_address(void) {
    return (g_running && g_listen_fd >= 0) ? g_sock_addr : NULL;
}

const char *usbmuxd_server_config_dir(void) {
    return g_config_dir[0] ? g_config_dir : NULL;
}

void usbmuxd_server_stop(void) {
    int was = g_running || g_core_started || g_reader_started;
    g_running = 0;
    g_reader_run = 0;
    wake_core();
    if (g_core_started) { pthread_join(g_core_thread, NULL); g_core_started = 0; }
    if (g_reader_started) { pthread_join(g_reader_thread, NULL); g_reader_started = 0; }
    struct usb_xfer *x = queue_take_all();
    while (x) { struct usb_xfer *nx = x->next; free(x); x = nx; }
    if (g_listen_fd >= 0) { close(g_listen_fd); g_listen_fd = -1; }
    for (int i = 0; i < 2; i++) if (g_wake_pipe[i] >= 0) { close(g_wake_pipe[i]); g_wake_pipe[i] = -1; }
    if (g_sock_path[0]) unlink(g_sock_path);
    set_dev_state(DEV_STOPPED);
    if (was) LOGI("[usbmux] usbmuxd nội bộ đã dừng");
}
