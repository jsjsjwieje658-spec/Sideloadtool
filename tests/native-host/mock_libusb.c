/*
 * mock_libusb.c — libusb giả cho test host: mô phỏng một iPhone có 4 USB
 * configuration (1 = PTP, 2 = audio, 3 = PTP + usbmux, 4 = PTP + usbmux +
 * USB-Ethernet), ĐANG Ở CONFIG 1 — đúng tình trạng trên Android. claim
 * interface usbmux khi chưa đổi configuration trả LIBUSB_ERROR_NOT_FOUND
 * (chính là lỗi trong log của bản cũ).
 *
 * Bulk transfer được chuyển tới fake_iphone.py qua Unix socket
 * ($FAKE_IPHONE_SOCK), mỗi transfer một frame [u32 LE len][data].
 * Transfer IN lớn hơn bộ đệm bị cắt như USB thật (phần còn lại ở lần đọc sau).
 */
#include <libusb.h>

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct libusb_context { int no_discovery; };
struct libusb_device { int dummy; };
struct libusb_device_handle { int fd; };

static struct libusb_context g_ctx_obj;
static struct libusb_device g_dev_obj;
static struct libusb_device_handle g_h_obj = { -1 };

int mock_current_config = 1;
int mock_set_config_calls = 0;
int mock_claimed_iface = -1;
int mock_no_discovery = 0;
int mock_zlp_sent = 0;

static pthread_mutex_t g_rd = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_wr = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *g_pending = NULL;
static uint32_t g_pending_len = 0, g_pending_off = 0;

/* ── descriptor ─────────────────────────────────────────────────────────── */
static const struct libusb_endpoint_descriptor ep_mux[2] = {
    { .bLength = 7, .bDescriptorType = 5, .bEndpointAddress = 0x04, .bmAttributes = 2, .wMaxPacketSize = 512 },
    { .bLength = 7, .bDescriptorType = 5, .bEndpointAddress = 0x85, .bmAttributes = 2, .wMaxPacketSize = 512 },
};
static const struct libusb_endpoint_descriptor ep_eth[2] = {
    { .bLength = 7, .bDescriptorType = 5, .bEndpointAddress = 0x06, .bmAttributes = 2, .wMaxPacketSize = 512 },
    { .bLength = 7, .bDescriptorType = 5, .bEndpointAddress = 0x87, .bmAttributes = 2, .wMaxPacketSize = 512 },
};
static const struct libusb_interface_descriptor alt_ptp = {
    .bLength = 9, .bDescriptorType = 4, .bInterfaceNumber = 0, .bNumEndpoints = 0,
    .bInterfaceClass = 6, .bInterfaceSubClass = 1, .bInterfaceProtocol = 1 };
static const struct libusb_interface_descriptor alt_audio = {
    .bLength = 9, .bDescriptorType = 4, .bInterfaceNumber = 0, .bNumEndpoints = 0,
    .bInterfaceClass = 1, .bInterfaceSubClass = 1, .bInterfaceProtocol = 0 };
static const struct libusb_interface_descriptor alt_mux = {
    .bLength = 9, .bDescriptorType = 4, .bInterfaceNumber = 1, .bNumEndpoints = 2,
    .bInterfaceClass = 255, .bInterfaceSubClass = 254, .bInterfaceProtocol = 2, .endpoint = ep_mux };
static const struct libusb_interface_descriptor alt_eth = {
    .bLength = 9, .bDescriptorType = 4, .bInterfaceNumber = 2, .bNumEndpoints = 2,
    .bInterfaceClass = 255, .bInterfaceSubClass = 253, .bInterfaceProtocol = 1, .endpoint = ep_eth };

static const struct libusb_interface if_ptp   = { .altsetting = &alt_ptp,   .num_altsetting = 1 };
static const struct libusb_interface if_audio = { .altsetting = &alt_audio, .num_altsetting = 1 };
static const struct libusb_interface if_mux   = { .altsetting = &alt_mux,   .num_altsetting = 1 };
static const struct libusb_interface if_eth   = { .altsetting = &alt_eth,   .num_altsetting = 1 };

static const struct libusb_interface cfg1_if[] = { if_ptp };
static const struct libusb_interface cfg2_if[] = { if_audio };
static const struct libusb_interface cfg3_if[] = { if_ptp, if_mux };
static const struct libusb_interface cfg4_if[] = { if_ptp, if_mux, if_eth };

