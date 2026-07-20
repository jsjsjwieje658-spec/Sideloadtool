/* android_usbmuxd_fix.h */
#ifndef ANDROID_USBMUXD_FIX_H
#define ANDROID_USBMUXD_FIX_H

#ifdef __cplusplus
extern "C" {
#endif

void android_fix_set_device(const char *udid, int product_id);

/* Android log helper used by LOGI/LOGE/LOGW macros in usb_fd_bridge.c
 * and usbmuxd_server.c. Forwards to __android_log_print. */
void android_usbmuxd_fix_log(const char *msg);

#ifdef __cplusplus
}
#endif

#endif
