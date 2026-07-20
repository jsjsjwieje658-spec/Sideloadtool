/*
 * usbmuxd_server.c  v35 (NO-ROOT)
 *
 * FIX tất cả lỗi để giao tiếp iPhone trên Android no-root:
 *   1. Packet reassembly cho USB fragmented packets
 *   2. Connection state machine
 *   3. Drain non-TCP packets trước SYN
 *   4. TCP header chuẩn (struct tcphdr)
 *   5. Đọc đúng payload length từ header
 *   6. Fix usb_recv_tcp - không bỏ qua packet có payload=0 (SYN+ACK)
 *   7. Fix tunnel - gửi ACK sau nhận data
 *   8. Proper cleanup khi disconnect
 */

#include "usbmuxd_server.h"
#include "usbmux.h"
#include "usb_fd_bridge.h"
#include "android_usbmuxd_fix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <time.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include <fcntl.h>

/* Protocol constants */
#define V1_MAGIC         0xfeedface
#define V1_PROTO_VER     0
#define V1_PROTO_CONTROL 1
#define V1_PROTO_SETUP   2
#ifdef V1_PROTO_TCP
#undef V1_PROTO_TCP
#endif
#define V1_PROTO_TCP     6   /* IPPROTO_TCP */

/* TCP flags */
#define TH_FIN  0x01
#define TH_SYN  0x02
#define TH_RST  0x04
#define TH_PUSH 0x08
#define TH_ACK  0x10

/* Packet structures */
typedef struct {
    uint32_t protocol;
    uint32_t length;
} v0_mux_hdr_t;

typedef struct {
    uint32_t protocol;
    uint32_t length;
    uint32_t magic;
    uint16_t tx_seq;
    uint16_t rx_seq;
} v1_mux_hdr_t;

/* Version header used during the v1 version-exchange handshake */
struct version_header {
    uint32_t major;
    uint32_t minor;
    uint8_t  padding;
};

/* ════════════════════════════════════════════════════════════════════════
 * Packet reassembly buffer
 * ════════════════════════════════════════════════════════════════════════ */
#define DEV_MRU 65536
static uint8_t  g_pktbuf[DEV_MRU];
static uint32_t g_pktlen = 0;
static pthread_mutex_t g_pktbuf_lock = PTHREAD_MUTEX_INITIALIZER;

/* Device state */
typedef enum {
    DEV_STATE_INIT,
    DEV_STATE_ACTIVE,
    DEV_STATE_DEAD
} device_state_t;

static device_state_t g_dev_state = DEV_STATE_INIT;
static pthread_mutex_t g_dev_state_lock = PTHREAD_MUTEX_INITIALIZER;

device_state_t usbmuxd_server_device_state(void) {
    pthread_mutex_lock(&g_dev_state_lock);
    device_state_t s = g_dev_state;
    pthread_mutex_unlock(&g_dev_state_lock);
    return s;
}

static void set_device_state(device_state_t s) {
    pthread_mutex_lock(&g_dev_state_lock);
    g_dev_state = s;
    pthread_mutex_unlock(&g_dev_state_lock);
}

/* Connection state */
typedef enum {
    TCP_CONN_IDLE,
    TCP_CONN_CONNECTING,
    TCP_CONN_CONNECTED,
    TCP_CONN_REFUSED,
    TCP_CONN_CLOSING,
    TCP_CONN_CLOSED
} tcp_conn_state_t;

typedef struct {
    tcp_conn_state_t state;
    uint16_t sport;
    uint16_t dport;
    uint32_t local_seq;
    uint32_t remote_seq;
    uint32_t tx_ack;
    uint32_t rx_win;
    uint32_t tx_win;
    pthread_mutex_t usb_tx_lock;
} tcp_state_t;

/* Mux sequence */
static uint16_t g_mux_tx_seq = 0;
static uint16_t g_mux_rx_seq = 0xFFFF;
static pthread_mutex_t g_mux_seq_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_version_done = 0;

/* Server state */
static char g_sock_path[256];
static int  g_listen_fd = -1;
static volatile int g_server_running = 0;
static pthread_t g_server_thread;

static char g_udid[64] = "";
static pthread_mutex_t g_udid_lock = PTHREAD_MUTEX_INITIALIZER;

static int g_listen_fds[64];
static int g_listen_count = 0;
static pthread_mutex_t g_listen_lock = PTHREAD_MUTEX_INITIALIZER;

