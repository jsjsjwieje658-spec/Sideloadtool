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
#include "android_usbmuxd_fix.h"
#include <libusb.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <android/log.h>

/*
 * FIX v36 (Critical): Forward LOGI/LOGE to UI log viewer.
 *
 * Trước đây, usb_fd_bridge.c chỉ gọi __android_log_print() → log chỉ xuất
 * hiện trong logcat của Android, KHÔNG hiển thị trong UI log viewer của app.
 *
 * Hậu quả: khi bulk_write() return -1, log trong UI chỉ thấy
 *   "usb_send_version: usb_write() returned -1"
 * mà KHÔNG thấy log chi tiết từ usb_bridge_bulk_write() như:
 *   "bulk_write: PIPE ep=0x04 attempt 1/5 — clear_halt retry"
 *   "bulk_write: OVERFLOW ep=0x04 attempt 1/5 — try clear_halt + long delay"
 *   "bulk_write: ACCESS DENIED ep=0x04 err=-3 — thiếu quyền USB Host"
 *
 * → Người dùng/không debug được error code thực sự của libusb.
 *
 * Fix: thêm android_usbmuxd_fix_logf() vào macro LOGI/LOGE để forward
 * log vào UI log viewer qua callback đã được set trong nativeInit().
 * (Giống cách usbmuxd_server.c đã làm.)
 */
#define TAG "usb_fd_bridge"
#define LOGI(...) do { \
    __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__); \
    android_usbmuxd_fix_logf(__VA_ARGS__); \
} while (0)
#define LOGE(...) do { \
    __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__); \
    android_usbmuxd_fix_logf(__VA_ARGS__); \
} while (0)

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
/* Chỉ true khi chính libusb claim thành công interface. Với fd từ
 * UsbDeviceConnection/termux-usb, Android có thể giữ ownership hoặc libusb
 * có thể trả NOT_SUPPORTED; trong cả hai trường hợp không được release
 * interface khi đóng handle. */
static int                   g_iface_claimed = 0;
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
                    g_iface_claimed = 1;
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

    /* FIX ROOT CAUSE #3: KHÔNG gọi libusb_reset_device(). */
    LOGI("usb_bridge_init: bỏ qua libusb_reset_device() (FIX ROOT CAUSE #3 — gây re-enum)");

    if (!discover_apple_endpoints()) {
        LOGE("discover_apple_endpoints() thất bại");
        libusb_close(g_handle);
        libusb_exit(g_ctx);
        g_handle = NULL;
        g_ctx    = NULL;
        return false;
    }

    LOGI("usb_bridge_init: giữ endpoint nguyên trạng; clear_halt chỉ khi transfer PIPE");

    g_initialized = 1;
    LOGI("usb_bridge_init: ✅ sẵn sàng — ep_in=0x%02x ep_out=0x%02x",
         g_ep_in, g_ep_out);
    return true;
}

/*
 * ════════════════════════════════════════════════════════════════════════
 * usb_bridge_init_from_fd2 — FIX v37
 *
 * Tương tự usb_bridge_init_from_fd() nhưng nhận endpoint addresses +
 * interface number trực tiếp từ Kotlin (đã discover qua UsbInterface API).
 *
 * Lý do (xem log user):
 *   discover_apple_endpoints() fail với Android fd vì
 *   libusb_get_active_config_descriptor() không trả descriptor đầy đủ
 *   cho wrapped sys device → fallback endpoint mặc định (0x85/0x04)
 *   NHƯNG libusb_claim_interface() KHÔNG được gọi → bulk_transfer
 *   trả LIBUSB_ERROR_IO, clear_halt trả LIBUSB_ERROR_NOT_FOUND.
 *
 * Giải pháp: Kotlin đã có endpoint addresses + interface number từ
 * UsbTransport.findUsbmuxIface(). Truyền thẳng xuống native.
 *   1. libusb_wrap_sys_device(fd)
 *   2. Nếu ep_in/ep_out/iface_num được cung cấp (≠ 0/-1) → DÙNG TRỰC TIẾP
 *      và gọi libusb_claim_interface(iface_num)
 *   3. Nếu không được cung cấp → fallback gọi discover_apple_endpoints()
 *      như cũ
 *   4. clear_halt() trên cả 2 endpoints
 * ════════════════════════════════════════════════════════════════════════
 */
