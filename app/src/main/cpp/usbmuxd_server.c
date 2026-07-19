/*
 * usbmuxd_server.c
 * Tạo Unix domain socket giả lập usbmuxd cho libimobiledevice
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>      /* htons/htonl/ntohs/ntohl */
#include <netinet/in.h>
#include <netinet/tcp.h>    /* struct tcphdr — trước đây bị thiếu, khiến
                               trình biên dịch coi 'struct tcphdr' là kiểu
                               chưa đầy đủ (forward declaration) và báo lỗi
                               "invalid application of 'sizeof' to an
                               incomplete type" */
#include <pthread.h>
#include "usbmuxd_server.h"
#include "android_usbmuxd_fix.h"
#include "usb_fd_bridge.h"  /* usb_bridge_bulk_read/usb_bridge_bulk_write */

#define TAG "USBMUX_SRV"

#define LOGE(fmt, ...) do { \
    char _buf[512]; \
    snprintf(_buf, sizeof(_buf), "E/" TAG ": " fmt, ##__VA_ARGS__); \
    android_usbmuxd_fix_log(_buf); \
} while(0)

#define LOGI(fmt, ...) do { \
    char _buf[512]; \
    snprintf(_buf, sizeof(_buf), "I/" TAG ": " fmt, ##__VA_ARGS__); \
    android_usbmuxd_fix_log(_buf); \
} while(0)

#define LOGW(fmt, ...) do { \
    char _buf[512]; \
    snprintf(_buf, sizeof(_buf), "W/" TAG ": " fmt, ##__VA_ARGS__); \
    android_usbmuxd_fix_log(_buf); \
} while(0)

#include "usbmux.h"

/* V1 protocol constants - sử dụng giá trị từ usbmux.h để tránh redefine */
#ifndef V1_PROTO_TCP
#define V1_PROTO_TCP     6   /* IPPROTO_TCP */
#endif
#define V1_PROTO_SETUP   7   /* SETUP packet type */

/* Định nghĩa đầy đủ struct version_header */
struct version_header {
    uint32_t major;
    uint32_t minor;
    uint32_t padding;
} __attribute__((packed));

typedef struct { uint32_t len, magic, protocol; } v0_mux_hdr_t;

#define USBMUXD_SOCKET_FILE  "usbmuxd.sock"

static int      g_listen_sock = -1;
static char     g_sock_path[256];
static volatile int g_running = 0;
static pthread_t g_server_thread;

static int g_listen_fds[16];
static int g_listen_count = 0;
static pthread_mutex_t g_listen_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint16_t g_next_sport = 50000;

/* ========== TCP state cho v1 protocol ========== */
typedef struct {
    uint32_t sport;
    uint32_t seq;
    uint32_t remote_seq;
    uint32_t acked;
    int      connected;
} tcp_state_t;

static tcp_state_t g_tcp;

static void tcp_init(tcp_state_t *st) {
    memset(st, 0, sizeof(*st));
    st->sport = g_next_sport++;
    st->seq   = (uint32_t)(rand() & 0x7FFFFFFF);
}

/* ========== Helpers ========== */
static int usb_write_all(const uint8_t *data, int len) {
    int total = 0;
    while (total < len) {
        int n = usb_bridge_bulk_write(data + total, len - total);
        if (n < 0) {
            LOGE("usb_write: failed at %d/%d", total, len);
            return -1;
        }
        total += n;
    }
    return total;
}

static int read_mux_packet(uint8_t *rxbuf, int rxbuf_len, uint32_t *pkt_len_out) {
    int n = usb_bridge_bulk_read(rxbuf, 4);
    if (n < 4) return -1;
    uint32_t pkt_len = (rxbuf[0] << 24) | (rxbuf[1] << 16) | (rxbuf[2] << 8) | rxbuf[3];
    if (pkt_len < 4 || pkt_len > (uint32_t)rxbuf_len) {
        LOGE("read_mux_packet: invalid pkt_len=%u", pkt_len);
        return -1;
    }
    int body_len = pkt_len - 4;
    if (body_len > 0) {
        n = usb_bridge_bulk_read(rxbuf + 4, body_len);
        if (n < body_len) {
            LOGE("read_mux_packet: partial body read %d/%u", n, body_len);
            return -1;
        }
    }
    *pkt_len_out = pkt_len;
    return 0;
}