/* Logging */
#define LOGI(fmt, ...) do { \
    char _buf[512]; \
    snprintf(_buf, sizeof(_buf), "[usbmuxd_srv] " fmt, ##__VA_ARGS__); \
    android_usbmuxd_fix_log(_buf); \
} while(0)

#define LOGE(fmt, ...) do { \
    char _buf[512]; \
    snprintf(_buf, sizeof(_buf), "[usbmuxd_srv] " fmt, ##__VA_ARGS__); \
    android_usbmuxd_fix_log(_buf); \
} while(0)

#define LOGW(fmt, ...) do { \
    char _buf[512]; \
    snprintf(_buf, sizeof(_buf), "[usbmuxd_srv] " fmt, ##__VA_ARGS__); \
    android_usbmuxd_fix_log(_buf); \
} while(0)

/* ════════════════════════════════════════════════════════════════════════
 * FIX v35: Packet reassembly — đọc packet hoàn chỉnh từ USB
 * 
 * iPhone có thể gửi packet lớn hơn buffer hoặc fragmented.
 * Chúng ta cần đọc đúng số byte theo length field trong header.
 * ════════════════════════════════════════════════════════════════════════ */
static int read_mux_packet(v1_mux_hdr_t *hdr, uint8_t *body, int max_body, int timeout_ms) {
    /* Đọc header (16 bytes) */
    uint8_t hdr_buf[16];
    int n = usb_bridge_bulk_read(hdr_buf, sizeof(hdr_buf), timeout_ms);
    if (n < (int)sizeof(hdr_buf)) {
        return -1;
    }

    memcpy(hdr, hdr_buf, sizeof(v1_mux_hdr_t));

    uint32_t pkt_len = ntohl(hdr->length);
    uint32_t body_len = (pkt_len > sizeof(v1_mux_hdr_t)) ? (pkt_len - sizeof(v1_mux_hdr_t)) : 0;

    /* Validate */
    if (pkt_len < sizeof(v1_mux_hdr_t) || pkt_len > DEV_MRU) {
        LOGE("read_mux_packet: invalid pkt_len=%u", pkt_len);
        return -1;
    }

    if (body_len > 0) {
        if (body_len > (uint32_t)max_body) {
            /* Packet quá lớn — đọc và bỏ */
            uint8_t *discard = malloc(body_len);
            if (discard) {
                usb_bridge_bulk_read(discard, body_len, timeout_ms);
                free(discard);
            }
            return -1;
        }

        n = usb_bridge_bulk_read(body, body_len, timeout_ms);
        if (n < (int)body_len) {
            LOGE("read_mux_packet: partial body read %d/%u", n, body_len);
            return -1;
        }
    }

    return (int)body_len;
}

/* ════════════════════════════════════════════════════════════════════════
 * USB helpers
 * ════════════════════════════════════════════════════════════════════════ */
static int usb_write(const void *buf, int len) {
    int total = 0;
    while (total < len) {
        int n = usb_bridge_bulk_write((const uint8_t *)buf + total, len - total, 5000);
        if (n <= 0) {
            LOGE("usb_write: failed at %d/%d", total, len);
            return -1;
        }
        total += n;
    }
    return total;
}

/* ════════════════════════════════════════════════════════════════════════
 * Version exchange
 * ════════════════════════════════════════════════════════════════════════ */
static int usb_send_version(void) {
    uint8_t pkt[sizeof(v0_mux_hdr_t) + sizeof(struct version_header)];
    memset(pkt, 0, sizeof(pkt));

    v0_mux_hdr_t *hdr = (v0_mux_hdr_t *)pkt;
    hdr->protocol = htonl(V1_PROTO_VER);
    hdr->length   = htonl(sizeof(pkt));

    struct version_header *vh = (struct version_header *)(pkt + sizeof(v0_mux_hdr_t));
    vh->major = htonl(2);
    vh->minor = htonl(0);
    vh->padding = 0;

    return usb_write(pkt, sizeof(pkt));
}

static int usb_recv_version(uint32_t *major_out) {
    v1_mux_hdr_t hdr;
    uint8_t body[sizeof(struct version_header)];

    int n = read_mux_packet(&hdr, body, sizeof(body), 5000);
    if (n < (int)sizeof(struct version_header)) return -1;

    if (ntohl(hdr.protocol) != V1_PROTO_VER) {
        LOGE("recv_version: unexpected proto=%d", ntohl(hdr.protocol));
        return -1;
    }

    struct version_header *vh = (struct version_header *)body;
    *major_out = ntohl(vh->major);
    return 0;
}

