# Báo cáo sửa lỗi v73 — 2026-10-26

**Repo:** `jsjsjwieje658-spec/Sideloadtool` (fork của `LLOS-Lord/Sideloadtool`), HEAD `e4cbe64`.
**Branch fix:** `fix/v73-failover-pairing-apple-compat`.
**Phạm vi:** 3 lỗi người dùng báo + rà soát tương thích Apple/iOS cuối tháng 10/2026.

---

## 0. Cách làm (không đoán bừa)

1. `git ls-files` → **100 file**, 19.979 dòng text; liệt kê cả file ẩn/symlink/binary.
2. Đọc lần lượt: toàn bộ `app/src/main/python/*.py` (apple_auth 1593 → ~1900 dòng, sideload_core 1411, developer_api 549, utils, device_link, config_manager, mux_usb), toàn bộ `jni_bridge_imd.c` (JNI thật sự được CMake biên dịch), `usbmuxd_server.c` (pair record), `CMakeLists.txt`, Kotlin bridge/UI (`DeviceNative`, `NativeBridge`, `AppConfig`, `PythonBridge`, `HomeViewModel`, `SettingsScreen`, `PairingFileManagerScreen`), CI, và 2 file test Python.
3. **Đo trực tiếp** các server anisette (không tin README): `curl` tới `/`, `/v3/client_info`, `/v3/get_headers` của 7 host, bật/tắt TLS verify.
4. **Đa nguồn bên ngoài**: source thật của iLoader (`src-tauri/src/pairing.rs`), SideStore (`PairingFileManager.swift`, `AppConstants.swift`), libimobiledevice (`src/lockdown.c`), JitterbugPair, SideInstaller NOTES, AltSign/AltStore PR, iPASide CHANGELOG, Apparition blog, GitHub issues.
5. Sau mỗi sửa: `py_compile`, chạy lại 5 bộ test Python, `gcc -fsyntax-only` với **header libplist 2.6.0 / libimobiledevice 1.3.0 thật**, và functional test C trích đúng code production.

> Kiểm chứng đã bắt ra lỗi trong chính bản vá (3 lần retry `-80046` đều dùng cùng một danh tính Mac) → sửa lại trước khi chốt. Xem mục 5.

---

## 1. Lỗi #1 — Server đầu lỗi là DỪNG LUÔN, không tự đổi server

### 1.1 Triệu chứng đo được (2026-10-26, 15:41 UTC+7)

| Server | `/` | `/v3/client_info` | `/v3/get_headers` |
|---|---|---|---|
| `mac-ani.jkcoxson.com` | 200 | 200 `<MacBookPro13,3> … Mac OS X;10.12.6` | 405 |
| `ani.jkcoxson.com` | 200 | 200 (cùng danh tính Mac) | 405 |
| `ani3.stikstore.app` | 200 | 200 Mac | 405 |
| `new.ani.sidestore.io` | 200 | 200 Mac | 404 |
| `ani2.stikstore.app` | 200 | 200 Mac | 405 |
| `ani.sidestore.io` | 200 | 200 `<MacBookPro13,2> … macOS;13.1` | 404 |
| **`win.ani.sidestore.io`** | **502** | **502** | **502** |

→ Đúng kịch bản người dùng: có server chết ngay trong danh sách seed; bản cũ có `win.ani.sidestore.io` ở vị trí 5.

### 1.2 Nguyên nhân gốc (trong `apple_auth.py`)

