# SUPER ALPHA Sideload — Android

Ứng dụng Android (Kotlin + Jetpack Compose + Chaquopy) để **ký và cài đặt file
.ipa lên iPhone/iPad qua cáp USB**, không cần Termux, không cần Termux:API,
không cần root, không cần máy tính. Đây là bản chuyển đổi từ công cụ CLI gốc
chạy trong Termux sang một ứng dụng Android độc lập, dùng chính điện thoại
Android làm "máy tính trung gian" nói chuyện với iPhone qua USB.

> ⚠️ **Đọc kỹ mục "Rủi ro đã biết" bên dưới trước khi dùng.** Phần giao tiếp
> USB với iPhone (usbmux/lockdown/AFC) là code **tự viết lại, chưa từng được
> test với phần cứng thật**, vì môi trường phát triển dự án này (Replit)
> không có SDK Android, không có Gradle, không có iPhone thật để thử.

---

## 1. Kiến trúc

```
(gốc dự án — chính là thư mục sau khi giải nén zip này)
├── app/
│   ├── build.gradle.kts          # Cấu hình Chaquopy (Python 3.11), pip deps, ABI arm64-v8a
│   └── src/main/
│       ├── java/com/superalpha/sideload/
│       │   ├── SuperAlphaApp.kt          # Khởi động Python interpreter (Chaquopy)
│       │   ├── MainActivity.kt           # Entry point, mount Compose UI
│       │   ├── bridge/
│       │   │   ├── AppPaths.kt           # Đường dẫn filesystem app-private cho Python dùng
│       │   │   ├── NativeLog.kt          # print() (Python) -> SharedFlow -> LogConsole UI
│       │   │   ├── UiPrompt.kt           # input() (Python, vd mã 2FA) -> AlertDialog -> quay lại
│       │   │   ├── UsbTransport.kt       # USB Host API: claim interface, bulk IN/OUT thô
│       │   │   ├── UsbPermissionManager.kt # Xin quyền USB, mở kết nối, đọc UDID (serialNumber)
│       │   │   └── UsbBridgeService.kt   # Foreground service giữ kết nối USB khi app ở nền
│       │   ├── python/PythonBridge.kt    # Gọi vào sideload_core.py từ Compose (Dispatchers.IO)
│       │   └── ui/                       # Màn hình Compose: Sideload, Thu hồi cert, Cài đặt, Log
│       └── python/
│           ├── sideload_core.py   # Điểm vào chính: do_sideload(), do_revoke_certs()
│           ├── apple_auth.py      # Đăng nhập Apple ID (GSA/SRP) + xử lý 2FA
│           ├── developer_api.py   # Gọi Apple Developer API (cert, App ID, provisioning profile)
│           ├── device_link.py     # lockdownd/pairing/AFC/installation_proxy qua mux_usb
│           ├── mux_usb.py         # Giao thức "usbmux" tự triển khai lại trên USB bulk transfer
│           ├── config_manager.py  # Đọc/ghi config.json
│           └── utils.py           # Giải nén/đóng gói IPA, đọc Info.plist, v.v.
├── app/src/main/jniLibs/arm64-v8a/libzsign.so   # Binary zsign (ký lại IPA) — chỉ arm64
└── .github/workflows/build-apk.yml   # CI build APK debug (xem mục 4)
```

**Luồng dữ liệu chính khi bấm "Ký & Cài đặt":**

1. `SideloadScreen` (Compose) gọi `PythonBridge.sideload(...)` trên `Dispatchers.IO`.
2. Chaquopy chạy `sideload_core.do_sideload(...)` — mọi `print()` trong Python
   được chuyển hướng sang `NativeLog` (Kotlin `SharedFlow`) để hiện trong ô
   "Nhật ký" của UI, vì Android không có terminal.
3. Nếu Apple yêu cầu mã 2FA, `apple_auth.py` gọi `input()` như bản CLI gốc,
   nhưng ở đây `input_func` được thay bằng `UiPrompt.requestInput(...)` — hàm
   này chặn (block) luồng Python và hiện một `AlertDialog` trong Compose;
   Python chỉ tiếp tục chạy sau khi người dùng nhập mã và bấm xác nhận.
4. `developer_api.py` gọi thẳng Apple Developer API (HTTPS, qua `requests`) —
   phần này **không** đi qua USB, giống hệt bản CLI gốc.
5. `device_link.py` mở kết nối tới iPhone qua `mux_usb.py` (xem mục 2) để:
   ghép nối (pairing, cần bấm "Trust" trên iPhone lần đầu), đẩy file IPA đã
   ký qua AFC, rồi ra lệnh cài đặt qua `installation_proxy`.
