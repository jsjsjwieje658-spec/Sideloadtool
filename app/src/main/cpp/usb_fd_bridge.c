/*
 * usb_fd_bridge.c — Android USB fd → libusb handle bridge (Mode 1)
 *
 * ════════════════════════════════════════════════════════════════════
 * KIẾN TRÚC HỌC TỪ termux-usbmuxd + termux-api (UsbAPI.java)
 * ════════════════════════════════════════════════════════════════════
 *
 * termux-usbmuxd dùng: termux-usb -E -e "usbmuxd_proxy ..." /dev/bus/usb/XXX
 *
 * UsbAPI.java open():
 *   connection = usbManager.openDevice(device)    // KHÔNG claim interface
 *   fd = connection.getFileDescriptor()
 *   openDevices.put(fd, connection)               // giữ connection alive
 *   return fd                                     // → TERMUX_USB_FD → libusb
 *
 * Sau khi libusb nhận fd SẠCH (không có Android interface claim):
 *   libusb_wrap_sys_device(ctx, fd, &handle)      // libusb quản lý fd
 *   libusb_claim_interface(handle, iface)         // THÀNH CÔNG (không BUSY)
 *   Endpoint ở trạng thái sạch → version exchange OK
 *
 * ════════════════════════════════════════════════════════════════════
 * VẤN ĐỀ CŨ (trước fix này)
 * ════════════════════════════════════════════════════════════════════
 *
 * UsbTransport.open() cũ gọi claimInterface() trước → Android owns endpoints
 * → libusb gặp LIBUSB_ERROR_BUSY khi claim → LIBUSB_ERROR_PIPE trên transfers
 * → version exchange thất bại ngay cả sau nhiều retry + clear_halt
 *
 * ════════════════════════════════════════════════════════════════════
 * FIX v27 (tất cả fixes)
 * ════════════════════════════════════════════════════════════════════
 *
 *  1. discover_apple_endpoints(): xử lý cả LIBUSB_SUCCESS (fd sạch từ Kotlin)
 *     lẫn LIBUSB_ERROR_BUSY (fd đã claim từ Android) — cả hai đều OK
 *
 *  2. usb_bridge_init_from_fd(): sau discover, proactive libusb_clear_halt()
 *     trên cả ep_out và ep_in với delay 100ms mỗi endpoint
 *
 *  3. usb_bridge_clear_endpoints_halt(): public function, gọi từ usbmuxd_server.c
 *     trước version exchange retry
 *
 *  4. usb_bridge_flush_in(): sau PIPE, TIẾP TỤC drain thay vì break ngay
 *
 *  5. bulk_write/bulk_read: retry từ 3 → 5 lần, delay từ 50ms → 80ms
 */
#include "usb_fd_bridge.h"
#include <libusb.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <android/log.h>

#define TAG "usb_fd_bridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Apple AMDI interface */
#define APPLE_IF_CLASS     0xFF
#define APPLE_IF_SUBCLASS  0xFE
#define APPLE_IF_PROTO     0x02

/* ── Global state ──────────────────────────────────────────────────────── */
static libusb_context       *g_ctx        = NULL;
static libusb_device_handle *g_handle     = NULL;
static uint8_t               g_ep_in      = 0;
static uint8_t               g_ep_out     = 0;
static int                   g_iface_num  = -1;
static int                   g_initialized = 0;

/* ════════════════════════════════════════════════════════════════════════
 * discover_apple_endpoints
 * ════════════════════════════════════════════════════════════════════════ */
