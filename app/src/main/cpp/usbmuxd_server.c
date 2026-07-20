/*
 * usbmuxd_server.c  v21-FIXED
 * Mini usbmuxd server nội bộ (in-process, Mode 1) — FIXED for Android Bionic
 *
 * Chạy một Unix domain socket server mô phỏng usbmuxd.
 * libimobiledevice (libusbmuxd) sẽ kết nối đến socket này thay vì
 * /var/run/usbmuxd (không tồn tại trên Android không root).
 *
 * FIX v21: Sửa lỗi biên dịch trên Android Bionic libc
 *   - struct tcphdr field names khác glibc → định nghĩa struct riêng
 *   - Thiếu #include <sys/types.h> cho htonl/ntohl
 *   - usb_bridge_bulk_write/read thiếu timeout parameter
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>

#include "usbmuxd_server.h"
#include "usb_fd_bridge.h"

/* ════════════════════════════════════════════════════════════════════════
 * FIX: Android Bionic libc có struct tcphdr với field names KHÁC glibc.
 * Định nghĩa struct riêng để tránh phụ thuộc libc.
 * ════════════════════════════════════════════════════════════════════════ */
#ifdef __ANDROID__
struct tcphdr {
    uint16_t th_sport;   /* source port */
    uint16_t th_dport;   /* destination port */
    uint32_t th_seq;     /* sequence number */
    uint32_t th_ack;     /* acknowledgement number */
#if __BYTE_ORDER == __LITTLE_ENDIAN
    uint8_t th_x2:4;     /* (unused) */
    uint8_t th_off:4;    /* data offset */
#else
    uint8_t th_off:4;    /* data offset */
    uint8_t th_x2:4;     /* (unused) */
#endif
    uint8_t  th_flags;   /* TCP flags */
    uint16_t th_win;     /* window */
    uint16_t th_sum;     /* checksum */
    uint16_t th_urp;     /* urgent pointer */
};
#endif

#define TAG "usbmuxd_srv"
#define LOGI(fmt, ...) do { \
    char _buf[512]; \
    snprintf(_buf, sizeof(_buf), "I/" TAG ": " fmt, ##__VA_ARGS__); \
    __android_log_write(ANDROID_LOG_INFO, TAG, _buf); \
} while(0)
#define LOGE(fmt, ...) do { \
    char _buf[512]; \
    snprintf(_buf, sizeof(_buf), "E/" TAG ": " fmt, ##__VA_ARGS__); \
    __android_log_write(ANDROID_LOG_ERROR, TAG, _buf); \
} while(0)

/* ── Protocol constants ── */
#define USBMUXD_PROTOCOL_VERSION 1

/* v0 binary protocol (legacy, for reference) */
typedef struct {
    uint32_t length;   /* total packet length */
    uint32_t version;  /* protocol version */
    uint32_t message;  /* message type */
    uint32_t tag;      /* client tag */
} v0_mux_hdr_t;

/* v1 protocol header (big-endian) */
typedef struct {
    uint32_t protocol; /* 0=version, 1=TCP */
    uint32_t length;   /* total length */
    uint32_t magic;    /* 0xfeedface */
    uint16_t tx_seq;   /* sender sequence */
    uint16_t rx_seq;   /* receiver sequence */
} v1_mux_hdr_t;

#define V1_MAGIC 0xfeedface

/* Version exchange body */
typedef struct {
    uint32_t major;
    uint32_t minor;
    uint32_t padding[2];
} version_header;

/* Message types (v0, for internal client communication) */
enum {
    MSG_RESULT = 1,
    MSG_CONNECT = 2,
    MSG_READ = 3,
    MSG_WRITE = 4,
    MSG_LISTEN = 5,
    MSG_ATTACHED = 6,
    MSG_DETACHED = 7,
    MSG_LISTDEVICES = 8,
    MSG_DEVICE_ADD = 9,
    MSG_DEVICE_REMOVE = 10,
};

/* Client connection state */
typedef struct client {
    int fd;
    uint32_t tag;
    int listening;
    int usb_connected;
    uint16_t usb_sport;
    uint16_t usb_dport;
    uint32_t usb_seq;
    uint32_t usb_ack;
    struct client *next;
} client_t;

