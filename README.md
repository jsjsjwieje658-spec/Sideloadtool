# Patch Fix - Sideloadtool iPhone Connection Issues

## Các file đã sửa

1. **app/src/main/cpp/android_usbmuxd_fix.c** - Viết lại hoàn toàn
   - Fix deadlock: `usbmuxd_get_device()` trả về placeholder khi chưa có UDID
   - Fix socket: `usbmuxd_connect()` đọc `USBMUXD_SOCKET_ADDRESS` từ environment

2. **app/src/main/cpp/jni_bridge_imd.c** - Patch 3 vị trí
   - Dòng ~378: Truyền placeholder UDID thay vì NULL khi khởi tạo
   - Dòng ~395: Gọi `android_fix_set_device()` sau khi lấy early UDID
   - Dòng ~531: Gọi `android_fix_set_device()` sau khi lấy UDID trong nativeConnect()

3. **app/src/main/cpp/CMakeLists.txt** - Thêm linker flag
   - `-Wl,--allow-multiple-definition` để lld (NDK r25+) cho phép override symbol

## Cách áp dụng

Copy 3 file này đè lên file gốc trong project:

```bash
cp app/src/main/cpp/android_usbmuxd_fix.c  /path/to/your/project/app/src/main/cpp/
cp app/src/main/cpp/jni_bridge_imd.c       /path/to/your/project/app/src/main/cpp/
cp app/src/main/cpp/CMakeLists.txt         /path/to/your/project/app/src/main/cpp/
```

Sau đó clean build:
```bash
./gradlew clean
./gradlew :app:assembleDebug
```

## Giải thích lỗi cốt lõi

### Lỗi 1: Deadlock `idevice_new_with_options() err=-3`
- `idevice_new_with_options()` gọi `usbmuxd_get_device()` để lấy device info
- `usbmuxd_get_device()` trả về `-ENOENT` vì chưa có UDID cached
- Không bao giờ tạo được device handle → không bao giờ lấy được UDID
- **Fix**: Trả về placeholder device (handle=1) để phá vỡ vòng lặp

### Lỗi 2: `usbmuxd_connect()` kết nối sai địa chỉ
- Code cũ hardcode TCP `127.0.0.1:27015`
- `usbmuxd_server.c` tạo Unix domain socket tại `/data/.../usbmuxd.sock`
- `jni_bridge_imd.c` set `USBMUXD_SOCKET_ADDRESS=unix:/path/to/socket`
- `usbmuxd_connect()` hoàn toàn bỏ qua biến môi trường này
- **Fix**: Đọc `USBMUXD_SOCKET_ADDRESS` và kết nối Unix socket đúng

### Lỗi 3: Build fail `duplicate symbol` (NDK r25+)
- `lld` (linker mặc định NDK r25+) không cho phép duplicate symbol
- `android_usbmuxd_fix.c` định nghĩa `usbmuxd_get_device`/`usbmuxd_connect`
- `libusbmuxd-2.0.a` cũng có 2 symbol này
- **Fix**: Thêm `-Wl,--allow-multiple-definition`
