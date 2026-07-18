# Hướng dẫn Build Native Libraries

## Câu hỏi thường gặp: "Tôi không thấy usbmuxd và libimobiledevice trong source!"

**Đây là thiết kế có chủ ý.** Các thư viện này KHÔNG được commit vào repo vì:

1. Chúng nặng (~50 MB prebuilt .a cho mỗi ABI)
2. Chúng là third-party code từ [libimobiledevice.org](https://libimobiledevice.org)
3. Chúng được tải và compile tự động bởi `scripts/build_all.sh`

**Khi nào build_all.sh chạy?**
- Khi developer chạy thủ công: `bash scripts/build_all.sh`
- Khi CI (GitHub Actions) chạy: Job 1 của `.github/workflows/buildapk.yml`

**Output ở đâu?**
```
app/src/main/cpp/prebuilt/
├── arm64-v8a/
│   ├── lib/
│   │   ├── libimobiledevice-1.0.a   ← libimobiledevice.org
│   │   ├── libusbmuxd-2.0.a         ← libimobiledevice.org
│   │   ├── libplist-2.0.a           ← libimobiledevice.org
│   │   ├── libimobiledevice-glue-1.0.a
│   │   ├── libusb-1.0.a             ← libusb.info
│   │   ├── libssl.a                 ← openssl.org
│   │   └── libcrypto.a
│   └── include/
│       ├── libimobiledevice/        ← header files
│       ├── libplist/
│       └── ...
└── x86_64/
    └── ...
```

Thư mục `prebuilt/` có trong `.gitignore` → không thấy trong repo.

---

## Kiến trúc: Tại sao không cần root, không cần usbmuxd daemon?

```
┌─────────────────────────────────────────────────────────────┐
│  Trên macOS/Linux (cách truyền thống):                      │
│                                                             │
│  usbmuxd daemon (cần root) ──┐                              │
│         ↓                    ↓                              │
│  /dev/bus/usb/*          iPhone                             │
│         ↑                                                   │
│  libimobiledevice (qua unix socket của usbmuxd)             │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│  Trên Android (cách này — KHÔNG cần root):                  │
│                                                             │
│  UsbManager.openDevice()      ← Android API, không cần root│
│       ↓ fileDescriptor                                      │
│  libusb_wrap_sys_device(ctx, fd, &handle)  ← libusb 1.0.27 │
│       ↓ handle                                              │
│  libimobiledevice → libusb → fd → iPhone  ← không qua usbmuxd│
└─────────────────────────────────────────────────────────────┘
```

`libusb_wrap_sys_device()` là API được thêm vào libusb 1.0.23 đặc biệt
để hỗ trợ Android USB Host API. Nó cho phép nhận một "raw fd" từ OS
và dùng nó như một libusb device handle thông thường.

---

## Luồng hoạt động đầy đủ

```
[Người dùng cắm cáp Lightning/USB-C]
        ↓
MainActivity → USB_DEVICE_ATTACHED intent
        ↓
UsbPermissionManager.request(device)   → Android hiện popup xin quyền
        ↓ (người dùng bấm OK)
HomeViewModel.onUsbDeviceGranted(device, usbManager)
        ↓
UsbTransport.open(device, usbManager)
  → findUsbmuxIface(device)             tìm interface class=0xFF/0xFE/0x02
  → usbManager.openDevice(device)       mở connection
  → conn.claimInterface(iface, true)    claim (retry 8 lần nếu cần)
  → trả về fd = conn.fileDescriptor
        ↓
NativeBridge.setUsbFd(fd, vid, pid)    [Mode 1 only]
  → usb_bridge_init_from_fd(fd, vid, pid)
  → libusb_init(&ctx)
  → libusb_wrap_sys_device(ctx, fd, &handle)
        ↓
NativeBridge.connect()
  → idevice_new_with_options(&device, NULL, USBMUX)
    [libimobiledevice dùng libusb để tìm iPhone qua handle]
  → lockdownd_client_new_with_handshake(&lockdown, device, "sideloadtool")
        ↓
[PairingScreen] NativeBridge.pair()
  → lockdownd_pair(lockdown, NULL)
    [nếu PENDING: chờ người dùng bấm "Tin cậy" trên iPhone]
        ↓
[SideloadScreen] NativeBridge.sideload(ipaPath)
  → lockdownd_start_service("com.apple.afc")    mở AFC port
  → afc_client_new(device, svc, &afc)
  → afc_make_directory("/PublicStaging")
  → afc_file_open + afc_file_write (chunk 256KB)  copy IPA
  → instproxy_client_start_service(&ipc)
  → instproxy_install(ipc, remote_path, opts)     cài đặt IPA
        ↓
✅ App xuất hiện trên màn hình iPhone
```

---

## Cách build thủ công

### Yêu cầu host

```bash
# macOS
brew install autoconf automake libtool pkg-config cmake

# Ubuntu/Debian
sudo apt-get install autoconf automake libtool pkg-config cmake curl ninja-build
```

### Các bước

```bash
# 1. Đặt đường dẫn NDK (phiên bản 25.2.9519653 hoặc mới hơn)
export ANDROID_NDK_HOME="$HOME/Library/Android/sdk/ndk/25.2.9519653"

# 2. Chạy script tổng hợp — tự động tải và cross-compile tất cả thư viện
bash scripts/build_all.sh

# Thêm x86_64 (dùng cho emulator):
TARGET_ABIS="arm64-v8a x86_64" bash scripts/build_all.sh

# Build sạch từ đầu (xoá cache):
bash scripts/build_all.sh --clean

# 3. Build APK
./gradlew :app:assembleDebug
```

Script tải các tarball sau và cross-compile lần lượt (~12 phút lần đầu):

| # | Thư viện | Phiên bản | Nguồn |
|---|---|---|---|
| 1 | OpenSSL | 3.3.1 | github.com/openssl/openssl |
| 2 | libplist | 2.6.0 | github.com/libimobiledevice/libplist |
| 3 | libimobiledevice-glue | 1.3.0 | github.com/libimobiledevice/libimobiledevice-glue |
| 4 | libusbmuxd | 2.0.2 | github.com/libimobiledevice/libusbmuxd |
| 5 | libimobiledevice | 1.3.0 | github.com/libimobiledevice/libimobiledevice |
| 6 | libusb | 1.0.27 | github.com/libusb/libusb |

Lần build tiếp theo, tarball được cache tại `~/.cache/sideloadtool-ndk-build/`,
và `prebuilt/` cũng được cache theo SHA của `build_all.sh`.

---

## Ba chế độ (CMakeLists.txt tự chọn)

| Mode | Điều kiện | JNI source | Ghi chú |
|---|---|---|---|
| **1** ✅ | `libimobiledevice-1.0.a` tồn tại | `jni_bridge_imd.c` + `usb_fd_bridge.c` | API thật, khuyến nghị |
| **2** ⚠️ | Chỉ có `libplist-2.0.a` | `jni_bridge.c` + custom protocol | Dùng được nhưng giới hạn |
| **3** ❌ | Không có prebuilt | `jni_bridge.c` + `plist_util.c` | Fallback hoàn toàn |

---

## Gỡ lỗi phổ biến

| Triệu chứng | Nguyên nhân | Khắc phục |
|---|---|---|
| `libusb_wrap_sys_device() failed` | NDK API < 21 hoặc libusb < 1.0.23 | Dùng NDK 25 + libusb 1.0.27 |
| `idevice_new_with_options() err=-3` | Màn hình iPhone khoá | Mở khoá màn hình trước khi cắm |
| `lockdownd_pair() = PENDING` | Trust popup chưa bấm | Bấm "Tin cậy" trên iPhone |
| `claimInterface() failed` | Driver khác đang giữ interface | Rút/cắm lại cáp, thử lại |
| `afc_file_write() err` | IPA quá lớn / timeout | Kiểm tra dung lượng iPhone |
| `instproxy_install() err` | Bundle ID xung đột hoặc cert hết hạn | Re-sign IPA với AltSign/Sideloadly |
| `configure: no acceptable C compiler` | ANDROID_NDK_HOME sai | Kiểm tra đường dẫn NDK |
