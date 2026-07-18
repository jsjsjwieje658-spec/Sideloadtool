/*
 * usbmux.c — Apple usbmux v1 TCP-over-USB protocol (Mode 2/3 fallback)
 *
 * Giao tiếp trực tiếp với iPhone qua USB bulk endpoints.
 * Không cần libimobiledevice hay usbmuxd — tự triển khai protocol.
 *
 * ════════════════════════════════════════════════════════════════════
 * FIX v28: Viết lại hoàn toàn từ v0 binary → v1 TCP-like protocol
 * ════════════════════════════════════════════════════════════════════
 *
 * VẤN ĐỀ CỦ (v0 binary — trước FIX v28):
 *   mux_do_setup() gửi CONNECT(type=1, port=0) dùng v0 binary header
 *   (little-endian, không có magic). iPhone iOS 7+ BỎ QUA hoàn toàn
 *   packet v0 vì không nhận ra format → mux_connect() thất bại ngay
 *   lần đầu (iPhone không gửi RESULT vì packet không hợp lệ).
 *
 * GIẢI PHÁP (học từ usbmuxd_server.c + termux-usbmuxd):
 *   1. mux_do_setup(): gửi VERSION packet theo v1 format (big-endian,
 *      magic=0xfeedface) — iPhone iOS 7+ nhận ra và respond đúng.
 *
 *   2. mux_connect(): gửi TCP SYN, chờ SYN+ACK, gửi ACK — giống
 *      TCP 3-way handshake thật sự.
 *
 *   3. mux_send()/mux_recv(): dùng v1 DATA packets với seq/ack tự
 *      quản lý — iPhone ACK mỗi DATA packet, chúng ta phải ACK lại.
 *
 *   4. mux_disconnect(): gửi FIN+ACK, chờ FIN hoặc RST từ iPhone.
 *
 * PROTOCOL FLOW (v1):
 *   setup:   → VERSION(proto=0, major=1, minor=0, magic)
 *            ← VERSION(proto=0, major=1, minor=0, magic)  [xác nhận]
 *
 *   connect: → TCP(SYN, sport, dport, seq=ISN, ack=0)
 *            ← TCP(SYN+ACK, seq=iPhone_ISN, ack=ISN+1)
 *            → TCP(ACK, seq=ISN+1, ack=iPhone_ISN+1)
 *
 *   data:    → TCP(PSH+ACK, seq, ack, data)
 *            ← TCP(ACK, seq, ack=our_seq+len)
 *            ← TCP(PSH+ACK, seq, ack, data)
 *            → TCP(ACK, seq, ack=their_seq+data_len)
 *
 *   close:   → TCP(FIN+ACK, seq, ack)
 *            ← TCP(FIN+ACK or RST)
 *
 * Tham khảo: usbmuxd_server.c (Mode 1), termux-usbmuxd/usbmuxd_proxy.c
 */
#include "usbmux.h"
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <android/log.h>

#define TAG "usbmux"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Timeout constants (ms) */
#define WRITE_TIMEOUT_MS    5000
#define READ_TIMEOUT_MS    15000
#define HANDSHAKE_TIMEOUT  3000   /* ms to wait for SYN+ACK / VERSION response */

/* ── Low-level USB I/O ──────────────────────────────────────────────────── */

static int raw_send(mux_conn_t *c, const void *buf, int len) {
    int total = 0;
    const char *p = (const char *)buf;
    int retries = 5;
    while (total < len && retries-- > 0) {
        int n = c->usb_write(p + total, len - total);
        if (n < 0) { LOGE("raw_send: write error %d", n); return -1; }
        if (n == 0) continue;
        total += n;
    }
    if (total < len) { LOGE("raw_send: partial write %d/%d", total, len); return -1; }
    return total;
}

/*
 * buf_read — đọc đúng `len` byte từ rxbuf nội bộ.
 * Block (gọi USB read nhiều lần) cho đến khi đủ hoặc lỗi.
 */