| # | Lỗi | Hệ quả |
|---|---|---|
| a | `generate_anisette_headers()` thử lại **3 lần trên CÙNG URL**, chỉ ở lần 2 mới gọi `_select_anisette()` | Server chết (502/timeout/JSON hỏng) → 3 lần chết → `raise Exception` |
| b | Exception đó rơi vào `authenticate() → except Exception → return None` | **Không có nhánh failover nào chạy**, tool dừng hẳn |
| c | `_select_anisette()` **gộp chung** "server còn sống" và "Apple chấp nhận danh tính": mỗi ứng viên phải vượt một request `o=init` tới `gsa.apple.com` | Khi Apple edge 429/503 (đúng lúc server anisette cũng lỗi) → mọi server bị đánh dấu "bị từ chối" → rơi vào `return ANISETTE_URL` (URL cứng, có thể đang chết) |
| d | `net_fail >= 3` → `break` + `candidates[:12]` | Các server "ở dưới" không bao giờ được thử |
| e | Cache 6 giờ **không kiểm chứng lại** server còn sống | Server đã 502 vẫn được dùng cả ngày |
| f | `_anisette_pinned` (server do người dùng chọn) chặn hoàn toàn recovery, kể cả khi server chết | Người dùng ghim server xong server die → kẹt |
| g | `_recover_identity()` chỉ đổi server **đúng 1 lần** (attempts==1), lần 2 bỏ cuộc | Nếu server thứ 2 cũng lỗi → dừng |

### 1.3 Cách sửa

- Tách 2 lớp: **lớp 1 = server còn sống** (chỉ GET, không cần Apple), **lớp 2 = Apple chấp nhận danh tính** (có ngân sách 6 probe). Thất bại lớp 2 **không bao giờ xoá** kết quả lớp 1.
- `generate_anisette_headers()` viết lại: mỗi lần lỗi (timeout / HTTP 429/5xx / JSON hỏng / thiếu `X-Apple-I-MD-M`) → loại server đó, tìm server **sống tiếp theo**, cập nhật lại `client_info` + `user_agent` theo server mới, và **thử lại ngay trong cùng lần gọi** (tối đa 24 server).
- `AnisetteServerUnavailable` (exception mới) được `authenticate()` bắt riêng → `_recover_identity(..., server_dead=True)` đổi server **ngay**, không xin lại OTP trên server chết, chạy lại SRP (tối đa 4 lần đổi, có chống vòng lặp).
- Server ghim **vẫn tự failover khi chết** (đúng yêu cầu); chỉ giữ nguyên ghim khi Apple trả `-80045` (cần người dùng chủ động chọn server khác).
- Cache: TTL 30 phút + **kiểm chứng liveness** trước khi tin.
- Danh sách seed cập nhật theo kết quả đo thật: Mac-ADI trước, bỏ `win.ani.sidestore.io` (502), thêm `ani3.stikstore.app` (theo iPASide 1.2.9 / iloader 2.4.0).

**Test:** `tests/python/test_anisette_failover.py` — 6 kịch bản, trong đó có "GSA probe 503 hoàn toàn" và "server ghim chết".

---

## 2. Lỗi #2 — Tạo/nhúng file ghép nối nhưng SideStore / LiveContainer không dùng được

### 2.1 Bằng chứng từ source của chính các tool "pair xong dùng ok"

- **iLoader** (`src-tauri/src/pairing.rs`):
  - `PAIRING_APPS["SideStore"] = "ALTPairingFile.mobiledevicepairing;PairingFile_RemoteRP.plist;PairingFile_Lockdown.plist"` — **3 tên file**, không phải 2.
  - `place_individual_file()` ghi `/Documents/<path>` sau `vend_documents()`, `mk_dir` thư mục cha.
  - `pairing_file()` trên iOS ≥ 17.4 **gộp lockdown plist + RPPairing plist** thành một file; dưới 17.4 chỉ lockdown.
  - `generate_lockdown_plist()`: lấy pair record, thêm `UDID`, `start_session`, **bật `EnableWifiDebugging`**, rồi mới serialize.