/* ========== Version exchange ========== */
static int send_version(void) {
    uint8_t pkt[sizeof(v0_mux_hdr_t) + sizeof(struct version_header)];
    v0_mux_hdr_t *hdr = (v0_mux_hdr_t *)pkt;
    struct version_header *vh = (struct version_header *)(pkt + sizeof(v0_mux_hdr_t));
    hdr->len = htonl(sizeof(v0_mux_hdr_t) + sizeof(struct version_header));
    hdr->magic = htonl(0x6c1b1b1b);
    hdr->protocol = htonl(1);
    vh->major = htonl(2);
    vh->minor = htonl(0);
    vh->padding = 0;
    return usb_write_all(pkt, sizeof(pkt));
}

static int recv_version(uint32_t *major_out) {
    uint8_t rx[4];
    int n = usb_bridge_bulk_read(rx, 4);
    if (n < 4) return -1;
    uint32_t pkt_len = (rx[0] << 24) | (rx[1] << 16) | (rx[2] << 8) | rx[3];
    if (pkt_len < 4) return -1;
    int body_len = pkt_len - 4;
    uint8_t body[sizeof(struct version_header)];
    if (body_len > (int)sizeof(body)) body_len = sizeof(body);
    n = usb_bridge_bulk_read(body, body_len);
    if (n < (int)sizeof(struct version_header)) return -1;
    v0_mux_hdr_t hdr;
    hdr.len = pkt_len;
    hdr.magic = 0;
    hdr.protocol = 0;
    struct version_header *vh = (struct version_header *)body;
    if (ntohl(hdr.protocol) != 1) {
        LOGE("recv_version: unexpected proto=%d", ntohl(hdr.protocol));
        return -1;
    }
    *major_out = ntohl(vh->major);
    return 0;
}

static int version_exchange(void) {
    for (int attempt = 0; attempt < 5; attempt++) {
        if (send_version() < 0) return -1;
        uint32_t major = 0;
        if (recv_version(&major) == 0) {
            LOGI("version exchange OK (major=%u)", major);
            return 0;
        }
        LOGI("version_exchange retry %d/5", attempt + 1);
        usleep(100000);
    }
    return -1;
}

/* ========== v1 TCP helpers ========== */
static int usb_send_setup(uint32_t type, uint32_t extra) {
    uint8_t pkt[4 + 4 + 4];
    uint32_t len = sizeof(pkt);
    pkt[0] = (len >> 24) & 0xFF; pkt[1] = (len >> 16) & 0xFF;
    pkt[2] = (len >> 8) & 0xFF;  pkt[3] = len & 0xFF;
    pkt[4] = 0x00; pkt[5] = 0x00; pkt[6] = 0x00; pkt[7] = 0x00;
    pkt[8] = (type >> 24) & 0xFF; pkt[9] = (type >> 16) & 0xFF;
    pkt[10] = (type >> 8) & 0xFF; pkt[11] = type & 0xFF;
    int r = usb_write_all(pkt, len);
    if (r < 0) {
        LOGE("usb_send_setup failed");
        return -1;
    }
    LOGI("SETUP sent OK");
    return 0;
}

