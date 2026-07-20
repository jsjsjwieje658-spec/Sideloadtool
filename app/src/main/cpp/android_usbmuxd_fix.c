/*
 * android_usbmuxd_fix.c  v1.1-FIXED
 *
 * FIX v1.1: Sửa strncpy size trong __wrap_usbmuxd_get_device
 *   - sizeof(cached) có thể > 44 bytes → dùng sizeof(g_fix_udid)
 */

#include <string.h>
#include <stdlib.h>

#include "android_usbmuxd_fix.h"

static char g_fix_udid[44] = "";
static int  g_fix_product_id = 0x12a8;

void android_usbmuxd_fix_set_device(const char *udid, int product_id) {
    if (udid) {
        /* FIX v1.1: Dùng sizeof(g_fix_udid) thay vì sizeof(cached) */
        strncpy(g_fix_udid, udid, sizeof(g_fix_udid) - 1);
        g_fix_udid[sizeof(g_fix_udid) - 1] = '\0';
    }
    g_fix_product_id = product_id;
}

/*
 * __wrap_usbmuxd_get_device — wrapper cho usbmuxd_get_device()
 * Trả về thông tin device đã cache thay vì quét USB.
 */
int __wrap_usbmuxd_get_device(const char *udid, usbmuxd_device_info_t *device,
                              int lookup_options) {
    (void)udid; (void)lookup_options;

    if (!g_fix_udid[0]) return -1;

    memset(device, 0, sizeof(*device));
    device->handle = 1;
    device->product_id = g_fix_product_id;
    /* FIX v1.1: Dùng sizeof(g_fix_udid) để tránh buffer overflow */
    strncpy(device->udid, g_fix_udid, sizeof(g_fix_udid) - 1);
    device->udid[sizeof(g_fix_udid) - 1] = '\0';
    device->conn_type = CONNECTION_TYPE_USB;

    return 1;
}

/*
 * __wrap_usbmuxd_subscribe — wrapper cho usbmuxd_subscribe()
 * Trả về ngay lập tức (không cần subscribe thật).
 */
int __wrap_usbmuxd_subscribe(usbmuxd_event_cb_t callback, void *user_data) {
    (void)callback; (void)user_data;
    return 0;
}

/*
 * __wrap_usbmuxd_unsubscribe — wrapper cho usbmuxd_unsubscribe()
 */
int __wrap_usbmuxd_unsubscribe(void) {
    return 0;
}