bool usbmux_version_exchange(void) {
    if (g_version_done) return true;

    for (int attempt = 0; attempt < 5; attempt++) {
        if (attempt > 0) {
            LOGI("version_exchange retry %d/5", attempt + 1);
            usb_bridge_clear_endpoints_halt();
            usb_bridge_flush_in(8, 200);
            usleep(1500 * 1000);
        }

        if (usb_send_version() < 0) continue;

        uint32_t major = 0;
        if (usb_recv_version(&major) == 0) {
            if (major >= 2) {
                /* Send SETUP */
                uint8_t setup_pkt[sizeof(v1_mux_hdr_t) + 1];
                memset(setup_pkt, 0, sizeof(setup_pkt));
                v1_mux_hdr_t *sh = (v1_mux_hdr_t *)setup_pkt;
                sh->protocol = htonl(V1_PROTO_SETUP);
                sh->length   = htonl(sizeof(setup_pkt));
                sh->magic    = htonl(V1_MAGIC);
                sh->tx_seq   = htons(0);
                sh->rx_seq   = htons(0xFFFF);
                setup_pkt[sizeof(v1_mux_hdr_t)] = 0x07;

                pthread_mutex_lock(&g_mux_seq_lock);
                g_mux_tx_seq = 1;
                g_mux_rx_seq = 0xFFFF;
                pthread_mutex_unlock(&g_mux_seq_lock);

                if (usb_write(setup_pkt, sizeof(setup_pkt)) < 0) {
                    LOGE("usb_send_setup failed");
                    continue;
                }
                LOGI("SETUP sent OK");
            }

            g_version_done = 1;
            set_device_state(DEV_STATE_ACTIVE);
            LOGI("version exchange OK (major=%u)", major);
            return true;
        }
    }
    return false;
}

/* ════════════════════════════════════════════════════════════════════════
 * FIX v35: TCP send/receive
 * 
 * Dùng struct tcphdr chuẩn từ netinet/tcp.h
 * ════════════════════════════════════════════════════════════════════════ */
static int usb_send_tcp(tcp_state_t *st, uint8_t flags, const void *data, uint32_t data_len) {
    uint32_t total = sizeof(v1_mux_hdr_t) + sizeof(struct tcphdr) + data_len;
    uint8_t *pkt = calloc(1, total);
    if (!pkt) return -1;

    v1_mux_hdr_t *mhdr = (v1_mux_hdr_t *)pkt;
    mhdr->protocol = htonl(V1_PROTO_TCP);
    mhdr->length   = htonl(total);
    mhdr->magic    = htonl(V1_MAGIC);

    pthread_mutex_lock(&g_mux_seq_lock);
    mhdr->tx_seq = htons(g_mux_tx_seq);
    mhdr->rx_seq = htons(g_mux_rx_seq);
    g_mux_tx_seq++;
    pthread_mutex_unlock(&g_mux_seq_lock);

    struct tcphdr *th = (struct tcphdr *)(pkt + sizeof(v1_mux_hdr_t));
    th->th_sport = htons(st->sport);
    th->th_dport = htons(st->dport);
    th->th_seq   = htonl(st->local_seq);
    th->th_ack   = htonl(st->remote_seq);
    th->th_off   = sizeof(struct tcphdr) / 4;  /* 5 = 20 bytes */
    th->th_flags = flags;
    th->th_win   = htons(st->rx_win >> 8);

    if (data && data_len > 0)
        memcpy(pkt + sizeof(v1_mux_hdr_t) + sizeof(struct tcphdr), data, data_len);

    pthread_mutex_lock(&st->usb_tx_lock);
    int r = usb_write(pkt, total);
    pthread_mutex_unlock(&st->usb_tx_lock);

    free(pkt);
    return r > 0 ? 0 : -1;
}

/* 
 * FIX v35 (CRITICAL): usb_recv_tcp 
 * 
 * Lỗi cũ: Khi nhận SYN+ACK, payload_len = 0 (vì SYN+ACK không có data).
 * Code cũ return 0 nhưng caller (do_usb_v1_connect) kiểm tra n < 0 → OK.
 * Nhưng sau đó kiểm tra flags — đúng.
 * 
 * Tuy nhiên, lỗi thực sự là: iPhone gửi CONTROL packet (type 7) giữa
 * các TCP packet. Chúng ta cần drain chúng.
 */
