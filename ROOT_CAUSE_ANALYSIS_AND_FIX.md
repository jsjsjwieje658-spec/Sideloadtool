# 🔍 Phân Tích Lỗi Cốt Lõi — Sideloadtool Không Giao Tiếp Được Với iPhone

## Tóm Tắt Nhanh

Sau nhiều lần sửa lỗi mà app vẫn không giao tiếp được với iPhone, nguyên nhân thực sự là **4 lỗi cốt lõi nằm ở tầng thấp nhất** của stack — mỗi lỗi đủ để làm hỏng toàn bộ giao tiếp USB:

| # | Lỗi | Vị trí | Hậu quả |
|---|-----|--------|---------|
| 1 | **Thiếu `AttachCurrentThread`** | `jni_bridge.c` | MỌI USB write/read trả -1 ngay lập tức |
| 2 | **`plist_get_str()` không đọc `<data>`** | `plist_util.c` + `lockdown.c` | DevicePublicKey luôn NULL → pairing không bao giờ thành công |
| 3 | **`libusb_reset_device()` gây re-enumeration** | `usb_fd_bridge.c` | fd bị invalidate sau khi gọi → USB crash |
| 4 | **Thiếu `setConfiguration()`** | `UsbTransport.kt` | Interface claim thất bại trên Samsung/MediaTek |

---

## 🐛 BUG #1 (CRITICAL — Lỗi chính) — `jni_bridge.c`

### Vấn đề

```c
// CODE CŨ — SAI:
static int usb_bulk_write(const void *buf, int len) {
    JNIEnv *env = NULL;
    (*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6);
    if (!env) { LOGE("usb_bulk_write: no JNIEnv"); return -1; }  ← LUÔN trả -1!
```

**Tại sao hỏng:** Khi `nativeConnect()` / `nativePair()` được gọi từ Kotlin qua `withContext(Dispatchers.IO)`, Kotlin tạo ra một **thread mới không được attach vào JVM**. Trên các thread như vậy, `GetEnv()` không trả `env` mà trả mã lỗi `JNI_EDETACHED` — không phải crash, chỉ đặt `env = NULL` trong tham số ra. Vì code không check return value mà chỉ check `env == NULL`, mọi USB write/read **luôn trả -1 ngay lập tức** mà không gửi một byte nào.

Trong khi đó, `jni_bridge_imd.c` (Mode 1) đã làm đúng từ đầu:
```c
// jni_bridge_imd.c — ĐÃ ĐÚNG:
if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
    (*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL);  ← Đúng!
    detach = true;
}
```

Nhưng `jni_bridge.c` (Mode 2/3 fallback) thì không — và Mode 2/3 là code path thực sự chạy khi không có prebuilt libimobiledevice, hoặc khi Mode 1 fallback.

### Fix

```c
// CODE MỚI — ĐÚNG:
static int usb_bulk_write(const void *buf, int len) {
    if (!g_jvm) { LOGE("usb_bulk_write: g_jvm NULL"); return -1; }
    JNIEnv *env = NULL;
    bool detach = false;
    jint jres = (*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6);
    if (jres == JNI_EDETACHED) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
            LOGE("usb_bulk_write: AttachCurrentThread thất bại"); return -1;
        }
        detach = true;
    }
    // ... gọi Java ...
    if (detach) (*g_jvm)->DetachCurrentThread(g_jvm);
    return (int)result;
}
```

**Áp dụng cho:** `usb_bulk_write()`, `usb_bulk_read()`, `jni_log_ui_cb()` trong `jni_bridge.c`.

---

## 🐛 BUG #2 (CRITICAL) — `plist_util.c` + `lockdown.c`

### Vấn đề

Apple lockdownd trả `DevicePublicKey` dưới dạng XML plist thế này:
```xml
<key>Value</key><data>MIIBCgKCAQEA...</data>
```

