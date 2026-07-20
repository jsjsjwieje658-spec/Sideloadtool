/*
 * usbmuxd_server.h
 *
 * Copyright (C) 2024 SideloadTool
 *
 * FIX v34 (COMPLETE): Thêm packet reassembly, connection state machine,
 * periodic ACK, và window management.
 */

#ifndef USBMUXD_SERVER_H
#define USBMUXD_SERVER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Khởi động/dừng usbmuxd server thread */
bool usbmuxd_server_start(const char *sock_dir);
void usbmuxd_server_stop(void);

/* Trả về đường dẫn socket hiện tại (NULL nếu chưa start) */
const char *usbmuxd_server_socket_path(void);

/* Cập nhật UDID khi đã biết (từ JNI bridge) */
void usbmuxd_server_update_udid(const char *udid);

/* Broadcast Attached event cho tất cả listeners */
void usbmuxd_server_broadcast_attached(void);

/*
 * FIX v34: Thêm device state machine
 */
typedef enum {
    DEV_STATE_INIT,      /* Đang version exchange */
    DEV_STATE_ACTIVE,    /* Version OK, SETUP sent, sẵn sàng */
    DEV_STATE_DEAD       /* Lỗi, không dùng được */
} device_state_t;

device_state_t usbmuxd_server_device_state(void);

#ifdef __cplusplus
}
#endif

#endif /* USBMUXD_SERVER_H */