static int usb_recv_tcp(tcp_state_t *st, void *data_out, int max_data,
                         uint8_t *flags_out, int timeout_ms) {
    *flags_out = 0;

    for (int retry = 0; retry < 30; retry++) {
        v1_mux_hdr_t hdr;
        uint8_t body[65536];

        int body_len = read_mux_packet(&hdr, body, sizeof(body), timeout_ms);
        if (body_len < 0) {
            if (retry > 0) LOGI("recv_tcp: timeout after %d drains", retry);
            return -1;
        }

        if (ntohl(hdr.magic) != V1_MAGIC) {
            LOGE("recv_tcp: bad magic=0x%08x", ntohl(hdr.magic));
            return -1;
        }

        /* Update rx_seq */
        pthread_mutex_lock(&g_mux_seq_lock);
        g_mux_rx_seq = ntohs(hdr.rx_seq);
        pthread_mutex_unlock(&g_mux_seq_lock);

        uint32_t proto = ntohl(hdr.protocol);

        /* Drain non-TCP packets */
        if (proto == V1_PROTO_CONTROL) {
            if (body_len > 0) {
                LOGI("recv_tcp: drain CONTROL type=%d", body[0]);
            } else {
                LOGI("recv_tcp: drain CONTROL (empty)");
            }
            continue;  /* Retry chờ TCP packet */
        }

        if (proto == V1_PROTO_SETUP) {
            LOGI("recv_tcp: drain SETUP");
            continue;
        }

        if (proto != V1_PROTO_TCP) {
            LOGW("recv_tcp: unexpected proto=%u", proto);
            continue;
        }

        /* TCP packet */
        if (body_len < (int)sizeof(struct tcphdr)) {
            LOGE("recv_tcp: TCP body too small %d < %zu", body_len, sizeof(struct tcphdr));
            return -1;
        }

        struct tcphdr *th = (struct tcphdr *)body;
        uint16_t sport = ntohs(th->th_sport);
        uint16_t dport = ntohs(th->th_dport);

        /* Verify ports */
        if (sport != st->dport || dport != st->sport) {
            LOGW("recv_tcp: port mismatch got %d->%d expect %d->%d",
                 sport, dport, st->dport, st->sport);
            continue;  /* Không phải của connection này */
        }

        st->remote_seq = ntohl(th->th_seq);
        st->tx_ack = ntohl(th->th_ack);
        st->tx_win = ntohs(th->th_win) << 8;
        *flags_out = th->th_flags;

        int payload_len = body_len - sizeof(struct tcphdr);
        if (payload_len > 0 && data_out && max_data > 0) {
            int copy_len = payload_len > max_data ? max_data : payload_len;
            memcpy(data_out, body + sizeof(struct tcphdr), copy_len);
        }

        /* Return payload_len (có thể = 0 cho SYN+ACK) */
        return payload_len;
    }

    return -1;
}

/* ════════════════════════════════════════════════════════════════════════
 * FIX v35: do_usb_v1_connect
 * 
 * Drain non-TCP packets trước SYN
 * Tăng timeout cho user bấm Trust
 * ════════════════════════════════════════════════════════════════════════ */