Nhưng `lockdown_get_value()` dùng `plist_get_str()`:
```c
const char *val = plist_get_str(resp, "Value");
```

Và `plist_get_str()` chỉ match `PTYPE_STR`:
```c
const char *plist_get_str(const plist_dict_t *d, const char *key) {
    for (int i = 0; i < d->count; i++) {
        if (d->entries[i].type == PTYPE_STR &&  ← Chỉ check STR, bỏ qua DATA!
            strcmp(d->entries[i].key, key) == 0)
            return d->entries[i].str_val;
    }
    return NULL;  ← Luôn trả NULL cho <data>
}
```

Kết quả: `pairing_do()` gọi `lockdown_get_value(ld, NULL, "DevicePublicKey", &dev_pub_b64)` → nhận NULL → log "Không lấy được DevicePublicKey" → return -1 → **pairing luôn thất bại ở bước 2/5** dù kết nối USB đã thành công.

### Fix

**Thêm `plist_get_data_str()` trong `plist_util.c`:**
```c
const char *plist_get_data_str(const plist_dict_t *d, const char *key) {
    if (!d || !key) return NULL;
    for (int i = 0; i < d->count; i++) {
        if (d->entries[i].type == PTYPE_DATA &&  ← Match PTYPE_DATA
            strcmp(d->entries[i].key, key) == 0)
            return d->entries[i].str_val;  ← Trả base64 thô
    }
    return NULL;
}
```

**Cập nhật `lockdown_get_value()` trong `lockdown.c`:**
```c
const char *val = plist_get_str(resp, "Value");
if (!val) val = plist_get_data_str(resp, "Value");  ← Fallback cho <data>
```

---

## 🐛 BUG #3 (CRITICAL) — `usb_fd_bridge.c`

### Vấn đề

```c
// CODE CŨ — SAI:
LOGI("usb_bridge_init: libusb_reset_device() để xóa stale state...");
int reset_r = libusb_reset_device(g_handle);  ← Gây disaster!
```

`libusb_reset_device()` thực hiện **USB bus reset** — tương đương với việc rút cáp và cắm lại ở mức hardware. Hậu quả:

1. iPhone nhận tín hiệu reset → ngắt kết nối USB
2. Android phát `USB_DEVICE_DETACHED` event
3. Android huỷ `UsbDeviceConnection` nội bộ
4. **fd (file descriptor) được truyền cho libusb trở nên vô hiệu**
5. Mọi `libusb_bulk_transfer()` sau đó trả `LIBUSB_ERROR_NO_DEVICE` hoặc `LIBUSB_ERROR_IO`

Vì sao code này được thêm vào: Nhầm hiểu cách termux-usbmuxd hoạt động. termux-usbmuxd dùng fd sạch (UsbAPI.java không claim interface), không cần reset. Với fd sạch, `clear_halt` + `flush` là đủ.

### Fix

```c
// CODE MỚI — ĐÚNG:
/* FIX ROOT CAUSE #3: KHÔNG gọi libusb_reset_device().
 * Gây USB re-enumeration → fd invalid → mọi transfer thất bại.
 * Dùng clear_halt + flush thay thế — đủ để xóa STALL mà không reset. */
LOGI("usb_bridge_init: bỏ qua libusb_reset_device() (gây re-enumeration)");
```

---

## 🐛 BUG #4 (HIGH) — `UsbTransport.kt`

### Vấn đề

FIX_NOTES.md đề cập: "Đã cập nhật UsbTransport.kt để gọi `setConfiguration()` trước khi `claimInterface()`" — nhưng lời gọi này **chưa được thêm vào code thực tế**.

Trên một số Android OEM (Samsung Exynos, MediaTek Helio), thiết bị USB không tự đặt vào đúng `bConfigurationValue` sau khi `openDevice()`. Khi đó `claimInterface()` có thể thất bại hoặc `bulkTransfer()` luôn trả -1.

### Fix

