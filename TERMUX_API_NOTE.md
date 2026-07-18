# Vì sao ứng dụng này không dùng Termux/termux-api/termux-usb

Bạn có hỏi về `termux-api` — đây là ghi chú ngắn giải thích lựa chọn kiến trúc,
không phải một thay đổi code.

## Termux-api/termux-usb là gì

`termux-api` là gói phụ trợ cho Termux (giả lập terminal Linux trên Android),
cung cấp các lệnh shell (`termux-usb`, `termux-battery-status`, v.v.) gọi vào
các API Android tương ứng qua một addon-app riêng (Termux:API) mà Termux gọi
qua Intent. `termux-usb` cụ thể là một wrapper mỏng quanh chính
`android.hardware.usb` UsbManager mà ứng dụng này đang dùng trực tiếp — nó xin
quyền, mở file descriptor của thiết bị, rồi đưa file descriptor đó cho một
tiến trình dòng lệnh (thường là gọi `libusb`/`libimobiledevice` biên dịch native
chạy trong Termux).

## Vì sao SUPER ALPHA Sideload không dùng nó

1. **Thêm một tầng phụ thuộc ngoài, không cần thiết.** Termux + Termux:API là
   hai app riêng phải cài thêm, cấp quyền riêng, và phải luôn chạy nền — trong
   khi `android.hardware.usb` (Java/Kotlin) đã có sẵn trong chính Android SDK,
   không cần app ngoài nào cả. Đây đúng là yêu cầu ban đầu của bạn: "không cần
   Termux, không cần root".
2. **Không có gì termux-usb làm được mà UsbManager không làm được.** Cả hai
   đều dùng chung một API hệ thống ở tầng dưới; termux-usb chỉ là một lớp vỏ
   gọi lệnh shell quanh nó. Gọi thẳng UsbManager từ Kotlin (như
   `UsbTransport.kt` trong app đang làm) nhanh hơn, không phải qua Intent/IPC
   sang app khác, và dễ debug hơn (không phải trace qua ranh giới
   app-gọi-app).
3. **Termux không được Google Play hỗ trợ ổn định trên mọi thiết bị/bản Android
   mới** (nhiều lần bị giới hạn quyền nền, notification, v.v. qua các đời
   Android). Phụ thuộc vào một app ngoài tầm kiểm soát của chính bạn để có
   tính năng lõi (kết nối USB) là một rủi ro vận hành không cần thiết cho một
   ứng dụng cần chạy ổn định.
4. **zsign** (ký lại IPA) trong bản Termux gốc là một binary native chạy qua
   dòng lệnh trong Termux; ở đây `AppPaths.zsignPath()` trỏ tới một binary
   zsign được đóng gói/biên dịch sẵn cùng app (native lib hoặc asset), gọi trực
   tiếp từ Kotlin/Python — cùng lý do: bớt một tầng phụ thuộc ngoài.

## Khi nào NÊN cân nhắc lại Termux

Nếu sau này bạn cần các tác vụ hệ thống mà Android SDK công khai không hỗ trợ
(vd. một số thao tác filesystem sâu, hoặc muốn tái sử dụng các công cụ dòng
lệnh Linux có sẵn thay vì viết lại bằng Python/Kotlin), Termux vẫn là lựa chọn
hợp lý cho *các tác vụ đó cụ thể* — nhưng với riêng luồng USB/usbmux mà ứng
dụng này cần, `android.hardware.usb` trực tiếp là lựa chọn đúng và đã được giữ
nguyên trong bản sửa lỗi này.
