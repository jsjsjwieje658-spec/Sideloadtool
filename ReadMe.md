# SUPER ALPHA Sideload

Ứng dụng Android **ký và cài đặt file `.ipa` lên iPhone trực tiếp qua cáp USB** —
không cần máy tính, không cần Termux, không cần root. Toàn bộ chuỗi công cụ
`usbmuxd → lockdownd → AFC → installation_proxy` của máy tính được tái hiện
ngay trong một chiếc điện thoại Android.

> Version hiện tại: **1.6.0 (v54)** · Android 8.0+ (API 26) · arm64-v8a & x86_64

---

## 1. Tính năng

- **Ký & cài IPA bằng Apple ID miễn phí** — đăng nhập SRP/GSA đầy đủ (có 2FA),
  tự tạo certificate, App ID, provisioning profile, ký bằng `zsign`, cài qua
  AFC + `installation_proxy`.
- **Giao tiếp iPhone qua USB Host API của Android** — lớp usbmuxd/lockdown
  viết lại từ giao thức gốc của `libimobiledevice`/`usbmuxd` (đã kiểm chứng
  trên iPhone thật, iOS 16+).
- **Tự ghép nối + tự đăng ký UDID** — cắm cáp là dùng, không cần thao tác riêng.
- **Tự thu hồi certificate** ngay khi tạo cert mới thất bại — không cần đủ
  giới hạn 2 cert mới kích hoạt (ưu tiên thu hồi cert do tool tạo, thu hồi
  xong tự tạo lại).
- **Thông minh khi hết hạn mức 10 App ID / 7 ngày**: đọc danh sách app đang
  cài trên iPhone (App ID đang bị chiếm — kể cả App ID extension của app
  đang cài — không được tái dùng), tự tái dùng App ID trống; nếu tài khoản
  có App ID wildcard (kể cả wildcard toàn team "*") thì giữ nguyên bundle id
  gốc và extension cũng được che phủ; App ID đã chọn lần trước mà giờ bị app
  khác trên iPhone chiếm thì tự chọn App ID khác (không đè app cũ). Khi hết
  lượt tạo App ID cho extension: ưu tiên wildcard che phủ, rồi TÁI DÙNG App
  ID trống khác cho extension (đổi bundle id extension cho khớp — kiểu
  "Customize App Extensions" của SideStore; profile khớp đúng từng bundle
  nên không lỗi verify); hết hẳn mới DỪNG SỚM kèm hướng dẫn. Lưu ý: giới hạn
  10 App ID / 7 ngày của Apple đếm số lượt TẠO — xoá App ID trên trang
  developer không trả lại lượt. Chấp nhận cả IPA
  zip lại từ .app trần (thiếu Payload/) và file ZIP chứa .ipa lồng bên trong
  (tự giải nén tiếp, tối đa 2 tầng).
- **Đăng nhập một lần** — Apple ID + mật khẩu lưu riêng tư trên máy; đổi tài
  khoản bằng nút "Đăng xuất" trong Cài đặt.
- **Thu hồi chứng chỉ Development** theo lựa chọn (tất cả / theo số thứ tự).
- **File ghép nối (.mobiledevicepairing)** cho SideStore / LiveContainer…
  (mô hình iLoader): sau khi cài app, tool tự ghi pair record + UDID vào
  `Documents` của app qua house_arrest/AFC — ghi cả 2 tên file
  (`ALTPairingFile.mobiledevicepairing` bản cũ và `PairingFile_Lockdown.plist`
  SideStore 0.7+). Lưu ý SideStore 0.7 không tự nạp file có sẵn: mở SideStore →
  chọn file khi được hỏi → "Trên iPhone của tôi" → SideStore →
  `PairingFile_Lockdown.plist` (1 lần duy nhất). Đồng thời ghi thẳng
  UserDefaults của SideStore (activePairingProtocol=lockdown,
  isPairingReset=false) → SideStore cài qua tool và CHƯA từng mở sẽ tự kích
  hoạt pairing, không cần chọn file. Tab **File ghép nối** còn cho xuất file
  và nhúng thủ công vào app đã cài.
- Chọn server **Anisette** (tự dò từ `servers.sidestore.io` hoặc nhập tay).
- Nhật ký thời gian thực từng bước — màu theo mức độ, sao chép được.
- Tự kết nối lại khi cáp bị rút cắm lại (backoff + retry).

## 2. Kiến trúc — mã nguồn hoạt động thế nào

Ứng dụng gồm **4 tầng** communicate qua các ranh giới rõ ràng:

```
┌──────────────────────────────────────────────────────────────┐
│  UI — Kotlin + Jetpack Compose (Material 3, theme dark)      │
│  MainActivity → LoginScreen (cổng đăng nhập)                 │
│              → AppNavHost: Cài IPA · Thu hồi cert · Cài đặt  │
│  HomeViewModel (state trung tâm) · LogBuffer (ring buffer)   │
├──────────────────────────────────────────────────────────────┤
│  bridge — Kotlin                                             │
│  UsbTransport      mở fd USB, claim interface usbmux         │
│  NativeBridge      JNI ↔ thư viện C (13 export)              │
│  UsbReconnectManager / UsbPermissionManager / DeviceStatus    │
├──────────────────────────────────────────────────────────────┤
│  Native — C (app/src/main/cpp, build bằng CMake + NDK)       │
│  usb_fd_bridge.c   libusb hoặc bulk transfer Android JNI     │
│  usbmuxd_server.c  usbmuxd nội bộ (Unix socket + threads)    │
│  jni_bridge_imd.c  lockdownd/AFC/instproxy (libimobiledevice)│
├──────────────────────────────────────────────────────────────┤
│  Python — Chaquopy 3.11 (app/src/main/python)                │
│  apple_auth.py     SRP/GSA login Apple ID (mỗi request một   │
│                    socket, client-info com.apple.akd)        │
│  developer_api.py  Apple Developer Services (team, cert,     │
│                    App ID, profile, device — mô hình AltSign) │
│  sideload_core.py  luồng chính do_sideload() 5 bước          │
│  device_link.py    gọi ngược Kotlin (DeviceNative…)          │
└──────────────────────────────────────────────────────────────┘
```

### Luồng ký & cài một IPA (`do_sideload`, 5 bước)

1. **Bước 0 — Kết nối iPhone**: `DeviceNative.connectAndPair()` → chọn đúng
   USB configuration chứa interface usbmux → bắt tay mux v2.0 → lockdownd →
   ghép nối (pair, hỏi "Tin cậy" trên iPhone nếu lần đầu) → UDID.
2. **Bước 1 — Đăng nhập Apple ID**: SRP qua GSA (2FA hiện dialog), đổi
   `GsIdmsToken` lấy team.
3. **Bước 2 — Certificate**: dùng lại cert đã lưu hoặc tạo mới (CSR + RSA);
   nếu bị Apple chặn vì đủ 2 cert → **tự thu hồi** cert cũ rồi thử lại.
4. **Bước 3 — App ID + profile**: đổi bundle id của IPA cho khớp App ID đã
   nộp (App ID là duy nhất toàn cầu — tự thêm hậu tố khi bị trùng), tải
   profile cho app chính và từng extension `.appex`.
5. **Bước 4–5 — Ký & cài**: `zsign` ký thư mục `.app` đã sửa (nhiều `-m`
   matched theo bundle id) → `nativeSideload()` đẩy IPA qua AFC
   (`PublicStaging/<tên>.ipa`, chunk 32 KB) → `instproxy_install` và chờ
   "Complete".

### Điểm giao nhau Kotlin ↔ Python ↔ C

- **Python gọi Kotlin** (Chaquopy Java-interop, theo tên class — vì vậy bản
  release R8 giữ nguyên toàn bộ `com.superalpha.sideload.**`):
  `AppPaths` (đường dẫn), `DeviceNative` (connect/pair/sideload/diagnostics),
  `NativeLog` (log), `UiPrompt` (hộp thoại 2FA/câu hỏi).
- **C gọi Kotlin** (JNI `GetMethodID`): `NativeBridge.onNativeLog`,
  `onTrustRequired`, `UsbTransport.nativeBulkRead/Write` (chế độ dự phòng khi
  libusb không claim được), `CertHelper`, `TlsHelper`.
- **Kotlin gọi C**: 13 hàm `Java_..._NativeBridge_native*` trong
  `jni_bridge_imd.c`.

## 3. Cấu trúc thư mục chính

```
app/src/main/
├── cpp/                    Native C (xem kiến trúc ở trên)
│   ├── CMakeLists.txt      build 4 file .c + link .native-deps
│   └── (jni_bridge.c, usbmux.c, … : mã "Mode 2/3" cũ, KHÔNG biên dịch)
├── java/com/superalpha/sideload/
│   ├── MainActivity.kt     cổng USB-attach + Login/App switch
│   ├── SuperAlphaApp.kt    Application (Python + native init nền)
│   ├── bridge/             UsbTransport, NativeBridge, LogBuffer, …
│   ├── python/PythonBridge.kt
│   └── ui/                 Compose screens + theme + components
├── python/                 mã Python chạy trong Chaquopy
├── jniLibs/arm64-v8a/      libzsign.so dựng sẵn (arm64)
└── assets/zsign_deps/      libssl/libcrypto/libc++ cho zsign
scripts/build_all.sh        cross-compile libplist/usbmuxd/libimobiledevice
.github/workflows/buildapk.yml   CI: test protocol + build APK
tests/                      test host (native protocol) + test Python flow
```

