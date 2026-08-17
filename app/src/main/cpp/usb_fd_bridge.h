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
 *   - VERSION_TIMEOUT tăng lên 12000ms, MAX_SKIP tăng lên 20
 *   - rx_seq=0x0000 (đúng spec usbmuxd thật — tag=0)
 */
#include <stdbool.h>
#include <stdint.h>

/*
 * usb_bridge_init_from_fd — khởi tạo libusb từ Android USB fd.
 *
 * fd: từ UsbDeviceConnection.getFileDescriptor() (KHÔNG cần claim interface trước)
 *
 * Sau khi init:
 *   1. libusb_wrap_sys_device(ctx, fd, &handle)
 *   2. discover_apple_endpoints() — tìm + claim interface (có thể SUCCESS hoặc BUSY)
 *   3. libusb_clear_halt() trên cả ep_in và ep_out (bắt buộc)
 */
bool usb_bridge_init_from_fd(int fd, int vendor_id, int product_id);

/*
 * FIX v37: usb_bridge_init_from_fd2 — nhận endpoint addresses + interface id
 * trực tiếp từ Kotlin (đã discover qua UsbInterface API).
 *
 * Lý do: khi dùng libusb_wrap_sys_device(fd) với Android fd, hàm
 * libusb_get_active_config_descriptor() thường fail (descriptor access
 * không đáng tin với wrapped sys device). Khi đó discover_apple_endpoints()
 * fallback về endpoint mặc định (0x85/0x04) NHƯNG không gọi
 * libusb_claim_interface() → mọi bulk_transfer fail với LIBUSB_ERROR_IO.
 *
 * Giải pháp: Kotlin đã có endpoint addresses + interface number từ
 * UsbInterface/UsbEndpoint API. Truyền thẳng xuống native → native
 * gọi libusb_claim_interface(iface_num) trực tiếp, bỏ qua discovery.
 *
 * @param fd          Android USB fd (từ UsbDeviceConnection.getFileDescriptor())
 * @param vendor_id   USB vendor ID (vd: 0x05ac cho Apple)
 * @param product_id  USB product ID (vd: 0x12a8 cho iPhone)
 * @param ep_in       Endpoint IN address (vd: 0x85) — 0 nếu không biết
 * @param ep_out      Endpoint OUT address (vd: 0x04) — 0 nếu không biết
 * @param iface_num   Interface number (vd: 0) — -1 nếu không biết
 * @return true nếu init thành công, false nếu thất bại
 */
bool usb_bridge_init_from_fd2(int fd, int vendor_id, int product_id,
                              int ep_in, int ep_out, int iface_num);

uint8_t usb_bridge_ep_in(void);
uint8_t usb_bridge_ep_out(void);

int  usb_bridge_bulk_write(const void *buf, int len, unsigned int timeout);
int  usb_bridge_bulk_read(void *buf, int len, unsigned int timeout);
void usb_bridge_flush_in(int max_packets, int timeout_ms);
void usb_bridge_close(void);

/*
 * usb_bridge_clear_endpoints_halt — clear halt trên cả ep_in và ep_out.
 * Gọi trước version exchange hoặc khi gặp nhiều lỗi PIPE liên tiếp.
 */
bool usb_bridge_clear_endpoints_halt(void);

/*
 * FIX v38: usb_bridge_set_android_mode — switch sang Android bulk transport.
 *
 * Khi libusb_claim_interface() fail với NOT_FOUND (thường gặp trên Android
 * với wrapped fd), libusb_bulk_transfer() cũng sẽ fail với LIBUSB_ERROR_IO.
 *
 * Giải pháp: route bulk_write/read qua JNI callbacks → Android
 * UsbDeviceConnection.bulkTransfer() — path ổn định vì Android USB service
 * đã claim interface từ Kotlin side (UsbTransport.prepareForBulkTransfers()).
 *
 * Sau khi gọi hàm này:
 *   - usb_bridge_bulk_write() sẽ gọi NativeBridge.onNativeBulkWrite() qua JNI
 *   - usb_bridge_bulk_read() sẽ gọi NativeBridge.onNativeBulkRead() qua JNI
 *   - usb_bridge_clear_endpoints_halt() return true (no-op, Android quản lý)
 *   - usb_bridge_flush_in() vẫn dùng bulk_read (sẽ tự động route qua JNI)
 *
 * @return true nếu mode switch thành công
 */
bool usb_bridge_set_android_mode(void);

/*
 * usb_bridge_using_android_mode — return true nếu đang dùng Android JNI
 * transport mode (sau khi set_android_mode() được gọi).
 */
bool usb_bridge_using_android_mode(void);

/*
 * usb_bridge_iface_claimed — return 1 nếu libusb_claim_interface() đã thành công
 * (trong usb_bridge_init_from_fd2). Caller dùng để quyết định có cần switch sang
 * Android JNI transport mode không.
 */
bool usb_bridge_iface_claimed(void);

/*
 * FIX v38: JNI bridge — cache JavaVM và NativeBridge instance để
 * có thể gọi onNativeBulkWrite/Read từ native code.
 *
 * usb_bridge_set_jvm() — gọi từ JNI_OnLoad (của jni_bridge_imd.c) để
 * truyền JavaVM pointer xuống.
 */
void usb_bridge_set_jvm(void *vm);  /* void* để không require jni.h trong header */

/*
 * usb_bridge_set_bridge_ref() — gọi từ nativeInit() với NativeBridge
 * instance (jobject). Cache global ref + pre-resolve method IDs để
 * tránh FindClass/GetStaticMethodID mỗi lần bulk_write/read.
 */
void usb_bridge_set_bridge_ref(void *bridge_obj);  /* void* để không require jni.h trong header */