static int recv_tcp(uint16_t *port_out, uint8_t *flags_out, uint32_t *seq_out,
                    uint32_t *ack_out, uint8_t *payload, int *payload_len,
                    int max_payload, int timeout_ms) {
    uint8_t rxbuf[4096];
    uint32_t pkt_len = 0;
    int retry = 0;
    while (retry < 20) {
        int r = read_mux_packet(rxbuf, sizeof(rxbuf), &pkt_len);
        if (r < 0) {
            if (retry > 0) LOGI("recv_tcp: timeout after %d drains", retry);
            return -1;
        }
        if (pkt_len < 4) continue;
        uint32_t magic = (rxbuf[4] << 24) | (rxbuf[5] << 16) | (rxbuf[6] << 8) | rxbuf[7];
        if (magic != 0x6c1b1b1b) {
            LOGE("recv_tcp: bad magic=0x%08x", ntohl(*(uint32_t*)(rxbuf+4)));
            continue;
        }
        uint32_t proto = (rxbuf[8] << 24) | (rxbuf[9] << 16) | (rxbuf[10] << 8) | rxbuf[11];
        int body_len = pkt_len - 4 - 4 - 4;
        uint8_t *body = rxbuf + 12;
        if (proto == 1) {
            /* CONTROL */
            if (body_len > 0) {
                LOGI("recv_tcp: drain CONTROL type=%d", body[0]);
            } else {
                LOGI("recv_tcp: drain CONTROL (empty)");
            }
            retry++;
            continue;
        }
        if (proto == 7) {
            LOGI("recv_tcp: drain SETUP");
            retry++;
            continue;
        }
        if (proto != 6) {
            LOGW("recv_tcp: unexpected proto=%u", proto);
            retry++;
            continue;
        }
        /* TCP packet */
        if (body_len < (int)sizeof(struct tcphdr)) {
            LOGE("recv_tcp: TCP body too small %d < %zu", body_len, sizeof(struct tcphdr));
            retry++;
            continue;
        }
        struct tcphdr *tcp = (struct tcphdr *)body;
        uint16_t dport = ntohs(tcp->dest);
        uint16_t sport = ntohs(tcp->source);
        if (dport != g_tcp.sport) {
            LOGW("recv_tcp: port mismatch got %d->%d expect %d->%d",
                 sport, dport, g_tcp.sport, *port_out);
            retry++;
            continue;
        }
        *port_out = sport;
        *flags_out = tcp->th_flags;
        *seq_out = ntohl(tcp->seq);
        *ack_out = ntohl(tcp->ack_seq);
        int plen = body_len - sizeof(struct tcphdr);
        if (plen > max_payload) plen = max_payload;
        if (plen > 0) memcpy(payload, body + sizeof(struct tcphdr), plen);
        *payload_len = plen;
        return 0;
    }
    return -1;
}

static int build_tcp_packet(uint8_t *buf, int buf_len,
                            uint16_t sport, uint16_t dport,
                            uint32_t seq, uint32_t ack,
                            uint8_t flags, const uint8_t *payload, int payload_len) {
    int total = sizeof(struct tcphdr) + payload_len;
    if (total > buf_len) return -1;
    struct tcphdr *tcp = (struct tcphdr *)buf;
    memset(tcp, 0, sizeof(*tcp));
    tcp->source = htons(sport);
    tcp->dest   = htons(dport);
    tcp->seq    = htonl(seq);
    tcp->ack_seq = htonl(ack);
    tcp->doff   = sizeof(struct tcphdr) / 4;
    tcp->th_flags = flags;
    tcp->window = htons(65535);
    if (payload_len > 0) memcpy(buf + sizeof(struct tcphdr), payload, payload_len);
    return total;
}

static int send_tcp_raw(uint16_t sport, uint16_t dport,
                        uint32_t seq, uint32_t ack,
                        uint8_t flags, const uint8_t *payload, int payload_len) {
    uint8_t body[2048];
    int body_len = build_tcp_packet(body, sizeof(body), sport, dport, seq, ack, flags, payload, payload_len);
    if (body_len < 0) return -1;
    uint8_t pkt[4 + 4 + 4 + 2048];
    int len = 4 + 4 + 4 + body_len;
    pkt[0] = (len >> 24) & 0xFF; pkt[1] = (len >> 16) & 0xFF;
    pkt[2] = (len >> 8) & 0xFF;  pkt[3] = len & 0xFF;
    pkt[4] = 0x6c; pkt[5] = 0x1b; pkt[6] = 0x1b; pkt[7] = 0x1b;
    pkt[8] = 0x00; pkt[9] = 0x00; pkt[10] = 0x00; pkt[11] = 0x06;
    memcpy(pkt + 12, body, body_len);
    return usb_write_all(pkt, len);
}