static int buf_read(mux_conn_t *c, void *out, int len) {
    char *dst = (char *)out;
    int got = 0;
    while (got < len) {
        /* Lấy từ buffer nội bộ trước */
        if (c->rxbuf_avail > 0) {
            int take = c->rxbuf_avail < (len - got) ? c->rxbuf_avail : (len - got);
            memcpy(dst + got, c->rxbuf + c->rxbuf_pos, take);
            c->rxbuf_pos   += take;
            c->rxbuf_avail -= take;
            got += take;
            continue;
        }
        /* Buffer rỗng — đọc từ USB */
        c->rxbuf_pos = 0;
        int n = c->usb_read(c->rxbuf, sizeof(c->rxbuf));
        if (n <= 0) {
            LOGE("buf_read: USB read error %d (got %d/%d)", n, got, len);
            return -1;
        }
        c->rxbuf_avail = n;
    }
    return got;
}

/* ════════════════════════════════════════════════════════════════════════
 * v1 packet builders/parsers
 *
 * Tất cả v1 packets: big-endian, magic=0xfeedface
 * ════════════════════════════════════════════════════════════════════════ */

/*
 * build_version_pkt — xây dựng VERSION packet (32 bytes).
 * Dùng cho setup handshake (protocol=0).
 */
static void build_version_pkt(uint8_t out[V1_VERSION_PKT_LEN]) {
    usbmux_v1hdr_t *hdr  = (usbmux_v1hdr_t *)out;
    usbmux_verbody_t *vb = (usbmux_verbody_t *)(out + sizeof(usbmux_v1hdr_t));

    hdr->protocol = htonl(V1_PROTO_VER);
    hdr->length   = htonl((uint32_t)V1_VERSION_PKT_LEN);
    hdr->magic    = htonl(V1_MAGIC);
    hdr->tx_seq   = htons(0);
    hdr->rx_seq   = htons(0xffff);  /* 0xffff = "không có packet nào trước" */

    vb->major = htonl(1);
    vb->minor = htonl(0);
    vb->pad1  = 0;
    vb->pad2  = 0;
}

/*
 * build_tcp_pkt — xây dựng TCP packet (header only, không có data).
 * Data (nếu có) được nối thêm sau bởi caller.
 *
 * @param out   buffer đầu ra (phải đủ V1_TCP_HDR_LEN + data_len)
 * @param c     connection state (sport, local_seq, remote_seq)
 * @param dport destination port (iPhone service)
 * @param flags TCP flags (TH_SYN, TH_ACK, TH_PSH, TH_FIN, TH_RST)
 * @param data_len số byte data sẽ nối thêm sau header
 */
static void build_tcp_hdr(uint8_t *out, mux_conn_t *c,
                            uint16_t dport, uint8_t flags, uint32_t data_len) {
    uint32_t total_len = (uint32_t)V1_TCP_HDR_LEN + data_len;
    usbmux_v1hdr_t *mhdr = (usbmux_v1hdr_t *)out;
    usbmux_tcphdr_t *tcp = (usbmux_tcphdr_t *)(out + sizeof(usbmux_v1hdr_t));

    mhdr->protocol = htonl(V1_PROTO_TCP);
    mhdr->length   = htonl(total_len);
    mhdr->magic    = htonl(V1_MAGIC);
    mhdr->tx_seq   = htons(0);
    mhdr->rx_seq   = htons(0xffff);

    tcp->sport  = htons(c->sport);
    tcp->dport  = htons(dport);
    tcp->seq    = htonl(c->local_seq);
    tcp->ack    = htonl(c->remote_seq);
    tcp->off    = 0x50;             /* 20 bytes / 4 = 5, shifted left → 0x50 */
    tcp->flags  = flags;
    tcp->window = htons(MUX_RECV_WINDOW);
    tcp->cksum  = 0;                /* iPhone không kiểm tra checksum qua USB */
    tcp->urgp   = 0;
}

/*
 * send_ack — gửi ACK thuần (không có data).
 * Dùng sau khi nhận DATA packet từ iPhone.
 */