- **SideStore** (`PairingFileManager.swift` + `AppConstants.swift`): bản `main` đọc `Documents/ALTPairingFile.mobiledevicepairing`, kiểm tra `UserDefaults.standard.isPairingReset`; bản nightly 0.7+ đọc thêm `PairingFile_Lockdown.plist` / `PairingFile_RemoteRP.plist`.
- **SideStore issue #1532** ghi log parser: `loaded pairing file keys: [WiFiMACAddress, HostCertificate, UDID, HostID, HostPrivateKey, EscrowBag, DeviceCertificate, SystemBUID, RootPrivateKey, RootCertificate]`.
- **ipaforge PR #26**: *"Current SideStore reads pairing files with a parser that requires `RootPrivateKey` and `WiFiMACAddress` … Records the OS creates may also lack `WiFiMACAddress`"* — và fix bằng cách **đọc `WiFiAddress` từ device khi record thiếu**.
- **libimobiledevice `src/lockdown.c`** (đọc thật): `WiFiMACAddress`/`EscrowBag` **chỉ được lưu khi `lockdownd_get_value("WiFiAddress")` trả value lúc pair**. iOS 26/27 với Private Wi-Fi Address / quyền mới có thể không trả.
- **SideInstaller NOTES.md**: `vend_documents` root AFC ở **container**, phải ghi `/Documents/...`; UserDefaults của SideStore trong LiveContainer nằm ở `Documents/SideStore/Library/Preferences/...`.

### 2.2 Nguyên nhân gốc trong repo

`jni_bridge_imd.c::build_pairing_file_xml()` **chỉ thêm `UDID`** vào pair record rồi ghi ra. Hệ quả:

1. Thiếu `WiFiMACAddress` / `EscrowBag` (iOS mới không trả lúc pair) → SideStore/minimuxer parse fail, báo "could not determine this device's UDID" / "invalid pairing file" mà log tool vẫn hiện `✅ Đã ghi file ghép nối`.
2. Thiếu `SystemBUID` khi pair record cũ hỏng → `StartSession` fail.
3. Một số pair record lưu cert dạng `<string>` trong khi reader cần `<data>`.
4. **Không ghi `PairingFile_RemoteRP.plist`** → SideStore nightly (iOS 17.4+/26+) ưu tiên RP nhưng không có file RP.
5. Chỉ thử `VendDocuments` + đúng 1 kiểu path; iOS/afcd khác nhau root ở `Documents` hoặc ở container → ghi nhầm `Documents/Documents/...` hoặc bị `PERM_DENIED`.
6. `afc_write_file_all()` trả `true` ngay sau `afc_file_close()` — **không đọc lại**, nên "ghi thành công" có nghĩa là rất ít.
7. UserDefaults chỉ ghi `/Library/Preferences/...` → **sai chỗ với SideStore chạy trong LiveContainer**.

### 2.3 Cách sửa

**C (`jni_bridge_imd.c`)**
- `build_pairing_file_xml()` viết lại:
  - luôn set `UDID`;
  - thiếu `SystemBUID` → lấy từ `usbmuxd_read_buid()`;
  - thiếu `WiFiMACAddress` → đọc trực tiếp `lockdownd GetValue("WiFiAddress")` (fallback `EthernetAddress`, `WiFiMACAddress`, hoặc copy từ khoá `WiFiAddress` có sẵn);
  - chuẩn hoá 7 khoá cert/key về `<data>`;
  - **gộp `rp_pairing_file.plist`** nếu người dùng đặt file RP (từ `idevice_pair` / StikPair) vào `filesDir`;
  - **kiểm chứng 8 key bắt buộc** → thiếu thì `return NULL` + log chỉ rõ key (fail-closed, không tạo file "có mà không dùng được").
- `vend_and_write()`: thử `VendContainer` → `VendDocuments`, mỗi lượt thử `/Documents/<rel>` rồi `<rel>`; mỗi lần dùng **house_arrest client mới**; ghi xong **đọc lại từng byte** để xác nhận.
- Có RP keys → tự ghi thêm file `PairingFile_RemoteRP.plist` bên cạnh (kể cả khi caller chỉ xin 1 tên). Không có RP keys → **bỏ qua** file tên RP (tránh SideStore đọc nhầm).
- `nativeWriteSideStorePrefs()`: ghi cả `/Library/Preferences/...` lẫn `/Documents/SideStore/Library/Preferences/...` (guest LiveContainer), 3 tên plist mỗi nơi.
- `ensure_wifi_debugging()`: thử thêm `EnableWifiConnections` nếu `EnableWifiDebugging` bị từ chối (một số build iOS 26/27).
- JNI mới: `nativeGetPairingFileReport()` (iOS, trạng thái Wi-Fi debug, danh sách key, RP có/không) và `nativeGetIosVersion()`.

