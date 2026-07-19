#pragma once
/*
 * usb_fd_bridge.h — Android USB fd → libusb handle bridge (Mode 1)
 *
 * ════════════════════════════════════════════════════════════════════
 * KIẾN TRÚC HỌC TỪ termux-usbmuxd + termux-api (UsbAPI.java)
 * ════════════════════════════════════════════════════════════════════
 *
 * termux-api (UsbAPI.java) open() pattern:
 *   connection = usbManager.openDevice(device)   // CHỈ open, KHÔNG claim
 *   fd = connection.getFileDescriptor()
 *   openDevices.put(fd, connection)              // giữ alive
 *   return fd → TERMUX_USB_FD → libusb_wrap_sys_device(ctx, fd, &handle)
 *
 * Sau khi libusb wrap fd:
 *   libusb_claim_interface() → SUCCESS (không có Android interference)
 *   Endpoint ở trạng thái SẠCH (không STALL)
 *   → usb_send_version() OK → usb_recv_version() OK → version exchange OK
 *
 * FIX v27 Summary:
 *   - Kotlin không còn claim interface trước (UsbTransport.open() thay đổi)
 *   - libusb_claim_interface() trong discover_apple_endpoints() có thể SUCCESS
 *     hoặc LIBUSB_ERROR_BUSY (cả hai đều được xử lý đúng)
 *   - Proactive libusb_clear_halt() sau discover: bắt buộc
 *   - usb_bridge_clear_endpoints_halt(): có thể gọi lại bất cứ lúc nào
 *
 * LƯU Ý (dọn doc — không đổi hành vi): dòng cũ ở đây từng ghi "rx_seq=0x0000"
 * và các con số VERSION_TIMEOUT/MAX_SKIP của một bản fix đã lỗi thời. Giá trị
 * ĐÚNG và đang chạy thật nằm trong usbmuxd_server.c: rx_seq=0xFFFF (xem "FIX
 * v28 CRITICAL" trong usb_send_version() — 0x0000 đã được xác nhận SAI và gây
 * regression một lần rồi), VERSION_TIMEOUT_MS=3000, MAX_SKIP=30. usb_fd_bridge.h
 * không tự ý lặp lại các con số này nữa để tránh hai nơi lệch nhau — luôn coi
 * usbmuxd_server.c là nguồn sự thật (source of truth) cho các hằng số đó.
 */
#include <stdbool.h>
#include <stdint.h>

/*
 * usb_bridge_init — khởi tạo libusb từ Android USB fd.
 *
 * usb_fd: từ UsbDeviceConnection.getFileDescriptor() (KHÔNG cần claim interface trước)
 *
 * Sau khi init:
 *   1. libusb_wrap_sys_device(ctx, fd, &handle)
 *   2. discover_endpoints() — tìm + claim interface (có thể SUCCESS hoặc BUSY)
 *
 * @return 0 nếu thành công, -1 nếu lỗi.
 */
int usb_bridge_init(int usb_fd);
uint8_t usb_bridge_ep_in(void);
uint8_t usb_bridge_ep_out(void);
int  usb_bridge_bulk_write(const uint8_t *data, int len);
int  usb_bridge_bulk_read(uint8_t *buf, int len);
/*
 * usb_bridge_cleanup — giải phóng libusb handle/context, reset trạng thái
 * nội bộ. Gọi khi đóng kết nối USB.
 */
void usb_bridge_cleanup(void);
/*
 * usb_bridge_clear_endpoints_halt — clear halt trên cả ep_in và ep_out.
 * Gọi trước version exchange hoặc khi gặp nhiều lỗi PIPE liên tiếp.
 */
bool usb_bridge_clear_endpoints_halt(void);
