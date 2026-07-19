#pragma once
/*
 * usbmuxd_server.h — Mini usbmuxd server nội bộ (in-process, Mode 1)
 *
 * Chạy một Unix domain socket server mô phỏng usbmuxd.
 * libimobiledevice (libusbmuxd) sẽ kết nối đến socket này thay vì
 * /var/run/usbmuxd (không tồn tại trên Android không root).
 *
 * Sử dụng:
 *   1. Gọi usb_bridge_init() trước (cần libusb handle)
 *   2. Gọi usbmuxd_server_start(filesDir, udid, productId)
 *   3. setenv("USBMUXD_SOCKET_ADDRESS", usbmuxd_server_get_socket_path(), 1)
 *   4. Gọi idevice_new_with_options() → libusbmuxd tự kết nối socket
 *   5. Gọi usbmuxd_server_stop() khi xong
 *
 * FIX v20: file này bị thiếu → Mode 1 không compile, usbmuxd server
 * không bao giờ chạy → idevice_new_with_options() trả IDEVICE_E_NO_DEVICE.
 */
#include <stdbool.h>

/*
 * usbmux_version_exchange — thực hiện version exchange v1 với iPhone.
 *
 * QUAN TRỌNG: iPhone v1 mux protocol chỉ chấp nhận MỘT version exchange
 * cho mỗi USB session. Hàm này PHẢI được gọi đúng một lần, ngay sau khi
 * usb_bridge_init() (libusb) khởi tạo xong và TRƯỚC KHI gọi
 * usbmuxd_server_start(). Idempotent: các lần gọi lại sau đó chỉ trả về
 * true ngay lập tức mà không lặp lại exchange thật (an toàn để gọi lại
 * như một safety check ở nơi khác).
 *
 * @return true nếu version exchange thành công (hoặc đã làm trước đó)
 */
bool usbmux_version_exchange(void);

/*
 * usbmuxd_server_reset_version_state — cho phép usbmux_version_exchange()
 * thực hiện lại. Gọi hàm này khi bắt đầu một USB session THẬT SỰ MỚI
 * (fd mới / thiết bị mới) — ví dụ ngay sau usb_bridge_init()
 * thành công và TRƯỚC KHI gọi usbmux_version_exchange() — để tránh việc
 * flag idempotent từ session trước làm session mới bị bỏ qua version
 * exchange (điều này sẽ khiến session mới treo/reject giống Bug B).
 */
void usbmuxd_server_reset_version_state(void);

/*
 * usbmuxd_server_start — khởi động server (Unix socket + TCP dual-socket).
 *
 * Học từ termux-usbmuxd/usbmuxd_proxy.c:
 *   - Dọn stale Unix socket trước khi bind (tránh EADDRINUSE).
 *   - Mở thêm TCP listener trên 127.0.0.1:27015 — giống socat trong
 *     termux-usbmuxd, cho phép cả Rust tools (idevice-tools) kết nối.
 *   - Set USBMUXD_SOCKET_ADDRESS = "127.0.0.1:27015" (TCP form) thay
 *     vì đường dẫn Unix thuần tuý (tránh AddrParseError trong Rust tools).
 *   - Ignore SIGPIPE, SIGHUP trong các server threads (như usbmuxd_proxy.c).
 *
 * @param files_dir  thư mục app (Unix socket: files_dir/usbmuxd.sock)
 * @param udid       UDID của iPhone (ListDevices response)
 *                   Nếu chưa biết, truyền "00000000-0000-0000-0000-000000000000"
 * @param product_id USB product ID của iPhone
 * @return true nếu Unix socket đã listen, false nếu thất bại
 *         (TCP listener start failure là non-fatal — fallback Unix socket)
 */
bool usbmuxd_server_start(const char *files_dir, const char *udid, int product_id);

/*
 * usbmuxd_server_update_udid — cập nhật UDID sau khi đã biết (gọi sau
 * idevice_get_udid()). Server cần UDID thật để ListDevices trả về đúng.
 *
 * FIX (báo cáo lỗi #2/#3): trước đây hàm này chỉ ghi đè g_udid trong bộ
 * nhớ mà không thông báo cho bất kỳ client nào đang Listen — các client
 * đã nhận "Attached" event trước đó (thường mang UDID placeholder
 * "00000000-...") sẽ không bao giờ biết UDID thật đã sẵn sàng. Giờ đây
 * hàm này tự động gọi broadcast lại "Attached" event (UDID mới) cho MỌI
 * client đang Listen ngay sau khi cập nhật xong.
 */
void usbmuxd_server_update_udid(const char *udid);

/*
 * usbmuxd_server_broadcast_attached — gửi lại "Attached" event (với UDID
 * hiện tại) cho MỌI client đang Listen.
 *
 * FIX (báo cáo lỗi #2/#3/#7): trước đây "Attached" chỉ được gửi MỘT LẦN,
 * ngay sau khi một client cụ thể gửi "Listen" — nếu client đó đã
 * disconnect, hoặc UDID thật chỉ được biết SAU đó (qua idevice_get_udid()),
 * không có cơ chế nào thông báo lại cho các client đang Listen khác.
 *
 * usbmuxd_server_update_udid() đã tự gọi hàm này, nhưng nó cũng được
 * export public để nơi gọi (VD: jni_bridge_imd.c ngay sau
 * idevice_get_udid() + usbmuxd_server_update_udid()) có thể chủ động gọi
 * lại như một safety-net tường minh, khớp đúng luồng usbmuxd thật.
 */
void usbmuxd_server_broadcast_attached(void);

/*
 * usbmuxd_server_get_socket_path — trả đường dẫn đến Unix socket.
 * Trả NULL nếu server chưa start.
 */
const char *usbmuxd_server_get_socket_path(void);

/*
 * usbmuxd_server_stop — dừng server và giải phóng tài nguyên.
 */
void usbmuxd_server_stop(void);