static int send_ack(mux_conn_t *c, uint16_t dport) {
    uint8_t pkt[V1_TCP_HDR_LEN];
    build_tcp_hdr(pkt, c, dport, TH_ACK, 0);
    return raw_send(c, pkt, sizeof(pkt));
}

/*
 * recv_tcp_pkt — đọc một v1 TCP packet, validate magic và protocol.
 * Body data (sau header) được lưu vào *body_out (caller phải free).
 * Chỉ body THUẦN sau hai header (mux+tcp) được trả về.
 *
 * Returns: số byte data trong packet (0 nếu chỉ có header như SYN+ACK),
 *          hoặc -1 nếu lỗi.
 */
static int recv_tcp_pkt(mux_conn_t *c, usbmux_tcphdr_t *tcp_out,
                         uint8_t **body_out) {
    usbmux_v1hdr_t mhdr;
    if (buf_read(c, &mhdr, sizeof(mhdr)) < 0) return -1;

    /* Validate */
    if (ntohl(mhdr.magic) != V1_MAGIC) {
        LOGE("recv_tcp_pkt: bad magic 0x%08x", ntohl(mhdr.magic));
        return -1;
    }
    if (ntohl(mhdr.protocol) != V1_PROTO_TCP) {
        LOGE("recv_tcp_pkt: protocol=%u (expected TCP=1)", ntohl(mhdr.protocol));
        return -1;
    }

    uint32_t total = ntohl(mhdr.length);
    if (total < (uint32_t)V1_TCP_HDR_LEN) {
        LOGE("recv_tcp_pkt: length=%u too small", total);
        return -1;
    }

    usbmux_tcphdr_t tcp;
    if (buf_read(c, &tcp, sizeof(tcp)) < 0) return -1;
    if (tcp_out) *tcp_out = tcp;

    int data_len = (int)(total - V1_TCP_HDR_LEN);
    if (data_len < 0) data_len = 0;

    if (body_out) {
        if (data_len > 0) {
            *body_out = malloc(data_len + 1);
            if (!*body_out) return -1;
            if (buf_read(c, *body_out, data_len) < 0) {
                free(*body_out); *body_out = NULL; return -1;
            }
            (*body_out)[data_len] = '\0';
        } else {
            *body_out = NULL;
        }
    } else if (data_len > 0) {
        /* Drain data we're not reading */
        uint8_t tmp[256];
        int rem = data_len;
        while (rem > 0) {
            int take = rem < (int)sizeof(tmp) ? rem : (int)sizeof(tmp);
            if (buf_read(c, tmp, take) < 0) break;
            rem -= take;
        }
    }

    return data_len;
}

/* ════════════════════════════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════════════════════════════ */

int mux_conn_init(mux_conn_t *c, usb_write_fn wfn, usb_read_fn rfn) {
    memset(c, 0, sizeof(*c));
    c->usb_write  = wfn;
    c->usb_read   = rfn;
    c->next_tag   = 1;
    c->sport      = MUX_EPHEMERAL_PORT;
    c->local_seq  = 1000;  /* ISN (Initial Sequence Number) */
    c->remote_seq = 0;
    return 0;
}

/*
 * mux_do_setup — VERSION exchange với iPhone.
 *
 * FIX v28: Gửi v1 VERSION packet (big-endian, magic=0xfeedface) thay vì
 * v0 CONNECT(port=0) không hợp lệ.
 *
 * iPhone iOS 7+ nhận ra VERSION packet và respond với VERSION response
 * cùng major=1, minor=0. Nếu không phản hồi trong 2 giây → bỏ qua
 * (backward compat với thiết bị cũ, dù hiếm gặp).
 *
 * So sánh với Mode 1 (usbmuxd_server.c): hàm usbmux_version_exchange()
 * thực hiện cùng logic nhưng với 5 retry và delay. Ở đây Mode 2/3 dùng
 * 1 lần để đơn giản (đã có retry tổng thể ở layer trên).
 */
