/*
 * usb_fd_bridge.c
 *
 * FIX v34 (COMPLETE): 
 *   - Không retry claim khi BUSY (Android đã claim)
 *   - Thêm OVERFLOW handling với drain + retry
 *   - Thêm flush_in/flush_out helpers
 */

#include "usb_fd_bridge.h"
#include "android_usbmuxd_fix.h"
#include <libusb-1.0/libusb.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>

#define LOG_TAG "usb_bridge"

static libusb_context       *g_ctx = NULL;
static libusb_device_handle *g_handle = NULL;
static int                   g_fd = -1;
static uint8_t               g_ep_in = 0;
static uint8_t               g_ep_out = 0;
static int                   g_iface = -1;
static int                   g_altsetting = -1;
static pthread_mutex_t       g_write_lock = PTHREAD_MUTEX_INITIALIZER;

/* ════════════════════════════════════════════════════════════════════════
 * Logging
 * ════════════════════════════════════════════════════════════════════════ */
#define LOGE(fmt, ...) do { \
    char _buf[256]; \
    snprintf(_buf, sizeof(_buf), "[usb] " fmt, ##__VA_ARGS__); \
    android_usbmuxd_fix_log(_buf); \
} while(0)

#define LOGI(fmt, ...) do { \
    char _buf[256]; \
    snprintf(_buf, sizeof(_buf), "[usb] " fmt, ##__VA_ARGS__); \
    android_usbmuxd_fix_log(_buf); \
} while(0)

#define LOGW(fmt, ...) do { \
    char _buf[256]; \
    snprintf(_buf, sizeof(_buf), "[usb] " fmt, ##__VA_ARGS__); \
    android_usbmuxd_fix_log(_buf); \
} while(0)

/* ════════════════════════════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════════════════════════════ */
int  usb_bridge_ep_in(void)  { return g_ep_in; }
int  usb_bridge_ep_out(void) { return g_ep_out; }

/* ════════════════════════════════════════════════════════════════════════
 * Discover Apple endpoints
 * ════════════════════════════════════════════════════════════════════════ */
static int discover_apple_endpoints(void) {
    struct libusb_config_descriptor *cfg = NULL;
    int r = libusb_get_active_config_descriptor(libusb_get_device(g_handle), &cfg);
    if (r < 0) {
        LOGE("discover: libusb_get_active_config_descriptor err=%d", r);
        return -1;
    }

    for (uint8_t i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *iface = &cfg->interface[i];
        for (int a = 0; a < iface->num_altsetting; a++) {
            const struct libusb_interface_descriptor *alt = &iface->altsetting[a];
            if (alt->bInterfaceClass != 0xFF || alt->bInterfaceSubClass != 0xFE) continue;

            uint8_t ep_in = 0, ep_out = 0;
            for (uint8_t e = 0; e < alt->bNumEndpoints; e++) {
                uint8_t addr = alt->endpoint[e].bEndpointAddress;
                if ((addr & 0x80) && !ep_in) ep_in = addr;
                if (!(addr & 0x80) && !ep_out) ep_out = addr;
            }
            if (!ep_in || !ep_out) continue;

            g_ep_in  = ep_in;
            g_ep_out = ep_out;
            g_iface = alt->bInterfaceNumber;
            g_altsetting = alt->bAlternateSetting;

            LOGI("discover: found iface=%d alt=%d ep_in=0x%02x ep_out=0x%02x",
                 g_iface, g_altsetting, g_ep_in, g_ep_out);

            /* 
             * FIX v34: Android UsbDeviceConnection đã claim interface.
             * libusb_claim_interface() sẽ trả BUSY. Không retry — dùng shared fd.
             */
            int cr = libusb_claim_interface(g_handle, g_iface);
            if (cr == 0) {
                LOGI("discover: ✅ interface claimed");
            } else if (cr == LIBUSB_ERROR_BUSY) {
                LOGI("discover: interface BUSY — Android đã claim, dùng shared fd (OK)");
            } else if (cr == LIBUSB_ERROR_NOT_SUPPORTED) {
                LOGI("discover: NOT_SUPPORTED — tiếp tục");
            } else {
                LOGW("discover: claim err=%d (%s)", cr, libusb_error_name(cr));
            }

            libusb_free_config_descriptor(cfg);
            return 0;
        }
    }

    libusb_free_config_descriptor(cfg);
    LOGE("discover: không tìm thấy Apple interface");
    return -1;
}

/* ════════════════════════════════════════════════════════════════════════
 * Init / Cleanup
 * ════════════════════════════════════════════════════════════════════════ */