static bool discover_apple_endpoints(void) {
    struct libusb_config_descriptor *cfg = NULL;
    libusb_device *dev = libusb_get_device(g_handle);
    if (!dev) return false;

    if (libusb_get_active_config_descriptor(dev, &cfg) != 0) {
        LOGE("discover: libusb_get_active_config_descriptor thất bại");
        return false;
    }

    bool found = false;
    for (int i = 0; i < (int)cfg->bNumInterfaces && !found; i++) {
        const struct libusb_interface *iface = &cfg->interface[i];
        for (int s = 0; s < iface->num_altsetting && !found; s++) {
            const struct libusb_interface_descriptor *alt = &iface->altsetting[s];

            bool is_amdi = (alt->bInterfaceClass    == APPLE_IF_CLASS &&
                            alt->bInterfaceSubClass == APPLE_IF_SUBCLASS &&
                            alt->bInterfaceProtocol == APPLE_IF_PROTO);
            if (!is_amdi) continue;

            LOGI("discover: Apple AMDI interface #%d altsetting=%d",
                 alt->bInterfaceNumber, alt->bAlternateSetting);

            uint8_t ep_in = 0, ep_out = 0;
            for (int e = 0; e < (int)alt->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
                if ((ep->bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_BULK) continue;
                if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                    if (!ep_in) ep_in = ep->bEndpointAddress;
                } else {
                    if (!ep_out) ep_out = ep->bEndpointAddress;
                }
            }

            if (ep_in && ep_out) {
                /*
                 * FIX v27: Xử lý cả hai trường hợp claim interface.
                 *
                 * TRƯỜNG HỢP A (termux-usbmuxd pattern — fd sạch từ Kotlin):
                 *   UsbTransport.open() không gọi claimInterface().
                 *   libusb_claim_interface() ở đây trả LIBUSB_SUCCESS.
                 *   Endpoint hoàn toàn sạch → version exchange dễ thành công.
                 *
                 * TRƯỜNG HỢP B (Android pre-claim — fd đã claim):
                 *   UsbTransport.open() đã gọi claimInterface() trước.
                 *   libusb_claim_interface() trả LIBUSB_ERROR_BUSY.
                 *   Vẫn hoạt động vì libusb chia sẻ fd với Android.
                 *   Nhưng endpoint có thể STALL — cần clear_halt tích cực.
                 *
                 * KHÔNG gọi libusb_detach_kernel_driver() — không áp dụng
                 * trên Android (không có kernel driver kiểu Linux desktop).
                 */
                int r = libusb_claim_interface(g_handle, alt->bInterfaceNumber);
                if (r == 0) {
                    LOGI("discover: ✅ interface %d claimed successfully (fd sạch — termux-api pattern)",
                         alt->bInterfaceNumber);
                } else if (r == LIBUSB_ERROR_BUSY) {
                    LOGI("discover: interface %d BUSY (Android pre-claimed) — chia sẻ fd, tiếp tục",
                         alt->bInterfaceNumber);
                } else if (r == LIBUSB_ERROR_NOT_SUPPORTED) {
                    LOGI("discover: interface %d NOT_SUPPORTED — tiếp tục (bình thường trên Android)",
                         alt->bInterfaceNumber);
                } else {
                    LOGE("discover: libusb_claim_interface(%d) err=%d (%s) — tiếp tục",
                         alt->bInterfaceNumber, r, libusb_error_name(r));
                }
                g_ep_in     = ep_in;
                g_ep_out    = ep_out;
                g_iface_num = alt->bInterfaceNumber;
                found = true;
                LOGI("discover: ep_in=0x%02x ep_out=0x%02x iface=%d",
                     g_ep_in, g_ep_out, g_iface_num);
            }
        }
    }
    libusb_free_config_descriptor(cfg);

    if (!found) {
        LOGE("discover: không tìm thấy Apple AMDI — dùng endpoint mặc định");
        g_ep_in  = 0x85;
        g_ep_out = 0x04;
        found = true;
    }
    return found;
}

/* ════════════════════════════════════════════════════════════════════════
 * usb_bridge_init_from_fd
 * ════════════════════════════════════════════════════════════════════════ */