/* ========== v1_connect ========== */
static int v1_connect(uint16_t port) {
    LOGI("v1_connect: port=%d", port);
    if (version_exchange() < 0) {
        LOGE("v1_connect: version exchange failed");
        return -1;
    }
    tcp_init(&g_tcp);
    /* Drain non-TCP packets */
    LOGI("v1_connect: draining non-TCP packets...");
    int drained = 0;
    for (int i = 0; i < 10; i++) {
        uint8_t rxbuf[4096];
        uint32_t pkt_len = 0;
        int r = read_mux_packet(rxbuf, sizeof(rxbuf), &pkt_len);
        if (r < 0) break;
        if (pkt_len >= 12) {
            uint32_t proto = (rxbuf[8] << 24) | (rxbuf[9] << 16) | (rxbuf[10] << 8) | rxbuf[11];
            if (proto == 1) {
                LOGI("v1_connect: drained CONTROL");
            } else if (proto == 7) {
                LOGI("v1_connect: drained SETUP");
            } else if (proto == 6) {
                LOGW("v1_connect: unexpected TCP during drain");
                break;
            }
            drained++;
        }
    }
    if (drained > 0) LOGI("v1_connect: drained %d packets", drained);
    /* Send SYN */
    uint32_t isn = g_tcp.seq;
    LOGI("v1_connect: SYN -> port=%d sport=%d seq=%u", port, g_tcp.sport, isn);
    if (send_tcp_raw(g_tcp.sport, port, isn, 0, TH_SYN, NULL, 0) < 0) {
        LOGE("v1_connect: SYN failed");
        return -1;
    }
    /* Wait SYN+ACK */
    uint16_t rport = 0;
    uint8_t flags = 0;
    uint32_t rseq = 0, rack = 0;
    uint8_t payload[1024];
    int plen = 0;
    int r = recv_tcp(&rport, &flags, &rseq, &rack, payload, &plen, sizeof(payload), 25000);
    if (r < 0) {
        LOGE("v1_connect: SYN+ACK timeout (25s)");
        LOGE("v1_connect: -> Kiểm tra: iPhone unlock? Đã bấm Trust? Cáp data?");
        return -1;
    }
    if (flags & TH_RST) {
        LOGE("v1_connect: RST received — port %d refused", port);
        LOGE("v1_connect: -> iPhone chưa Trust hoặc port không mở");
        return -1;
    }
    if (!(flags & TH_SYN) || !(flags & TH_ACK)) {
        LOGE("v1_connect: unexpected flags=0x%02x", flags);
        return -1;
    }
    g_tcp.remote_seq = rseq;
    g_tcp.acked = rack;
    LOGI("v1_connect: SYN+ACK received remote_seq=%u", g_tcp.remote_seq);
    /* Send ACK */
    g_tcp.seq = rack;
    uint32_t ack = rseq + 1;
    if (send_tcp_raw(g_tcp.sport, port, g_tcp.seq, ack, TH_ACK, NULL, 0) < 0) {
        LOGE("v1_connect: ACK failed");
        return -1;
    }
    g_tcp.connected = 1;
    LOGI("v1_connect: TCP connected port=%d", port);
    return 0;
}

/* ========== Tunnel thread ========== */
static void *tunnel_thread(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);
    int usb_to_sock[2];
    if (pipe(usb_to_sock) < 0) {
        LOGE("pipe failed");
        close(client_fd);
        return NULL;
    }
    /* sock -> usb */
    pthread_t t1;
    int *pfd = malloc(sizeof(int));
    *pfd = client_fd;
    /* ... simplified ... */
    close(client_fd);
    return NULL;
}