int mux_do_setup(mux_conn_t *c) {
    if (!c || !c->usb_write || !c->usb_read) return -1;

    if (c->ui_log) c->ui_log("[mux] Gửi v1 VERSION packet (iOS 7+ protocol)...");
    LOGI("mux_do_setup: gửi VERSION v1 (magic=0xfeedface)");

    /* Xây dựng và gửi VERSION packet */
    uint8_t vpkt[V1_VERSION_PKT_LEN];
    build_version_pkt(vpkt);

    if (raw_send(c, vpkt, sizeof(vpkt)) < 0) {
        if (c->ui_log)
            c->ui_log("[mux] ⚠️ Không gửi được VERSION packet (USB busy?) — tiếp tục");
        LOGE("mux_do_setup: raw_send thất bại — tiếp tục không có version exchange");
        return 0;  /* non-fatal */
    }

    /*
     * Chờ VERSION response từ iPhone.
     * Đọc đúng V1_VERSION_PKT_LEN byte (32 bytes).
     * Nếu không đủ hoặc magic sai → iPhone không hỗ trợ v1 (rất cũ) → tiếp tục.
     */
    uint8_t resp[V1_VERSION_PKT_LEN + 64];  /* +64 cho phép nhận thêm nếu cần */
    int got = c->usb_read(resp, sizeof(resp));

    if (got < (int)sizeof(usbmux_v1hdr_t)) {
        if (c->ui_log)
            c->ui_log("[mux] ⚠️ Không nhận được VERSION response — tiếp tục (backward compat)");
        LOGI("mux_do_setup: no response (got=%d) — assume old firmware, tiếp tục", got);
        return 0;  /* non-fatal */
    }

    usbmux_v1hdr_t *rhdr = (usbmux_v1hdr_t *)resp;
    uint32_t magic    = ntohl(rhdr->magic);
    uint32_t protocol = ntohl(rhdr->protocol);

    if (magic != V1_MAGIC || protocol != V1_PROTO_VER) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "[mux] ⚠️ VERSION response: magic=0x%08x proto=%u (không phải v1) — tiếp tục",
                 magic, protocol);
        if (c->ui_log) c->ui_log(msg);
        LOGI("mux_do_setup: unexpected response magic=0x%08x proto=%u", magic, protocol);
        return 0;  /* non-fatal */
    }

    /* Thành công: đọc major/minor từ body */
    uint32_t resp_major = 0, resp_minor = 0;
    if (got >= (int)V1_VERSION_PKT_LEN) {
        usbmux_verbody_t *vb = (usbmux_verbody_t *)(resp + sizeof(usbmux_v1hdr_t));
        resp_major = ntohl(vb->major);
        resp_minor = ntohl(vb->minor);
    }

    c->v1_ok = 1;

    char msg[128];
    snprintf(msg, sizeof(msg),
             "[mux] ✅ v1 VERSION OK: major=%u minor=%u (iOS 7+ compatible)",
             resp_major, resp_minor);
    if (c->ui_log) c->ui_log(msg);
    LOGI("mux_do_setup: ✅ v1 negotiated major=%u minor=%u", resp_major, resp_minor);
    return 0;
}

/*
 * mux_connect — Mở kết nối TCP đến `port` trên iPhone.
 *
 * FIX v28: Dùng v1 SYN → SYN+ACK → ACK handshake thay vì v0 CONNECT packet.
 *
 * Sequence:
 *   → SYN(sport, dport, seq=ISN, ack=0)
 *   ← SYN+ACK(seq=iPhone_ISN, ack=ISN+1)
 *   → ACK(seq=ISN+1, ack=iPhone_ISN+1)
 */
