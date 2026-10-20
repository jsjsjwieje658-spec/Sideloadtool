# Sửa lỗi đăng nhập Apple ID (GSA -80045) — v72, 2026-10-20

Tài liệu này ghi lại: hiện tượng → điều tra → nguyên nhân gốc → cách sửa → bằng chứng kiểm chứng → việc còn lại.

---

## 1. Hiện tượng

Log người dùng:

```
[anisette] ✅ Chọn server: ethxn's omnisette server :3 (https://omni.ethxn.xyz) — 1501ms
[gsa] Request operation: init
[gsa] Response status: 200
[gsa] Response keys: ['Status', 'ptxid']
[icloud] Failed to authenticate: {'Status': {'hsc': 500, 'ec': -80045,
          'em': 'This action cannot be completed at this time.' ...}}
```

Điểm quan trọng: **HTTP 200** (edge không chặn), nhưng `Status.ec = -80045`
ở bước `o=init` — tức Apple từ chối **trước khi** mật khẩu được gửi. Đây
không phải sai mật khẩu, không phải anisette server "chết".

## 2. Điều tra (không đoán — đo trực tiếp)

Các phép đo chạy ngay với `gsa.apple.com` ngày 2026-10-20:

| Thí nghiệm | Kết quả | Kết luận |
|---|---|---|
| Gửi init với danh tính `com.apple.dt.Xcode` (nguyên văn từ server) | **HTTP 503 HTML** | token Xcode bị edge chặn (đúng như bản vá 09/2026) |
| Gửi init **không có** `cpd` | `ec -80009` "MID is invalid" | Apple đọc và kiểm machine-id |
| Gửi init với danh tính Mac bịa ra + machine-id của `omni.ethxn.xyz` | `ec -80045` | mismatch nguồn gốc machine-id ↔ danh tính |
| Cùng body, đổi sang danh tính **do server trả** (`mac-ani.jkcoxson.com`) | **`ec 0` + SRP challenge**, cả với Apple ID thật của người dùng | danh tính do server cấp thì được chấp nhận |
| Quét 19 server trong `servers.sidestore.io/servers.json` | chỉ `mac-ani.jkcoxson.com` được chấp nhận; còn lại `-80045`, `503`, hoặc chết | danh sách SideStore hiện gần như vô dụng |

Kết quả khớp với sự cố đã được cộng đồng ghi nhận từ 12/10/2026
(`Dadoum/anisette-v3-server#61`, `rileytestut/AltSign#61`,
`altstoreio/AltStore#1834`, `xtool-org/xtool#337`, `nab138/iloader#859`).

## 3. Nguyên nhân gốc (trong chính mã nguồn này)

`app/src/main/python/apple_auth.py` (bản cũ):

1. `__init__` **bịa** danh tính:
   ```python
   self.client_info = "<MacBookPro18,3> <macOS;13.4.1;22F8> <com.apple.AuthKit/1 (com.apple.akd/1.0)>"
   ```
2. `generate_cpd()` **ghi đè** danh tính của server bằng chuỗi bịa trên
   (`self.client_info = self._sanitize_client_info(anisette_data["X-MMe-Client-Info"])`
   — thực chất giữ nguyên chuỗi bịa, không dùng giá trị server trả).
3. `get_best_anisette_server()` chỉ kiểm tra "server trả HTTP 200" →
   `omni.ethxn.xyz` (machine-id Android CoreADI) luôn được chọn → `-80045`
   100% số lần.
4. Khi xoay server, `client_info` **không** được cập nhật theo server mới →
   thêm một nguồn mismatch nữa.
5. `_sanitize_client_info()` thay token Xcode → akd một cách mù quáng: với
   server "Mac thật" thì chuỗi akd **không khớp** machine-id mà server cấp.

Nói ngắn: **tool đang nói dối Apple về việc "tôi là máy Mac nào"**, trong khi
machine-id thì do một môi trường khác sinh ra. Từ 12/10/2026 Apple đối chiếu
hai thứ đó.

## 4. Cách sửa

### 4.1 `apple_auth.py` — lớp "danh tính anisette" mới

| Thành phần | Việc làm |
|---|---|
| `_fetch_identity(addr)` | `GET /v3/client_info` → `{client_info, user_agent}`; fallback `GET /` |
| `_identity_variants()` | `raw` (nguyên văn) là **mặc định**; `akd` chỉ là dự phòng khi chuỗi gốc còn token Xcode |
| `_identity_accepted(addr, ident)` | gửi `o=init` với Apple ID **ngẫu nhiên không tồn tại** → Apple chấp nhận hay không. Không gửi mật khẩu, không đụng tài khoản người dùng |
| `_gsa_init_probe()` | request kiểm chứng duy nhất, dựng body/header y như request thật |
| `_select_anisette()` | dò danh sách seed → SideStore → cache; chỉ chọn server vượt kiểm chứng; giới hạn 12 ứng viên |
| `_set_identity()` | đặt `client_info`/`user_agent` **theo server đang dùng**; gọi lại mỗi khi server đổi |
| `_build_cpd()` | `cpd["X-MMe-Client-Info"]` **luôn** = danh tính trong header HTTP |
| `AnisetteIdentityRejected` | lỗi riêng cho `-80045/-80009/-80010/-29004/-80044` và HTTP 5xx HTML |
| `_recover_identity()` | lần 1: xin OTP mới trên cùng server; lần 2: đổi server đã kiểm chứng; sau đó báo lỗi có hướng dẫn |
| `_explain_gsa_status()` | dịch `ec` của Apple thành câu tiếng Việt có thể hành động (`-20101`, `-20209`, `-22411`, `-80045`, …) |

