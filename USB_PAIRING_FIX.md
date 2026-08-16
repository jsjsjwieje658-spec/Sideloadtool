# Bản sửa kết nối USB và ghép nối iPhone

## Phạm vi

Bản sửa này tập trung vào luồng **Android USB Host → libusb → usbmuxd nội bộ → libimobiledevice → lockdownd**. Lỗi trong ảnh chụp có dạng `lockdownd_client_new_with_handshake() err=-8`; theo enum trong header libimobiledevice đi kèm dự án, `-8` là `LOCKDOWN_E_MUX_ERROR`, tức lỗi transport/mux chứ không phải mã “pair thất bại”.

## Nguyên nhân đã xác định

| Khu vực | Vấn đề | Hậu quả |
| --- | --- | --- |
| JNI Mode 1 | Bản source hiện tại thiếu các JNI entry point quan trọng như `nativeSetUsbFd`, `nativeReset` và các getter trạng thái, trong khi Kotlin vẫn gọi chúng. | Android có thể rơi vào nhánh `UnsatisfiedLinkError`, còn libusb không nhận được fd hợp lệ. |
| Lockdown | `nativeConnect()` dùng `lockdownd_client_new_with_handshake()` ngay cả khi chưa có pair record hợp lệ. | Lần kết nối đầu có thể trả `LOCKDOWN_E_MUX_ERROR (-8)` trước khi UI kịp chạy bước Pair/Trust. |
| Pairing | Sau khi Pair thành công, code mở lại một handshake/session khác. | Tạo thêm mux session ngay sau Trust, dễ tái phát lỗi transport và làm mất session vừa pair. |
| usbmuxd nội bộ | `handle_client()` gửi `Detached` mỗi khi một client control đóng socket. | Việc đóng lockdownd/AFC/installation_proxy bị hiểu nhầm là iPhone đã rút USB. |
| Reconnect | Reconnect chỉ gọi lại `nativeConnect()` trên native state và fd cũ. | Sau detach/replug, libusb handle, socket và tunnel cũ có thể bị tái sử dụng. |
| Cleanup | Detach Android chỉ đóng `UsbDeviceConnection`, chưa dọn native server/libusb trước đó. | Thread native còn giữ descriptor đã mất, dẫn tới các lần kết nối sau trả lỗi mux. |

## Thay đổi chính

1. Khôi phục và đồng bộ lại Mode 1 giữa `jni_bridge_imd.c`, `usb_fd_bridge.c`, `usb_fd_bridge.h`, `usbmuxd_server.c` và `usbmuxd_server.h`.

2. `nativeSetUsbFd()` nhận thêm UDID hint từ Android, dọn lockdown client, thiết bị, usbmuxd server và libusb handle cũ trước khi bắt đầu USB session mới.

3. `nativeConnect()` hiện mở `lockdownd_client_new()` thuần, không thực hiện handshake/TLS trước Pair. `nativePair()` thực hiện Pair/Trust trên client hiện tại và giữ nguyên session sau khi Pair thành công.

4. Xóa `Detached` event giả khi control client đóng socket. Trạng thái device chỉ được dọn khi có sự kiện USB detach thực tế hoặc khi ứng dụng chủ động reset.

5. `NativeBridge.reset()` dọn native trước rồi đóng `UsbTransport`, còn `SuperAlphaApp` gọi reset native ngay khi nhận `ACTION_USB_DEVICE_DETACHED`.

6. `UsbReconnectManager` không còn dùng lại fd/session cũ. Mỗi lần reconnect sẽ reset native, gọi lại quy trình mở `UsbDeviceConnection` và xin quyền USB, sau đó mới gọi `nativeConnect()`.

## Kiểm thử đã thực hiện

| Kiểm thử | Kết quả |
| --- | --- |
| `git diff --check` | Đạt. |
| Kiểm tra đủ JNI methods giữa Kotlin và C | Đạt; gồm `nativeSetUsbFd`, `nativeConnect`, `nativePair`, `nativeReset`, getter trạng thái và diagnostics. |
| GCC host-side `-fsyntax-only -Wall -Wextra -Werror` cho ba file Mode 1 | Đạt: `jni_bridge_imd.c`, `usb_fd_bridge.c`, `usbmuxd_server.c`. |
| Gradle wrapper | Đã khôi phục `gradle-wrapper.jar` để có thể chạy wrapper. |
| Android `assembleDebug` | Chưa thể hoàn tất trong sandbox vì không có Android SDK/NDK tương ứng; Gradle dừng ở lỗi thiếu `ANDROID_HOME`/`sdk.dir`. |

## Quy trình kiểm thử trên thiết bị thật

Trước hết, gỡ bản APK cũ và cài APK được build từ commit này để tránh nhầm với binary cũ còn gọi `lockdownd_client_new_with_handshake()`. Trên iPhone, mở khóa màn hình, chọn **Tin cậy máy tính này** nếu được hỏi, rồi giữ nguyên cáp dữ liệu trong suốt quá trình version exchange.

Kịch bản tối thiểu gồm bốn lượt. Lượt thứ nhất là cắm iPhone, cấp quyền USB và bấm **Kết nối**; log đúng phải có `libusb_wrap_sys_device OK`, `version exchange OK`, `idevice OK` và `lockdownd client OK (no-TLS; sẵn sàng Pair/Trust)`. Lượt thứ hai là bấm **Ghép nối**, sau đó bấm **Tin cậy** trên iPhone; log không được quay lại `client_new_with_handshake()` và phải kết thúc bằng `Pair thành công`.

Lượt thứ ba là đóng/mở lại màn hình iPhone rồi thực hiện một lần kết nối hoặc sideload khác trên cùng dây. Lượt thứ tư là rút cáp, chờ thông báo detach, cắm lại và quan sát log reconnect; log phải thể hiện reset session cũ, mở USB mới, tạo fd mới và bắt đầu lại version exchange. Nếu sau khi rút-cắm mà log vẫn dùng fd cũ hoặc xuất hiện `Detached` chỉ vì một service đóng socket, đó là dấu hiệu binary chưa được build từ source mới.

> Bản sửa loại bỏ nguyên nhân phần mềm chính trong repository. Việc kết nối vẫn có thể thất bại nếu cáp chỉ sạc, adapter OTG không hỗ trợ data, iPhone đang khóa bằng mật mã hoặc người dùng từ chối Trust.

## Tệp đã thay đổi

| Tệp | Vai trò |
| --- | --- |
| `app/src/main/cpp/jni_bridge_imd.c` | JNI Mode 1, lifecycle fd/server, connect no-TLS và Pair/Trust. |
| `app/src/main/cpp/usb_fd_bridge.c` | Bọc fd Android bằng libusb, discover endpoint, clear halt, retry và close. |
| `app/src/main/cpp/usb_fd_bridge.h` | Đồng bộ API bridge với implementation. |
| `app/src/main/cpp/usbmuxd_server.c` | usbmuxd nội bộ, version state và client/tunnel lifecycle. |
| `app/src/main/cpp/usbmuxd_server.h` | Đồng bộ API server. |
| `app/src/main/java/com/superalpha/sideload/bridge/NativeBridge.kt` | Reset native rồi đóng Android USB connection. |
| `app/src/main/java/com/superalpha/sideload/bridge/UsbReconnectManager.kt` | Reopen USB và refresh fd trước reconnect. |
| `app/src/main/java/com/superalpha/sideload/SuperAlphaApp.kt` | Cleanup native khi Android phát hiện USB detach. |
| `gradle/wrapper/gradle-wrapper.jar` | Bổ sung file wrapper bị thiếu trong bản repository đã tải. |