int mux_connect(mux_conn_t *c, int port) {
    if (!c || !c->usb_write || !c->usb_read) return -1;

    if (!c->v1_ok) {
        /* Fallback: v1 version exchange chưa chạy — thử setup trước */
        LOGI("mux_connect: v1_ok=0, thử mux_do_setup() trước");
        mux_do_setup(c);
    }

    char logmsg[128];
    snprintf(logmsg, sizeof(logmsg), "[mux] SYN → port %d (v1 TCP handshake)...", port);
    if (c->ui_log) c->ui_log(logmsg);
    LOGI("mux_connect: port=%d local_seq=%u", port, c->local_seq);

    /* ── Bước 1: Gửi SYN ─────────────────────────────────────────────── */
    uint8_t syn_pkt[V1_TCP_HDR_LEN];
    build_tcp_hdr(syn_pkt, c, (uint16_t)port, TH_SYN, 0);

    if (raw_send(c, syn_pkt, sizeof(syn_pkt)) < 0) {
        LOGE("mux_connect: gửi SYN thất bại");
        return -1;
    }

    /* ── Bước 2: Chờ SYN+ACK ─────────────────────────────────────────── */
    usbmux_tcphdr_t rtcp;
    uint8_t *body = NULL;
    if (recv_tcp_pkt(c, &rtcp, &body) < 0) {
        LOGE("mux_connect: không nhận được SYN+ACK");
        return -1;
    }
    if (body) { free(body); body = NULL; }

    /* Kiểm tra SYN+ACK flags */
    if ((rtcp.flags & (TH_SYN | TH_ACK)) != (TH_SYN | TH_ACK)) {
        if (rtcp.flags & TH_RST) {
            snprintf(logmsg, sizeof(logmsg),
                     "[mux] ❌ CONNECT RESET (port=%d không mở trên iPhone)", port);
            if (c->ui_log) c->ui_log(logmsg);
            LOGE("mux_connect: RST received (port %d refused)", port);
        } else {
            LOGE("mux_connect: unexpected flags=0x%02x (expected SYN+ACK)", rtcp.flags);
        }
        return -1;
    }

    /* Cập nhật seq/ack state từ SYN+ACK */
    c->remote_seq = ntohl(rtcp.seq) + 1;  /* +1 vì SYN consume 1 seq */
    c->local_seq  = ntohl(rtcp.ack);       /* iPhone ACKed đến đây */

    LOGI("mux_connect: SYN+ACK OK — remote_seq=%u local_seq=%u",
         c->remote_seq, c->local_seq);

    /* ── Bước 3: Gửi ACK ─────────────────────────────────────────────── */
    if (send_ack(c, (uint16_t)port) < 0) {
        LOGE("mux_connect: gửi ACK thất bại");
        return -1;
    }

    snprintf(logmsg, sizeof(logmsg),
             "[mux] ✅ kết nối port %d thành công (v1 handshake)", port);
    if (c->ui_log) c->ui_log(logmsg);
    LOGI("mux_connect: port=%d connected ✅", port);

    c->connected = 1;
    c->conn_id   = (uint32_t)port;  /* lưu port để dùng trong send_ack */
    return 0;
}

/*
 * mux_send — Gửi data qua kênh đã connect.
 *
 * FIX v28: Dùng v1 PSH+ACK DATA packet thay vì v0 DATA packet.
 * Sau khi gửi, cập nhật local_seq += data_len.
 */
int mux_send(mux_conn_t *c, const void *data, int len) {
    if (!c || len <= 0) return -1;

    if (!c->connected) {
        /* Fallback: gửi raw (pre-connect data) */
        return raw_send(c, data, len) >= 0 ? len : -1;
    }

    /* Xây dựng v1 PSH+ACK packet: header + data */
    int pkt_len = (int)V1_TCP_HDR_LEN + len;
    uint8_t *pkt = malloc(pkt_len);
    if (!pkt) return -1;

    build_tcp_hdr(pkt, c, (uint16_t)c->conn_id, TH_PSH | TH_ACK, (uint32_t)len);
    memcpy(pkt + V1_TCP_HDR_LEN, data, len);

    int r = raw_send(c, pkt, pkt_len);
    free(pkt);

    if (r < 0) {
        LOGE("mux_send: raw_send thất bại");
        return -1;
    }

    /* Tăng seq sau khi gửi thành công */
    c->local_seq += (uint32_t)len;
    LOGI("mux_send: %d bytes gửi, local_seq=%u", len, c->local_seq);
    return len;
}

