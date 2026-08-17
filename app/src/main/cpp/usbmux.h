#pragma once
/*
 * usbmux.h — TCP-over-USB usbmux protocol cho Mode 2/3 (no libimobiledevice)
 *
 * Giao tiếp trực tiếp với iPhone qua USB bulk endpoints.
 *
 * Protocol: Apple usbmux v1 (iOS 7+) — big-endian, magic=0xfeedface,
 *           TCP-like SYN/ACK/DATA/FIN over USB bulk.
 *
 * FIX v28: Thay thế hoàn toàn v0 binary protocol (CONNECT/DATA/RESULT)
 *          bằng v1 TCP-like protocol, đúng với iPhone iOS 7+ (iPhone 4S+).
 *          v0 protocol chỉ hoạt động với iPhone 3GS và cũ hơn.
 *
 *          Học từ:
 *            - usbmuxd_server.c (Mode 1) — v1 protocol đã implement đúng
 *            - termux-usbmuxd source — session lifecycle và endpoint usage
 *
 * Kiến trúc single-connection: mỗi mux_conn_t là một kênh TCP độc lập
 * trên USB. Khi cần nhiều service (lockdownd, AFC, instproxy), dùng
 * nhiều mux_conn_t riêng biệt theo thứ tự tuần tự.
 */
#include <stdint.h>
#include <stddef.h>

/* USB read/write MRU — khớp với iPhone USB bulk packet size tối đa */
#define MUX_DEV_MRU 65536

/* ── I/O callbacks (được gọi từ usb_bulk_write/read trong jni_bridge) ──── */
typedef int  (*usb_write_fn)(const void *buf, int len);
typedef int  (*usb_read_fn) (void       *buf, int len);
typedef void (*ui_log_fn)   (const char *msg);

/* ── Apple usbmux v1 protocol headers (all big-endian) ──────────────────
 *
 * FIX v28: Sử dụng v1 header (giống usbmuxd_server.c) thay vì v0 header.
 *
 * v1 mux header (16 bytes):
 *   protocol: 0=VERSION packet, 1=TCP packet
 *   length:   tổng độ dài gói (header + tcp_header + data)
 *   magic:    0xfeedface — phân biệt với v0 garbage
 *   tx_seq:   frame counter của bên gửi (chúng ta gửi 0)
 *   rx_seq:   frame counter của packet cuối nhận được (0xffff = init)
 *
 * v1 TCP header (20 bytes, ngay sau mux header trong TCP packets):
 *   sport, dport, seq, ack, off(=0x50), flags, window, cksum(=0), urgp(=0)
 * ────────────────────────────────────────────────────────────────────── */
#pragma pack(push, 1)

typedef struct {
    uint32_t protocol;  /* BE: 0=VERSION, 1=TCP */
    uint32_t length;    /* BE: total packet length including this header */
    uint32_t magic;     /* BE: 0xfeedface */
    uint16_t tx_seq;    /* BE: our frame seq (send 0) */
    uint16_t rx_seq;    /* BE: last received frame (send 0xffff at start) */
} usbmux_v1hdr_t;

typedef struct {
    uint16_t sport;     /* BE: source port (our ephemeral) */
    uint16_t dport;     /* BE: destination port (iPhone service) */
    uint32_t seq;       /* BE: our sequence number */
    uint32_t ack;       /* BE: acknowledged seq from iPhone */
    uint8_t  off;       /* data offset in 32-bit words: 0x50 = 20 bytes */
    uint8_t  flags;     /* TCP flags (see TH_* below) */
    uint16_t window;    /* BE: receive window */
    uint16_t cksum;     /* checksum (0 = skip, iPhone accepts 0) */
    uint16_t urgp;      /* urgent pointer (0) */
} usbmux_tcphdr_t;

typedef struct {
    uint32_t major;     /* BE: protocol version major (1) */
    uint32_t minor;     /* BE: protocol version minor (0) */
    uint32_t pad1;
    uint32_t pad2;
} usbmux_verbody_t;

#pragma pack(pop)

/* v1 protocol constants */
#define V1_MAGIC        0xfeedface
#define V1_PROTO_VER    0           /* VERSION packet type */
#define V1_PROTO_TCP    1           /* TCP packet type */
#define V1_VERSION_PKT_LEN  (sizeof(usbmux_v1hdr_t) + sizeof(usbmux_verbody_t))
#define V1_TCP_HDR_LEN      (sizeof(usbmux_v1hdr_t) + sizeof(usbmux_tcphdr_t))

/* TCP flags */
#define TH_FIN  0x01
#define TH_SYN  0x02
#define TH_RST  0x04
#define TH_PSH  0x08
#define TH_ACK  0x10

/* Ephemeral source port chúng ta dùng (bất kỳ port > 1024 đều OK) */
#define MUX_EPHEMERAL_PORT  12321

/* Receive window quảng bá cho iPhone */
#define MUX_RECV_WINDOW     0x2000   /* 8192 bytes */

/* ── Connection state ─────────────────────────────────────────────────── */
typedef struct mux_conn {
    usb_write_fn  usb_write;      /* USB bulk OUT callback */
    usb_read_fn   usb_read;       /* USB bulk IN callback  */
    ui_log_fn     ui_log;         /* UI log callback (nullable) */

    /* v1 protocol negotiated state (FIX v28) */
    int           v1_ok;          /* 1 khi VERSION exchange thành công */
    uint16_t      sport;          /* source port của kết nối hiện tại */
    uint32_t      local_seq;      /* seq tiếp theo chúng ta gửi */
    uint32_t      remote_seq;     /* seq tiếp theo chúng ta expect từ iPhone */

    /* Legacy fields (vẫn dùng cho conn tracking) */
    uint32_t      next_tag;       /* unused in v1, kept for compat */
    uint32_t      conn_id;        /* unused in v1, kept for compat */
    int           connected;      /* 1 after mux_connect succeeds */

    /* Receive ring buffer */
    uint8_t       rxbuf[MUX_DEV_MRU];
    int           rxbuf_avail;    /* bytes available in rxbuf */
    int           rxbuf_pos;      /* read position in rxbuf */
} mux_conn_t;

/* ── API ──────────────────────────────────────────────────────────────── */

/* Khởi tạo mux connection với USB I/O callbacks */
int  mux_conn_init  (mux_conn_t *c, usb_write_fn wfn, usb_read_fn rfn);

/*
 * Bắt tay usbmux v1 với iPhone.
 * Gửi VERSION packet (big-endian, magic=0xfeedface) và nhận response.
 * FIX v28: Thay thế v0 CONNECT(port=0) probe không hợp lệ.
 */
int  mux_do_setup   (mux_conn_t *c);

/* Mở kết nối TCP đến port trên iPhone (v1 SYN handshake) */
int  mux_connect    (mux_conn_t *c, int port);

/* Gửi dữ liệu qua kênh đã kết nối (v1 PSH+ACK data packet) */
int  mux_send       (mux_conn_t *c, const void *data, int len);

/* Nhận dữ liệu (có thể ít hơn len; gửi ACK tự động) */
int  mux_recv       (mux_conn_t *c, void *buf, int len);

/* Nhận đúng `len` byte (block cho đến khi đủ hoặc lỗi) */
int  mux_recv_exact (mux_conn_t *c, void *buf, int len);

/* Ngắt kết nối (gửi FIN+ACK trong v1 mode) */
void mux_disconnect (mux_conn_t *c);
