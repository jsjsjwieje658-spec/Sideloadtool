/*
 * usb_fd_bridge.c
 * Android USB bridge qua libusb với fd từ UsbDeviceConnection
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb.h>          /* libusb_context, libusb_device_handle, struct
                                 libusb_config_descriptor, LIBUSB_ERROR_*, ...
                                 — trước đây bị thiếu dù CMakeLists.txt đã có
                                 sẵn include path prebuilt/<abi>/include/libusb-1.0 */
#include "usb_fd_bridge.h"
#include "android_usbmuxd_fix.h"

#define TAG "USB_FD"

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

static libusb_context       *g_ctx = NULL;
static libusb_device_handle *g_handle = NULL;
static uint8_t  g_ep_in  = 0;
static uint8_t  g_ep_out = 0;
static int      g_initialized = 0;
static int      g_usb_fd = -1;

uint8_t usb_bridge_ep_in(void)  { return g_ep_in; }
uint8_t usb_bridge_ep_out(void) { return g_ep_out; }

static int discover_endpoints(libusb_device_handle *h) {
    struct libusb_config_descriptor *cfg = NULL;
    int r = libusb_get_active_config_descriptor(libusb_get_device(h), &cfg);
    if (r < 0) {
        LOGE("discover: libusb_get_active_config_descriptor err=%d", r);
        return -1;
    }
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *iface = &cfg->interface[i];
        for (int a = 0; a < iface->num_altsetting; a++) {
            const struct libusb_interface_descriptor *alt = &iface->altsetting[a];
            if (alt->bInterfaceClass != 0xFF || alt->bInterfaceSubClass != 0xF0 || alt->bInterfaceProtocol != 0x00)
                continue;
            uint8_t ep_in = 0, ep_out = 0;
            for (int e = 0; e < alt->bNumEndpoints; e++) {
                uint8_t addr = alt->endpoint[e].bEndpointAddress;
                if ((addr & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN)
                    ep_in = addr;
                else
                    ep_out = addr;
            }
            if (ep_in && ep_out) {
                LOGI("discover: iface=%d alt=%d ep_in=0x%02x ep_out=0x%02x",
                     i, a, ep_in, ep_out);
                int cr = libusb_claim_interface(h, i);
                if (cr == 0) {
                    g_ep_in = ep_in;
                    g_ep_out = ep_out;
                    libusb_free_config_descriptor(cfg);
                    LOGI("discover: interface claimed");
                    return 0;
                } else if (cr == LIBUSB_ERROR_BUSY) {
                    LOGI("discover: interface BUSY — dùng shared fd (OK)");
                    g_ep_in = ep_in;
                    g_ep_out = ep_out;
                    libusb_free_config_descriptor(cfg);
                    return 0;
                } else if (cr == LIBUSB_ERROR_NOT_SUPPORTED) {
                    LOGI("discover: NOT_SUPPORTED — tiếp tục");
                    continue;
                } else {
                    LOGW("discover: claim err=%d (%s)", cr, libusb_error_name(cr));
                }
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    LOGE("discover: không tìm thấy Apple interface");
    return -1;
}

int usb_bridge_init(int usb_fd) {
    if (g_initialized) {
        LOGI("init: already initialized");
        return 0;
    }
    g_usb_fd = usb_fd;
    int r = libusb_init(&g_ctx);
    if (r < 0) {
        LOGE("init: libusb_init err=%d", r);
        return -1;
    }
    r = libusb_wrap_sys_device(g_ctx, (intptr_t)usb_fd, &g_handle);
    if (r < 0) {
        LOGE("init: libusb_wrap_sys_device err=%d (%s)", r, libusb_error_name(r));
        libusb_exit(g_ctx);
        g_ctx = NULL;
        return -1;
    }
    LOGI("init: libusb_wrap_sys_device OK");
    r = discover_endpoints(g_handle);
    if (r < 0) {
        libusb_close(g_handle);
        g_handle = NULL;
        libusb_exit(g_ctx);
        g_ctx = NULL;
        return -1;
    }
    g_initialized = 1;
    LOGI("init: USB bridge ready");
    return 0;
}

void usb_bridge_cleanup(void) {
    if (!g_initialized) return;
    if (g_handle) {
        libusb_release_interface(g_handle, 0);
        libusb_close(g_handle);
        g_handle = NULL;
    }
    if (g_ctx) {
        libusb_exit(g_ctx);
        g_ctx = NULL;
    }
    g_initialized = 0;
    g_ep_in = 0;
    g_ep_out = 0;
    g_usb_fd = -1;
    LOGI("cleanup: done");
}

int usb_bridge_bulk_read(uint8_t *buf, int len) {
    if (!g_initialized || !g_handle) return -1;
    int transferred = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        int r = libusb_bulk_transfer(g_handle, g_ep_in, buf, len, &transferred, 5000);
        if (r == 0) return transferred;
        if (r == LIBUSB_ERROR_PIPE) {
            LOGW("bulk_read: PIPE — clear_halt + retry (%d/5)", attempt + 1);
            libusb_clear_halt(g_handle, g_ep_in);
            usleep(10000);
            continue;
        }
        if (r == LIBUSB_ERROR_OVERFLOW) {
            LOGW("bulk_read: OVERFLOW — draining (%d/5)", attempt + 1);
            uint8_t drain[4096];
            int dtrans;
            libusb_bulk_transfer(g_handle, g_ep_in, drain, sizeof(drain), &dtrans, 100);
            usleep(5000);
            continue;
        }
        LOGE("bulk_read: err=%d (%s) attempt=%d/5", r, libusb_error_name(r), attempt + 1);
        return -1;
    }
    return -1;
}

int usb_bridge_bulk_write(const uint8_t *data, int len) {
    if (!g_initialized || !g_handle) return -1;
    int transferred = 0;
    int r = libusb_bulk_transfer(g_handle, g_ep_out, (unsigned char *)data, len, &transferred, 5000);
    if (r == 0) return transferred;
    if (r == LIBUSB_ERROR_PIPE) {
        LOGW("bulk_write: PIPE — clear_halt");
        libusb_clear_halt(g_handle, g_ep_out);
        r = libusb_bulk_transfer(g_handle, g_ep_out, (unsigned char *)data, len, &transferred, 5000);
        if (r == 0) return transferred;
    }
    LOGE("bulk_write: err=%d (%s)", r, libusb_error_name(r));
    return -1;
}

bool usb_bridge_clear_endpoints_halt(void) {
    if (!g_initialized || !g_handle) return false;
    int r1 = libusb_clear_halt(g_handle, g_ep_in);
    int r2 = libusb_clear_halt(g_handle, g_ep_out);
    LOGI("clear_halt: in=%d out=%d", r1, r2);
    return (r1 == 0 && r2 == 0);
}