bool usb_bridge_init_from_fd2(int fd, int vendor_id, int product_id,
                              int ep_in, int ep_out, int iface_num) {
    (void)vendor_id;

    if (g_initialized) {
        LOGI("usb_bridge_init2: đã init — reset trước");
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
    LOGI("usb_bridge_init2: bỏ qua libusb_reset_device() (FIX ROOT CAUSE #3)");

    /*
     * FIX v37: Nếu caller cung cấp endpoint addresses + interface number,
     * dùng trực tiếp thay vì discover. Đây là path chính trên Android.
     */
    bool have_endpoints = (ep_in != 0 && ep_out != 0 && iface_num >= 0);
    if (have_endpoints) {
        LOGI("usb_bridge_init2: dùng endpoint từ Kotlin: "
             "ep_in=0x%02x ep_out=0x%02x iface=%d",
             (uint8_t)ep_in, (uint8_t)ep_out, iface_num);

        g_ep_in     = (uint8_t)ep_in;
        g_ep_out    = (uint8_t)ep_out;
        g_iface_num = iface_num;

        /*
         * CLAIM INTERFACE — bắt buộc cho bulk_transfer hoạt động.
         *
         * Trên Android với wrapped fd:
         *   - LIBUSB_SUCCESS (0): fd sạch, libusb claim được
         *   - LIBUSB_ERROR_BUSY (-6): Android đã claim → share fd, vẫn OK
         *   - LIBUSB_ERROR_NOT_SUPPORTED (-12): Android kernel không hỗ trợ
         *     claim qua wrapped fd → vẫn thử bulk_transfer, có thể hoạt động
         *
         * Nếu claim fail với error khác → thử detach kernel driver (Linux only)
         * hoặc return false.
         */
        int cr = libusb_claim_interface(g_handle, iface_num);
        if (cr == 0) {
            g_iface_claimed = 1;
            LOGI("usb_bridge_init2: ✅ interface %d claimed successfully (fd sạch)",
                 iface_num);
        } else if (cr == LIBUSB_ERROR_BUSY) {
            LOGI("usb_bridge_init2: interface %d BUSY (Android pre-claimed) — share fd, tiếp tục",
                 iface_num);
            /* Đánh dấu claimed để release sau nếu cần */
            g_iface_claimed = 0;
        } else if (cr == LIBUSB_ERROR_NOT_SUPPORTED) {
            LOGI("usb_bridge_init2: interface %d NOT_SUPPORTED — tiếp tục (thường trên Android)",
                 iface_num);
            g_iface_claimed = 0;
        } else if (cr == LIBUSB_ERROR_NOT_FOUND) {
            /*
             * FIX v37: NOT_FOUND thường xảy ra khi descriptor access không
             * hoạt động với wrapped fd (đây là tình huống đang gặp).
             * Endpoint addresses đã được set từ Kotlin, nên chỉ cần tiếp
             * tục — bulk_transfer có thể vẫn hoạt động qua shared fd.
             */
            LOGI("usb_bridge_init2: libusb_claim_interface(%d) NOT_FOUND — "
                 "endpoint đã set từ Kotlin, tiếp tục bulk_transfer qua shared fd",
                 iface_num);
            g_iface_claimed = 0;
        } else {
            LOGE("usb_bridge_init2: libusb_claim_interface(%d) err=%d (%s) — tiếp tục anyway",
                 iface_num, cr, libusb_error_name(cr));
            g_iface_claimed = 0;
        }

        /* Clear halt trên cả 2 endpoints để đảm bảo sạch */
        if (g_ep_out) {
            int hr = libusb_clear_halt(g_handle, g_ep_out);
            LOGI("usb_bridge_init2: clear_halt ep_out=0x%02x → %d (%s)",
                 g_ep_out, hr, libusb_error_name(hr));
            if (hr == 0 || hr == LIBUSB_ERROR_NOT_FOUND) {
                /* NOT_FOUND không phải lỗi nghiêm trọng lúc init —
                 * sẽ retry khi transfer PIPE */
            }
            usleep(80 * 1000);
        }
        if (g_ep_in) {
            int hr = libusb_clear_halt(g_handle, g_ep_in);
            LOGI("usb_bridge_init2: clear_halt ep_in=0x%02x → %d (%s)",
                 g_ep_in, hr, libusb_error_name(hr));
            usleep(80 * 1000);
        }
    } else {
        /* Caller không cung cấp endpoints — fallback discovery */
        LOGI("usb_bridge_init2: caller không cung cấp endpoints — fallback discover_apple_endpoints()");
        if (!discover_apple_endpoints()) {
            LOGE("usb_bridge_init2: discover_apple_endpoints() thất bại");
            libusb_close(g_handle);
            libusb_exit(g_ctx);
            g_handle = NULL;
            g_ctx    = NULL;
            return false;
        }
    }

    LOGI("usb_bridge_init2: ✅ sẵn sàng — ep_in=0x%02x ep_out=0x%02x iface=%d claimed=%d",
         g_ep_in, g_ep_out, g_iface_num, g_iface_claimed);

    g_initialized = 1;
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
 *
 * FIX v35: Log error code CỤ THỂ cho mọi path lỗi (không chỉ PIPE).
 * Trước đây chỉ log "PIPE ep=..." và return -1 cho các error khác mà
 * không nói rõ là NOT_FOUND/NO_DEVICE/ACCESS/etc → khó debug.
 *
 * Các lỗi phổ biến trên Android:
 *   LIBUSB_ERROR_NOT_FOUND (-5): handle đã bị close do UsbDeviceConnection
 *     bị Android reclaim. Cần re-open.
 *   LIBUSB_ERROR_NO_DEVICE (-4): device đã detach.
 *   LIBUSB_ERROR_ACCESS (-3): thiếu quyền USB.
 *   LIBUSB_ERROR_BUSY (-6): interface bị process khác hold.
 *   LIBUSB_ERROR_OVERFLOW (-8): packet lớn hơn endpoint max packet size.
 *   LIBUSB_ERROR_PIPE (-9): endpoint STALL — clear_halt và retry.
 *   LIBUSB_ERROR_TIMEOUT (-7): timeout.
 * ════════════════════════════════════════════════════════════════════════ */
int usb_bridge_bulk_write(const void *buf, int len, unsigned int timeout) {
    if (!g_handle || !g_ep_out) {
        LOGE("bulk_write: bad state — handle=%p ep_out=0x%02x", (void*)g_handle, g_ep_out);
        return -1;
    }
    int last_err = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        int transferred = 0;
        int r = libusb_bulk_transfer(g_handle, g_ep_out,
                                      (unsigned char *)buf, len,
                                      &transferred, timeout ? timeout : 5000);
        if (r == 0) return transferred;
        if (r == LIBUSB_ERROR_TIMEOUT) {
            LOGI("bulk_write: TIMEOUT ep=0x%02x attempt %d/5 — transferred=%d/%d",
                 g_ep_out, attempt+1, transferred, len);
            /* TIMEOUT không retry ngay — tăng timeout cho lần sau */
            continue;
        }
        if (r == LIBUSB_ERROR_PIPE) {
            LOGI("bulk_write: PIPE ep=0x%02x attempt %d/5 — clear_halt retry",
                 g_ep_out, attempt+1);
            libusb_clear_halt(g_handle, g_ep_out);
            usleep(80 * 1000);
            continue;
        }
        if (r == LIBUSB_ERROR_OVERFLOW) {
            /*
             * FIX v35 (Critical): OVERFLOW thường xảy ra khi Android UsbDeviceConnection
             * đã claim interface và libusb chia sẻ fd. Khi đó libusb không gửi được
             * packet đúng kích thước. Clear_halt không giúp — cần raw USB reset
             * qua control transfer CLEAR_FEATURE(ENDPOINT_HALT) để reset endpoint
             * toggle state. Thử clear_halt + delay dài hơn.
             */
            LOGI("bulk_write: OVERFLOW ep=0x%02x attempt %d/5 — try clear_halt + long delay",
                 g_ep_out, attempt+1);
            libusb_clear_halt(g_handle, g_ep_out);
            usleep(200 * 1000);  /* delay dài hơn cho OVERFLOW */
            continue;
        }
        if (r == LIBUSB_ERROR_NOT_FOUND || r == LIBUSB_ERROR_NO_DEVICE) {
            /*
             * FIX v35: Handle bị mất — không retry được, return ngay.
             * Caller sẽ thấy error này và trigger USB re-open.
             */
            LOGE("bulk_write: %s ep=0x%02x err=%d — handle mất, không retry",
                 libusb_error_name(r), g_ep_out, r);
            return -1;
        }
        if (r == LIBUSB_ERROR_BUSY) {
            LOGI("bulk_write: BUSY ep=0x%02x attempt %d/5 — chờ 100ms và retry",
                 g_ep_out, attempt+1);
            usleep(100 * 1000);
            continue;
        }
        if (r == LIBUSB_ERROR_ACCESS) {
            LOGE("bulk_write: ACCESS DENIED ep=0x%02x err=%d — thiếu quyền USB Host",
                 g_ep_out, r);
            return -1;
        }
        /* Lỗi khác — log + retry */
        LOGE("bulk_write: ep=0x%02x err=%d (%s) attempt %d/5 — retry",
             g_ep_out, r, libusb_error_name(r), attempt+1);
        last_err = r;
        usleep(80 * 1000);
    }
    LOGE("bulk_write: thất bại sau 5 lần thử, ep=0x%02x, last_err=%d (%s)",
         g_ep_out, last_err, libusb_error_name(last_err));
    return -1;
}

/* ════════════════════════════════════════════════════════════════════════
 * usb_bridge_bulk_read — 5 retry, 80ms delay
 *
 * FIX v35: Log error code CỤ THỂ cho mọi path lỗi (không chỉ PIPE).
 * ════════════════════════════════════════════════════════════════════════ */
int usb_bridge_bulk_read(void *buf, int len, unsigned int timeout) {
    if (!g_handle || !g_ep_in) {
        LOGE("bulk_read: bad state — handle=%p ep_in=0x%02x", (void*)g_handle, g_ep_in);
        return -1;
    }
    int last_err = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        int transferred = 0;
        int r = libusb_bulk_transfer(g_handle, g_ep_in,
                                      (unsigned char *)buf, len,
                                      &transferred, timeout ? timeout : 10000);
        if (r == 0) return transferred;
        if (r == LIBUSB_ERROR_TIMEOUT) {
            /* TIMEOUT = không có data — return 0 (caller sẽ xử lý) */
            return 0;
        }
        if (r == LIBUSB_ERROR_PIPE) {
            LOGI("bulk_read: PIPE ep=0x%02x attempt %d/5 — clear_halt retry",
                 g_ep_in, attempt+1);
            libusb_clear_halt(g_handle, g_ep_in);
            usleep(80 * 1000);
            continue;
        }
        if (r == LIBUSB_ERROR_OVERFLOW) {
            LOGI("bulk_read: OVERFLOW ep=0x%02x attempt %d/5 — buffer quá nhỏ? clear_halt và retry",
                 g_ep_in, attempt+1);
            libusb_clear_halt(g_handle, g_ep_in);
            usleep(200 * 1000);
            continue;
        }
        if (r == LIBUSB_ERROR_NOT_FOUND || r == LIBUSB_ERROR_NO_DEVICE) {
            LOGE("bulk_read: %s ep=0x%02x err=%d — handle mất, không retry",
                 libusb_error_name(r), g_ep_in, r);
            return -1;
        }
        if (r == LIBUSB_ERROR_BUSY) {
            LOGI("bulk_read: BUSY ep=0x%02x attempt %d/5 — chờ 100ms và retry",
                 g_ep_in, attempt+1);
            usleep(100 * 1000);
            continue;
        }
        if (r == LIBUSB_ERROR_ACCESS) {
            LOGE("bulk_read: ACCESS DENIED ep=0x%02x err=%d — thiếu quyền USB Host",
                 g_ep_in, r);
            return -1;
        }
        LOGE("bulk_read: ep=0x%02x err=%d (%s) attempt %d/5 — retry",
             g_ep_in, r, libusb_error_name(r), attempt+1);
        last_err = r;
        usleep(80 * 1000);
    }
    LOGE("bulk_read: thất bại sau 5 lần thử, ep=0x%02x, last_err=%d (%s)",
         g_ep_in, last_err, libusb_error_name(last_err));
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
        if (g_iface_claimed && g_iface_num >= 0) {
            libusb_release_interface(g_handle, g_iface_num);
        }
        g_iface_num = -1;
        g_iface_claimed = 0;
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
