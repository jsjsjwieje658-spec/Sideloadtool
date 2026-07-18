# Tích hợp usbmuxd và libimobiledevice gốc (No Root)

## Các thay đổi chính:

1.  **Cross-compile usbmuxd & libimobiledevice**: Toàn bộ mã nguồn gốc từ GitHub của `libimobiledevice` đã được đưa vào thư mục `app/src/main/cpp/external`.
2.  **Chạy usbmuxd như một thread**: Thay vì cần root để cài usbmuxd vào hệ thống, app giờ đây tự khởi chạy một "internal usbmuxd daemon" dưới dạng một thread khi khởi động.
3.  **Unix Socket nội bộ**: usbmuxd daemon này lắng nghe trên một Unix socket nằm trong thư mục dữ liệu của app (`filesDir/usbmuxd`). Các thư viện client (`libimobiledevice`) được cấu hình để kết nối đến socket này thay vì `/var/run/usbmuxd` mặc định.
4.  **libusb tích hợp**: usbmuxd sử dụng `libusb` được biên dịch trực tiếp để giao tiếp với iPhone qua Android USB Host API.
5.  **Cập nhật CMakeLists.txt**: Hệ thống build đã được cấu hình lại hoàn toàn để Android NDK tự động biên dịch tất cả các thư viện phụ thuộc (libplist, libusbmuxd, libimobiledevice-glue, libtatsu, libusb, usbmuxd).

## Lợi ích:
- **Ổn định hơn**: Sử dụng giao thức gốc của Apple được duy trì bởi cộng đồng libimobiledevice.
- **Không cần Root**: Hoạt động hoàn toàn trên các thiết bị Android chưa root thông qua quyền USB Host.
- **Dễ bảo trì**: Mã nguồn nằm trực tiếp trong project, có thể cập nhật dễ dàng bằng cách thay thế file trong thư mục `external`.

## Cách sử dụng:
Chỉ cần biên dịch app như bình thường trong Android Studio. App sẽ tự động khởi chạy usbmuxd và quản lý kết nối.
