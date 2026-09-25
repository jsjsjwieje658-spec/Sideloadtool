# SideloadTool Patch v49 — Kết nối được iPhone + sửa luồng ký/cài như bản Termux

## Tóm tắt

v20→v48 vá chồng 27+ lần lên một "mini usbmuxd" tự viết mà chưa bao giờ chạy đúng.
v49 **làm lại tầng USB/usbmux theo đúng cách termux-usbmuxd hoạt động** (usbmuxd
upstream trên fd của `termux-usb`), đồng thời sửa ở tầng libimobiledevice và
Python đúng những lỗi đã gặp và đã sửa ở công cụ Termux (429, App ID, extension,
IPA phải mang đúng App ID đã nộp).

---

## 1. termux-api + termux-usbmuxd kết nối iPhone thế nào

```
termux-usb -e usbmuxd_proxy …      (termux-api: UsbAPI.java)
  └─ UsbManager.openDevice(dev).getFileDescriptor()   ← CHỈ mở, không claim
       └─ fd → biến môi trường → usbmuxd upstream (libusb_wrap_sys_device)
            ├─ set_valid_configuration(): chọn configuration CÓ interface usbmux
            ├─ libusb_claim_interface()
            ├─ VERSION → SETUP → TCP-over-USB (device.c), MỘT luồng RX cho mọi kết nối
            └─ socket usbmuxd ← idevicepair / ideviceinstaller (libimobiledevice gốc)
```

Nó "100% hoạt động" vì mọi thứ phía sau fd là code upstream. App này giờ làm
**đúng chuỗi đó trong tiến trình app**: Kotlin chỉ `openDevice()` → fd →
`usb_fd_bridge.c` (libusb) → `usbmuxd_server.c` (port device.c/client.c/conf.c của
usbmuxd) → libusbmuxd + libimobiledevice **nguyên bản**.

---

## 2. Nguyên nhân gốc — tầng USB / usbmux (vì sao "không kết nối được iPhone")

| # | Lỗi | Hậu quả | Sửa |
|---|---|---|---|
| 1 | **Không đổi USB configuration.** iPhone có 4–6 config; Android để ở config 1 (chỉ PTP). Interface usbmux chỉ có ở config 3/4 (5/6 máy mới). | `libusb_claim_interface` → **NOT_FOUND** (đúng dòng log trong PATCH_V48), mọi transfer lỗi IO, phải chạy "Android JNI mode". | Chọn config cao nhất có usbmux + `libusb_set_configuration` như upstream `set_valid_configuration()`. |
| 2 | Mỗi kết nối TCP có **luồng riêng tự đọc endpoint bulk-IN dùng chung**, không lọc theo cổng. | Khi libimobiledevice mở kết nối thứ 2 (lockdownd luôn còn mở lúc StartService/AFC/installation_proxy), SYN+ACK/dữ liệu bị luồng kia "ăn" → RST, "no matching socket", treo. | 1 luồng đọc USB duy nhất + 1 luồng lõi (poll) phân kênh theo (sport, dport), port từ `device.c`. |
| 3 | Đọc 8/16 byte header rồi mới đọc phần sau (coi bulk như stream). | `LIBUSB_ERROR_OVERFLOW`, mất dữ liệu; phải chồng clear_halt/flush. | Đọc nguyên transfer 16 KiB (USB_MRU), gom gói theo trường length. |
| 4 | Không gửi **ZLP** khi độ dài gói chia hết wMaxPacketSize (đường libusb). | iPhone chờ mãi phần còn lại của gói → treo ngẫu nhiên khi chép IPA. | Gửi ZLP như `usb_send()` upstream. |
| 5 | Dữ liệu gửi với cờ **PSH\|ACK**; `rx_seq` = copy `tx_seq` của iPhone (comment v47 ghi upstream làm vậy — sai, upstream: `dev->rx_seq = ntohs(mhdr->rx_seq)`); đóng bằng **FIN** rồi đọc USB từ luồng khác. | Lệch giao thức so với usbmuxd thật. | Đúng upstream: chỉ ACK, copy rx_seq, đóng bằng RST. |
| 6 | Không flow-control, payload tới 64 KiB. | Vượt USB_MTU (49152) / cửa sổ iPhone. | `sendable = rx_win − (tx_seq − rx_ack)`, ≤ 49116 byte/gói; tx_ack theo byte đã giao cho client. |

## 3. Nguyên nhân gốc — giao thức usbmuxd (vì sao pair xong vẫn "như chưa pair")

