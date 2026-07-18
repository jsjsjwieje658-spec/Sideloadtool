# Android USBMUXD Link-Time Override Fix

## Vấn đề

libimobiledevice prebuilt được link STATIC (.a) vào `libsideloadnative.so`.
`libusbmuxd` trong prebuilt không đọc được `USBMUXD_SOCKET_ADDRESS` trên Android
→ `idevice_new_with_options() err=-3`.

## Giải pháp

Thêm file `android_usbmuxd_fix.c` build cùng project. Vì object file được link
TRƯỚC static archive `libusbmuxd-2.0.a`, linker ưu tiên symbols từ object file:

- `usbmuxd_get_device()` → trả về device info từ cache (bypass socket query)
- `usbmuxd_connect()` → kết nối TCP `127.0.0.1:27015` trực tiếp

## Cài đặt

### Bước 1: Copy 2 file mới vào `app/src/main/cpp/`

```bash
cp android_usbmuxd_fix.c android_usbmuxd_fix.h    Sideloadtool/app/src/main/cpp/
```

### Bước 2: Sửa `CMakeLists.txt`

Thêm `android_usbmuxd_fix.c` vào `add_library(sideloadnative SHARED ...)`:

```cmake
  add_library(sideloadnative SHARED
    jni_bridge_imd.c
    usb_fd_bridge.c
    android_usbmuxd_fix.c   # <-- THÊM DÒNG NÀY
    usbmuxd_server.c
  )
```

### Bước 3: Sửa `jni_bridge_imd.c`

Thêm `#include "android_usbmuxd_fix.h"` và gọi `android_fix_set_device()`
sau khi server start (trong `nativeSetUsbFd()`).

### Bước 4: Build

```bash
./gradlew assembleDebug
```

## Log mong đợi

```
[usbmuxd_srv] ✅ Server listening: /data/.../usbmuxd.sock (UDID: (chưa có))
[bridge] ✅ libusb bridge ready — fd sạch (termux-api pattern)
[imd] USBMUXD_SOCKET_ADDRESS=/data/.../usbmuxd.sock
[imd] usbmuxd socket ready ✅
[imd] idevice_new_with_options(USBMUX)...
[imd] ✅ idevice OK
[imd] ✅ iPhone UDID: 102e03e0d...
[lockdown] ✅ lockdownd session OK
```