bool usb_bridge_init_from_fd(int fd, int vendor_id, int product_id) {
    (void)vendor_id;

    if (g_initialized) {
        LOGI("usb_bridge_init: đã init — reset trước");
        usb_bridge_close();
    }

    int r = libusb_init(&g_ctx);
    if (r != 0) {
        LOGE("libusb_init() err=%d (%s)", r, libusb_error_name(r));
        return false;
    }

    libusb_set_option(g_ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);

    r = libusb_wrap_sys_device(g_ctx, (intptr_t)fd, &g_handle);
    if (r != 0) {
        LOGE("libusb_wrap_sys_device(fd=%d, pid=0x%04x) err=%d (%s)",
             fd, product_id, r, libusb_error_name(r));
        libusb_exit(g_ctx);
        g_ctx = NULL;
        return false;
    }

    LOGI("libusb_wrap_sys_device OK: fd=%d pid=0x%04x", fd, product_id);

    /* FIX ROOT CAUSE #3: KHÔNG gọi libusb_reset_device().
     *
     * libusb_reset_device() gây USB bus reset → iPhone ngắt kết nối khỏi
     * Android USB host → Android nhận USB_DEVICE_DETACHED event → Android
     * huỷ UsbDeviceConnection → fd không còn hợp lệ → mọi libusb operation
     * sau đó thất bại với LIBUSB_ERROR_NO_DEVICE hoặc LIBUSB_ERROR_IO.
     *
     * termux-usbmuxd KHÔNG cần reset vì nó nhận fd SẠCH (UsbAPI.java chỉ
     * openDevice, không claim). Sideloadtool v27+ cũng dùng fd sạch
     * (UsbTransport.open() không claimInterface nữa). Không cần reset.
     *
     * Thay thế: dùng clear_halt + flush sau khi discover endpoints — đủ để
     * xóa trạng thái STALL mà không gây re-enumeration.
     */
    LOGI("usb_bridge_init: bỏ qua libusb_reset_device() (FIX ROOT CAUSE #3 — gây re-enum)");

    if (!discover_apple_endpoints()) {
        LOGE("discover_apple_endpoints() thất bại");
        libusb_close(g_handle);
        libusb_exit(g_ctx);
        g_handle = NULL;
        g_ctx    = NULL;
        return false;
    }

    /*
     * FIX v27 (Critical): Proactive libusb_clear_halt() ngay sau discover.
     *
     * Dù fd có sạch (chưa claim từ Android) hay không, endpoint vẫn có thể
     * ở trạng thái STALL sau khi iPhone vừa kết nối USB. Bước này bắt buộc.
     *
     * Nếu fd sạch (termux-api pattern): clear_halt thường trả SUCCESS → OK
     * Nếu fd đã claim (Android pattern): clear_halt có thể trả error → non-fatal
     */
    LOGI("usb_bridge_init: clear endpoint halts...");
    {
        int re = libusb_clear_halt(g_handle, g_ep_out);
        LOGI("clear_halt ep_out=0x%02x → %d (%s)", g_ep_out, re,
             re == 0 ? "OK" : libusb_error_name(re));
        usleep(100 * 1000);
    }
    {
        int re = libusb_clear_halt(g_handle, g_ep_in);
        LOGI("clear_halt ep_in=0x%02x → %d (%s)", g_ep_in, re,
             re == 0 ? "OK" : libusb_error_name(re));
        usleep(100 * 1000);
    }

    g_initialized = 1;
    LOGI("usb_bridge_init: ✅ sẵn sàng — ep_in=0x%02x ep_out=0x%02x",
         g_ep_in, g_ep_out);
    return true;
}

/* ════════════════════════════════════════════════════════════════════════
 * usb_bridge_clear_endpoints_halt (public)
 * ════════════════════════════════════════════════════════════════════════ */
bool usb_bridge_clear_endpoints_halt(void) {
    if (!g_handle) return false;
    bool any_ok = false;

    if (g_ep_out) {
        int r = libusb_clear_halt(g_handle, g_ep_out);
        LOGI("clear_halt ep_out=0x%02x → %d", g_ep_out, r);
        if (r == 0 || r == LIBUSB_ERROR_NOT_FOUND) any_ok = true;
        usleep(80 * 1000);
    }
    if (g_ep_in) {
        int r = libusb_clear_halt(g_handle, g_ep_in);
        LOGI("clear_halt ep_in=0x%02x → %d", g_ep_in, r);
        if (r == 0 || r == LIBUSB_ERROR_NOT_FOUND) any_ok = true;
        usleep(80 * 1000);
    }
    return any_ok;
}

uint8_t usb_bridge_ep_in(void)  { return g_ep_in;  }
uint8_t usb_bridge_ep_out(void) { return g_ep_out; }

/* ════════════════════════════════════════════════════════════════════════
 * usb_bridge_bulk_write — 5 retry, 80ms delay
 * ════════════════════════════════════════════════════════════════════════ */