| # | Lỗi | Sửa |
|---|---|---|
| 7 | `USBMUXD_SOCKET_ADDRESS` bị đặt thành **đường dẫn trần**. libusbmuxd 2.0.2 chỉ hiểu `UNIX:/path` hoặc `host:port` → ReadBUID/ReadPairRecord/SavePairRecord nối tới `/var/run/usbmuxd` (không tồn tại). | Đặt `UNIX:<filesDir>/usbmuxd.sock`; bỏ `-Wl,--wrap` (libusbmuxd chạy nguyên bản). |
| 8 | ReadBUID trả BUID toàn số 0 kèm `MessageType=Result` → libusbmuxd **vứt bỏ** BUID. ListDevices cũng vậy. | Phản hồi đúng định dạng upstream (không MessageType); SystemBUID ngẫu nhiên, lưu bền. |
| 9 | **SavePairRecord không lưu gì** (trả OK), ReadPairRecord luôn rỗng. | Lưu `filesDir/lockdown/<UDID>.plist`; chưa có → ENOENT (libimobiledevice hiểu "chưa pair"). Mở lại app không phải Trust lại. |
| 10 | TCP listener 127.0.0.1:27015 mở cho mọi app trên máy. | Chỉ Unix socket trong thư mục riêng của app. |

## 4. Tầng libimobiledevice (jni_bridge_imd.c) + build

| # | Lỗi | Sửa |
|---|---|---|
| 11 | `lockdownd_start_service()` trên client **không có phiên SSL** → luôn bị từ chối. | `afc_client_start_service` / `instproxy_client_start_service` (tự handshake như ideviceinstaller). |
| 12 | `PackageType=Developer` cho **file .ipa** (chỉ dùng cho thư mục .app), đường dẫn `/PublicStaging/...` có `/` đầu. | Như ideviceinstaller: `PublicStaging/<tên>.ipa`, không PackageType; chờ "Complete", in lỗi cụ thể (ApplicationVerificationFailed…) kèm gợi ý. |
| 13 | libimobiledevice 1.3.0 + OpenSSL 3.2: iOS không hỗ trợ RFC 5746 → "unsafe legacy renegotiation disabled" → SSL_ERROR. | `build_all.sh` backport `SSL_OP_LEGACY_SERVER_CONNECT` + `SSL_OP_IGNORE_UNEXPECTED_EOF` từ libimobiledevice master. |
| 14 | UDID tạm "pending-device" dùng làm tên pair record; iSerialNumber 24 ký tự thiếu `-`. | UDID thật từ lockdown (`UniqueDeviceID`), chèn `-` cho UDID kiểu mới như usbmuxd. |
| 15 | Gọi lại `nativeSetUsbFd` với cùng fd → phá phiên đang chạy. Attach/detach JVM mỗi dòng log/transfer. | Cùng fd = dùng lại; JNIEnv cache theo luồng. |

## 5. Python — cùng các lỗi như tool Termux lúc đầu

