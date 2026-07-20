/*
 * usb_fd_bridge.c  v13-FIXED
 * Android USB fd → libusb handle + endpoint discovery — FIXED header path
 *
 * FIX v13: Sửa include path libusb.h để biên dịch đúng trên Android NDK
 *   - CMakeLists.txt đã thêm ${PREBUILT_INC}/libusb-1.0 vào include path
 *   - Code cũ include <libusb-1.0/libusb.h> → compiler tìm ở sai path
 *   - Fix: đổi thành #include <libusb.h>
 */

/* FIX v13: CMakeLists.txt đã thêm ${PREBUILT_INC}/libusb-1.0 vào include path
 * nên chỉ cần #include <libusb.h> thay vì <libusb-1.0/libusb.h> */
#include <libusb.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <android/log.h>

#include "usb_fd_bridge.h"

#define TAG "usb_bridge"
#define LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO, TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) __android_log_print(ANDROID_LOG_WARN, TAG, fmt, ##__VA_ARGS__)

static libusb_context       *g_ctx = NULL;
static libusb_device_handle *g_handle = NULL;
static uint8_t               g_ep_in = 0;
static uint8_t               g_ep_out = 0;
static pthread_mutex_t       g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Apple vendor-specific interface class */
#define APPLE_VENDOR_CLASS  0xFF
#define APPLE_VENDOR_SUBCLASS 0xF0
#define APPLE_VENDOR_PROTOCOL 0x00

static int discover_endpoints(libusb_device_handle *h) {
    struct libusb_config_descriptor *cfg = NULL;
    int r = libusb_get_active_config_descriptor(libusb_get_device(h), &cfg);
    if (r < 0) {
        LOGE("discover: get_config_descriptor err=%d", r);
        return -1;
    }

    g_ep_in = 0;
    g_ep_out = 0;

    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *iface = &cfg->interface[i];
        for (int a = 0; a < iface->num_altsetting; a++) {
            const struct libusb_interface_descriptor *alt = &iface->altsetting[a];
            if (alt->bInterfaceClass != APPLE_VENDOR_CLASS ||
                alt->bInterfaceSubClass != APPLE_VENDOR_SUBCLASS ||
                alt->bInterfaceProtocol != APPLE_VENDOR_PROTOCOL)
                continue;

            for (int e = 0; e < alt->bNumEndpoints; e++) {
                uint8_t addr = alt->endpoint[e].bEndpointAddress;
                if ((addr & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN)
                    g_ep_in = addr;
                else
                    g_ep_out = addr;
            }

            int cr = libusb_claim_interface(h, i);
            if (cr == 0) {
                LOGI("claimed interface %d (in=0x%02x out=0x%02x)",
                     i, g_ep_in, g_ep_out);
                libusb_free_config_descriptor(cfg);
                return 0;
            } else if (cr == LIBUSB_ERROR_BUSY) {
                LOGW("interface %d busy, retrying...", i);
                usleep(100000);
                cr = libusb_claim_interface(h, i);
                if (cr == 0) {
                    LOGI("claimed interface %d after retry", i);
                    libusb_free_config_descriptor(cfg);
                    return 0;
                }
            } else if (cr == LIBUSB_ERROR_NOT_SUPPORTED) {
                LOGW("interface %d not supported", i);
            } else {
                LOGW("discover: claim err=%d (%s)", cr, libusb_error_name(cr));
            }
        }
    }

    libusb_free_config_descriptor(cfg);
    LOGE("discover: no suitable interface found");
    return -1;
}

/* ════════════════════════════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════════════════════════════ */
int usb_bridge_init_from_fd(int usb_fd) {
    pthread_mutex_lock(&g_lock);

    if (g_handle) {
        LOGI("already initialized");
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    int r = libusb_init(&g_ctx);
    if (r < 0) {
        LOGE("init: libusb_init err=%d", r);
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    r = libusb_wrap_sys_device(g_ctx, (intptr_t)usb_fd, &g_handle);
    if (r < 0) {
        LOGE("init: libusb_wrap_sys_device err=%d (%s)", r, libusb_error_name(r));
        libusb_exit(g_ctx);
        g_ctx = NULL;
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    if (discover_endpoints(g_handle) < 0) {
        libusb_close(g_handle);
        g_handle = NULL;
        libusb_exit(g_ctx);
        g_ctx = NULL;
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    LOGI("USB bridge init OK (fd=%d)", usb_fd);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void usb_bridge_close(void) {
    pthread_mutex_lock(&g_lock);
    if (g_handle) {
        libusb_release_interface(g_handle, 0);
        libusb_close(g_handle);
        g_handle = NULL;
    }
    if (g_ctx) {
        libusb_exit(g_ctx);
        g_ctx = NULL;
    }
    g_ep_in = 0;
    g_ep_out = 0;
    pthread_mutex_unlock(&g_lock);
    LOGI("USB bridge closed");
}

int usb_bridge_bulk_write(const void *buf, int len, unsigned int timeout) {
    pthread_mutex_lock(&g_lock);
    if (!g_handle || !g_ep_out) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    int transferred = 0;
    int r = libusb_bulk_transfer(g_handle, g_ep_out, (unsigned char*)buf, len,
                                 &transferred, timeout);
    if (r == LIBUSB_ERROR_PIPE) {
        LOGW("bulk_write: PIPE error, clearing halt");
        libusb_clear_halt(g_handle, g_ep_out);
        r = libusb_bulk_transfer(g_handle, g_ep_out, (unsigned char*)buf, len,
                                 &transferred, timeout);
    }

    pthread_mutex_unlock(&g_lock);

    if (r < 0 && r != LIBUSB_ERROR_TIMEOUT) {
        LOGE("bulk_write: err=%d (%s)", r, libusb_error_name(r));
        return -1;
    }
    return transferred;
}

int usb_bridge_bulk_read(void *buf, int len, unsigned int timeout) {
    pthread_mutex_lock(&g_lock);
    if (!g_handle || !g_ep_in) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    int transferred = 0;
    int attempt;
    for (attempt = 0; attempt < 5; attempt++) {
        int r = libusb_bulk_transfer(g_handle, g_ep_in, (unsigned char*)buf, len,
                                     &transferred, timeout);
        if (r == 0) break;
        if (r == LIBUSB_ERROR_PIPE) {
            LOGW("bulk_read: PIPE error, clearing halt (attempt %d)", attempt + 1);
            libusb_clear_halt(g_handle, g_ep_in);
            continue;
        }
        if (r == LIBUSB_ERROR_OVERFLOW) {
            LOGW("bulk_read: OVERFLOW (attempt %d)", attempt + 1);
            transferred = len;
            break;
        }
        if (r == LIBUSB_ERROR_TIMEOUT) {
            /* Timeout is OK for non-blocking reads */
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        LOGE("bulk_read: err=%d (%s) attempt=%d/5", r, libusb_error_name(r), attempt + 1);
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    pthread_mutex_unlock(&g_lock);
    return transferred;
}

int usb_bridge_ep_in(void)  { return g_ep_in; }
int usb_bridge_ep_out(void) { return g_ep_out; }
