# Sửa lỗi đăng nhập Apple ID `-36607` / `-23782` — v73, 2026-10-25

Tiếp nối `FIX_REPORT_v72.md` (lỗi `-80045` ngày 20/10). Đây là **làn sóng chặn thứ hai** của Apple.

---

## 1. Hiện tượng (log người dùng, 25/10/2026)

```
[anisette] ✅ Chọn server: https://mac-ani.jkcoxson.com — danh tính được GSA chấp nhận (ec -20970)
[icloud] Client -Info: <MacBookPro13,3> <Mac OS X;10.12.6;16G29> <com.apple.AuthKit/1 (com.apple.akd/1.0)>
[gsa] Request operation: init      → 200, có sp/s/B/c/i   (danh tính QUA cửa 1)
[srp] SRP challenge processed successfully
[gsa] Request operation: complete  → 200
[icloud] Authentication failed: Status {hsc: 401, ec: -36607,
         em: "Unable to sign you in to your Apple Account. Try again later."}
```

Điểm quan trọng: `init` **thành công**, `complete` mới lỗi ⇒ **mật khẩu đã đúng**,
`SRP` đúng, danh tính đúng nguồn gốc. Đây không phải lỗi code SRP.

## 2. Điều tra

| Nguồn / phép đo | Kết luận |
|---|---|
| Apple Support (Sign in with Apple, `-36607`) | *"bạn đã thử đăng nhập quá nhiều tài khoản trên **thiết bị/trình duyệt đó** trong thời gian ngắn; phải chờ **≥ 24 giờ**"* |
| Cộng đồng SideStore/AltServer (`-36607`) | *"anisette server đó có quá nhiều tài khoản gắn vào cùng một **fake Mac**"* → đổi anisette server / tự host |
| `nab138/iloader` #1032, #1052, #1057, #1030 (21–25/10) | lỗi mới `-23782`: *"A Mac with macOS version 10.13 or later is required…"* — Apple đòi danh tính khai macOS **≥ 10.13** |
| `FrizzleM/SideInstaller` #89 + commit `c252bff` | cùng thời điểm, cùng cách xử lý: đổi/loại server theo lỗi |
| Đo trực tiếp của tôi, 25/10/2026 | `mac-ani.jkcoxson.com` **và** `ani.jkcoxson.com` trả **cùng** `X-Mme-Device-Id = 49A2FCC7…` → **một** danh tính dùng chung cho mọi người dùng 2 server này |
| Đo trực tiếp | `new.ani.sidestore.io` cấp **device-id mới theo từng phiên** (01619234… → 2643D57D…) |
| Đo trực tiếp | 10/15 server còn lại (`macOS;13.1`, machine-id Android CoreADI) → **HTTP 503** ở `init` |

⇒ Nguyên nhân thật: **không phải tài khoản, không phải mật khẩu, không phải SRP**,
mà là **danh tính anisette** (`X-Mme-Device-Id` + machine-id) đã bị dùng quá mức.
Bản v72 lại ưu tiên đúng cái server bị dùng nhiều nhất (`mac-ani.jkcoxson.com`),
vì lúc đó nó là server *duy nhất* vượt được `-80045`. Apple đổi luật → tool rơi
vào bẫy mới.

## 3. Cách sửa (v73)

| # | Thay đổi | Lý do |
|---|---|---|
| 1 | **Danh tính quản lý theo `X-Mme-Device-Id`**, không theo URL | 2 URL khác nhau có thể dùng chung 1 danh tính; xoay URL là vô ích (đúng lỗi người dùng gặp) |
| 2 | Loại trùng theo device-id khi dò server | không tốn request vào "cùng một fake Mac" |
| 3 | `-36607` → **đốt danh tính 24h** (`.anisette_burned.json`) + chọn danh tính **khác device-id** + chạy lại SRP, tối đa 3 lần | đúng cơ chế hạn mức 24h theo thiết bị của Apple |
| 4 | `-23782` → ưu tiên danh tính khai **macOS ≥ 10.13**; nếu không có thì cảnh báo rõ | cửa chặn mới từ 21/10 |
| 5 | `-20209` (khoá tài khoản) → **không thử lại**, chỉ dẫn iforgot | thử lại làm khoá nặng thêm |
| 6 | Hết danh tính dự phòng → **dừng**, báo "chờ ≥ 24h hoặc tự host", không lặp vô hạn | tránh vòng lặp đốt hết danh tính |
| 7 | Thứ tự server: `new.ani.sidestore.io` → `ani3/ani2.stikstore.app` → `mac-ani.jkcoxson.com` | ưu tiên server cấp danh tính mới; jkcoxson xuống cuối |
| 8 | Cache anisette lưu kèm `device_id` + `os_version`; cache trỏ danh tính đã đốt thì tự bỏ | tránh tái sử dụng danh tính đã cháy |
| 9 | Chẩn đoán hiện device-id, macOS của danh tính, cảnh báo nếu đã bị từ chối | người dùng tự thấy nguyên nhân |
| 10 | `generate_anisette_headers` giữ device-id hiện tại (không để danh tính cũ sót lại) | tránh mismatch kiểu `-80045` |

## 4. Bằng chứng

- **Test offline** `tests/python/test_anisette_identity.py` — 9 nhóm, tất cả ĐẠT:
  chọn server theo danh tính, cpd = header, cache, server chết giữa phiên,
  `-80045` tự chạy lại, `hsc 409`, **`-36607` đốt + đổi danh tính**, **`-23782`
  chọn danh tính macOS 13.5**, **2 URL cùng device-id chỉ thử 1 lần**.
- **Test hồi quy** `tests/python/test_sideload_core.py` — ĐẠT.
- **Live (25/10/2026)**: chọn danh tính `01619234…` trên `new.ani.sidestore.io`
  → `init` với Apple ID thật `ec 0`, có `sp/s/B/c/i`; giả lập `-36607` → đốt,
  xoay sang danh tính `2643D57D…`, `init` vẫn `ec 0`.

## 5. Việc người dùng cần làm

1. Cài bản **v73** (hoặc mới hơn).
2. Cài đặt → Server Anisette → **"Tự động"**. Nếu đang ghim `mac-ani.jkcoxson.com`
   thì **bỏ ghim** (danh tính đó đang bị `-36607`), nếu không tool sẽ không tự đổi.
3. Đăng nhập lại. Nếu gặp `-20209`: **dừng**, mở <https://iforgot.apple.com>.

## 6. Giới hạn / điều còn nghi ngờ

- Chưa kiểm chứng end-to-end tới `app token → ký → cài` (không có mật khẩu Apple
  ID và thiết bị iOS trong môi trường này). Đã kiểm chứng tới đúng cửa chặn.
- `-36607` là hạn mức **24 giờ** phía Apple. Nếu đã đốt hết danh tính công khai
  trong ngày, cách duy nhất là **chờ** hoặc **tự host anisette-v3-server**
  (danh tính riêng, không chia sẻ với ai).
- Apple có thể siết tiếp (đã 3 lần trong 2 tuần). Vì vậy logic được viết để
  **tự kiểm chứng lại từng danh tính** thay vì tin danh sách cứng.