| # | Lỗi | Sửa |
|---|---|---|
| 16 | **GSA 429** (requests.Session giữ socket keep-alive; edge Apple trả 429 cho request thứ 2 trên cùng socket) + X-MMe-Client-Info `com.apple.dt.Xcode` bị chặn (503). | Dùng đúng `apple_auth.py` đã sửa của bản Termux: mỗi request gsa.apple.com một socket (`Connection: close`), gửi lại ≤3 lần, định danh `com.apple.akd`. |
| 17 | `AppleAuth()` không nhận `input_func` → 2FA gọi `input()` → EOFError trên Android. | 2FA qua `UiPrompt` (dialog). |
| 18 | **zsign ký FILE IPA GỐC** sau khi đã đổi bundle id trong thư mục giải nén → bundle id mới bị bỏ, profile lệch → ApplicationVerificationFailed. | Đổi bundle id → tải profile → ký **thư mục .app đã sửa**. |
| 19 | Extension (.appex) không có App ID/profile riêng, zsign 1 `-m`. | App ID + profile cho từng .appex, zsign nhiều `-m` (app chính trước), không `-b`. |
| 20 | UDID fallback cuối là **đường dẫn filesDir**; `"2fa_completed"` → KeyError. | Kết nối + ghép nối iPhone TRƯỚC, lấy UDID thật; xử lý đúng các trường hợp. |
| 21 | Hộp thoại nhập liệu (`PromptDialog.kt`) đặt câu hỏi làm *label* của ô nhập 1 dòng → câu hỏi nhiều dòng ("bỏ extension?", DSID) bị cắt, không đọc được; không có nút huỷ; `UiPrompt.submitResponse()` dùng `SynchronousQueue.put()` trên main thread → bấm "Gửi" 2 lần là **treo app (ANR)**. Đường này nay mới thật sự được dùng vì 2FA đi qua dialog (#17). | Câu hỏi hiện thành nội dung (cuộn được), ô nhập riêng; mã 2FA dùng bàn phím số, lọc 6 chữ số; thêm nút **Huỷ** (gửi rỗng = huỷ/bỏ qua phía Python); `offer()` không bao giờ chặn main thread. |

---

## 6. Kiểm chứng (không cần iPhone)

```bash
python3 tests/python/test_sideload_core.py        # luồng ký & cài: 30 kiểm tra
bash tests/native-host/build_host_deps.sh         # libplist/libusbmuxd/libimobiledevice host
bash tests/native-host/run.sh                     # usbmuxd nội bộ ↔ iPhone giả lập: 28 kiểm tra
```

`run.sh` dùng **libusbmuxd 2.0.2 + libimobiledevice 1.3.0 thật** (cùng phiên bản CI
build cho Android) và `fake_iphone.py` — phía thiết bị của giao thức mux, ghi lại
mọi vi phạm so với usbmuxd upstream. `mock_libusb.c` mô phỏng iPhone đang ở
config 1 như trên Android. Kết quả: đổi config 1→4, claim OK, bắt tay VERSION/SETUP,
ListDevices/BUID/pair record qua libusbmuxd, lockdownd QueryType/GetValue qua
libimobiledevice, 3 kết nối echo song song (≈4,3 MiB) khi lockdownd vẫn mở, đẩy
6 MiB (như chép IPA), nhận 3 MiB (gói 40 KB gom qua nhiều transfer), từ chối/RST
đúng — **0 vi phạm**. CI chạy thêm job `protocol-tests` cho các test này.

Mã cũ với cùng libusb giả lập: `interface 1 NOT_FOUND — libusb bulk transfer sẽ
fail`, không gọi `set_configuration`, gửi VERSION → `LIBUSB_ERROR_IO`.

## 7. Log kỳ vọng trên máy thật

```
[usb] iPhone pid=0x12a8: 4 configuration, đang dùng config 1
[usb] ✅ Đổi USB configuration 1 → 4 (interface usbmux nằm ở config 4)
[usb] ✅ Claim interface 1 (config 4) — ep_in=0x85 ep_out=0x04 maxpkt=512
[usbmux] Gửi VERSION 2.0 tới iPhone (lần 1)
[usbmux] ✅ Đã bắt tay mux v2.0 với iPhone (UDID …)
[imd] ✅ iPhone — iOS 17.x — UDID …
[pair] ⏳ iPhone đang hiện "Tin cậy máy tính này?" — bấm Tin cậy + nhập mật mã
[usbmux] ✅ Đã lưu pair record cho …
[pair] ✅ Ghép nối + phiên SSL OK
[afc] Đang chép IPA... 50% …
[instproxy] ✅ Cài đặt thành công!
```

Nếu vẫn lỗi, gửi các dòng `[usb]`, `[usbmux]`, `[lockdown]`/`[pair]`, `[instproxy]`
(và mục Chẩn đoán). Bật log từng gói: đặt biến môi trường `SIDELOAD_MUX_VERBOSE=1`.

## 8. Lưu ý

1. Cáp **data** (không phải cáp chỉ sạc), iPhone **mở khoá** khi ghép nối.
2. Gỡ APK cũ trước khi cài APK mới (tránh dùng lại lib cũ); pair record cũ của bản
   v48 không dùng được (bản cũ không lưu gì), sẽ phải bấm Tin cậy một lần.
3. Sau khi cài app lần đầu: Cài đặt > Cài đặt chung > Quản lý VPN & Thiết bị >
   tin cậy Apple ID. iOS 16+: bật Chế độ nhà phát triển nếu được hỏi.
4. `jni_bridge.c`, `usbmux.c`, `lockdown.c`, `pairing.c`, `afc.c`,
   `install_proxy.c`, `plist_util.c` là mã "Mode 2/3" cũ — **không được biên dịch**
   (CMakeLists chỉ build 4 file), giữ lại để tham khảo.

*Patch v49 — 2026-09-24*

---

# SideloadTool Patch v50 — Làm lại UI (đẹp + mượt) và bỏ 2 tab thừa

*2026-09-25*

## 9. Thay đổi

| # | Vấn đề | Sửa |
|---|---|---|
| 22 | **UI lag trên máy thật**: (a) mỗi dòng log → `List.copy + takeLast(500)` + phát StateFlow → recompose toàn màn; (b) `LazyColumn` không key → mọi dòng đang thấy compose lại cho MỖI dòng log mới; (c) `animateScrollToItem()` chạy liên tục khi log dồn; (d) copy file IPA (có thể vài trăm MB) ngay trên main thread trong callback chọn file; (e) APK debug (Compose debug không tối ưu, không R8). | (a) `LogBuffer`: ring buffer 2000 dòng, gộp xuất snapshot **1 lần/100 ms**; (b) `key = chỉ số toàn cục` cho từng dòng; (c) `scrollToItem` không animation + chỉ auto-cuộn khi người dùng đang ở cuối (cuộn lên đọc → hiện nút "Cuộn xuống cuối"); (d) copy trên `Dispatchers.IO`; (e) **bản release R8 minify** (keep rule cho `com.superalpha.sideload.**` + `com.chaquo.python.**` vì Python/JNI gọi Kotlin theo tên class). |
| 23 | 2 tab thừa **"Ghép nối"** + **"Đăng ký UDID"**: cả hai việc đều đã diễn ra NGẦM trong luồng "Cài IPA" (`do_sideload` Bước 0/5: connect → pair → đăng ký UDID). | Xoá `PairingScreen.kt`, `RegisterDeviceScreen.kt`, mục nav tương ứng; xoá `FileProvider` + `xml/file_paths.xml` (chỉ dùng cho tab Ghép nối). Thay bằng **thẻ trạng thái iPhone** ngay trên màn chính (`DeviceCard`): cáp USB / chờ Trust (kèm hướng dẫn bấm Tin cậy) / sẵn sàng — poll trạng thái 1 giây/lần. |
| 24 | Giao diện cũ: chữ dày đặc, không phân nhóm, `Divider` deprecated, màu Material3 mặc định (không container màu riêng). | Thiết kế lại toàn bộ: theme dark "Super Alpha" đủ bộ màu M3, thẻ bo góc + viền mảnh, banner đầu màn, nút chính gradient vàng, console log màu theo mức độ (lỗi đỏ/cảnh báo cam/thành công mint/tiến trình xanh), thanh phân đoạn cho chọn chứng chỉ thu hồi. 3 tab còn lại: **Cài IPA · Thu hồi cert · Cài đặt**. |

## 10. Lưu ý khi nâng cấp lên v50

1. **Cài artifact `superalpha-sideload-release`** (khuyến nghị — mượt hơn). Còn
   `superalpha-sideload-debug` để dự phòng.
2. Bản release ký bằng **debug keystore của CI runner** — chữ ký khác bản debug
   cũ → phải **gỡ app cũ một lần** rồi cài bản mới. Gỡ app làm mất pair record
   (nằm trong dữ liệu app) → khi kết nối iPhone lần đầu phải bấm **"Tin cậy"**
   lại một lần. Máy đã từng đăng ký UDID vào team Apple thì không cần làm lại.
3. Luồng sử dụng mới: cắm cáp → thẻ iPhone hiện "sẵn sàng" → chọn IPA → nhập
   Apple ID + mật khẩu → "Ký & Cài đặt". Mọi bước ghép nối/đăng ký UDID tự chạy.

## 11. v50.1 — Sửa "Không cài được IPA: [afc] afc_file_write lỗi 30 sau 0 byte"

| # | Lỗi | Sửa |
|---|---|---|
| 25 | **Bước 5/5 thất bại ngay byte đầu**: `afc_file_write` trả `AFC_E_MUX_ERROR (30)`. Nguyên nhân: `AFC_CHUNK` cũ = **1 MB** một lần gọi. libusbmuxd 2.0.2 để socket O_NONBLOCK và `usbmuxd_send()` chỉ làm MỘT syscall `send()` không loop — gói 1 MB bị ghi **đền gói ~200 KB** (buffer socket unix); iPhone nhận gói AFC cụt theo `entire_length` → chờ mãn phần còn lại → không reply → `afc_receive_data()` timeout → lỗi 30. Lỗi có từ v49 (lần đầu test cài thật trên máy — các lần trước chỉ tới connect/pair/UDID với gói nhỏ). | `AFC_CHUNK` = **32 KB**. Mỗi `afc_file_write()` đã chờ reply của chính nó (libimobiledevice 1.3.0) nên socket luôn drain sạch giữa 2 lần ghi — 32 KB vừa buffer mọi máy, 26 MB ≈ 850 vòng ≈ 2–4 s. Tầng usbmuxd/USB giữ nguyên (`usb_bridge_bulk_write` vốn loop + ZLP). |

*2026-09-25 (v50.1)*


---

# SideloadTool Patch v51 — Tự thu hồi cert khi hết chỗ + đăng nhập Apple ID lần đầu

*2026-09-25*

## 12. Thay đổi

| # | Vấn đề | Sửa |
|---|---|---|
| 26 | Khi tài khoản Apple ID đã đủ **2 certificate Development** (giới hạn miễn phí), Bước 2/5 của luồng sideload fail: `❌ Không tạo được certificate...` — người dùng phải vào tab "Thu hồi chứng chỉ" revoke tay rồi chạy lại từ đầu. | **Tự động thu hồi**: khi tạo cert thất bại VÀ tài khoản đang có ≥ 2 cert → tự revoke (ưu tiên cert do tool tạo — machine name `ios-sideload-tool*`; nếu không có cert nào của tool thì revoke tất cả), chờ 3 s cho Apple xử lý, rồi **tạo lại MỘT lần**. Không đủ 2 cert thì không revoke gì (lỗi là do mạng/session). |
| 27 | Mỗi lần ký IPA / thu hồi cert đều phải gõ lại Apple ID + mật khẩu; không có khái niệm "tài khoản" trong app. | **Cổng đăng nhập**: lần đầu mở app (hoặc sau khi Đăng xuất) hiện màn `LoginScreen` — nhập Apple ID + mật khẩu MỘT LẦN, app xác thực với Apple (`do_login`, 2FA hiện dialog nếu cần) rồi lưu riêng tư trên máy (`allowBackup=false` — không bao giờ rời thiết bị). Các màn Cài IPA / Thu hồi cert dùng luôn thông tin đã lưu (hiện tên tài khoản, không còn ô nhập). **Tab Cài đặt** thêm nút **"Đăng xuất Apple ID"** (có hộp thoại xác nhận) — sau khi đăng xuất, lần mở app tiếp theo phải đăng nhập lại. Có nút "Lưu mà không xác thực" cho trường hợp mạng/Anisette chập chờn. |

## 13. Ghi chú

1. Mật khẩu Apple ID được lưu **plaintext trong storage riêng tư của app** (người dùng
   yêu cầu tường minh). Không root thì không app nào đọc được; `allowBackup=false` nên
   không vào backup cloud. Muốn xoá: Đăng xuất trong Cài đặt (hoặc gỡ app).
2. Đăng xuất KHÔNG xoá pair record iPhone (ghi trong `filesDir/lockdown/`) — cắm lại
   không phải bấm Tin cậy lại.
3. Nâng cấp từ v50/v50.1: cài đè trực tiếp (cùng chữ ký); lần đầu mở v51 sẽ hiện màn
   đăng nhập (v50 chưa lưu mật khẩu).


---

# SideloadTool Patch v52 — Khởi động nhanh (hết màn đen 0.5 s) + mượt hơn

*2026-09-25*

## 14. Thay đổi

| # | Vấn đề | Sửa |
|---|---|---|
| 28 | **Mở app bị "màn đen ~0.5 s rồi mới vào"**: `SuperAlphaApp.onCreate()` chạy `Python.start(AndroidPlatform)` (Chaquopy: 300–800 ms — giải nén bootstrap + load libpython + init interpreter) và `DeviceNative.init()` (loadLibrary 5.5 MB + nativeInit + usbmuxd threads) **ngay trên main thread** trước khi frame đầu tiên vẽ xong; trong lúc chờ, cửa sổ hiện `windowBackground` màu gần đen (#0B0F14). | **Defer sang thread nền**: hai việc nặng chạy song song với frame đầu (appScope + Dispatchers.IO), UI hiện gần như tức thì. Action đầu cần Python/bridge (`ensurePython()` / `getBridge()`) có lock — warm-up chưa xong thì chờ tối đa vài trăm ms. `DeviceNative.getBridge()` làm race-safe (mọi đường tạo instance trong cùng một khối synchronized — giữ nguyên fix v21 một-bridge-một-nativeInit). `HomeViewModel.nativeBridge` chuyển `by lazy` để constructor không chờ. |
| 29 | Màn chờ cold start màu đen trơn → cảm giác app chậm dù tổng thời gian ổn. | **Splash đúng thương hiệu**: API 26–30 windowBackground = layer-list (nền #101823 — đúng màu đỉnh gradient của app + logo giữa màn); API 31+ dùng splash hệ thống (`windowSplashScreenBackground`) cùng màu. Màu status/navigation bar khớp luôn → chuyển tiếp mượt, không còn "nháy đen". |

*Ghi chú: nếu bạn vẫn thấy "hơi lag khi dùng" — kiểm tra lại đang cài artifact
`superalpha-sideload-release` (R8) chứ không phải `-debug`; bản debug của
Compose chậm hơn rõ rệt.*
