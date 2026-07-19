/*
 * android_usbmuxd_fix.h
 * Header cho Android usbmuxd fix
 */

#ifndef ANDROID_USBMUXD_FIX_H
#define ANDROID_USBMUXD_FIX_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Gửi log về Java */
void android_usbmuxd_fix_log(const char *msg);

/* Thông báo sự kiện về Java */
void android_usbmuxd_fix_notify(int event, const char *udid);

/* Lưu thông tin device */
void android_usbmuxd_fix_set_device(const char *udid, int product_id);

#ifdef __cplusplus
}
#endif

#endif /* ANDROID_USBMUXD_FIX_H */