static bool do_usb_v1_connect(tcp_state_t *st, int port) {
    LOGI("v1_connect: port=%d", port);

    if (usbmuxd_server_device_state() != DEV_STATE_ACTIVE) {
        if (!usbmux_version_exchange()) {
            LOGE("v1_connect: version exchange failed");
            return false;
        }
    }

    /* Init state */
    srand((unsigned)time(NULL));
    st->state = TCP_CONN_CONNECTING;
    st->sport = (uint16_t)(49152 + (rand() % 16383));
    st->dport = (uint16_t)port;
    st->local_seq = (uint32_t)rand();
    st->remote_seq = 0;
    st->tx_ack = 0;
    st->rx_win = 131072;  /* 128KB */
    st->tx_win = 0;
    pthread_mutex_init(&st->usb_tx_lock, NULL);

    /* Drain non-TCP packets từ iPhone */
    LOGI("v1_connect: draining non-TCP packets...");
    int drained = 0;
    for (int i = 0; i < 15; i++) {
        v1_mux_hdr_t hdr;
        uint8_t body[1024];
        int n = read_mux_packet(&hdr, body, sizeof(body), 300);
        if (n < 0) break;

        uint32_t proto = ntohl(hdr.protocol);
        if (proto != V1_PROTO_TCP) {
            if (n > 0 && proto == V1_PROTO_CONTROL) {
                LOGI("v1_connect: drained CONTROL type=%d", body[0]);
            } else {
                LOGI("v1_connect: drained proto=%u", proto);
            }
            drained++;
        } else {
            /* TCP packet bất thường — có thể là stale */
            LOGW("v1_connect: unexpected TCP during drain");
        }
    }
    if (drained > 0) LOGI("v1_connect: drained %d packets", drained);

    /* Send SYN */
    uint32_t isn = st->local_seq;
    LOGI("v1_connect: SYN -> port=%d sport=%d seq=%u", port, st->sport, isn);
    if (usb_send_tcp(st, TH_SYN, NULL, 0) < 0) {
        LOGE("v1_connect: SYN failed");
        st->state = TCP_CONN_CLOSED;
        return false;
    }

    /* Wait SYN+ACK — tăng timeout cho user bấm Trust */
    uint8_t flags = 0;
    uint8_t dummy[1];
    int n = usb_recv_tcp(st, dummy, sizeof(dummy), &flags, 25000);
    if (n < 0) {
        LOGE("v1_connect: SYN+ACK timeout (25s)");
        LOGE("v1_connect: -> Kiểm tra: iPhone unlock? Đã bấm Trust? Cáp data?");
        st->state = TCP_CONN_REFUSED;
        return false;
    }

    if (!(flags & TH_SYN) || !(flags & TH_ACK)) {
        if (flags & TH_RST) {
            LOGE("v1_connect: RST received — port %d refused", port);
            LOGE("v1_connect: -> iPhone chưa Trust hoặc port không mở");
        } else {
            LOGE("v1_connect: unexpected flags=0x%02x", flags);
        }
        st->state = TCP_CONN_REFUSED;
        return false;
    }

    LOGI("v1_connect: SYN+ACK received remote_seq=%u", st->remote_seq);

    /* Send ACK */
    st->local_seq = isn + 1;
    if (usb_send_tcp(st, TH_ACK, NULL, 0) < 0) {
        LOGE("v1_connect: ACK failed");
        st->state = TCP_CONN_CLOSED;
        return false;
    }

    st->state = TCP_CONN_CONNECTED;
    LOGI("v1_connect: ✅ TCP connected port=%d", port);
    return true;
}

/* ════════════════════════════════════════════════════════════════════════
 * FIX v35: Tunnel threads
 * 
 * sock->usb: đọc từ socket, gửi qua USB với ACK flag
 * usb->sock: đọc từ USB, gửi qua socket
 * ════════════════════════════════════════════════════════════════════════ */
typedef struct {
    int client_fd;
    tcp_state_t *st;
} tunnel_arg_t;

static void *tunnel_sock_to_usb(void *arg) {
    tunnel_arg_t *ta = (tunnel_arg_t *)arg;
    int fd = ta->client_fd;
    tcp_state_t *st = ta->st;
    free(ta);

    uint8_t buf[32768];
    while (st->state == TCP_CONN_CONNECTED) {
        int n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                usleep(1000);
                continue;
            }
            LOGI("sock->usb: client closed or error");
            break;
        }

        /* Gửi data với ACK flag */
        if (usb_send_tcp(st, TH_ACK, buf, n) < 0) {
            LOGE("sock->usb: send failed");
            break;
        }
        st->local_seq += n;
    }

    st->state = TCP_CONN_CLOSING;
    return NULL;
}