int usb_bridge_bulk_write(const void *buf, int len, unsigned int timeout) {
    if (!g_handle || !g_ep_out) return -1;
    for (int attempt = 0; attempt < 5; attempt++) {
        int transferred = 0;
        int r = libusb_bulk_transfer(g_handle, g_ep_out,
                                      (unsigned char *)buf, len,
                                      &transferred, timeout ? timeout : 5000);
        if (r == 0) return transferred;
        if (r == LIBUSB_ERROR_TIMEOUT) return transferred;
        if (r == LIBUSB_ERROR_PIPE) {
            LOGI("bulk_write: PIPE ep=0x%02x — clear_halt retry %d/5", g_ep_out, attempt+1);
            libusb_clear_halt(g_handle, g_ep_out);
            usleep(80 * 1000);
            continue;
        }
        LOGE("bulk_write ep=0x%02x err=%d (%s)", g_ep_out, r, libusb_error_name(r));
        return -1;
    }
    LOGE("bulk_write: 5 lần PIPE, ep=0x%02x", g_ep_out);
    return -1;
}

/* ════════════════════════════════════════════════════════════════════════
 * usb_bridge_bulk_read — 5 retry, 80ms delay
 * ════════════════════════════════════════════════════════════════════════ */
int usb_bridge_bulk_read(void *buf, int len, unsigned int timeout) {
    if (!g_handle || !g_ep_in) return -1;
    for (int attempt = 0; attempt < 5; attempt++) {
        int transferred = 0;
        int r = libusb_bulk_transfer(g_handle, g_ep_in,
                                      (unsigned char *)buf, len,
                                      &transferred, timeout ? timeout : 10000);
        if (r == 0) return transferred;
        if (r == LIBUSB_ERROR_TIMEOUT) return 0;
        if (r == LIBUSB_ERROR_PIPE) {
            LOGI("bulk_read: PIPE ep=0x%02x — clear_halt retry %d/5", g_ep_in, attempt+1);
            libusb_clear_halt(g_handle, g_ep_in);
            usleep(80 * 1000);
            continue;
        }
        LOGE("bulk_read ep=0x%02x err=%d (%s)", g_ep_in, r, libusb_error_name(r));
        return -1;
    }
    LOGE("bulk_read: 5 lần PIPE, ep=0x%02x", g_ep_in);
    return -1;
}

/* ════════════════════════════════════════════════════════════════════════
 * usb_bridge_flush_in — drain stale data, tiếp tục sau PIPE
 * ════════════════════════════════════════════════════════════════════════ */
void usb_bridge_flush_in(int max_packets, int timeout_ms) {
    if (!g_handle || !g_ep_in) return;

    uint8_t *buf = malloc(65536);
    if (!buf) return;

    int drained = 0;
    int pipe_count = 0;
    for (int i = 0; i < max_packets; i++) {
        int transferred = 0;
        int r = libusb_bulk_transfer(g_handle, g_ep_in,
                                      buf, 65536, &transferred,
                                      (unsigned int)timeout_ms);
        if (r == LIBUSB_ERROR_TIMEOUT) break;
        if (r == LIBUSB_ERROR_PIPE) {
            libusb_clear_halt(g_handle, g_ep_in);
            usleep(50 * 1000);
            if (++pipe_count >= 3) break;
            continue;  /* FIX: tiếp tục drain sau PIPE, không break ngay */
        }
        if (r != 0) break;
        if (transferred > 0) {
            drained += transferred;
            pipe_count = 0;
            LOGI("flush_in: drained %d bytes (packet %d)", transferred, i+1);
        } else {
            break;
        }
    }
    free(buf);
    LOGI("flush_in: tổng %d bytes drained", drained);
}

/* ════════════════════════════════════════════════════════════════════════
 * usb_bridge_close
 * ════════════════════════════════════════════════════════════════════════ */
void usb_bridge_close(void) {
    if (g_handle) {
        if (g_iface_num >= 0) {
            libusb_release_interface(g_handle, g_iface_num);
            g_iface_num = -1;
        }
        libusb_close(g_handle);
        g_handle = NULL;
    }
    if (g_ctx) {
        libusb_exit(g_ctx);
        g_ctx = NULL;
    }
    g_ep_in       = 0;
    g_ep_out      = 0;
    g_initialized = 0;
    LOGI("usb_bridge_close: done");
}