Các sửa lỗi phụ trong cùng file:

- `hsc 434` (Apple yêu cầu đồng bộ lại anisette) → lấy OTP mới, thử lại 1 lần.
- `hsc 409` (từ 13/10/2026 Apple **tự** gửi mã 2FA ngay sau bước mật khẩu,
  không có `au`) → coi như trusted-device 2FA nhưng **không** gọi lại endpoint
  trigger (gọi lại sẽ làm mã vừa gửi mất hiệu lực) → tránh `-22411`.
- Header 2FA dùng `user_agent` của server thay vì UA bịa; `X-Apple-I-MD-LU`
  giữ giá trị do server cấp (chỉ fallback `base64(dsid)` khi server không có) —
  ghi đè LU có thể làm OTP không còn hợp lệ.
- Bỏ `locale.getdefaultlocale()` (DeprecationWarning xuất hiện trong log người
  dùng) → helper `_locale_id()`.

### 4.2 `sideload_core.py`

- `get_last_error()` / `set_last_error()`: UI đọc được **lý do thật** của lần
  thất bại gần nhất (mã lỗi Apple + tên server anisette).
- `do_diagnostics()`: mục Anisette giờ kiểm tra **danh tính có được Apple chấp
  nhận**, thay vì chỉ "server trả HTTP 200" (một server sống vẫn có thể luôn
  khiến đăng nhập thất bại).
- `list_anisette_servers()`: server đã kiểm chứng đứng đầu.

### 4.3 Kotlin

- `PythonBridge`: 4 outcome (`login`, `sideload`, `registerDevice`,
  `revokeCerts`) hiện thêm `get_last_error()` thay vì câu chung "xem nhật ký";
  danh sách server ưu tiên server đã biết tốt.
- `AppConfig.defaultAnisetteServers`: bỏ "SideStore Official" khỏi vị trí đầu.

## 5. Bằng chứng kiểm chứng

### 5.1 Trực tiếp với Apple (chỉ `o=init`, không gửi mật khẩu)

```
TRƯỚC khi sửa:
  server omni.ethxn.xyz + danh tính Mac bịa → ec -80045
  server ani.sidestore.io + danh tính Mac bịa → ec -80045
  Apple ID thật của người dùng            → ec -80045

SAU khi sửa (chạy chính module apple_auth.py đã vá):
  [anisette] ✅ Chọn server: https://mac-ani.jkcoxson.com
             — danh tính được GSA chấp nhận (ec -20970)
  [icloud] Client -Info: <MacBookPro13,3> <Mac OS X;10.12.6;16G29> (com.apple.akd/1.0)
  [icloud] User-Agent  : akd/1.0 CFNetwork/811.5.4 Darwin/16.7.0
  INIT (Apple ID thật) → ec 0, sp s2k, có salt / B / c / i   ← lấy được SRP challenge
```

Thời gian chọn server khi khởi động nguội: **0,6 s** (seed đứng đầu, cache 6 giờ).

### 5.2 Test tự động (offline)

- `tests/python/test_sideload_core.py` — **TẤT CẢ ĐỀU ĐẠT** (không hồi quy).
- `tests/python/test_anisette_identity.py` — **mới**, 6 nhóm kiểm tra:
  1. loại server bị từ chối (`-80045`, `503`), chỉ chọn server được chấp nhận;
  2. `client_info`/`user_agent` lấy nguyên văn từ server (không bịa);
  3. header HTTP và `cpd` **trùng nhau**;
  4. server chết giữa phiên → đổi server **và** đổi danh tính theo;
  5. `-80045` lúc đăng nhập → tự chạy lại SRP;
  6. `hsc 409` → không gọi lại endpoint trigger.

## 6. Việc người dùng cần làm

1. Cài bản v72 (hoặc chạy lại app sau khi cập nhật).
2. Cài đặt → Server Anisette → chọn **"Tự động"** (khuyến nghị). Nếu đang ghim
   `ani.sidestore.io`/`omni.ethxn.xyz` thì phải bỏ ghim, nếu không tool sẽ
   không tự đổi server.
3. Xoá `.anisette_cache.json` nếu trước đó đã ghim server hỏng (tool tự xoá khi
   cache không qua kiểm chứng, nhưng xoá tay là chắc chắn nhất).
4. Đăng nhập lại. Nếu Apple trả `-20209`: **dừng thử**, mở
   <https://iforgot.apple.com> để mở khoá (Apple khoá tài khoản khi thấy nhiều
   lần đăng nhập bất thường trong giai đoạn này).

## 7. Điều còn nghi ngờ / giới hạn

- **Chưa kiểm chứng được end-to-end** (init → complete → 2FA → app token →
  Developer API → ký → cài): môi trường này không có mật khẩu Apple ID của
  người dùng và không có thiết bị iOS. Đã kiểm chứng tới đúng điểm Apple từ
  chối (`init`) + toàn bộ nhánh logic bằng test giả lập.
- Đây là **bản vá theo hành vi hiện tại của Apple** — Apple có thể siết tiếp.
  Vì vậy phần chọn server được thiết kế để **tự kiểm chứng lại** thay vì tin
  một danh sách cứng, và `_recover_identity()` tự đổi server khi bị từ chối.
- `ani2.stikstore.app` (Windows ADI) trả `-22421` cho Apple ID không tồn tại;
  được xếp vào nhóm "chấp nhận được" nhưng chưa chứng minh được bằng tài khoản
  thật.
- SMS 2FA: cộng đồng ghi nhận Apple chặn gửi mã qua SMS từ 16/10/2026
  (`iloader` 2.3.8 cũng bỏ). Luồng trusted-device là đường chính.