int usb_bridge_init(int fd) {
    if (g_handle) {
        LOGI("init: already initialized");
        return 0;
    }

    g_fd = fd;

    int r = libusb_init(&g_ctx);
    if (r < 0) {
        LOGE("init: libusb_init err=%d", r);
        return -1;
    }

    r = libusb_wrap_sys_device(g_ctx, (intptr_t)fd, &g_handle);
    if (r < 0) {
        LOGE("init: libusb_wrap_sys_device err=%d (%s)", r, libusb_error_name(r));
        libusb_exit(g_ctx);
        g_ctx = NULL;
        return -1;
    }

    LOGI("init: libusb_wrap_sys_device OK");

    if (discover_apple_endpoints() < 0) {
        libusb_close(g_handle);
        g_handle = NULL;
        libusb_exit(g_ctx);
        g_ctx = NULL;
        return -1;
    }

    /* Clear halt */
    libusb_clear_halt(g_handle, g_ep_in);
    libusb_clear_halt(g_handle, g_ep_out);

    LOGI("init: ✅ USB bridge ready (ep_in=0x%02x ep_out=0x%02x)", g_ep_in, g_ep_out);
    return 0;
}

void usb_bridge_cleanup(void) {
    if (g_handle) {
        if (g_iface >= 0) {
            libusb_release_interface(g_handle, g_iface);
        }
        libusb_close(g_handle);
        g_handle = NULL;
    }
    if (g_ctx) {
        libusb_exit(g_ctx);
        g_ctx = NULL;
    }
    g_fd = -1;
    g_ep_in = 0;
    g_ep_out = 0;
    g_iface = -1;
    LOGI("cleanup: done");
}

/* ════════════════════════════════════════════════════════════════════════
 * FIX v34: Bulk read với OVERFLOW handling
 * ════════════════════════════════════════════════════════════════════════ */
int usb_bridge_bulk_read(void *buf, int len, unsigned int timeout) {
    if (!g_handle || !g_ep_in) return -1;

    for (int attempt = 0; attempt < 5; attempt++) {
        int transferred = 0;
        int r = libusb_bulk_transfer(g_handle, g_ep_in,
                                      (unsigned char *)buf, len,
                                      &transferred, timeout);
        if (r == 0) {
            if (transferred > 0) return transferred;
            /* transferred == 0, retry */
            usleep(1000);
            continue;
        }

        if (r == LIBUSB_ERROR_PIPE) {
            LOGW("bulk_read: PIPE — clear_halt + retry (%d/5)", attempt + 1);
            libusb_clear_halt(g_handle, g_ep_in);
            usleep(80 * 1000);
            continue;
        }

        if (r == LIBUSB_ERROR_OVERFLOW) {
            /* 
             * FIX v34: OVERFLOW — buffer nhỏ hơn packet.
             * Drain stale data và retry.
             */
            LOGW("bulk_read: OVERFLOW — draining stale data (%d/5)", attempt + 1);
            usb_bridge_flush_in(8, 200);
            usleep(100 * 1000);
            continue;
        }

        if (r == LIBUSB_ERROR_TIMEOUT) {
            return -1;  /* Timeout là normal */
        }

        LOGE("bulk_read: err=%d (%s) attempt=%d/5", r, libusb_error_name(r), attempt + 1);
        usleep(80 * 1000);
    }
    return -1;
}

int usb_bridge_bulk_write(const void *buf, int len, unsigned int timeout) {
    if (!g_handle || !g_ep_out) return -1;

    pthread_mutex_lock(&g_write_lock);
    int transferred = 0;
    int r = libusb_bulk_transfer(g_handle, g_ep_out,
                                  (unsigned char *)buf, len,
                                  &transferred, timeout);
    pthread_mutex_unlock(&g_write_lock);

    if (r == 0) return transferred;

    if (r == LIBUSB_ERROR_PIPE) {
        LOGW("bulk_write: PIPE — clear_halt");
        libusb_clear_halt(g_handle, g_ep_out);
    }

    LOGE("bulk_write: err=%d (%s)", r, libusb_error_name(r));
    return -1;
}

/* ════════════════════════════════════════════════════════════════════════
 * Flush helpers
 * ════════════════════════════════════════════════════════════════════════ */
void usb_bridge_flush_in(int max_packets, int timeout_ms) {
    if (!g_handle || !g_ep_in) return;
    uint8_t drain[4096];
    for (int i = 0; i < max_packets; i++) {
        int transferred = 0;
        int r = libusb_bulk_transfer(g_handle, g_ep_in, drain, sizeof(drain),
                                      &transferred, timeout_ms);
        if (r != 0 || transferred == 0) break;
    }
}

void usb_bridge_flush_out(int timeout_ms) {
    if (!g_handle || !g_ep_out) return;
    /* Không có cách dễ dàng để flush OUT endpoint */
    (void)timeout_ms;
}

void usb_bridge_clear_endpoints_halt(void) {
    if (g_handle) {
        if (g_ep_in)  libusb_clear_halt(g_handle, g_ep_in);
        if (g_ep_out) libusb_clear_halt(g_handle, g_ep_out);
    }
}