static int make_cfg(int value, struct libusb_config_descriptor **out) {
    struct libusb_config_descriptor *c = calloc(1, sizeof(*c));
    if (!c) return LIBUSB_ERROR_NO_MEM;
    c->bLength = 9; c->bDescriptorType = 2; c->bConfigurationValue = (uint8_t)value;
    switch (value) {
        case 1: c->bNumInterfaces = 1; c->interface = cfg1_if; break;
        case 2: c->bNumInterfaces = 1; c->interface = cfg2_if; break;
        case 3: c->bNumInterfaces = 2; c->interface = cfg3_if; break;
        case 4: c->bNumInterfaces = 3; c->interface = cfg4_if; break;
        default: free(c); return LIBUSB_ERROR_NOT_FOUND;
    }
    *out = c;
    return 0;
}

/* ── API ────────────────────────────────────────────────────────────────── */
int LIBUSB_CALL libusb_init_context(libusb_context **ctx, const struct libusb_init_option options[], int num_options) {
    for (int i = 0; i < num_options; i++)
        if (options[i].option == LIBUSB_OPTION_NO_DEVICE_DISCOVERY) mock_no_discovery = 1;
    *ctx = &g_ctx_obj;
    return 0;
}
int LIBUSB_CALL libusb_init(libusb_context **ctx) { *ctx = &g_ctx_obj; return 0; }
void LIBUSB_CALL libusb_exit(libusb_context *ctx) { (void)ctx; }
int LIBUSB_CALLV libusb_set_option(libusb_context *ctx, enum libusb_option option, ...) { (void)ctx; (void)option; return 0; }

int LIBUSB_CALL libusb_wrap_sys_device(libusb_context *ctx, intptr_t sys_dev, libusb_device_handle **dev_handle) {
    (void)ctx; (void)sys_dev;
    const char *path = getenv("FAKE_IPHONE_SOCK");
    if (!path) return LIBUSB_ERROR_IO;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { close(fd); return LIBUSB_ERROR_IO; }
    g_h_obj.fd = fd;
    *dev_handle = &g_h_obj;
    return 0;
}
libusb_device *LIBUSB_CALL libusb_get_device(libusb_device_handle *h) { (void)h; return &g_dev_obj; }
void LIBUSB_CALL libusb_close(libusb_device_handle *h) { if (h && h->fd >= 0) { close(h->fd); h->fd = -1; } }

int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *d) {
    (void)dev;
    memset(d, 0, sizeof(*d));
    d->bLength = 18; d->bDescriptorType = 1; d->idVendor = 0x05ac; d->idProduct = 0x12a8;
    d->bNumConfigurations = 4; d->iSerialNumber = 3;
    return 0;
}
int LIBUSB_CALL libusb_get_configuration(libusb_device_handle *h, int *config) { (void)h; *config = mock_current_config; return 0; }
int LIBUSB_CALL libusb_get_config_descriptor(libusb_device *dev, uint8_t idx, struct libusb_config_descriptor **c) {
    (void)dev; return make_cfg(idx + 1, c);
}
int LIBUSB_CALL libusb_get_config_descriptor_by_value(libusb_device *dev, uint8_t v, struct libusb_config_descriptor **c) {
    (void)dev; return make_cfg(v, c);
}
void LIBUSB_CALL libusb_free_config_descriptor(struct libusb_config_descriptor *c) { free(c); }
int LIBUSB_CALL libusb_kernel_driver_active(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
int LIBUSB_CALL libusb_detach_kernel_driver(libusb_device_handle *h, int i) { (void)h; (void)i; return LIBUSB_ERROR_NOT_FOUND; }
int LIBUSB_CALL libusb_set_configuration(libusb_device_handle *h, int c) {
    (void)h;
    mock_set_config_calls++;
    if (c < 1 || c > 4) return LIBUSB_ERROR_NOT_FOUND;
    mock_current_config = c;
    return 0;
}
int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *h, int iface) {
    (void)h;
    struct libusb_config_descriptor *c = NULL;
    make_cfg(mock_current_config, &c);
    int ok = 0;
    for (int i = 0; c && i < c->bNumInterfaces; i++)
        if (c->interface[i].altsetting[0].bInterfaceNumber == iface) ok = 1;
    free(c);
    if (!ok) return LIBUSB_ERROR_NOT_FOUND;         /* đúng hành vi usbfs (ENOENT) */
    mock_claimed_iface = iface;
    return 0;
}
int LIBUSB_CALL libusb_release_interface(libusb_device_handle *h, int i) { (void)h; (void)i; mock_claimed_iface = -1; return 0; }
int LIBUSB_CALL libusb_clear_halt(libusb_device_handle *h, unsigned char ep) { (void)h; (void)ep; return 0; }
const char *LIBUSB_CALL libusb_error_name(int e) {
    switch (e) {
        case 0: return "LIBUSB_SUCCESS";
        case LIBUSB_ERROR_IO: return "LIBUSB_ERROR_IO";
        case LIBUSB_ERROR_NOT_FOUND: return "LIBUSB_ERROR_NOT_FOUND";
        case LIBUSB_ERROR_BUSY: return "LIBUSB_ERROR_BUSY";
        case LIBUSB_ERROR_TIMEOUT: return "LIBUSB_ERROR_TIMEOUT";
        case LIBUSB_ERROR_PIPE: return "LIBUSB_ERROR_PIPE";
        default: return "LIBUSB_ERROR_OTHER";
    }
}
int LIBUSB_CALL libusb_get_string_descriptor_ascii(libusb_device_handle *h, uint8_t idx, unsigned char *data, int length) {
    (void)h;
    if (idx != 3) return LIBUSB_ERROR_INVALID_PARAM;
    const char *serial = getenv("FAKE_SERIAL") ? getenv("FAKE_SERIAL") : "00008030001A35E80C41802E";
    int n = (int)strlen(serial);
    if (n > length) n = length;
    memcpy(data, serial, (size_t)n);
    return n;
}

