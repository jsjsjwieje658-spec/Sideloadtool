"""device_link.py — ĐÃ PORT SANG NATIVE C (v8).

Tất cả thao tác USB/lockdown (pair, AFC push, install) hiện chạy qua
native C (libsideloadnative.so) thay vì Python:
  - mux_usb.py      → usbmux.c + jni_bridge.c
  - device_link.py  → lockdown.c + pairing.c + afc.c + install_proxy.c

File này được GIỮ LẠI như một lớp adapter mỏng để sideload_core.py không cần
thay đổi cú pháp gọi hàm. Thao tác USB thực sự đi qua DeviceNative.kt.

Các hàm Apple API HTTP (apple_auth.py, developer_api.py) KHÔNG bị ảnh hưởng —
chúng vẫn chạy hoàn toàn bằng Python qua Chaquopy như trước.

FIX v8: file này được viết lại từ đầu — không còn import mux_usb hay dùng
Python SSL/TCP trực tiếp. Mọi I/O với thiết bị iOS đều qua DeviceNative.kt.
"""

import sideload_core


# ─────────────────────────────────────────────────────────────────────────
# Native bridge import
# ─────────────────────────────────────────────────────────────────────────

def _native():
    """Trả về DeviceNative Kotlin object qua Chaquopy Java interop."""
    from com.superalpha.sideload.bridge import DeviceNative
    return DeviceNative


# ─────────────────────────────────────────────────────────────────────────
# Pairing
# ─────────────────────────────────────────────────────────────────────────

def get_udid_from_usb():
    """Đọc UDID đã cache (set qua sideload_core.set_current_udid)."""
    return sideload_core.get_cached_udid()


def reset_mux_device():
    """Reset trạng thái native (MuxDevice, lockdown session)."""
    try:
        _native().reset()
    except Exception as e:
        print(f"[device_link] reset_mux_device: {e}")


def validate_pair_record(pair_record: dict) -> bool:
    """Trong native mode, pairing được quản lý hoàn toàn bởi C layer.
    Trả về True nếu có UDID (nghĩa là đã kết nối USB thành công trước đó)."""
    if not pair_record:
        return False
    # Pair record native được đại diện bởi một dict đơn giản với trường "native"
    return pair_record.get("native") is True


def pair_device(udid: str = "") -> dict:
    """Thực hiện USB connect + lockdown pairing qua native C.

    Trả về một "pair_record" đặc biệt {"native": True, "udid": ...} để
    sideload_core.py có thể truyền qua các lời gọi tiếp theo (afc_push_ipa,
    install_ipa). Toàn bộ pair record thật (certificates, keys) được lưu và
    quản lý bởi C layer trong filesDir.

    Raises LockdownError nếu connect/pair thất bại.
    """
    print("[device_link] Đang kết nối và ghép nối thiết bị qua native C...")
    ok = _native().connectAndPair()
    if not ok:
        raise LockdownError(
            "Kết nối/ghép nối thất bại. Kiểm tra: cáp USB, thiết bị đã mở khoá, "
            "và bấm 'Tin cậy' trên màn hình iPhone nếu được hỏi."
        )
    device_udid = _native().getUdid() or udid or sideload_core.get_cached_udid() or ""
    print(f"[device_link] ✅ Ghép nối thành công. UDID: {device_udid}")
    return {"native": True, "udid": device_udid}


# Alias để tương thích với mọi phiên bản sideload_core.py
def pair_with_device(udid: str = "") -> dict:
    return pair_device(udid)


class LockdownError(Exception):
    pass


class LockdownRstError(LockdownError):
    pass


# ─────────────────────────────────────────────────────────────────────────
# AFC / Install — wrapper qua DeviceNative.sideloadIpa()
# ─────────────────────────────────────────────────────────────────────────

# Biến nội bộ: lưu đường dẫn IPA local để install_ipa() dùng lại
_staged_ipa_path = None


def afc_push_ipa(pair_record: dict, local_ipa_path: str, remote_filename: str,
                 progress_cb=None) -> str:
    """Ghi nhận đường dẫn IPA local để native C push qua AFC khi install_ipa() gọi.

    Native layer (DeviceNative.sideloadIpa) thực hiện cả push lẫn install trong
    một lần gọi duy nhất — nên hàm này chỉ "stage" path, không push ngay.
    Trả về remote_path dự kiến (sideload_core.py cần giá trị này để truyền vào
    install_ipa(), nhưng native không dùng giá trị đó).
    """
    global _staged_ipa_path
    _staged_ipa_path = local_ipa_path
    print(f"[device_link] IPA đã staged để native push: {local_ipa_path}")
    return f"/PublicStaging/{remote_filename}"


def install_ipa(pair_record: dict, remote_ipa_path: str) -> bool:
    """Đẩy IPA lên device qua AFC và cài đặt qua install_proxy (native C).

    Dùng đường dẫn IPA đã stage bởi afc_push_ipa() — native xử lý toàn bộ:
    AFC mkdir + file write + InstallationProxy install.
    Raises LockdownError nếu thất bại.
    """
    global _staged_ipa_path
    ipa_path = _staged_ipa_path
    _staged_ipa_path = None

    if not ipa_path:
        raise LockdownError(
            "install_ipa: không tìm thấy IPA path. "
            "Gọi afc_push_ipa() trước install_ipa()."
        )

    print(f"[device_link] Đang push & cài đặt IPA qua native: {ipa_path}")
    ok = _native().sideloadIpa(ipa_path)
    if not ok:
        raise LockdownError(
            "Cài đặt IPA thất bại (native). "
            "Xem log để biết chi tiết lỗi từ AFC/install_proxy."
        )
    print("[device_link] ✅ Cài đặt IPA thành công qua native.")
    return True


def list_installed_apps(pair_record: dict) -> list:
    """Trả về danh sách bundle ID của các app User đang cài trên thiết bị.

    Gọi DeviceNative.listInstalledApps() → nativeListInstalledApps() JNI →
    com.apple.mobile.installation_proxy → trả về list bundle ID strings.

    Điều này cho phép sideload_core.py kiểm tra xem App ID đã tồn tại
    trên thiết bị hay chưa, tránh tạo App ID mới không cần thiết và vi phạm
    quota 10 App ID / 7 ngày của Apple Developer free account.
    """
    try:
        from com.superalpha.sideload.bridge import DeviceNative
        bundle_ids = DeviceNative.listInstalledApps()
        if bundle_ids is not None:
            result = list(bundle_ids)
            print(f"[device_link] list_installed_apps: tìm thấy {len(result)} app.")
            return result
    except Exception as e:
        print(f"[device_link] list_installed_apps: {e}")
    return []


def diagnose() -> str:
    """Chạy chẩn đoán kết nối iPhone — học từ lệnh 'termux-usbmuxd doctor'.

    Trả về chuỗi báo cáo để hiển thị trên UI / LogConsole.
    """
    try:
        from com.superalpha.sideload.bridge import DeviceNative
        report = DeviceNative.diagnostics()
        return report or "(diagnostics không có dữ liệu)"
    except Exception as e:
        return f"[device_link] diagnose() lỗi: {e}"


# ─────────────────────────────────────────────────────────────────────────
# Tiện ích cho sideload_core.py
# ─────────────────────────────────────────────────────────────────────────

def reset_mux_device_alias():
    """Alias backward compat."""
    reset_mux_device()