## 4. Build

### CI (khuyên dùng)
Mỗi push lên `main` chạy GitHub Actions:
1. **Protocol tests** — build libimobiledevice cho host, chạy 28 test giao
   thức usbmux/lockdown + 36 test luồng Python.
2. **Build APK** — NDK 25.2 cross-compile native (cache theo hash
   `scripts/build_all.sh`), Gradle assemble **debug + release** (R8 minify,
   keep toàn bộ package app + Chaquopy), ký release bằng keystore từ
   Secrets (`RELEASE_KEYSTORE_*` — thiếu thì fallback debug key).
Artifacts: `superalpha-sideload-release` (khuyên dùng) và
`superalpha-sideload-debug` (dự phòng).

### Local
```bash
# Yêu cầu: JDK 17, Android SDK 34, NDK 25.2, CMake 3.22
sdkmanager "platforms;android-34" "build-tools;34.0.0" "cmake;3.22.1"

# 1) Native deps (cache vào .native-deps)
TARGET_ABIS="arm64-v8a x86_64" NATIVE_DEPS_BASE="$PWD/.native-deps" bash scripts/build_all.sh

# 2) APK
gradle :app:assembleRelease

# 3) Test (không cần thiết bị thật)
tests/native-host/build_host_deps.sh && tests/native-host/run.sh
python3 tests/python/test_sideload_core.py
```

## 5. Cài đặt & sử dụng

1. Tải artifact `superalpha-sideload-release` từ Actions → cài lên máy
   Android (arm64). Cài đè các bản trước trực tiếp (chữ ký ổn định).
2. Lần đầu mở app: **đăng nhập Apple ID** (mật khẩu lưu riêng tư trên máy —
   `allowBackup=false` nên không bao giờ rời thiết bị; Đăng xuất trong
   Cài đặt để xoá).
3. Cắm cáp **data** vào iPhone → thẻ trạng thái hiện "iPhone sẵn sàng"
   (lần đầu bấm "Tin cậy" trên iPhone).
4. Chọn file `.ipa` → **Ký & Cài đặt** → theo dõi nhật ký từng bước.
5. Sau khi cài: iPhone → Cài đặt > Cài đặt chung > Quản lý VPN & Thiết bị >
   tin cậy Apple ID của bạn. iOS 16+: bật Chế độ nhà phát triển nếu được hỏi.

**Lưu ý**: tài khoản Apple ID miễn phí giới hạn 2 certificate / 10 App ID mới
mỗi 7 ngày; app tự xử lý cả hai (tự thu hồi cert, tự đổi bundle id + tái dùng
App ID phái sinh). App cài được giữ 7 ngày rồi phải ký lại.

## 6. Bảo mật & rủi ro đã biết

- Mật khẩu Apple ID lưu **plaintext trong storage riêng tư** của app (theo
  yêu cầu trải nghiệm "đăng nhập một lần") — an toàn với máy chưa root; gỡ
  app hoặc Đăng xuất để xoá sạch.
- Lớp usbmux/lockdown tự triển khai lại từ giao thức gốc — **chưa** qua kiểm
  định chính thức của Apple; dùng với thiết bị chứa dữ liệu quan trọng hãy
  cân nhắc.
- `usesCleartextTraffic=false`; mọi kết nối Apple qua HTTPS; server Anisette
  do người dùng chọn.

## 7. Ghi công

Dựa trên / học từ mã nguồn mở của các dự án:

- [libimobiledevice / libusbmuxd / usbmuxd / libplist](https://libimobiledevice.org)
  (LGPL-2.1) — giao thức usbmux, lockdown, AFC, instproxy.
- [libusb](https://libusb.info) (LGPL-2.1) — truy cập USB trực tiếp.
- [zsign](https://github.com/zhlynn/zsign) — ký lại IPA.
- [AltSign](https://github.com/rileytestut/AltSign) (rileytestut) — mô hình
  Apple Developer API (team/cert/profile).
- [SideStore / anisette servers](https://sidestore.io) — danh sách server
  Anisette công khai.
- [Chaquopy](https://chaquo.com/chaquopy/) — chạy Python trên Android.

---

*SuperAlpha Sideload — mã nguồn phục vụ mục đích học tập/nghiên cứu giao thức
Apple USB. Bạn chịu trách nhiệm với tài khoản Apple ID và thiết bị của mình.*
