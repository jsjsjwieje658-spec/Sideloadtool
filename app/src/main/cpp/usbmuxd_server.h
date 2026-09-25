#pragma once
/*
 * usbmuxd_server.h — usbmuxd chạy trong tiến trình app (port giao thức của
 * libimobiledevice/usbmuxd: device.c + client.c + conf.c).
 *
 * Dùng:
 *   1. usb_bridge_init_from_fd2(...)                 (USB sẵn sàng)
 *   2. usbmuxd_server_start(filesDir, udid, pid)     (gửi VERSION tới iPhone)
 *   3. setenv("USBMUXD_SOCKET_ADDRESS", usbmuxd_server_socket_address(), 1)
 *      → dạng "UNIX:/…/usbmuxd.sock" mà libusbmuxd 2.0.2 hiểu (đường dẫn trần
 *        KHÔNG có "UNIX:" sẽ bị libusbmuxd bỏ qua → rơi về /var/run/usbmuxd).
 *   4. usbmuxd_server_device_ready(timeout)          (chờ bắt tay mux xong)
 *   5. libimobiledevice dùng như trên PC (pair record lưu ở filesDir/lockdown)
 *   6. usbmuxd_server_stop() trước usb_bridge_close()
 */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool usbmuxd_server_start(const char *files_dir, const char *udid, int product_id);
void usbmuxd_server_stop(void);

/* Chờ tới khi iPhone trả lời VERSION (thiết bị ACTIVE). */
bool usbmuxd_server_device_ready(int timeout_ms);
/* 0 = dừng, 1 = đang bắt tay, 2 = sẵn sàng, 3 = mất kết nối */
int  usbmuxd_server_device_state(void);

void usbmuxd_server_update_udid(const char *udid);
const char *usbmuxd_server_socket_path(void);      /* đường dẫn trần, hoặc NULL */
const char *usbmuxd_server_socket_address(void);   /* "UNIX:<path>", hoặc NULL  */
const char *usbmuxd_server_config_dir(void);       /* <filesDir>/lockdown       */

/* Tương thích ngược với code cũ. */
bool usbmux_version_exchange(void);                /* = device_ready(15000) */
void usbmuxd_server_reset_version_state(void);     /* không còn tác dụng    */

#ifdef __cplusplus
}
#endif
