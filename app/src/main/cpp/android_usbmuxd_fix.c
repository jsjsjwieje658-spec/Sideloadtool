/* android_usbmuxd_fix.c — tiện ích log dùng chung cho tầng native.
 *
 * v49: BỎ hai hàm __wrap_usbmuxd_get_device / __wrap_usbmuxd_connect (và cờ
 * -Wl,--wrap trong CMakeLists.txt). Chúng từng bắt libimobiledevice đi đường
 * vòng: get_device luôn trả một thiết bị giả "pending-device" không qua
 * usbmuxd, còn connect tự viết lại gói Connect. Trong khi đó các hàm KHÁC của
 * libusbmuxd (ReadBUID, ReadPairRecord, SavePairRecord, ListDevices) vẫn đi
 * đường chuẩn và đọc USBMUXD_SOCKET_ADDRESS — nhưng biến này lại bị đặt thành
 * đường dẫn trần (không có tiền tố "UNIX:") nên libusbmuxd 2.0.2 bỏ qua và nối
 * tới /var/run/usbmuxd (không tồn tại trên Android) → không đọc được BUID,
 * không lưu/đọc được pair record → pair xong vẫn như chưa pair, không mở được
 * phiên SSL, không StartService được.
 *
 * Nay usbmuxd_server.c nói đúng giao thức usbmuxd upstream và địa chỉ socket là
 * "UNIX:<filesDir>/usbmuxd.sock", nên libusbmuxd/libimobiledevice chạy NGUYÊN
 * BẢN, giống hệt trên Termux/PC.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "android_usbmuxd_fix.h"

static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static android_usbmuxd_log_callback_t g_log_callback = NULL;

void android_usbmuxd_fix_set_log_callback(android_usbmuxd_log_callback_t callback)
{
    pthread_mutex_lock(&g_log_mutex);
    g_log_callback = callback;
    pthread_mutex_unlock(&g_log_mutex);
}

void android_usbmuxd_fix_log(const char *msg)
{
    if (!msg) return;
    pthread_mutex_lock(&g_log_mutex);
    android_usbmuxd_log_callback_t callback = g_log_callback;
    pthread_mutex_unlock(&g_log_mutex);
    if (callback) callback(msg);
}

void android_usbmuxd_fix_logf(const char *fmt, ...)
{
    if (!fmt) return;
    char msg[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    android_usbmuxd_fix_log(msg);
}

/* Giữ lại để tương thích ABI; UDID giờ do usbmuxd_server quản lý. */
void android_fix_set_device(const char *udid, int product_id)
{
    (void)udid;
    (void)product_id;
}

void android_usbmuxd_fix_set_device(const char *udid, int product_id)
{
    android_fix_set_device(udid, product_id);
}