**Python (`sideload_core.py`)**
- `validate_pairing_file_xml()`: parse plist, kiểm tra 8 key bắt buộc + 2 key khuyến nghị + UDID + nhận diện RP keys.
- `_pairing_preflight()` chạy **trước khi nhúng**: in báo cáo native, cảnh báo iOS ≥17.4 về RemotePairing, và **chặn nhúng** nếu thiếu key (ghi rõ vào `get_last_error()` để UI hiện).
- `_pairing_rel_paths_for(..., has_rp=...)`: chỉ thêm `PairingFile_RemoteRP.plist` khi thật sự có RP keys.
- Khi nhúng thất bại: `set_last_error()` nêu rõ nguyên nhân (Wi-Fi Debugging / mật mã màn hình / house_arrest / AFC) thay vì câu chung.

**Kotlin**
- `DeviceNative.getPairingFileXml()/getPairingFileReport()/getIosVersion()` (`@JvmStatic` cho Chaquopy).
- `NativeBridge.pairingFileReport()/iosVersion()`; `HomeViewModel.pairingReport` (chạy trên `Dispatchers.IO` để không ANR).
- `PairingFileManagerScreen`: hiện báo cáo kiểm chứng + hướng dẫn iOS 17.4+/26+.
- `PythonBridge.sideload()`: vẫn hiện cảnh báo `get_last_error()` **ngay cả khi cài app thành công**.

**Test:** `tests/python/test_pairing_validation.py` (8 nhóm), `tests/host/test_pairing_helpers.py` (biên dịch & chạy **đúng code C production** với libplist 2.6.0).

---

## 3. Lỗi #3 — Tương thích iPhone / Apple: các thay đổi gần đây

### 3.1 Dòng thời gian đã đối chiếu (nguồn ở mục 7)

| Thời điểm | Thay đổi phía Apple | Ảnh hưởng tới repo |
|---|---|---|
| 09/2026 | `X-MMe-Client-Info` chứa `com.apple.dt.Xcode` bị edge trả 503 | Repo đã xử lý (v72) ✅ |
| **12–13/10/2026** | GrandSlam `o=init` trả **-80045** khi anisette sinh từ Android CoreADI nhưng request khai Mac | Repo đã xử lý (v72) ✅ |
| **13/10/2026** | Apple **tự đẩy mã 2FA** sau bước mật khẩu: `hsc 409`, không có `au` | Repo đã xử lý (v72) ✅ |
| **14/10/2026 ~20:50 UTC** | Mọi request khai client **Android** bị 503 HTML | Repo v72 *nói* là xử lý, nhưng danh sách seed vẫn chứa server Windows/đã chết → cần failover (mục 1) ⚠️ |
| 16/10/2026 | Cộng đồng ghi nhận Apple chặn **SMS 2FA** | `handle_2fa_sms()` vẫn là fallback; giữ nguyên nhưng ưu tiên trusted-device ✅ |
| **22/10/2026** | **-80046** ở `GsService2/apptokens`: Apple không cấp developer token cho request khai Android/Windows, dù `init`/`complete` thành công | **CHƯA xử lý trong repo → fix ở v73** ❌→✅ |
| 23/10/2026 | iPASide 1.2.9 / iloader 2.4.0 / xtool 1.23.0 chuyển sang **Mac-ADI anisette** (`mac-ani.jkcoxson.com`, `new.ani.sidestore.io`, `ani3.stikstore.app`) | Cập nhật seed list ✅ |
| iOS 17.4+ | SideStore nightly ưu tiên **RemotePairing**; trên iOS 27 beta lockdown pairing bị đổi khiến minimuxer không đọc được UDID | Cảnh báo + cơ chế gộp `rp_pairing_file.plist` ✅ (xem giới hạn) |
| iOS 26/27 | `WiFiAddress` có thể không trả lúc pair; `EnableWifiDebugging` có thể cần biến thể `EnableWifiConnections` | Bổ sung key lúc xuất file + fallback set_value ✅ |