/* Global state */
static int g_server_fd = -1;
static int g_running = 0;
static pthread_t g_server_thread;
static pthread_t g_usb_rx_thread;
static pthread_mutex_t g_clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static client_t *g_clients = NULL;
static char g_sock_path[256] = "";
static char g_udid[44] = "";
static int g_product_id = 0x12a8;
static pthread_mutex_t g_udid_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t g_current_usb_tag = 0;
static int g_version_exchanged = 0;
static pthread_mutex_t g_version_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ════════════════════════════════════════════════════════════════════════
 * USB I/O helpers
 * ════════════════════════════════════════════════════════════════════════ */
static int usb_send_all(const uint8_t *data, int len) {
    int total = 0;
    while (total < len) {
        int remain = len - total;
        /* FIX v21: thêm timeout parameter */
        int n = usb_bridge_bulk_write(data + total, remain, 5000);
        if (n <= 0) {
            LOGE("usb_send_all: write failed (%d)", n);
            return -1;
        }
        total += n;
    }
    return 0;
}

/* FIX v21: thêm timeout parameter */
static int usb_recv_all(uint8_t *rxbuf, int len) {
    int n = usb_bridge_bulk_read(rxbuf, len, 5000);
    if (n != len) {
        LOGE("usb_recv_all: expected %d got %d", len, n);
        return -1;
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════
 * Version exchange (v1 protocol)
 * ════════════════════════════════════════════════════════════════════════ */
bool usbmux_version_exchange(void) {
    pthread_mutex_lock(&g_version_mutex);
    if (g_version_exchanged) {
        pthread_mutex_unlock(&g_version_mutex);
        return true;
    }

    /* 1. Send version packet */
    v1_mux_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.protocol = htonl(0); /* VERSION packet */
    hdr.length = htonl(sizeof(v1_mux_hdr_t) + sizeof(version_header));
    hdr.magic = htonl(V1_MAGIC);
    hdr.tx_seq = htons(0);
    hdr.rx_seq = htons(0xffff);

    version_header ver;
    memset(&ver, 0, sizeof(ver));
    ver.major = htonl(1);
    ver.minor = htonl(0);

    uint8_t tx[sizeof(hdr) + sizeof(ver)];
    memcpy(tx, &hdr, sizeof(hdr));
    memcpy(tx + sizeof(hdr), &ver, sizeof(ver));

    if (usb_send_all(tx, sizeof(tx)) < 0) {
        LOGE("version_exchange: send failed");
        pthread_mutex_unlock(&g_version_mutex);
        return false;
    }

    /* 2. Receive 4-byte length */
    uint8_t rx[4];
    /* FIX v21: thêm timeout parameter */
    int n = usb_bridge_bulk_read(rx, 4, 5000);
    if (n != 4) {
        LOGE("version_exchange: read len failed (%d)", n);
        pthread_mutex_unlock(&g_version_mutex);
        return false;
    }
    uint32_t pkt_len = ntohl(*(uint32_t*)rx);
    if (pkt_len < sizeof(v1_mux_hdr_t) || pkt_len > 4096) {
        LOGE("version_exchange: bad len %u", pkt_len);
        pthread_mutex_unlock(&g_version_mutex);
        return false;
    }

    /* 3. Receive rest of packet */
    uint8_t *pkt = (uint8_t*)malloc(pkt_len);
    if (!pkt) {
        pthread_mutex_unlock(&g_version_mutex);
        return false;
    }
    *(uint32_t*)pkt = htonl(pkt_len);
    if (usb_recv_all(pkt + 4, pkt_len - 4) < 0) {
        free(pkt);
        pthread_mutex_unlock(&g_version_mutex);
        return false;
    }

    /* 4. Parse response */
    v1_mux_hdr_t *rh = (v1_mux_hdr_t*)pkt;
    if (ntohl(rh->magic) != V1_MAGIC) {
        LOGE("version_exchange: bad magic=0x%08x", ntohl(rh->magic));
        free(pkt);
        pthread_mutex_unlock(&g_version_mutex);
        return false;
    }
    if (ntohl(rh->protocol) != 1) {
        LOGE("version_exchange: bad protocol=%u", ntohl(rh->protocol));
        free(pkt);
        pthread_mutex_unlock(&g_version_mutex);
        return false;
    }

    LOGI("version_exchange: OK (v%u.%u)",
         ntohl(((version_header*)(pkt + sizeof(v1_mux_hdr_t)))->major),
         ntohl(((version_header*)(pkt + sizeof(v1_mux_hdr_t)))->minor));

    free(pkt);
    g_version_exchanged = 1;
    pthread_mutex_unlock(&g_version_mutex);
    return true;
}

void usbmuxd_server_reset_version_state(void) {
    pthread_mutex_lock(&g_version_mutex);
    g_version_exchanged = 0;
    pthread_mutex_unlock(&g_version_mutex);
}

/* ════════════════════════════════════════════════════════════════════════
 * TCP-over-USB helpers (v1 protocol)
 * ════════════════════════════════════════════════════════════════════════ */
static int recv_tcp(uint16_t *sport_out, uint16_t *dport_out,
                    uint8_t *flags_out, uint32_t *seq_out, uint32_t *ack_out,
                    uint8_t *payload, int *payload_len) {
    /* Read v1 mux header (16 bytes) */
    v1_mux_hdr_t hdr;
    if (usb_recv_all((uint8_t*)&hdr, sizeof(hdr)) < 0) {
        return -1;
    }
    if (ntohl(hdr.magic) != V1_MAGIC) {
        LOGE("recv_tcp: bad magic=0x%08x", ntohl(hdr.magic));
        return -1;
    }
    if (ntohl(hdr.protocol) != 1) {
        LOGE("recv_tcp: not TCP packet (proto=%u)", ntohl(hdr.protocol));
        return -1;
    }

    uint32_t body_len = ntohl(hdr.length) - sizeof(v1_mux_hdr_t);
    if (body_len > 65536) {
        LOGE("recv_tcp: body too large %u", body_len);
        return -1;
    }

    uint8_t *body = (uint8_t*)malloc(body_len);
    if (!body) return -1;
    if (usb_recv_all(body, body_len) < 0) {
        free(body);
        return -1;
    }

    /* FIX v21: Dùng field names của struct tcphdr đã định nghĩa */
    if (body_len >= (int)sizeof(struct tcphdr)) {
        struct tcphdr *tcp = (struct tcphdr *)body;
        uint16_t dport = ntohs(tcp->th_dport);
        uint16_t sport = ntohs(tcp->th_sport);

        if (sport_out) *sport_out = sport;
        if (dport_out) *dport_out = dport;
        *flags_out = tcp->th_flags;
        *seq_out = ntohl(tcp->th_seq);
        *ack_out = ntohl(tcp->th_ack);
        int plen = body_len - sizeof(struct tcphdr);
        if (plen > 0 && payload && payload_len) {
            if (plen > *payload_len) plen = *payload_len;
            memcpy(payload, body + sizeof(struct tcphdr), plen);
        }
        *payload_len = plen;
        free(body);
        return 0;
    }

    free(body);
    return -1;
}

static int send_tcp(uint16_t sport, uint16_t dport,
                    uint32_t seq, uint32_t ack, uint8_t flags,
                    const uint8_t *payload, int payload_len) {
    int total = sizeof(v1_mux_hdr_t) + sizeof(struct tcphdr) + payload_len;
    uint8_t *pkt = (uint8_t*)malloc(total);
    if (!pkt) return -1;

    v1_mux_hdr_t *hdr = (v1_mux_hdr_t*)pkt;
    memset(hdr, 0, sizeof(*hdr));
    hdr->protocol = htonl(1);
    hdr->length = htonl(total);
    hdr->magic = htonl(V1_MAGIC);
    hdr->tx_seq = htons(0);
    hdr->rx_seq = htons(0xffff);

    /* FIX v21: Dùng field names của struct tcphdr đã định nghĩa */
    struct tcphdr *tcp = (struct tcphdr *)(pkt + sizeof(v1_mux_hdr_t));
    memset(tcp, 0, sizeof(*tcp));
    tcp->th_sport = htons(sport);
    tcp->th_dport = htons(dport);
    tcp->th_seq   = htonl(seq);
    tcp->th_ack   = htonl(ack);
    tcp->th_off   = sizeof(struct tcphdr) / 4;
    tcp->th_flags = flags;
    tcp->th_win   = htons(65535);

    if (payload_len > 0) {
        memcpy(pkt + sizeof(v1_mux_hdr_t) + sizeof(struct tcphdr), payload, payload_len);
    }

    int ret = usb_send_all(pkt, total);
    free(pkt);
    return ret;
}

/* ════════════════════════════════════════════════════════════════════════
 * USB connect/disconnect (v1 protocol)
 * ════════════════════════════════════════════════════════════════════════ */
static int do_usb_v1_connect(uint16_t dport, uint32_t *seq_out) {
    if (!usbmux_version_exchange()) {
        LOGE("do_usb_v1_connect: version exchange failed");
        return -1;
    }

    uint32_t seq = 1;
    if (send_tcp(12321, dport, seq, 0, TH_SYN, NULL, 0) < 0) {
        return -1;
    }

    uint8_t flags;
    uint32_t ack_seq;
    uint16_t sport, rport;
    uint8_t payload[1024];
    int plen = sizeof(payload);
    if (recv_tcp(&sport, &rport, &flags, &ack_seq, seq, payload, &plen) < 0) {
        return -1;
    }
    if (!(flags & TH_SYN) || !(flags & TH_ACK)) {
        LOGE("do_usb_v1_connect: expected SYN+ACK, got flags=0x%02x", flags);
        return -1;
    }

    seq++;
    if (send_tcp(12321, dport, seq, ack_seq + 1, TH_ACK, NULL, 0) < 0) {
        return -1;
    }

    *seq_out = seq;
    LOGI("USB v1 connect to port %u OK (seq=%u ack=%u)", dport, seq, ack_seq + 1);
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════
 * Client management
 * ════════════════════════════════════════════════════════════════════════ */
static client_t *find_client_by_fd(int fd) {
    client_t *c = g_clients;
    while (c) {
        if (c->fd == fd) return c;
        c = c->next;
    }
    return NULL;
}

static client_t *find_client_by_tag(uint32_t tag) {
    client_t *c = g_clients;
    while (c) {
        if (c->tag == tag) return c;
        c = c->next;
    }
    return NULL;
}

static client_t *find_client_by_usb_tag(uint32_t tag) {
    client_t *c = g_clients;
    while (c) {
        if (c->usb_connected && c->tag == tag) return c;
        c = c->next;
    }
    return NULL;
}

static void remove_client(int fd) {
    pthread_mutex_lock(&g_clients_mutex);
    client_t **pp = &g_clients;
    while (*pp) {
        if ((*pp)->fd == fd) {
            client_t *tmp = *pp;
            *pp = tmp->next;
            close(tmp->fd);
            free(tmp);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_clients_mutex);
}

/* ════════════════════════════════════════════════════════════════════════
 * Packet I/O with clients (v0 protocol for internal communication)
 * ════════════════════════════════════════════════════════════════════════ */
static int recv_pkt(int fd, uint32_t *msg_out, uint32_t *tag_out,
                    uint8_t *payload, int *payload_len) {
    v0_mux_hdr_t hdr;
    int n = recv(fd, &hdr, sizeof(hdr), MSG_WAITALL);
    if (n != sizeof(hdr)) return -1;

    uint32_t len = ntohl(hdr.length);
    uint32_t ver = ntohl(hdr.version);
    uint32_t msg = ntohl(hdr.message);
    uint32_t tag = ntohl(hdr.tag);

    if (ver != USBMUXD_PROTOCOL_VERSION) {
        LOGE("recv_pkt: bad version %u", ver);
        return -1;
    }

    *msg_out = msg;
    *tag_out = tag;

    uint32_t body_len = len - sizeof(hdr);
    if (body_len > 0) {
        if (body_len > (uint32_t)*payload_len) {
            LOGE("recv_pkt: payload too large %u > %d", body_len, *payload_len);
            return -1;
        }
        n = recv(fd, payload, body_len, MSG_WAITALL);
        if (n != (int)body_len) return -1;
    }
    *payload_len = body_len;
    return 0;
}

static int send_pkt(int fd, uint32_t msg, uint32_t tag,
                    const uint8_t *payload, int payload_len) {
    v0_mux_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.length = htonl(sizeof(hdr) + payload_len);
    hdr.version = htonl(USBMUXD_PROTOCOL_VERSION);
    hdr.message = htonl(msg);
    hdr.tag = htonl(tag);

    struct iovec iov[2];
    iov[0].iov_base = &hdr;
    iov[0].iov_len = sizeof(hdr);
    iov[1].iov_base = (void*)payload;
    iov[1].iov_len = payload_len;

    int total = sizeof(hdr) + payload_len;
    int sent = writev(fd, iov, payload_len > 0 ? 2 : 1);
    if (sent != total) {
        LOGE("send_pkt: short write %d != %d", sent, total);
        return -1;
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════
 * Client thread
 * ════════════════════════════════════════════════════════════════════════ */
static void *client_thread(void *arg) {
    int cfd = (int)(intptr_t)arg;
    LOGI("client_thread: fd=%d started", cfd);

    uint8_t payload[4096];
    int payload_len;
    uint32_t msg, tag;

    while (g_running) {
        payload_len = sizeof(payload);
        if (recv_pkt(cfd, &msg, &tag, payload, &payload_len) < 0) {
            break;
        }

        switch (msg) {
        case MSG_LISTDEVICES: {
            /* Return device info */
            pthread_mutex_lock(&g_udid_mutex);
            char udid_copy[44];
            strncpy(udid_copy, g_udid, sizeof(udid_copy) - 1);
            udid_copy[sizeof(udid_copy) - 1] = '\0';
            int pid = g_product_id;
            pthread_mutex_unlock(&g_udid_mutex);

            /* Simple plist-like response */
            char resp[512];
            int resp_len = snprintf(resp, sizeof(resp),
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" ...>\n"
                "<plist version=\"1.0\">\n"
                "<dict>\n"
                "  <key>DeviceList</key>\n"
                "  <array>\n"
                "    <dict>\n"
                "      <key>DeviceID</key>\n"
                "      <integer>1</integer>\n"
                "      <key>ProductID</key>\n"
                "      <integer>%d</integer>\n"
                "      <key>SerialNumber</key>\n"
                "      <string>%s</string>\n"
                "      <key>ConnectionType</key>\n"
                "      <string>USB</string>\n"
                "    </dict>\n"
                "  </array>\n"
                "</dict>\n"
                "</plist>\n", pid, udid_copy);

            send_pkt(cfd, MSG_RESULT, tag, (uint8_t*)resp, resp_len);
            break;
        }

        case MSG_LISTEN: {
            pthread_mutex_lock(&g_clients_mutex);
            client_t *c = find_client_by_fd(cfd);
            if (c) c->listening = 1;
            pthread_mutex_unlock(&g_clients_mutex);

            /* Send Attached event */
            pthread_mutex_lock(&g_udid_mutex);
            char udid_copy[44];
            strncpy(udid_copy, g_udid, sizeof(udid_copy) - 1);
            udid_copy[sizeof(udid_copy) - 1] = '\0';
            pthread_mutex_unlock(&g_udid_mutex);

            char attached[256];
            int alen = snprintf(attached, sizeof(attached),
                "<?xml version=\"1.0\"?>\n"
                "<plist version=\"1.0\">\n"
                "<dict>\n"
                "  <key>MessageType</key>\n"
                "  <string>Attached</string>\n"
                "  <key>DeviceID</key>\n"
                "  <integer>1</integer>\n"
                "  <key>Properties</key>\n"
                "  <dict>\n"
                "    <key>SerialNumber</key>\n"
                "    <string>%s</string>\n"
                "    <key>ConnectionType</key>\n"
                "    <string>USB</string>\n"
                "  </dict>\n"
                "</dict>\n"
                "</plist>\n", udid_copy);
            send_pkt(cfd, MSG_DEVICE_ADD, 0, (uint8_t*)attached, alen);

            uint32_t res = 0;
            send_pkt(cfd, MSG_RESULT, tag, (const uint8_t *)&res, sizeof(res));
            break;
        }

        case MSG_CONNECT: {
            if (payload_len < 2) {
                uint32_t res = htonl(EBADF);
                send_pkt(cfd, MSG_RESULT, tag, (const uint8_t *)&res, sizeof(res));
                break;
            }
            uint16_t port = ntohs(*(uint16_t*)payload);
            LOGI("client requested connect to port %u", port);

            uint32_t seq = 0;
            if (do_usb_v1_connect(port, &seq) < 0) {
                uint32_t res = htonl(ECONNREFUSED);
                send_pkt(cfd, MSG_RESULT, tag, (const uint8_t *)&res, sizeof(res));
                break;
            }

            pthread_mutex_lock(&g_clients_mutex);
            client_t *c = find_client_by_fd(cfd);
            if (c) {
                c->usb_connected = 1;
                c->usb_dport = port;
                c->usb_seq = seq;
                c->usb_ack = 0;
                g_current_usb_tag = tag;
            }
            pthread_mutex_unlock(&g_clients_mutex);

            uint32_t res = 0;
            send_pkt(cfd, MSG_RESULT, tag, (const uint8_t *)&res, sizeof(res));
            break;
        }

        case MSG_READ: {
            pthread_mutex_lock(&g_clients_mutex);
            client_t *c = find_client_by_fd(cfd);
            if (!c || !c->usb_connected) {
                pthread_mutex_unlock(&g_clients_mutex);
                uint32_t res = htonl(ENOTCONN);
                send_pkt(cfd, MSG_RESULT, tag, (const uint8_t *)&res, sizeof(res));
                break;
            }
            pthread_mutex_unlock(&g_clients_mutex);

            /* Read from USB */
            uint8_t usb_rx[4096];
            int usb_rx_len = sizeof(usb_rx);
            if (payload_len >= 4) {
                usb_rx_len = ntohl(*(uint32_t*)payload);
                if (usb_rx_len > (int)sizeof(usb_rx)) usb_rx_len = sizeof(usb_rx);
            }

            /* FIX v21: thêm timeout parameter */
            int n = usb_bridge_bulk_read(usb_rx, usb_rx_len, 5000);
            if (n > 0) {
                send_pkt(cfd, MSG_DATA, tag, usb_rx, n);
            } else {
                uint32_t res = 0;
                send_pkt(cfd, MSG_RESULT, tag, (const uint8_t *)&res, sizeof(res));
            }
            break;
        }

        case MSG_WRITE: {
            pthread_mutex_lock(&g_clients_mutex);
            client_t *c = find_client_by_fd(cfd);
            if (!c || !c->usb_connected) {
                pthread_mutex_unlock(&g_clients_mutex);
                uint32_t res = htonl(ENOTCONN);
                send_pkt(cfd, MSG_RESULT, tag, (const uint8_t *)&res, sizeof(res));
                break;
            }
            pthread_mutex_unlock(&g_clients_mutex);

            if (payload_len > 0) {
                /* FIX v21: thêm timeout parameter */
                int n = usb_bridge_bulk_write(payload, payload_len, 5000);
                if (n == payload_len) {
                    uint32_t res = 0;
                    send_pkt(cfd, MSG_RESULT, tag, (const uint8_t *)&res, sizeof(res));
                } else {
                    uint32_t res = htonl(EIO);
                    send_pkt(cfd, MSG_RESULT, tag, (const uint8_t *)&res, sizeof(res));
                }
            }
            break;
        }

        default:
            LOGI("unknown msg type %u", msg);
            break;
        }
    }

    LOGI("client_thread: fd=%d exiting", cfd);
    remove_client(cfd);
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════
 * USB RX thread — forward USB data to connected client
 * ════════════════════════════════════════════════════════════════════════ */
static void *usb_rx_thread(void *arg) {
    (void)arg;
    LOGI("usb_rx_thread started");

    while (g_running) {
        uint8_t buf[4096];
        /* FIX v21: thêm timeout parameter */
        int n = usb_bridge_bulk_read(buf, sizeof(buf), 5000);
        if (n > 0) {
            pthread_mutex_lock(&g_clients_mutex);
            client_t *c = find_client_by_usb_tag(g_current_usb_tag);
            if (c && c->listening) {
                send_pkt(c->fd, MSG_DATA, g_current_usb_tag, buf, n);
            }
            pthread_mutex_unlock(&g_clients_mutex);
        }
    }

    LOGI("usb_rx_thread exiting");
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════
 * Server thread
 * ════════════════════════════════════════════════════════════════════════ */
static void *server_thread(void *arg) {
    (void)arg;
    LOGI("server_thread started on %s", g_sock_path);

    while (g_running) {
        struct sockaddr_un sa;
        socklen_t len = sizeof(sa);
        int cfd = accept(g_server_fd, (struct sockaddr *)&sa, &len);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            LOGE("accept failed: %s", strerror(errno));
            break;
        }

        LOGI("new client fd=%d", cfd);

        pthread_mutex_lock(&g_clients_mutex);
        client_t *c = (client_t*)calloc(1, sizeof(client_t));
        c->fd = cfd;
        c->next = g_clients;
        g_clients = c;
        pthread_mutex_unlock(&g_clients_mutex);

        pthread_t tid;
        pthread_create(&tid, NULL, client_thread, (void*)(intptr_t)cfd);
        pthread_detach(tid);
    }

    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════════════════════════════ */
bool usbmuxd_server_start(const char *files_dir, const char *udid, int product_id) {
    if (g_running) return true;

    /* FIX v21: Nhận đủ 3 tham số như định nghĩa trong .h */
    if (!files_dir || !files_dir[0]) {
        LOGE("usbmuxd_server_start: files_dir is NULL");
        return false;
    }

    /* Setup socket path */
    snprintf(g_sock_path, sizeof(g_sock_path), "%s/usbmuxd.sock", files_dir);

    /* Update UDID and product ID */
    pthread_mutex_lock(&g_udid_mutex);
    if (udid && udid[0]) {
        strncpy(g_udid, udid, sizeof(g_udid) - 1);
        g_udid[sizeof(g_udid) - 1] = '\0';
    } else {
        strcpy(g_udid, "00000000-0000000000000000");
    }
    g_product_id = product_id;
    pthread_mutex_unlock(&g_udid_mutex);

    /* Remove stale socket */
    unlink(g_sock_path);

    /* Create Unix socket */
    g_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        LOGE("socket failed: %s", strerror(errno));
        return false;
    }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, g_sock_path, sizeof(sa.sun_path) - 1);

    if (bind(g_server_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        LOGE("bind failed: %s", strerror(errno));
        close(g_server_fd);
        g_server_fd = -1;
        return false;
    }

    if (listen(g_server_fd, 10) < 0) {
        LOGE("listen failed: %s", strerror(errno));
        close(g_server_fd);
        g_server_fd = -1;
        unlink(g_sock_path);
        return false;
    }

    g_running = 1;

    /* Start threads */
    pthread_create(&g_server_thread, NULL, server_thread, NULL);
    pthread_create(&g_usb_rx_thread, NULL, usb_rx_thread, NULL);

    LOGI("usbmuxd server started on %s", g_sock_path);
    return true;
}

void usbmuxd_server_update_udid(const char *udid) {
    if (!udid) return;
    pthread_mutex_lock(&g_udid_mutex);
    strncpy(g_udid, udid, sizeof(g_udid) - 1);
    g_udid[sizeof(g_udid) - 1] = '\0';
    pthread_mutex_unlock(&g_udid_mutex);

    /* Broadcast Attached event to all listening clients */
    usbmuxd_server_broadcast_attached();
}

void usbmuxd_server_broadcast_attached(void) {
    pthread_mutex_lock(&g_udid_mutex);
    char udid_copy[44];
    strncpy(udid_copy, g_udid, sizeof(udid_copy) - 1);
    udid_copy[sizeof(udid_copy) - 1] = '\0';
    pthread_mutex_unlock(&g_udid_mutex);

    char attached[512];
    int alen = snprintf(attached, sizeof(attached),
        "<?xml version=\"1.0\"?>\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "  <key>MessageType</key>\n"
        "  <string>Attached</string>\n"
        "  <key>DeviceID</key>\n"
        "  <integer>1</integer>\n"
        "  <key>Properties</key>\n"
        "  <dict>\n"
        "    <key>SerialNumber</key>\n"
        "    <string>%s</string>\n"
        "    <key>ConnectionType</key>\n"
        "    <string>USB</string>\n"
        "  </dict>\n"
        "</dict>\n"
        "</plist>\n", udid_copy);

    pthread_mutex_lock(&g_clients_mutex);
    client_t *c = g_clients;
    while (c) {
        if (c->listening) {
            send_pkt(c->fd, MSG_DEVICE_ADD, 0, (uint8_t*)attached, alen);
        }
        c = c->next;
    }
    pthread_mutex_unlock(&g_clients_mutex);
}

const char *usbmuxd_server_socket_path(void) {
    return g_sock_path[0] ? g_sock_path : NULL;
}

void usbmuxd_server_stop(void) {
    if (!g_running) return;
    g_running = 0;

    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }

    pthread_mutex_lock(&g_clients_mutex);
    client_t *c = g_clients;
    while (c) {
        close(c->fd);
        client_t *next = c->next;
        free(c);
        c = next;
    }
    g_clients = NULL;
    pthread_mutex_unlock(&g_clients_mutex);

    if (g_sock_path[0]) {
        unlink(g_sock_path);
        g_sock_path[0] = '\0';
    }

    pthread_join(g_server_thread, NULL);
    pthread_join(g_usb_rx_thread, NULL);

    g_version_exchanged = 0;
    LOGI("usbmuxd server stopped");
}