6. `libzsign.so` được gọi như một tiến trình con (`subprocess`) để ký lại IPA
   bằng certificate/provisioning profile vừa lấy từ Apple.

---

## 2. Đã sửa trong bản này

So với bản trước, các vấn đề sau đã được sửa trực tiếp trong code (không phải
build/test được trên phần cứng thật ở môi trường viết code này — xem mục
"Rủi ro đã biết" ngay dưới đây để biết phần nào vẫn cần bạn tự kiểm chứng):

- **Anisette server sai địa chỉ (rất có thể là nguyên nhân đăng nhập/2FA thất
  bại từ trước tới nay):** `apple_auth.py` trước đây trỏ `OFFICIAL_SERVERS_URL`
  và `ANISETTE_URL` vào `127.0.0.1:6969` — một server LOCAL không tồn tại trên
  Android (không giống bản Termux gốc, ứng dụng này không tự chạy anisette
  server nào). Đã sửa để trỏ đúng vào `https://servers.sidestore.io/servers.json`
  (danh sách server công khai thật, đã kiểm tra định dạng response) và
  `https://ani.sidestore.io` (fallback), nên `AppleAuth` giờ có thể thực sự
  lấy được thông tin xác thực thiết bị cần cho đăng nhập/2FA.
- **Kết nối USB thất bại lặp lại sau khi đã cấp quyền:** `UsbTransport.open()`
  giờ chọn cấu hình USB trước khi claim interface, thử lại tối đa 3 lần với
  khoảng nghỉ ngắn (khắc phục các thất bại tạm thời ngay sau khi vừa được cấp
  quyền/vừa cắm lại thiết bị), và báo lỗi cụ thể (không tìm thấy interface /
  thiếu endpoint / `openDevice` trả null / không claim được interface) thay vì
  một câu chung "Mở kết nối USB thất bại." — cả trong log lẫn để debug.
  `UsbPermissionManager` cũng chặn việc bấm "Kết nối" nhiều lần liên tục tạo ra
  nhiều yêu cầu quyền song song (nguyên nhân của các dòng log lặp lại giống
  nhau bạn có thể đã thấy), và các lệnh mở kết nối USB thật giờ chạy trên luồng
  nền, không chặn UI. Ứng dụng cũng tự đóng kết nối khi phát hiện thiết bị bị
  rút (`SuperAlphaApp`), và tự động thử kết nối khi bạn cắm iPhone vào lúc app
  đang mở sẵn (`MainActivity.onNewIntent`, nhờ intent-filter
  `USB_DEVICE_ATTACHED` đã có sẵn trong `AndroidManifest.xml` nhưng trước đây
  không được xử lý).
- **Cảm giác trễ khi đổi tab dưới cùng:** nguyên nhân chính là `LogConsole`
  luôn CHẠY ANIMATION cuộn từ đầu tới cuối log (tới 500 dòng) mỗi lần một tab
  được mount lại — kể cả khi log không có dòng mới. Đã sửa để lần mount đầu
  tiên nhảy thẳng tới cuối (không animation), chỉ animate khi có dòng log mới
  thật xuất hiện trong lúc màn hình đang mở. Đồng thời bỏ animation chuyển
  cảnh mặc định của `NavHost` khi đổi giữa 3 tab (Sideload/Thu hồi cert/Cài
  đặt) — đây là điều hướng ngang cấp, animation trượt chỉ tạo cảm giác trễ mà
  không có giá trị điều hướng nào.