### 3.2 Cách sửa cụ thể

1. **-80046 / developer token.** `fetch_app_token()` giờ gửi `apptokens` với **danh tính Mac** (`token_identity` / `token_user_agent`), thử lại tối đa 3 danh tính Mac **khác nhau** khi Apple trả `-80046` / `-45061`. Không đăng nhập lại SRP (session còn nguyên). Header HTTP và `cpd` luôn **trùng** danh tính (tránh `-80044`).
2. **Developer services.** `DeveloperAPI._auth_headers()` dùng `auth.developer_client_info` (chính danh tính Mac đã được cấp token) thay vì lấy nguyên `X-MMe-Client-Info` từ anisette server. Apple "tie" token vào danh tính đó; đổi giữa chừng gây `resultCode 1100 "session has expired"`.
3. **TLS.** Bật lại `verify=True` cho mọi kết nối Apple + server anisette công khai (đo thật: `curl ssl=0` trên 6/6 host). Trước đó `verify=False` khiến mật khẩu Apple ID có thể bị MITM mà không có lý do kỹ thuật nào bắt buộc. Chỉ bỏ verify cho `http://localhost` / custom local server.
4. **Danh tính & lỗi mới.** Thêm hint hành động cho `-80046`, `-45061`, giữ hint `-80045/-20209/-22411/-20101…`; `_identity_score()` ưu tiên Mac-ADI > Windows > Android > Xcode token khi không probe được Apple.
5. **iOS version.** `nativeGetIosVersion()` + `_pairing_preflight()` cảnh báo rõ khi iOS ≥ 17.4 mà chưa có RP pairing, kèm hướng dẫn tạo `rp_pairing_file.plist`.

**Test:** `tests/python/test_apple_oct2026.py` (5 nhóm).

---

## 4. Bảng thay đổi file

| File | Thay đổi |
|---|---|
| `app/src/main/python/apple_auth.py` | Failover 2 lớp, `AnisetteServerUnavailable`, seed list, cache liveness, TLS on, Mac token identity + retry -80046, recovery ≥4 server, error hints |
| `app/src/main/python/developer_api.py` | `_auth_headers()` dùng `developer_client_info`, log chẩn đoán resultCode 1100 |
| `app/src/main/python/sideload_core.py` | `validate_pairing_file_xml`, `_pairing_preflight`, lọc path RP, `set_last_error` rõ lý do |
| `app/src/main/cpp/jni_bridge_imd.c` | Làm giàu + kiểm chứng pair record, gộp RP, `VendContainer/VendDocuments` + 2 path, đọc lại file, prefs LiveContainer, 2 JNI getter, fallback Wi-Fi debug |
| `NativeBridge.kt` / `DeviceNative.kt` | Khai báo + wrapper 2 JNI mới |
| `AppConfig.kt` / `PythonBridge.kt` | Danh sách server đã đo; hiện cảnh báo cả khi cài thành công |
| `HomeViewModel.kt` / `PairingFileManagerScreen.kt` / `SettingsScreen.kt` | Báo cáo kiểm chứng, chạy IO, hướng dẫn iOS mới, ghi chú failover |
| `.github/workflows/buildapk.yml` | 4 job test mới (3 Python + C syntax + C functional) |
| `tests/python/test_anisette_failover.py` | Mới — 6 kịch bản failover |
| `tests/python/test_apple_oct2026.py` | Mới — -80046 / danh tính / TLS |
| `tests/python/test_pairing_validation.py` | Mới — 8 nhóm kiểm chứng file |
| `tests/host/check_native_syntax.sh` | Mới — compile-check 4 file C |
| `tests/host/test_pairing_helpers.py` | Mới — chạy đúng code C production với libplist |
| `FIX_REPORT_v73.md`, `ReadMe.md` | Báo cáo + tóm tắt |

---

## 5. Kiểm chứng (chạy thật trong workspace)