```kotlin
// Thêm trước vòng retry claimInterface():
try {
    val setConfResult = conn.setConfiguration(1)
    Log.i(TAG, "setConfiguration(1) = $setConfResult")
} catch (e: Exception) {
    Log.w(TAG, "setConfiguration() exception (non-fatal): ${e.message}")
}
```

---

## 🔧 BUG #5 (BONUS) — `jni_bridge_imd.c`

`SOCKET_READY_RETRIES = 30 × 100ms = 3s` là không đủ trên Android low-end. Tăng lên `60 × 100ms = 6s`.

---

## 📋 Danh Sách File Đã Sửa

| File | Nội dung thay đổi |
|------|------------------|
| `app/src/main/cpp/jni_bridge.c` | AttachCurrentThread trong usb_bulk_write, usb_bulk_read, jni_log_ui_cb |
| `app/src/main/cpp/plist_util.c` | Thêm hàm plist_get_data_str() |
| `app/src/main/cpp/plist_util.h` | Khai báo plist_get_data_str() |
| `app/src/main/cpp/lockdown.c` | Dùng plist_get_data_str() fallback cho Value |
| `app/src/main/cpp/usb_fd_bridge.c` | Xóa libusb_reset_device() |
| `app/src/main/java/.../UsbTransport.kt` | Thêm setConfiguration(1) trước claimInterface() |
| `app/src/main/cpp/jni_bridge_imd.c` | Tăng SOCKET_READY_RETRIES từ 30→60 |

---

## 🧪 Cách Verify Sau Khi Build

1. Build APK từ Android Studio → Install lên thiết bị Android
2. Cắm iPhone vào Android qua cáp USB (có OTG nếu cần)
3. Mở app, bấm **Kết nối** → cấp quyền USB
4. Trong LogConsole, phải thấy:
   - `✅ USB open (libusb mode — no interface claim)`
   - `usb_recv_version: ✅ iPhone v1 protocol confirmed`
   - `✅ lockdown_open: kết nối port 62078 thành công`
   - `Bước 2/5: GetValue(DevicePublicKey)...`
   - (Sau đó): `DevicePublicKey PKCS#1 DER: XXX byte` ← **Đây là test quan trọng nhất**
5. Bấm **Ghép nối** → iPhone hỏi "Trust this computer?" → bấm **Trust**
6. Pairing thành công → thấy `✅ Kết nối thành công`

**Nếu thấy "Bước 2/5" và "DevicePublicKey PKCS#1 DER" thì Bug #2 đã fix đúng.**  
**Nếu không thấy "usb_bulk_write: no JNIEnv" trong log thì Bug #1 đã fix đúng.**

---

## 📌 Ghi Chú Kỹ Thuật

- **Tại sao termux-usbmuxd hoạt động còn app này không?**
  - termux-usbmuxd chạy `usbmuxd_proxy` như một process native, không qua JNI
  - Không có vấn đề JNI thread attachment vì không có JVM
  - fd được lấy từ UsbAPI.java (không claim) — endpoint sạch ngay từ đầu

- **Tại sao lỗi #1 khó phát hiện?**
  - `GetEnv()` không crash — chỉ đặt env = NULL và trả JNI_EDETACHED
  - Log "usb_bulk_write: no JNIEnv" có thể bị scroll qua trong list log dài
  - Từ phía Kotlin, `nativeConnect()` trả `false` mà không có stack trace

- **Mode 1 vs Mode 2/3:**
  - Mode 1 (libimobiledevice) dùng `jni_bridge_imd.c` — đã có AttachCurrentThread
  - Mode 2/3 (custom protocol) dùng `jni_bridge.c` — thiếu AttachCurrentThread
  - Với prebuilt libs đầy đủ, Mode 1 chạy trước. Nhưng nếu version exchange thất bại (do Bug #3 reset device), code fallback sang Mode 2/3 — gặp ngay Bug #1