/*
 * mux_recv — Nhận data từ kênh đã connect.
 *
 * FIX v28: Parse v1 TCP packets, gửi ACK sau mỗi DATA packet.
 * Trả số byte data thực sự đọc được (có thể ít hơn `len`).
 */
int mux_recv(mux_conn_t *c, void *buf, int len) {
    if (!c || len <= 0) return -1;

    if (!c->connected) {
        return buf_read(c, buf, len);
    }

    usbmux_tcphdr_t tcp;
    uint8_t *body = NULL;
    int data_len = recv_tcp_pkt(c, &tcp, &body);

    if (data_len < 0) {
        LOGE("mux_recv: recv_tcp_pkt thất bại");
        return -1;
    }

    /* Xử lý theo flags */
    if (tcp.flags & TH_RST) {
        LOGE("mux_recv: nhận RST — iPhone đóng kết nối");
        if (body) free(body);
        c->connected = 0;
        return -1;
    }

    if (tcp.flags & TH_FIN) {
        LOGI("mux_recv: nhận FIN — iPhone đóng kết nối gracefully");
        if (body) free(body);
        c->connected = 0;
        return 0;  /* EOF */
    }

    if (data_len == 0 && (tcp.flags & TH_ACK)) {
        /* Thuần ACK — không có data, thử lại */
        if (body) free(body);
        return 0;
    }

    /* Có data — cập nhật remote_seq và gửi ACK */
    c->remote_seq = ntohl(tcp.seq) + (uint32_t)data_len;

    /* Copy data ra buffer caller */
    int copy_len = data_len < len ? data_len : len;
    if (body && copy_len > 0) {
        memcpy(buf, body, copy_len);
    }
    if (body) free(body);

    LOGI("mux_recv: %d bytes nhận, remote_seq=%u", data_len, c->remote_seq);

    /* Gửi ACK ngay */
    send_ack(c, (uint16_t)c->conn_id);

    return copy_len;
}

/*
 * mux_recv_exact — Nhận đúng `len` byte (block cho đến khi đủ hoặc lỗi).
 */
int mux_recv_exact(mux_conn_t *c, void *buf, int len) {
    char *dst = (char *)buf;
    int got = 0;

    if (!c->connected) {
        return buf_read(c, buf, len);
    }

    while (got < len) {
        int n = mux_recv(c, dst + got, len - got);
        if (n < 0) {
            LOGE("mux_recv_exact: lỗi tại byte %d/%d", got, len);
            return -1;
        }
        if (n == 0) {
            /* EOF hoặc ACK thuần — kiểm tra connected */
            if (!c->connected) break;
            continue;  /* retry: đây là ACK-only packet */
        }
        got += n;
    }
    return got;
}

/*
 * mux_disconnect — Đóng kết nối gracefully.
 *
 * FIX v28: Gửi FIN+ACK trong v1 mode thay vì chỉ reset state.
 */
void mux_disconnect(mux_conn_t *c) {
    if (!c) return;

    if (c->connected && c->v1_ok && c->conn_id > 0) {
        /* Gửi FIN+ACK */
        uint8_t fin_pkt[V1_TCP_HDR_LEN];
        build_tcp_hdr(fin_pkt, c, (uint16_t)c->conn_id, TH_FIN | TH_ACK, 0);
        raw_send(c, fin_pkt, sizeof(fin_pkt));
        c->local_seq++;  /* FIN consume 1 seq */
        LOGI("mux_disconnect: FIN+ACK gửi, chờ iPhone close...");

        /* Drain incoming FIN+ACK hoặc RST từ iPhone (non-blocking best-effort) */
        usbmux_tcphdr_t rtcp;
        uint8_t *body = NULL;
        recv_tcp_pkt(c, &rtcp, &body);
        if (body) free(body);
    }

    c->connected   = 0;
    c->rxbuf_avail = 0;
    c->rxbuf_pos   = 0;
    c->next_tag    = 1;
    c->v1_ok       = 0;
    c->local_seq   = 1000;
    c->remote_seq  = 0;
    LOGI("mux_disconnect: done");
}
