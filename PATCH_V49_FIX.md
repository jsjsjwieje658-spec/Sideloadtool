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