```
python3 tests/python/test_sideload_core.py          ✅ TẤT CẢ KIỂM TRA ĐỀU ĐẠT
python3 tests/python/test_anisette_identity.py     ✅ TẤT CẢ KIỂM TRA ĐỀU ĐẠT
python3 tests/python/test_anisette_failover.py     ✅ (6 kịch bản)
python3 tests/python/test_apple_oct2026.py         ✅ (5 nhóm)
python3 tests/python/test_pairing_validation.py    ✅ (8 nhóm)
python3 tests/host/test_pairing_helpers.py         ✅ PAIRING C HELPERS OK
bash tests/host/check_native_syntax.sh             ✅ 4/4 file C
python3 -m py_compile app/src/main/python/*.py     ✅
```

Trong quá trình viết, `test_apple_oct2026.py` **đã bắt được lỗi thật**: cả 3 lần retry `apptokens` dùng cùng một danh tính Mac (do ánh xạ `retry → identity` sai). Đã sửa thành 3 danh tính khác nhau và test lại xanh. Đây là lý do có thêm `MAC_TOKEN_IDENTITIES[2]`.

**Đo mạng thật:** 6/6 server anisette trả HTTP 200 với `ssl_verify_result=0`; `win.ani.sidestore.io` 502 → đã loại khỏi seed.

---

## 6. Giới hạn trung thực (cần thiết bị thật để chốt 100%)

1. **Không có iPhone thật + Apple ID + mật khẩu** trong môi trường này → chưa chạy end-to-end `init → complete → 2FA → apptokens → developer API → ký → cài`. Mọi nhánh logic đều có test giả lập, C được biên dịch với header thật, nhưng **hành vi cuối cùng của Apple chỉ có thể xác nhận trên máy người dùng**.
2. **RPPairing chưa tự sinh.** Giao thức RemotePairing (SRP-3072, Ed25519, CoreDevice tunnel, PIN 000000) là cả một subsystem; các tool như iLoader/StikPair dùng crate Rust `idevice`. Tool v73 **không bịa** file RP: nó chỉ ghi `PairingFile_RemoteRP.plist` khi có RP keys thật. Cách dùng: tạo pairing bằng `idevice_pair` (Remote Pairing) → export `.plist` → chép vào thư mục dữ liệu của app với tên `rp_pairing_file.plist` → tool tự gộp vào mọi file ghép nối. Nếu không có RP, SideStore bản `main`/0.6 vẫn đọc `ALTPairingFile.mobiledevicepairing` như cũ.
3. `WiFiMACAddress` chỉ lấy được khi iPhone cho phép đọc `WiFiAddress`. Nếu iOS 27 chặn hẳn, log sẽ nói rõ và khuyên **tắt Private Wi-Fi Address**; không thể bịa MAC.
4. Vẫn giữ `Wi-Fi Debugging` fail-closed: iPhone **phải có mật mã màn hình**. Đây là yêu cầu của JitterbugPair/iLoader, không phải lỗi tool.
5. Mật khẩu Apple ID vẫn lưu plaintext trong `SharedPreferences` riêng tư của app (thiết kế có sẵn từ v51, `allowBackup=false`). Đã bật TLS để giảm rủi ro trên đường truyền; nếu muốn an toàn hơn nữa cần Keystore + không lưu mật khẩu (thay đổi lớn về UX).
6. Server anisette công khai là bên thứ ba: chúng thấy IP + dữ liệu provisioning mờ, **không** thấy Apple ID/mật khẩu/mã 2FA/token (các request đó đi thẳng tới Apple).

---

## 7. Nguồn tham khảo