- **Sửa tiếp lỗi "Không claim được interface usbmux" (lặp lại kể cả sau khi đã
  thử lại nhiều lần):** đây là một lỗi khác, sâu hơn, so với các lỗi USB đã sửa
  ở trên — không phải lỗi tạm thời mà là ép sai cấu hình (configuration) USB
  một cách hệ thống. `UsbDevice.getInterface()` của Android gộp chung interface
  từ TẤT CẢ các cấu hình USB mà thiết bị khai báo, kể cả những cấu hình không
  phải cấu hình đang thực sự hoạt động trên phần cứng lúc đó; code cũ luôn ép
  `setConfiguration()` về cấu hình đầu tiên bất kể interface usbmux tìm được
  thực sự nằm ở cấu hình nào, nên nếu iPhone của bạn khai báo interface usbmux
  ở một cấu hình khác cấu hình đầu tiên, `claimInterface()` sẽ luôn thất bại —
  không có số lần thử lại nào sửa được. Đã sửa để dò đúng cấu hình chứa
  interface usbmux trước, set đúng cấu hình đó, rồi mới claim (xem
  `UsbTransport.findUsbmuxInterfaceWithConfig`). Đã tham khảo repo
  [termux-usbmuxd](https://github.com/LLOS-Lord/termux-usbmuxd) mà bạn gửi để
  đối chiếu — repo đó dùng `termux-usb` (Termux:API) để lấy quyền + file
  descriptor USB rồi giao cho `usbmuxd` gốc (C, dùng libusb) xử lý phần
  claim/giao thức, nên bản thân nó không gặp lỗi này (không đi qua đúng đoạn
  code Java `UsbDevice.getInterface()` có hành vi gộp cấu hình nói trên); đây
  là gợi ý hữu ích để xác nhận hướng sửa, nhưng dự án này vẫn giữ nguyên cách
  tiếp cận dùng thẳng `android.hardware.usb` (không qua Termux) như yêu cầu ban
  đầu của bạn.
- **Màn "Cài đặt" giờ có chức năng thật** (trước đây chỉ là văn bản tĩnh):
  lưu Apple ID (không lưu mật khẩu — xem lý do trong mã), và cho chọn server
  Anisette: "Tự động" hoặc chọn tay từ danh sách công khai thật lấy từ
  `servers.sidestore.io`, hoặc nhập URL riêng. Lựa chọn này được dùng thật ở
  tab Sideload và Thu hồi Certificate (trước đây cả hai tab luôn truyền
  `null`, bỏ qua hoàn toàn phần chọn server dù logic backend đã có sẵn).
- **Sửa lỗi đăng ký App ID thất bại ("An App ID with Identifier '...' is not
  available") + tự động tái sử dụng App ID khi đạt giới hạn 10/7 ngày:**
  bản trước gặp 2 vấn đề khi xử lý App ID:
  1. Apple trả lỗi resultCode **9401** ("is not available") khi bundle id bị
     trùng **TOÀN CẦU** (bundle id là chuỗi duy nhất trên toàn Apple Developer,
     không riêng tài khoản bạn — rất hay gặp với bundle id mặc định chưa đổi
     như `com.SideStore.SideStore` vì rất nhiều người khác cũng từng đăng ký
     đúng chuỗi đó bằng tài khoản riêng của họ). Bản cũ coi đây là lỗi chết,
     dừng luôn. Giờ `sideload_core.py::_resolve_app_id()` tự phát hiện đúng
     loại lỗi này (`developer_api.classify_app_id_error()`), tự thêm một hậu
     tố **ổn định** riêng cho tài khoản (không đổi mỗi lần chạy lại) vào
     bundle id rồi thử đăng ký lại — đúng cách AltStore/SideStore/iLoader xử
     lý tình huống này.
  2. Lỗi **giới hạn 10 App ID mới / 7 ngày** (tài khoản Apple ID miễn phí)
     giờ được xử lý theo đúng thứ tự ưu tiên: (a) nếu app đang cài đã có App
     ID đăng ký trong 7 ngày gần nhất (lưu trong `sideload_state.json`) thì
     dùng lại ngay, không tốn thêm hạn mức; (b) nếu chưa, tự tìm và xoá một
     App ID **cũ do chính tool này tạo** mà hiện không app nào trên thiết bị
     đang cắm dùng, để giải phóng chỗ rồi tạo App ID mới cho app hiện tại;
     (c) chỉ khi không còn cách nào khác mới mượn tạm một App ID có sẵn bất
     kỳ trên tài khoản (hành vi gốc, giữ lại làm phương án cuối).
- **Sửa lỗi "Your team has no devices from which to generate a provisioning
  profile" (resultCode 8220) khi tải Provisioning Profile:** nguyên nhân là
  một bug đảo ngược thứ tự tham số ở lời gọi
  `dev_api.register_device(udid, f"iPhone-{udid[:8]}")` trong
  `sideload_core.py` — chữ ký thật của hàm là
  `register_device(device_name, device_udid)`, nên Apple thực nhận
  `deviceNumber="iPhone-XXXXXXXX"` (chuỗi giả) và `name=<UDID thật>`. Kết quả:
  UDID thật của iPhone **không bao giờ thực sự được đăng ký** với Apple, dù
  log không báo lỗi gì ở bước đó (giá trị trả về của `register_device()`
  cũng chưa từng được kiểm tra). Tới bước tải Provisioning Profile, Apple
  thấy team chưa có thiết bị nào nên báo lỗi 8220 — đúng lỗi trong log bạn
  gửi. Đã sửa: gọi đúng thứ tự tham số, và giờ luôn kiểm tra kết quả đăng ký
  thiết bị — nếu thất bại sẽ dừng lại và báo lỗi rõ ràng ngay ở bước 1.5/6
  thay vì âm thầm đi tiếp rồi mới lộ lỗi khó hiểu ở bước tải profile.
- **Sửa lỗi ký IPA thất bại: `CANNOT LINK EXECUTABLE ".../libzsign.so":
  library "libssl.so.3" not found`:** `libzsign.so` (đóng gói sẵn trong
  `jniLibs/arm64-v8a/`) thực chất được build trong môi trường Termux —
  kiểm tra bằng `readelf -d` cho thấy nó cần `libssl.so.3`, `libcrypto.so.3`,
  `libc++_shared.so`, với RUNPATH trỏ thẳng vào
  `/data/data/com.termux/files/usr/lib` (chỉ tồn tại nếu máy có cài Termux).
  Trên điện thoại thường (không cài Termux, đúng mục tiêu ban đầu của app
  này), linker không tìm thấy 3 thư viện đó nên zsign luôn thoát ngay lập
  tức với "CANNOT LINK EXECUTABLE" — **không IPA nào ký được**, dù mọi bước
  trước đó (App ID, đăng ký thiết bị, Provisioning Profile) đã đúng.
  Đã sửa bằng cách đóng gói kèm 3 thư viện đó (lấy từ gói `openssl` và
  `libc++` chính thức của Termux cho kiến trúc aarch64, xác nhận tương thích
  ABI qua symbol version `OPENSSL_3.0.0` mà `libzsign.so` yêu cầu) dưới dạng
  asset (`assets/zsign_deps/`), tự giải nén ra thư mục riêng của app lúc
  chạy (`AppPaths.nativeDepsDir()`), rồi set `LD_LIBRARY_PATH` trỏ vào đó khi
  gọi zsign (`run_command(..., extra_env=...)` trong `utils.py`/
  `sideload_core.py`). Không đặt trực tiếp trong `jniLibs/` như `libzsign.so`
  được vì tên file có hậu tố phiên bản (`.so.3`) không khớp quy ước
  `lib*.so` mà trình cài đặt Android dùng để giải nén thư viện native.
- **Sửa nguyên nhân gốc của lỗi "Thiết bị không phản hồi đúng bắt tay phiên
  bản usbmux" (không bao giờ thấy hộp thoại "Trust This Computer?" trên
  iPhone, USB tự rút sau đó):** đối chiếu byte-for-byte `mux_usb.py` với mã
  nguồn tham khảo chính chủ
  [libimobiledevice/usbmuxd](https://github.com/libimobiledevice/usbmuxd)
  (`src/device.c`, `src/device.h`) phát hiện **3 lỗi định dạng giao thức
  độc lập nhau**, đều nằm ở `mux_usb.py`, đủ để một mình mỗi lỗi cũng làm
  hỏng hoàn toàn luồng ghép nối:
  1. Gói bắt tay phiên bản (version handshake) — gói ĐẦU TIÊN gửi đi khi vừa
     mở USB — phải dùng header **ngắn 8 byte** (`{protocol, length}`, không
     có `magic`/`tx_seq`/`rx_seq`), vì usbmuxd thật luôn tính
     `mux_header_size = (dev->version < 2) ? 8 : 16` và version bắt đầu ở
     0. Bản trước luôn gửi/nhận gói version bằng header 20 byte (đủ cả
     magic+seq) — sai kích thước ngay từ gói đầu tiên, khiến phần đọc
     payload lệch byte và bị tưởng là "thiết bị không phản hồi đúng" — đúng
     y lỗi bạn gặp.
  2. Với mọi gói tin sau bắt tay (version>=2), `mux_header` thật chỉ dài
     **16 byte**: `tx_seq`/`rx_seq` là **16-bit**, không phải 32-bit. Bản
     trước dùng định dạng 5×32-bit (20 byte) — lệch 4 byte mỗi gói, nên kể
     cả khi lỗi (1) được sửa riêng, mọi gói TCP-mô-phỏng dùng để mở kết nối
     tới lockdownd (bước tạo ra hộp thoại "Trust") vẫn sẽ hỏng.
  3. Gói `MUX_PROTO_SETUP` (bắt buộc phải gửi ngay sau khi bắt tay version
     thành công, trước khi coi thiết bị là sẵn sàng) **hoàn toàn chưa được
     gửi** ở bản trước.
  4. Lỗi thứ 4, độc lập, sẽ crash ngay khi mở kết nối TCP-mô-phỏng đầu
     tiên: trường "window" 16-bit được gán trực tiếp giá trị 131072 (vượt
     quá giới hạn 65535 của kiểu 16-bit) — usbmuxd thật luôn dịch phải 8
     bit (`>>8`) trước khi ghi vào trường này (và dịch trái lại khi đọc).
  Đã sửa cả 4 điểm trên trong `mux_usb.py`, theo đúng logic của
  `device_add()`, `device_version_input()`, `device_data_input()`,
  `send_packet()`/`send_tcp()` trong `device.c`. **Vẫn chưa test được với
  phần cứng thật** (xem mục 3) — đây là sửa theo đối chiếu mã nguồn tham
  khảo, không phải xác nhận bằng bắt gói tin thật; nhiều khả năng vẫn còn
  vấn đề ở tầng pairing/TLS (`device_link.py`) hoặc AFC sau khi bắt tay mux
  hoạt động đúng.
- **2 lỗi tiếp theo trong tầng pairing/TLS (`device_link.py`), tìm ra sau khi
  đối chiếu chéo 3 nguồn: mã nguồn thật `libimobiledevice`
  (`src/lockdown.c`, `common/userpref.c`), repo tham khảo
  [termux-usbmuxd](https://github.com/LLOS-Lord/termux-usbmuxd) (dùng để xác
  nhận đây không phải vấn đề kiến trúc — repo đó không tự viết lại giao thức
  lockdown, mà bọc thẳng `usbmuxd`/`libimobiledevice` gốc), và một bản triển
  khai lại giao thức lockdown bằng Python độc lập, đã được cộng đồng dùng
  rộng rãi trên nhiều hệ điều hành
  ([pymobiledevice3](https://github.com/doronz88/pymobiledevice3)) để xác
  nhận cách diễn giải tài liệu là đúng:**
  1. **Thiếu hoàn toàn bước tạo "DeviceCertificate" — rất có thể khiến
     lockdownd từ chối yêu cầu Pair ngay cả khi mux đã hoạt động đúng, trước
     khi kịp hiện hộp thoại "Trust This Computer?":** `PairRecord` gửi lên
     thiết bị **phải** chứa 5 khóa đúng tên
     `DeviceCertificate`/`HostCertificate`/`HostID`/`RootCertificate`/
     `SystemBUID` (xem `lockdownd_pair_record_to_plist()` trong
     `src/lockdown.c`). `DeviceCertificate` không phải là public key thô của
     thiết bị, mà là một **chứng chỉ X.509 mới**, chứa public key của thiết
     bị, do máy Android tự ký bằng khóa Root CA mà chính nó vừa sinh ra (xem
     `pair_record_generate_keys_and_certs()` trong `common/userpref.c`).
     Bản trước bỏ qua hoàn toàn bước ký này — gửi thẳng
     `"DevicePublicKey": <public key thô>` vào PairRecord, tức là trường bắt
     buộc `DeviceCertificate` **luôn bị thiếu** trong mọi yêu cầu Pair. Đã
     sửa: `_generate_host_identity()` giờ nhận public key thô của thiết bị,
     tự tạo chuỗi chứng chỉ Root CA (tự ký) -> Host cert -> Device cert (cả
     hai cert lá đều ký bởi Root CA, đúng cấu trúc `subject`/`issuer` rỗng,
     `BasicConstraints`/`KeyUsage`, và chọn SHA-1 thay vì SHA-256 khi
     `ProductVersion` thiết bị < 4.0.0 — khớp logic thật của
     `idevicepair`/`lockdownd`), rồi gửi đúng `DeviceCertificate` đã ký lên
     thiết bị.
  2. **Nâng cấp TLS (`start_session_tls`/`TlsLockdownClient._wrap_tls`) gần
     như chắc chắn thất bại trên OpenSSL hiện đại, dù mọi thứ ở tầng pairing
     đều đúng:** lockdownd trên iPhone dùng một stack TLS rất cũ (nhóm
     Diffie-Hellman yếu, thiếu các phần mở rộng hiện đại). Từ OpenSSL 3.x,
     mức bảo mật mặc định (`@SECLEVEL=2`) sẽ từ chối thẳng handshake kiểu
     này với các lỗi khó hiểu như `dh key too small`,
     `sslv3 alert handshake failure`, hoặc `certificate verify failed` — đây
     là vấn đề tương thích đã được biết đến rộng rãi khi nói chuyện với
     lockdownd bằng thư viện TLS hiện đại (không phải lỗi logic pairing/cert
     ở trên), và là lý do `pymobiledevice3` phải tự hạ `@SECLEVEL=0` và bật
     lại cờ legacy renegotiation (`SSL_OP_LEGACY_SERVER_CONNECT`) một cách
     tường minh thay vì dùng cấu hình `ssl.SSLContext` mặc định. Bản trước
     dùng `ssl.SSLContext` mặc định (không có 2 chỉnh sửa này) — đã sửa
     `_wrap_tls()` để áp dụng đúng
     `set_ciphers("ALL:!aNULL:!eNULL:@SECLEVEL=0")` (trên OpenSSL) và bật
     `SSL_OP_LEGACY_SERVER_CONNECT`, đồng thời đổi biên phiên bản TLS từ
     `TLSv1`-mặc định sang `TLSv1_2`–`TLSv1_3` khớp cấu hình đã được xác
     nhận hoạt động trong `pymobiledevice3`.
  **Vẫn chưa test được với phần cứng thật** — 2 lỗi trên được xác nhận qua
  đối chiếu 3 nguồn độc lập (rất đáng tin cậy về mặt lý thuyết), nhưng AFC và
  các bước sau TLS vẫn có thể còn vấn đề chưa lộ ra vì chưa từng chạy tới
  được bước đó trên thiết bị thật.

---

## 3. Rủi ro đã biết (đọc trước khi dùng)

Dự án này được viết trong môi trường Replit, **không có SDK Android, không
có Gradle, không có trình giả lập/thiết bị Android, và không có iPhone thật**
để build hay test. Vì vậy:

| Thành phần | Mức độ tin cậy | Ghi chú |
|---|---|---|
| UI Compose, điều hướng, luồng gọi Python | Cao | Logic thuần Kotlin/Compose, không phụ thuộc phần cứng đặc thù |
| `apple_auth.py`, `developer_api.py` | Cao | Copy gần như nguyên vẹn từ bản CLI gốc đã hoạt động trong Termux, chỉ đổi `input()` |
| `utils.py` (giải nén/ký IPA, đọc plist) | Cao | Thuần Python, không đổi so với bản gốc |
| USB Host API claim/bulk transfer (`UsbTransport.kt`) | Trung bình | Dùng đúng API chuẩn của Android, nhưng chưa test với iPhone thật cắm qua USB |
| **`mux_usb.py`** (giao thức usbmux tự viết lại) | **Thấp — CHƯA KIỂM CHỨNG TRÊN PHẦN CỨNG** | 4 lỗi định dạng gói tin đã sửa sau khi đối chiếu byte-for-byte với `libimobiledevice/usbmuxd` (mục 2) — về lý thuyết khớp đúng giao thức thật, nhưng vẫn CHƯA có lần bắt tay/pairing nào chạy qua trên iPhone/Android thật để xác nhận |
| **`device_link.py`**, đặc biệt pairing (`pair_device`) và TLS (`TlsLockdownClient`/`start_session_tls`) | **Thấp-Trung bình — CHƯA KIỂM CHỨNG TRÊN PHẦN CỨNG** | 2 lỗi đã sửa sau khi đối chiếu chéo `libimobiledevice` + `pymobiledevice3` (mục 2): thiếu `DeviceCertificate` trong PairRecord, và thiếu hạ `@SECLEVEL`/bật legacy renegotiation cho TLS. Về lý thuyết khớp đúng cấu hình đã xác nhận hoạt động trong `pymobiledevice3`, nhưng nâng cấp TLS qua `ssl.MemoryBIO` bơm tay là phần tự ráp nối — vẫn CHƯA có lần chạy thật nào tới được bước AFC/cài đặt |
| **AFC (đẩy file lên iPhone)** | **Thấp — CHƯA KIỂM CHỨNG** | Giao thức nhị phân tự triển khai lại theo tài liệu |

**Vì sao rủi ro nằm ở lớp USB:** Chạy `usbmuxd` (daemon chuẩn của Apple/
libimobiledevice để nói chuyện với iPhone qua USB) **không cần root** trên
Android là một vấn đề **chưa có giải pháp được cộng đồng công nhận** — xem
các issue còn mở trên GitHub của dự án
[libimobiledevice/usbmuxd](https://github.com/libimobiledevice/usbmuxd) từ
khoảng 2019–2021 bàn về việc này. `mux_usb.py` trong repo này là một nỗ lực
tự triển khai lại giao thức usbmux **trực tiếp trên USB Host API của
Android** (không qua daemon), dựa trên đọc mã nguồn tham khảo
(`usbmuxd/src/usb.c`, `usbmuxd/src/usb.h`, `usbmuxd/src/device.c`) — **không
phải** bằng cách chạy thử và bắt gói tin thật. Có khả năng cao một số chi
tiết byte-level (thứ tự trường trong header, hằng số, cách xử lý window/ACK
của lớp mô phỏng TCP) cần chỉnh sửa sau khi bạn build và test với:

- Android Studio + một điện thoại Android thật (USB Host API không hoạt động
  tốt trên trình giả lập).
- Một iPhone/iPad thật cắm qua cáp USB (không qua hub USB nếu có thể).
- Công cụ bắt gói USB (vd `Wireshark` + `usbmon` trên máy Linux dùng làm cầu
  nối để so sánh, hoặc log chi tiết `Log.d` thêm vào `UsbTransport.kt`).

**Nếu ghép nối/cài đặt qua USB không hoạt động ngay lần đầu**, đây là nơi cần
xem đầu tiên — không phải lỗi ở logic ký hay ở Apple Developer API (hai phần
đó dùng lại gần như nguyên vẹn logic đã chạy được trong bản CLI gốc).

**Phạm vi sử dụng dự định:** chỉ dùng với thiết bị và Apple ID của chính bạn.

---

## 4. Build bằng Android Studio (khuyến nghị để tự test/debug)

1. Cài **Android Studio** (bản mới, hỗ trợ AGP 8.6.x trở lên) và **JDK 17**.
2. Giải nén zip này, rồi mở **thư mục vừa giải nén** bằng Android Studio
   ("Open" -> chọn thư mục đó — đây là gốc dự án, đã chứa sẵn
   `settings.gradle.kts` ngay bên trong).
3. Android Studio sẽ tự tải Gradle wrapper (repo không commit sẵn file
   `gradle-wrapper.jar` — môi trường viết code này không có Java/Gradle để
   tạo file đó). Nếu Android Studio không tự tạo wrapper, chạy trong
   Terminal của Android Studio (nơi có sẵn Gradle đi kèm IDE):
   ```
   gradle wrapper --gradle-version 8.9
   ```
4. Đồng bộ Gradle ("Sync Now"), rồi Build > Build APK(s), hoặc chạy trực tiếp
   lên điện thoại Android thật đã bật "USB debugging" bằng nút Run.
5. Cài file `.ipa` cần sideload vào máy Android (vd tải xuống thư mục
   Downloads) để có thể chọn bằng nút "Chọn file IPA" trong app.

**Lưu ý phần cứng:** cần một điện thoại Android thật hỗ trợ **USB Host mode**
(USB OTG) và một cáp/adapter phù hợp để vừa cấp nguồn vừa truyền dữ liệu tới
iPhone (nhiều điện thoại Android cần cáp USB-C-to-Lightning "data", không
phải cáp sạc thường; hoặc dùng adapter USB OTG + cáp Lightning gốc của Apple).

---

## 5. Build tự động qua GitHub Actions (không cần máy có Android Studio)

Workflow tại **`.github/workflows/build-apk.yml`** (đã có sẵn ở gốc thư mục
zip này) sẽ tự build APK debug mỗi khi có push/PR. Cách lấy APK:

1. Giải nén zip này, tạo một repository GitHub mới (hoặc dùng repo có sẵn),
   rồi đẩy (push) toàn bộ nội dung đã giải nén lên đó — sao cho
   `settings.gradle.kts` và thư mục `.github/` nằm ngay ở gốc repository,
   không nằm trong thư mục con.
2. Vào tab **Actions** trên GitHub -> chọn lần chạy workflow "Build APK" mới
   nhất -> kéo xuống mục **Artifacts** -> tải `superalpha-sideload-debug-apk`.
3. Cài file APK đó lên điện thoại Android (cần bật "Cài từ nguồn không xác
   định" cho ứng dụng bạn dùng để mở file APK).

Vì môi trường CI không có iPhone thật cắm qua USB, workflow này **chỉ xác
nhận code biên dịch được**, không xác nhận luồng USB/ghép nối/cài đặt hoạt
động — việc đó cần test thủ công theo mục 3 và 4.

### Lỗi thường gặp: "Couldn't find Python 3.11"

Chaquopy cần chạy `pip` **ngay trên máy build** (không phải trên máy ảo
Android) để tải các thư viện Python (`requests`, `cryptography`, `srp`), nên
nó cần tìm được một Python 3.11 thật trên `PATH` của máy build — khớp đúng
major.minor với `version = "3.11"` khai báo trong `app/build.gradle.kts`.
Runner `ubuntu-latest` của GitHub Actions không đảm bảo có sẵn Python 3.11
trên `PATH`, nên workflow có bước **"Set up build Python (3.11, required by
Chaquopy)"** (dùng `actions/setup-python@v5`) chạy trước bước build — bước
này **bắt buộc phải có** trước bước "Build debug APK", nếu không Gradle sẽ
báo lỗi này khi chạy task `:app:installDebugPythonRequirements`. Nếu bạn tự
build local bằng Android Studio (mục 4) và gặp lỗi tương tự, hãy cài Python
3.11 trên máy và đảm bảo nó có trên `PATH` (Chaquopy tự tìm bằng lệnh
`python3.11`, rồi `python3`, rồi `python`; xem
[tài liệu buildPython](https://chaquo.com/chaquopy/doc/current/android.html#buildpython)).

---

## 6. Sử dụng ứng dụng

1. Mở app, vào tab **Sideload**.
2. Cắm iPhone/iPad vào điện thoại Android qua cáp USB (xem lưu ý phần cứng ở
   mục 4). Nếu app đang mở sẵn, việc cắm dây giờ tự động thử kết nối luôn;
   nếu chưa, bấm **Kết nối** — Android sẽ hỏi quyền truy cập USB, chọn "Cho
   phép" (và có thể tick "luôn dùng cho thiết bị này" để không phải hỏi lại).
3. Bấm **Chọn file IPA**, chọn file `.ipa` cần cài.
4. Nhập **Apple ID** và **mật khẩu**. Nếu tài khoản bật xác thực 2 yếu tố,
   app sẽ hiện hộp thoại yêu cầu nhập mã 6 số gửi tới thiết bị Apple khác của
   bạn — nhập mã rồi bấm xác nhận, sau đó **bấm "Ký & Cài đặt" lại lần nữa**
   (2FA chỉ cần xác thực một lần trong phiên, việc đăng nhập lần đầu dừng lại
   sau khi 2FA xong để bạn bấm chạy lại với phiên đã xác thực).
5. Theo dõi tiến trình trong ô **Nhật ký**. Lần đầu ghép nối với một iPhone,
   màn hình iPhone sẽ hiện hộp thoại "Trust This Computer?" — bấm **Trust**
   và nhập mã khoá màn hình của iPhone khi được hỏi.
6. Nếu gặp lỗi "đã đạt giới hạn certificate" (tài khoản Apple ID miễn phí chỉ
   được 1 certificate Development hoạt động cùng lúc), app sẽ **tự động thu
   hồi certificate cũ nhất** để lấy chỗ tạo mới — không cần vào tab "Thu hồi
   Certificate" thủ công trừ khi bạn muốn chủ động dọn certificate.

### Khác biệt nhỏ so với bản CLI gốc

- **Thu hồi certificate không còn hỏi xác nhận y/n cho từng cái** như bản CLI
  gốc — vì trên UI, việc bạn bấm nút "Thu hồi" (sau khi đã chọn rõ certificate
  nào hoặc "all") đã là một hành động xác nhận rõ ràng, không cần hỏi lại lần
  hai như khi gõ lệnh trong terminal.
- Khi đạt giới hạn certificate, thay vì hỏi bạn có muốn thu hồi cái cũ nhất
  hay không (như bản CLI), app **tự động thu hồi** — vì trên điện thoại không
  có "terminal" để hỏi/trả lời tương tác giữa chừng một tác vụ nền dài; nếu
  bạn muốn kiểm soát certificate nào bị thu hồi, hãy chủ động dùng tab "Thu
  hồi Certificate" trước khi bấm Sideload.

---

## 7. Lưu ý pháp lý / đạo đức

Công cụ này chỉ nên dùng để **cài ứng dụng lên chính thiết bị của bạn, bằng
chính Apple ID của bạn** (sideload cá nhân, không phân phối lại app đã ký cho
người khác). Việc dùng chứng chỉ ký của người khác, chia sẻ file IPA đã ký
cho nhiều người, hoặc dùng để cài phần mềm crack/vi phạm bản quyền đều nằm
ngoài phạm vi và mục đích của công cụ này, và có thể vi phạm điều khoản dịch
vụ của Apple.
