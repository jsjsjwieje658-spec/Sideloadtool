#pragma once
/*
 * usb_fd_bridge.h — fd USB của Android (UsbDeviceConnection) → libusb.
 *
 * Mô hình giống termux-usb + usbmuxd upstream:
 *   UsbManager.openDevice(dev).getFileDescriptor()  (Kotlin, KHÔNG claim)
 *   → libusb_wrap_sys_device(fd)
 *   → chọn configuration có interface usbmux (0xFF/0xFE/2) — set nếu cần
 *   → libusb_claim_interface → bulk I/O (ZLP khi cần)
 * Nếu libusb không claim được, dùng UsbDeviceConnection.bulkTransfer() qua JNI.
 */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Khởi tạo từ fd. ep_in/ep_out/iface_num (từ Kotlin) chỉ dùng khi descriptor
 * không liệt kê được interface usbmux; truyền 0/0/-1 nếu không có. */
bool usb_bridge_init_from_fd2(int fd, int vendor_id, int product_id,
                              int ep_in, int ep_out, int iface_num);
bool usb_bridge_init_from_fd(int fd, int vendor_id, int product_id);

uint8_t usb_bridge_ep_in(void);
uint8_t usb_bridge_ep_out(void);
int  usb_bridge_max_packet_size(void);
int  usb_bridge_active_config(void);
int  usb_bridge_interface(void);
/* iSerialNumber của iPhone (= UDID, đã chèn '-' cho UDID 24 ký tự), hoặc NULL. */
const char *usb_bridge_serial(void);

/* Ghi trọn một gói (tự gửi ZLP). Trả len khi thành công, <0 khi lỗi. */
int  usb_bridge_bulk_write(const void *buf, int len, unsigned int timeout);
/* Đọc một transfer. 0 = timeout, <0 = lỗi. */
int  usb_bridge_bulk_read(void *buf, int len, unsigned int timeout);
/* Như trên; *completed = 0 khi transfer bị cắt vì timeout (còn dữ liệu sau). */
int  usb_bridge_bulk_read_ex(void *buf, int len, unsigned int timeout, int *completed);

void usb_bridge_flush_in(int max_packets, int timeout_ms);
void usb_bridge_close(void);
bool usb_bridge_clear_endpoints_halt(void);

/* Dự phòng: bulk I/O qua NativeBridge.onNativeBulkWrite/Read (Java). */
bool usb_bridge_set_android_mode(void);
bool usb_bridge_using_android_mode(void);
bool usb_bridge_iface_claimed(void);

void usb_bridge_set_jvm(void *vm);
void usb_bridge_set_bridge_ref(void *bridge_obj);

#ifdef __cplusplus
}
#endif