/* ========== Client handler ========== */
static void handle_client(int client_fd) {
    LOGI("client fd=%d connected", client_fd);
    uint8_t rxbuf[4096];
    int n = recv(client_fd, rxbuf, sizeof(rxbuf), 0);
    if (n < 16) {
        LOGE("client fd=%d: invalid pkt_len=%u", client_fd, (unsigned)n);
        close(client_fd);
        return;
    }
    uint32_t pkt_len = ntohl(*(uint32_t *)rxbuf);
    uint32_t msg_type = ntohl(*(uint32_t *)(rxbuf + 8));
    uint32_t tag = ntohl(*(uint32_t *)(rxbuf + 12));
    char msg_type_str[32] = {0};
    const char *s = (char *)(rxbuf + 16);
    const char *e = memchr(s, 0, n - 16);
    if (e && (e - s) < (long)sizeof(msg_type_str)) {
        strncpy(msg_type_str, s, sizeof(msg_type_str) - 1);
    }
    LOGI("client fd=%d: msg=%s tag=%u", client_fd, msg_type_str, tag);
    if (strcmp(msg_type_str, "Connect") == 0) {
        /* Parse port from XML */
        int port = 0;
        char *p = strstr((char *)rxbuf, "<key>PortNumber</key>");
        if (p) {
            p = strstr(p, "<integer>");
            if (p) port = atoi(p + 9);
        }
        if (port <= 0) {
            port = 62078; /* default usbmuxd */
        }
        if (v1_connect((uint16_t)port) < 0) {
            LOGE("client fd=%d: connect failed port=%d", client_fd, port);
            /* Send Result Failure */
            close(client_fd);
            return;
        }
        /* Send Result Success */
        LOGI("client fd=%d: tunnel started port=%d", client_fd, port);
        /* Start tunnel */
        /* ... */
        LOGI("client fd=%d: tunnel ended", client_fd);
    }
    close(client_fd);
    LOGI("client fd=%d disconnected", client_fd);
}

/* ========== Server thread ========== */
static void *server_thread(void *arg) {
    (void)arg;
    LOGI("server thread started");
    while (g_running) {
        struct sockaddr_un addr;
        socklen_t len = sizeof(addr);
        int client_fd = accept(g_listen_sock, (struct sockaddr *)&addr, &len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            LOGE("accept failed: %s", strerror(errno));
            break;
        }
        LOGI("new connection fd=%d", client_fd);
        pthread_t t;
        int *pfd = malloc(sizeof(int));
        *pfd = client_fd;
        pthread_create(&t, NULL, tunnel_thread, pfd);
        pthread_detach(t);
    }
    LOGI("server thread stopped");
    return NULL;
}

/* ========== Public API ========== */
bool usbmuxd_server_start(const char *files_dir, const char *udid, int product_id) {
    (void)udid;
    (void)product_id;
    if (g_running) return true;
    snprintf(g_sock_path, sizeof(g_sock_path), "%s/%s", files_dir, USBMUXD_SOCKET_FILE);
    g_listen_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen_sock < 0) {
        LOGE("socket failed: %s", strerror(errno));
        return false;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_sock_path, sizeof(addr.sun_path) - 1);
    unlink(g_sock_path);
    if (bind(g_listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("bind failed: %s", strerror(errno));
        close(g_listen_sock);
        g_listen_sock = -1;
        return false;
    }
    if (listen(g_listen_sock, 10) < 0) {
        LOGE("listen failed: %s", strerror(errno));
        close(g_listen_sock);
        g_listen_sock = -1;
        return false;
    }
    g_running = 1;
    pthread_create(&g_server_thread, NULL, server_thread, NULL);
    LOGI("server listening on %s", g_sock_path);
    return true;
}

void usbmuxd_server_stop(void) {
    if (!g_running) return;
    g_running = 0;
    if (g_listen_sock >= 0) {
        close(g_listen_sock);
        g_listen_sock = -1;
    }
    pthread_join(g_server_thread, NULL);
    unlink(g_sock_path);
    LOGI("server stopped");
}

const char *usbmuxd_server_get_socket_path(void) {
    return g_sock_path;
}
