# Ghi chú sửa lỗi biên dịch (Fix lỗi triệt để)

## 1. Lỗi Gradle: Thiếu file `python-config.gradle`
- **Vấn đề**: Log GitHub Actions cho thấy Gradle thất bại vì không tìm thấy `/home/runner/work/Sideloadtool/Sideloadtool/app/python-config.gradle`.
- **Nguyên nhân**: File này có thể chưa được commit vào repository hoặc bị Git bỏ qua.
- **Giải pháp**: 
    - Đã tạo lại file `app/python-config.gradle` với cấu hình Python 3.11 (phù hợp với GitHub Actions runner).
    - Cập nhật `app/build.gradle.kts` sử dụng `apply(from = file("python-config.gradle"))` để đảm bảo đường dẫn chính xác.

## 2. Lỗi NDK/CMake: Cấu trúc thư viện native
- **Vấn đề**: Việc tích hợp các thư viện gốc (libimobiledevice, usbmuxd, v.v.) vào project Android đòi hỏi cấu hình CMake rất chi tiết.
- **Giải pháp**:
    - Đã tạo `app/src/main/cpp/CMakeLists.txt` hoàn chỉnh, liệt kê đầy đủ các file source cho từng thư viện: `plist`, `limd_glue`, `tatsu`, `usb`, `usbmuxd_client`, `usbmuxd_daemon`.
    - Thêm `app/src/main/cpp/external/config.h` để cung cấp các macro cần thiết cho mã nguồn C mà không cần chạy script `configure`.
    - Tích hợp `usbmuxd_bridge.c` để khởi chạy usbmuxd daemon như một thread nội bộ trong app.

## 3. Khắc phục lỗi "Claim Interface"
- **Vấn đề**: Lỗi phổ biến khi iPhone không được nhận diện hoặc bị ngắt kết nối liên tục.
- **Giải pháp**:
    - Đã cập nhật `UsbTransport.kt` để gọi `setConfiguration()` trước khi `claimInterface()`.
    - Thêm cơ chế retry và cooldown trong `UsbPermissionManager.kt` để tránh vòng lặp re-enumeration của Android.

## 4. Đồng bộ hóa Python
- **Vấn đề**: Chaquopy cần các thư viện Python đúng phiên bản.
- **Giải pháp**: Đã chỉ định rõ `requests`, `cryptography`, và `srp` trong `python-config.gradle`.