static void *tunnel_usb_to_sock(void *arg) {
    tunnel_arg_t *ta = (tunnel_arg_t *)arg;
    int fd = ta->client_fd;
    tcp_state_t *st = ta->st;
    free(ta);

    uint8_t data[65536];
    while (st->state == TCP_CONN_CONNECTED) {
        uint8_t flags = 0;
        int n = usb_recv_tcp(st, data, sizeof(data), &flags, 5000);

        if (n < 0) {
            if (st->state != TCP_CONN_CONNECTED) break;
            continue;
        }

        if (flags & TH_RST) {
            LOGI("usb->sock: RST received");
            st->state = TCP_CONN_CLOSING;
            break;
        }

        if (flags & TH_FIN) {
            LOGI("usb->sock: FIN received");
            st->state = TCP_CONN_CLOSING;
            break;
        }

        /* Gửi ACK cho data đã nhận */
        if (n >= 0) {
            st->remote_seq += (n > 0) ? n : 1;  /* SYN/FIN tiêu thụ 1 seq */
            usb_send_tcp(st, TH_ACK, NULL, 0);
        }

        if (n > 0) {
            int sent = 0;
            while (sent < n) {
                int r = send(fd, data + sent, n - sent, MSG_NOSIGNAL);
                if (r <= 0) {
                    LOGE("usb->sock: send failed");
                    st->state = TCP_CONN_CLOSING;
                    break;
                }
                sent += r;
            }
            if (st->state != TCP_CONN_CONNECTED) break;
        }
    }

    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════
 * Plist helpers
 * ════════════════════════════════════════════════════════════════════════ */
static char *make_device_plist(const char *udid, int device_id) {
    char *out = NULL;
    asprintf(&out,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\""
        " \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
        "<plist version=\"1.0\"><dict>"
        "<key>MessageType</key><string>Attached</string>"
        "<key>DeviceID</key><integer>%d</integer>"
        "<key>Properties</key><dict>"
        "<key>SerialNumber</key><string>%s</string>"
        "<key>ConnectionType</key><string>USB</string>"
        "<key>ProductID</key><integer>0x12a8</integer>"
        "</dict></dict></plist>",
        device_id, udid);
    return out;
}

static char *make_detached_event(void) {
    char *out = NULL;
    asprintf(&out,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\""
        " \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
        "<plist version=\"1.0\"><dict>"
        "<key>MessageType</key><string>Detached</string>"
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
        code);
    return out;
}

static char *make_device_list(const char *udid) {
    if (!udid || !udid[0]) {
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

    char *out = NULL;
    asprintf(&out,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\""
        " \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
        "<plist version=\"1.0\"><dict>"
        "<key>MessageType</key><string>Result</string>"
        "<key>Number</key><integer>0</integer>"
        "<key>DeviceList</key><array>"
        "<dict>"
        "<key>DeviceID</key><integer>1</integer>"
        "<key>Properties</key><dict>"
        "<key>SerialNumber</key><string>%s</string>"
        "<key>ConnectionType</key><string>USB</string>"
        "</dict></dict></array></dict></plist>",
        udid);
    return out;
}

/* ════════════════════════════════════════════════════════════════════════
 * Socket helpers
 * ════════════════════════════════════════════════════════════════════════ */
static int send_plist(int fd, uint32_t tag, const char *xml) {
    if (!xml) return -1;
    uint32_t len = (uint32_t)strlen(xml);
    struct {
        uint32_t len;
        uint32_t ver;
        uint32_t msg;
        uint32_t tag;
    } hdr = {
        htonl(len + 16),
        htonl(1),
        htonl(8),
        htonl(tag)
    };
    if (send(fd, &hdr, sizeof(hdr), MSG_NOSIGNAL) != sizeof(hdr)) return -1;
    if (send(fd, xml, len, MSG_NOSIGNAL) != (ssize_t)len) return -1;
    return 0;
}

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Listener registry */
static void register_listener(int fd) {
    pthread_mutex_lock(&g_listen_lock);
    if (g_listen_count < 64) {
        g_listen_fds[g_listen_count++] = fd;
        LOGI("listener registered fd=%d (count=%d)", fd, g_listen_count);
    }
    pthread_mutex_unlock(&g_listen_lock);
}

static void unregister_listener(int fd) {
    pthread_mutex_lock(&g_listen_lock);
    for (int i = 0; i < g_listen_count; i++) {
        if (g_listen_fds[i] == fd) {
            g_listen_fds[i] = g_listen_fds[--g_listen_count];
            LOGI("listener unregistered fd=%d (count=%d)", fd, g_listen_count);
            break;
        }
    }
    pthread_mutex_unlock(&g_listen_lock);
}

void usbmuxd_server_broadcast_attached(void) {
    pthread_mutex_lock(&g_udid_lock);
    char udid[64];
    strncpy(udid, g_udid, sizeof(udid) - 1);
    udid[sizeof(udid) - 1] = '\0';
    pthread_mutex_unlock(&g_udid_lock);

    if (!udid[0]) {
        LOGW("broadcast_attached: no UDID");
        return;
    }

    char *plist = make_device_plist(udid, 1);
    if (!plist) return;

    pthread_mutex_lock(&g_listen_lock);
    for (int i = 0; i < g_listen_count; i++) {
        send_plist(g_listen_fds[i], 0, plist);
    }
    pthread_mutex_unlock(&g_listen_lock);
    free(plist);
    LOGI("broadcast_attached: sent to %d listeners", g_listen_count);
}

void usbmuxd_server_update_udid(const char *udid) {
    pthread_mutex_lock(&g_udid_lock);
    strncpy(g_udid, udid, sizeof(g_udid) - 1);
    g_udid[sizeof(g_udid) - 1] = '\0';
    pthread_mutex_unlock(&g_udid_lock);
}

/* ════════════════════════════════════════════════════════════════════════
 * Client handler
 * ════════════════════════════════════════════════════════════════════════ */
static void *handle_client(void *arg) {
    int client_fd = (int)(intptr_t)arg;
    LOGI("client fd=%d connected", client_fd);

    set_nonblock(client_fd);

    char rxbuf[4096];
    int rxlen = 0;
    int is_listener = 0;

    while (g_server_running) {
        int n = recv(client_fd, rxbuf + rxlen, sizeof(rxbuf) - rxlen, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            break;
        }
        if (n == 0) break;
        rxlen += n;

        if (rxlen < 16) continue;

        uint32_t pkt_len = ntohl(*(uint32_t *)rxbuf);
        if (pkt_len < 16 || pkt_len > sizeof(rxbuf)) {
            LOGE("client fd=%d: invalid pkt_len=%u", client_fd, pkt_len);
            break;
        }
        if (rxlen < (int)pkt_len) continue;

        uint32_t msg_type = ntohl(*(uint32_t *)(rxbuf + 8));
        uint32_t tag = ntohl(*(uint32_t *)(rxbuf + 12));
        char *xml = (char *)(rxbuf + 16);
        int xml_len = pkt_len - 16;

        /* Parse MessageType */
        char msg_type_str[32] = "";
        const char *mt = strstr(xml, "<key>MessageType</key>");
        if (mt) {
            const char *s = strstr(mt, "<string>");
            if (s) {
                s += 8;
                const char *e = strstr(s, "</string>");
                if (e && (size_t)(e - s) < sizeof(msg_type_str)) {
                    memcpy(msg_type_str, s, e - s);
                    msg_type_str[e - s] = '\0';
                }
            }
        }

        LOGI("client fd=%d: msg=%s tag=%u", client_fd, msg_type_str, tag);

        if (strcmp(msg_type_str, "Listen") == 0) {
            register_listener(client_fd);
            is_listener = 1;
            char *resp = make_connect_result(0);
            if (resp) { send_plist(client_fd, tag, resp); free(resp); }

            pthread_mutex_lock(&g_udid_lock);
            char udid[64];
            strncpy(udid, g_udid, sizeof(udid) - 1);
            pthread_mutex_unlock(&g_udid_lock);

            char *list = make_device_list(udid[0] ? udid : NULL);
            if (list) { send_plist(client_fd, tag, list); free(list); }

            if (udid[0]) {
                char *attached = make_device_plist(udid, 1);
                if (attached) { send_plist(client_fd, 0, attached); free(attached); }
            }
        }
        else if (strcmp(msg_type_str, "Connect") == 0) {
            /* Parse port */
            long port_be = 0;
            const char *pn = strstr(xml, "<key>PortNumber</key>");
            if (pn) {
                const char *i = strstr(pn, "<integer>");
                if (i) port_be = atol(i + 9);
            }
            int port = (int)ntohs((uint16_t)(port_be & 0xFFFF));

            unregister_listener(client_fd);

            tcp_state_t *st = calloc(1, sizeof(tcp_state_t));
            bool ok = false;
            if (st) {
                ok = do_usb_v1_connect(st, port);
            }

            char *resp = make_connect_result(ok ? 0 : 3);
            if (resp) { send_plist(client_fd, tag, resp); free(resp); }

            if (!ok) {
                LOGE("client fd=%d: connect failed port=%d", client_fd, port);
                if (st) {
                    pthread_mutex_destroy(&st->usb_tx_lock);
                    free(st);
                }
                break;
            }

            LOGI("client fd=%d: tunnel started port=%d", client_fd, port);

            /* Start tunnel threads */
            tunnel_arg_t *ta1 = malloc(sizeof(tunnel_arg_t));
            tunnel_arg_t *ta2 = malloc(sizeof(tunnel_arg_t));
            if (!ta1 || !ta2) {
                free(ta1); free(ta2);
                pthread_mutex_destroy(&st->usb_tx_lock);
                free(st);
                break;
            }

            ta1->client_fd = client_fd;
            ta1->st = st;
            ta2->client_fd = client_fd;
            ta2->st = st;

            pthread_t t1, t2;
            pthread_create(&t1, NULL, tunnel_sock_to_usb, ta1);
            pthread_create(&t2, NULL, tunnel_usb_to_sock, ta2);

            pthread_join(t1, NULL);
            pthread_join(t2, NULL);

            /* Cleanup */
            if (st->state != TCP_CONN_CLOSED) {
                usb_send_tcp(st, TH_RST, NULL, 0);
            }
            pthread_mutex_destroy(&st->usb_tx_lock);
            free(st);

            LOGI("client fd=%d: tunnel ended", client_fd);
            break;
        }
        else if (strcmp(msg_type_str, "ListDevices") == 0) {
            pthread_mutex_lock(&g_udid_lock);
            char udid[64];
            strncpy(udid, g_udid, sizeof(udid) - 1);
            pthread_mutex_unlock(&g_udid_lock);

            char *list = make_device_list(udid[0] ? udid : NULL);
            if (list) { send_plist(client_fd, tag, list); free(list); }
        }

        rxlen -= pkt_len;
        if (rxlen > 0) memmove(rxbuf, rxbuf + pkt_len, rxlen);
    }

    if (is_listener) unregister_listener(client_fd);

    /* Broadcast detached */
    char *detached = make_detached_event();
    if (detached) {
        pthread_mutex_lock(&g_listen_lock);
        for (int i = 0; i < g_listen_count; i++) {
            if (g_listen_fds[i] != client_fd) {
                send_plist(g_listen_fds[i], 0, detached);
            }
        }
        pthread_mutex_unlock(&g_listen_lock);
        free(detached);
    }

    close(client_fd);
    LOGI("client fd=%d disconnected", client_fd);
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════
 * Server main loop
 * ════════════════════════════════════════════════════════════════════════ */
static void *server_thread(void *arg) {
    (void)arg;
    LOGI("server thread started");

    while (g_server_running) {
        struct sockaddr_un sa;
        socklen_t len = sizeof(sa);
        int client_fd = accept(g_listen_fd, (struct sockaddr *)&sa, &len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            if (errno == EBADF || errno == EINVAL) break;
            LOGE("accept failed: %s", strerror(errno));
            usleep(100000);
            continue;
        }

        LOGI("new connection fd=%d", client_fd);
        pthread_t t;
        pthread_create(&t, NULL, handle_client, (void *)(intptr_t)client_fd);
        pthread_detach(t);
    }

    LOGI("server thread stopped");
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════════════════════════════ */
bool usbmuxd_server_start(const char *files_dir, const char *udid, int product_id) {
    if (g_server_running) return true;

    g_version_done = 0;
    g_mux_tx_seq = 0;
    g_mux_rx_seq = 0xFFFF;
    g_pktlen = 0;
    set_device_state(DEV_STATE_INIT);

    /* Store UDID and product_id if provided */
    if (udid && udid[0]) {
        pthread_mutex_lock(&g_udid_lock);
        strncpy(g_udid, udid, sizeof(g_udid) - 1);
        g_udid[sizeof(g_udid) - 1] = '\0';
        pthread_mutex_unlock(&g_udid_lock);
    }
    (void)product_id; /* currently unused — UDID is the key used by ListDevices */

    g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        LOGE("socket failed: %s", strerror(errno));
        return false;
    }

    snprintf(g_sock_path, sizeof(g_sock_path), "%s/usbmuxd.sock", files_dir);
    unlink(g_sock_path);

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, g_sock_path, sizeof(sa.sun_path) - 1);

    if (bind(g_listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        LOGE("bind failed: %s", strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return false;
    }

    if (listen(g_listen_fd, 16) < 0) {
        LOGE("listen failed: %s", strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return false;
    }

    set_nonblock(g_listen_fd);

    g_server_running = 1;
    pthread_create(&g_server_thread, NULL, server_thread, NULL);

    LOGI("server listening on %s", g_sock_path);
    return true;
}

void usbmuxd_server_stop(void) {
    if (!g_server_running) return;
    g_server_running = 0;

    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }

    pthread_join(g_server_thread, NULL);
    unlink(g_sock_path);
    g_sock_path[0] = '\0';

    pthread_mutex_lock(&g_listen_lock);
    g_listen_count = 0;
    pthread_mutex_unlock(&g_listen_lock);

    LOGI("server stopped");
}

const char *usbmuxd_server_socket_path(void) {
    return g_server_running ? g_sock_path : NULL;
}
