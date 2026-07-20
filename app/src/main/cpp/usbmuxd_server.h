/*
 * usbmuxd_server.h
 * Public API cho mini usbmuxd server
 */

#ifndef USBMUXD_SERVER_H
#define USBMUXD_SERVER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Khởi động usbmuxd server.
 * @param files_dir  Thư mục files của app (để tạo Unix socket)
 * @param udid       UDID của iPhone (có thể NULL)
 * @param product_id Product ID của iPhone (ví dụ: 0x12a8)
 * @return true nếu thành công
 */
bool usbmuxd_server_start(const char *files_dir, const char *udid, int product_id);

/*
 * Cập nhật UDID và broadcast Attached event
 */
void usbmuxd_server_update_udid(const char *udid);

/*
 * Broadcast Attached event đến tất cả listening clients
 */
void usbmuxd_server_broadcast_attached(void);

/*
 * Lấy đường dẫn socket đang listen
 * @return NULL nếu server chưa chạy
 */
const char *usbmuxd_server_socket_path(void);

/*
 * Dừng server và dọn dẹp
 */
void usbmuxd_server_stop(void);

/*
 * Reset trạng thái version exchange (gọi khi reconnect USB)
 */
void usbmuxd_server_reset_version_state(void);

/*
 * Thực hiện version exchange với iPhone
 * @return true nếu thành công
 */
bool usbmux_version_exchange(void);

#ifdef __cplusplus
}
#endif

#endif /* USBMUXD_SERVER_H */