- Dadoum/anisette-v3-server#61 — https://github.com/Dadoum/anisette-v3-server/issues/61
- SideStore/SideStore#1532, #1658, #1693 — https://github.com/SideStore/SideStore/issues/1532 , https://github.com/SideStore/SideStore/issues/1658
- rileytestut/AltSign#61 — https://github.com/rileytestut/AltSign/pull/61
- altstoreio/AltStore#1832, #1834, #1840 — https://github.com/altstoreio/AltStore/pull/1834
- nab138/iloader#859, #921; `src-tauri/src/pairing.rs` — https://github.com/nab138/iloader
- pwnapplehat/iPASide#9, #10, CHANGELOG 1.2.8/1.2.9 — https://github.com/pwnapplehat/iPASide/blob/main/CHANGELOG.md
- Ixion, "iOS sideloading error -80046" (2026-10-24) — https://blog.illixion.com/2026/10/sideloading-error-80046/
- FrizzleM/SideInstaller NOTES.md — https://github.com/FrizzleM/SideInstaller/blob/main/NOTES.md
- SirReaDy/ipaforge#26 — https://github.com/SirReaDy/ipaforge/pull/26
- SideStore `PairingFileManager.swift` / `AppConstants.swift` — https://github.com/SideStore/SideStore
- libimobiledevice `src/lockdown.c` (WiFiAddress/EscrowBag) — https://github.com/libimobiledevice/libimobiledevice
- SideStore docs: pairing file + error codes — https://docs.sidestore.io/docs/advanced/pairing-file , https://docs.sidestore.io/docs/troubleshooting/error-codes
- jkcoxson/idevice_pair — https://deepwiki.com/jkcoxson/idevice_pair/4.3-pairing-file-management
- osy/Jitterbug `JitterbugPair/main.c` — https://github.com/osy/Jitterbug

---

## 8. Người dùng cần làm gì

1. Cài bản build từ branch `fix/v73-failover-pairing-apple-compat`.
2. **Cài đặt → Server Anisette → "Tự động (khuyến nghị)"** (bản v73 vẫn tự đổi server nếu bạn ghim, nhưng Tự động là tối ưu).
3. iPhone: **đặt mật mã màn hình** (bắt buộc cho Wi-Fi Debugging), cắm cáp, bấm **Tin cậy** khi được hỏi.
4. Sau khi cài SideStore/LiveContainer, mở tab **File ghép nối** → đọc dòng `Kiểm chứng native: ios=…;status=OK;keys=…`. Nếu `status=INVALID` → làm theo log (xoá pair record, ghép nối lại).
5. Nếu dùng **SideStore nightly trên iOS 17.4+/26+** và app vẫn đòi pairing: dùng `idevice_pair` tạo **Remote Pairing**, export `.plist`, chép vào thư mục dữ liệu của tool với tên `rp_pairing_file.plist`, rồi bấm **Quét app → Nhúng** lại. Tool sẽ ghi cả 3 tên file.
6. Nếu gặp `-20209`: **dừng thử**, mở https://iforgot.apple.com để mở khoá tài khoản.

---

## 9. CI follow-up — sửa lỗi link test host (2026-10-27)

- GitHub Actions run `40364605831`: job **Build APK** thành công; job **Protocol tests** dừng tại `tests/host/test_pairing_helpers.py`.
- Nguyên nhân gốc: CI build libplist 2.6.0 thành **static archive**. `plist.o`/`jplist.o` tham chiếu `fmin()`/`pow()` từ libm, nhưng lệnh link của harness chỉ truyền `pkg-config --libs libplist-2.0` rồi `-lplist-2.0`, không thêm `-lm`. Linker vì vậy báo `undefined reference to fmin` và `pow`. Đây là lỗi phụ thuộc của **host test harness**, không phải lỗi compile/link APK; job APK đã xanh.
- Sửa tối thiểu: thêm `-lm` **sau** các thư viện trong `tests/host/test_pairing_helpers.py`, giữ thứ tự liên kết tĩnh đúng. Không thay đổi mã production.
- Verify sau sửa: tái tạo host dependencies theo đúng `tests/native-host/build_host_deps.sh` (libplist 2.6.0 static), chạy đủ 5 bộ Python, C syntax (4/4), functional pairing-helper C và fake-iPhone native-host integration; tất cả đều đạt. Chờ run GitHub Actions kế tiếp để xác nhận CI xanh từ đầu đến cuối.