static int write_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        ssize_t w = send(fd, c, n, MSG_NOSIGNAL);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return -1; }
        c += w; n -= (size_t)w;
    }
    return 0;
}
static int read_all(int fd, void *p, size_t n) {
    char *c = p;
    while (n) {
        ssize_t r = recv(fd, c, n, 0);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
        c += r; n -= (size_t)r;
    }
    return 0;
}

int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle *h, unsigned char endpoint, unsigned char *data,
                                     int length, int *transferred, unsigned int timeout) {
    *transferred = 0;
    if (h->fd < 0) return LIBUSB_ERROR_NO_DEVICE;
    if (!(endpoint & 0x80)) {
        if (mock_claimed_iface != 1 || endpoint != 0x04) return LIBUSB_ERROR_IO;
        pthread_mutex_lock(&g_wr);
        uint32_t len = (uint32_t)length;
        int r = write_all(h->fd, &len, 4);
        if (r == 0 && length) r = write_all(h->fd, data, (size_t)length);
        pthread_mutex_unlock(&g_wr);
        if (r != 0) return LIBUSB_ERROR_NO_DEVICE;
        if (length == 0) mock_zlp_sent++;
        *transferred = length;
        return 0;
    }
    if (mock_claimed_iface != 1 || endpoint != 0x85) return LIBUSB_ERROR_IO;
    pthread_mutex_lock(&g_rd);
    if (!g_pending || g_pending_off >= g_pending_len) {
        struct pollfd p = { .fd = h->fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&p, 1, (int)(timeout ? timeout : 1000));
        if (pr == 0) { pthread_mutex_unlock(&g_rd); return LIBUSB_ERROR_TIMEOUT; }
        uint32_t len = 0;
        if (read_all(h->fd, &len, 4) != 0) { pthread_mutex_unlock(&g_rd); return LIBUSB_ERROR_NO_DEVICE; }
        free(g_pending);
        g_pending = malloc(len ? len : 1);
        g_pending_len = len;
        g_pending_off = 0;
        if (len && read_all(h->fd, g_pending, len) != 0) { pthread_mutex_unlock(&g_rd); return LIBUSB_ERROR_NO_DEVICE; }
        if (len == 0) { pthread_mutex_unlock(&g_rd); return 0; }   /* ZLP */
    }
    uint32_t avail = g_pending_len - g_pending_off;
    uint32_t n = avail < (uint32_t)length ? avail : (uint32_t)length;
    memcpy(data, g_pending + g_pending_off, n);
    g_pending_off += n;
    *transferred = (int)n;
    pthread_mutex_unlock(&g_rd);
    return 0;
}
