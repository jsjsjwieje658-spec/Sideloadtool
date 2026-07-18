"""sideload_core.py — điểm vào duy nhất mà Kotlin (PythonBridge.kt) gọi vào.

Thay thế main.py gốc (menu CLI tương tác trong Termux) bằng 3 hàm thuần tuý
mà UI Compose gọi trực tiếp: do_sideload(), do_revoke_certs(),
get_connected_udid(). Không còn input()/getpass() chặn màn hình console —
2FA (nếu cần) đi qua UiPrompt (Kotlin) thay vì stdin.

Cũng chịu trách nhiệm:
  - Chuyển hướng mọi print() và sys.stderr sang NativeLog (Kotlin) để hiện
    trong LogConsole của app, vì ứng dụng Android không có terminal nào xem.
  - Lưu/đọc UDID hiện tại (do UsbPermissionManager/SideloadScreen phát hiện).
  - Lưu/đọc "pair record" (kết quả ghép nối lockdown) trong AppPaths.filesDir().
"""

import builtins
import hashlib
import os
import plistlib
import random
import shutil
import string
import sys
import time
import uuid
from datetime import datetime, timedelta, timezone

from com.superalpha.sideload.bridge import AppPaths, NativeLog, UiPrompt

from apple_auth import AppleAuth, fetch_official_servers
from developer_api import DeveloperAPI, classify_app_id_error
from utils import (
    run_command, extract_ipa, find_app_bundle, get_bundle_id, get_app_name,
    set_bundle_id, save_certificate_as_pem, decode_apple_data_field,
)
import config_manager
import device_link

# Apple: tài khoản Apple ID miễn phí chỉ được tạo tối đa 10 App ID MỚI mỗi
# 7 ngày — mỗi App ID mới tạo cũng chỉ "sống" đủ 7 ngày trước khi bị Apple
# tự vô hiệu. Toàn bộ hằng số ngày dưới đây tham chiếu đúng chu kỳ 7 ngày này.
APP_ID_QUOTA_WINDOW_DAYS = 7


# ══════════════════════════════════════════════════════════════════════════════
# Chuyển hướng print() VÀ sys.stderr sang NativeLog (Kotlin SharedFlow → UI)
#
# FIX: Dùng NativeLog.log(tag, message) — phiên bản 2 tham số, được đánh dấu
# @JvmStatic trong NativeLog.kt — thay vì NativeLog.log(message) (1 tham số,
# không @JvmStatic). Chaquopy tìm kiếm static method signature: nếu method
# không có @JvmStatic, nó chỉ tồn tại trên NativeLog.INSTANCE (instance method)
# chứ không phải trên class → Chaquopy ném NoSuchMethodError → _bridged_print
# bắt ngoại lệ và bỏ qua silently → log Python không hiện trên UI.
# ══════════════════════════════════════════════════════════════════════════════

_original_print = builtins.print


def _bridged_print(*args, **kwargs):
    """Ghi log vào NativeLog UI VÀ stdout gốc (Logcat qua Chaquopy)."""
    text = " ".join(str(a) for a in args)
    try:
        NativeLog.log("python", text)      # @JvmStatic 2-arg version
    except Exception:
        pass
    _original_print(*args, **kwargs)       # cũng ghi vào Logcat


builtins.print = _bridged_print


class _StderrBridge:
    """Chuyển hướng sys.stderr (traceback Python) sang NativeLog để hiện trong UI."""

    def __init__(self):
        self._buf = ""

    def write(self, s):
        self._buf += s
        while "\n" in self._buf:
            line, self._buf = self._buf.split("\n", 1)
            line = line.rstrip("\r")
            if line:
                try:
                    NativeLog.log("python-err", line)
                except Exception:
                    pass

    def flush(self):
        chunk = self._buf.strip()
        if chunk:
            try:
                NativeLog.log("python-err", chunk)
            except Exception:
                pass
        self._buf = ""

    def isatty(self):
        return False


sys.stderr = _StderrBridge()


# ── State helpers ─────────────────────────────────────────────────────────────

_current_udid = None


def set_current_udid(udid: str):
    """Gọi từ Kotlin (UsbPermissionManager) ngay khi USB permission được cấp."""
    global _current_udid
    _current_udid = udid


def get_cached_udid():
    return _current_udid


def get_connected_udid():
    return config_manager.get_connected_udid()


# ── Cầu nối cho màn "Cài đặt" ────────────────────────────────────────────────

def get_saved_apple_id() -> str:
    return config_manager.get_apple_id()


def save_apple_id(apple_id: str):
    config_manager.set_apple_id(apple_id)


def get_saved_anisette_url() -> str:
    return config_manager.get_anisette_url()


def save_anisette_url(url: str):
    config_manager.set_anisette_url(url)


def list_anisette_servers() -> str:
    """Trả về danh sách server Anisette công khai dạng JSON string."""
    import json
    try:
        servers = fetch_official_servers()
        simplified = [
            {"name": s.get("name") or "?", "address": s.get("address")}
            for s in servers if s.get("address")
        ]
        return json.dumps(simplified)
    except Exception as e:
        print(f"[anisette] Lỗi khi lấy danh sách server: {e}")
        return "[]"


# ── Internal state persistence ────────────────────────────────────────────────

def _state_path():
    return os.path.join(str(AppPaths.filesDir()), "sideload_state.json")


def _load_state():
    import json
    try:
        with open(_state_path()) as f:
            return json.load(f)
    except Exception:
        return {}


def _save_state(state: dict):
    import json
    with open(_state_path(), "w") as f:
        json.dump(state, f)


def _clear_cert_from_state(state: dict):
    for key in ("certificate_id", "certificate_pem", "private_key_pem"):
        state.pop(key, None)
    _save_state(state)


# ── App ID resolution ─────────────────────────────────────────────────────────
#
# BUGFIX v13 [CRITICAL]: Trước đây, khi Apple trả resultCode 9401 ("An App ID
# with Identifier '...' is not available. Please enter a different string.")
# — đúng lỗi thấy trong log người dùng report với 'com.SideStore.SideStore' —
# code CHỈ gọi classify_app_id_error() để in ra một thông báo lỗi thân thiện
# hơn, RỒI VẪN return False NGAY, bỏ cuộc hoàn toàn. Docstring của FIX-4 trong
# developer_api.py (classify_app_id_error/delete_app_id) đã ghi rõ ý định
# "nền tảng cho chức năng tự động đổi bundle id khi bị trùng" và tham chiếu
# một hàm `sideload_core.py::_resolve_app_id()` — nhưng hàm đó CHƯA TỪNG được
# viết. Nói cách khác: hạ tầng phân loại lỗi đã có, nhưng phần XỬ LÝ THẬT bị
# thiếu — đây chính là lý do "Ký & Cài đặt" luôn thất bại với bundle id phổ
# biến (SideStore, AltStore, ...) đã bị hàng nghìn tài khoản khác đăng ký mất,
# vì App ID là chuỗi DUY NHẤT TOÀN CẦU trên Apple Developer, không chỉ trong
# phạm vi tài khoản của bạn.
#
# _resolve_app_id() bên dưới lấp đúng lỗ hổng này:
#   - 'unavailable' (9401): tự thêm hậu tố ngẫu nhiên vào bundle id và thử
#     lại (tối đa 5 lần) — đúng cách AltStore/SideStore/Sideloadly xử lý.
#   - 'quota' (giới hạn 10 App ID mới/7 ngày của tài khoản free): tự xoá 1
#     App ID cũ do CHÍNH TOOL NÀY tạo trước đó (tra trong registry cục bộ
#     state["app_id_map"], KHÔNG bao giờ đụng App ID không rõ nguồn gốc) để
#     giải phóng hạn mức, rồi thử lại.
#   - Kết quả (bundle id hiệu lực + appIdId) được lưu lại trong state để lần
#     sideload SAU của CÙNG app trên CÙNG tài khoản dùng lại đúng App ID đó,
#     tránh vừa tốn thêm App ID trong hạn mức mỗi lần bấm "Ký & Cài đặt", vừa
#     tránh cài app đó thành 2 bản riêng biệt trên máy chỉ vì bundle id đổi
#     ngẫu nhiên mỗi lần.

_APP_ID_UNAVAILABLE_MAX_RETRIES = 5


def _random_bundle_suffix(length: int = 5) -> str:
    return "".join(random.choices(string.ascii_lowercase + string.digits, k=length))


def _find_app_id_by_bundle(dev_api: "DeveloperAPI", team_id: str, bundle: str):
    """Tra list_app_ids() cho identifier == '{team_id}.{bundle}' (định dạng
    Apple trả về từ old plist API — xem BUGFIX v11/BUG-5). Trả về appIdId
    hoặc None."""
    full_id = f"{team_id}.{bundle}"
    for a in dev_api.list_app_ids():
        identifier = a.get("identifier") or a.get("attributes", {}).get("identifier", "")
        if identifier == full_id:
            return a.get("appIdId") or a.get("id")
    return None


def _free_one_own_app_id(dev_api: "DeveloperAPI", state: dict, skip_map_key: str = None) -> bool:
    """Xoá 1 App ID cũ do CHÍNH TOOL NÀY tạo (tra trong state["app_id_map"])
    để giải phóng hạn mức 10 App ID mới/7 ngày. KHÔNG xoá App ID đang cần
    dùng (skip_map_key) và KHÔNG đụng App ID nào không có trong registry cục
    bộ này — App ID lạ có thể do Xcode hoặc app khác của người dùng tạo."""
    app_id_map = state.setdefault("app_id_map", {})
    for key, entry in list(app_id_map.items()):
        if key == skip_map_key:
            continue
        app_id_id = entry.get("app_id_id")
        if not app_id_id:
            continue
        if dev_api.delete_app_id(app_id_id):
            app_id_map.pop(key, None)
            _save_state(state)
            return True
    return False


def _resolve_app_id(dev_api: "DeveloperAPI", team_id: str, base_bundle: str, app_name: str, state: dict):
    """Tạo hoặc tái sử dụng App ID cho base_bundle. Trả về
    (effective_bundle_id, app_id_id), hoặc (None, None) nếu thất bại hẳn."""
    map_key = f"{team_id}:{base_bundle}"
    app_id_map = state.setdefault("app_id_map", {})

    # 1) Đã "chốt" một bundle id hiệu lực cho đúng app + team này trước đây?
    cached = app_id_map.get(map_key)
    if cached:
        effective_bundle = cached.get("effective_bundle", base_bundle)
        app_id_id = _find_app_id_by_bundle(dev_api, team_id, effective_bundle)
        if app_id_id:
            print(f"Dùng lại App ID đã chốt trước đó: {team_id}.{effective_bundle}")
            return effective_bundle, app_id_id
        # App ID đã biến mất phía Apple (vd hết hạn/bị revoke) — xoá cache hỏng.
        app_id_map.pop(map_key, None)

    # 2) App ID với đúng bundle id gốc đã có sẵn trên tài khoản?
    existing = _find_app_id_by_bundle(dev_api, team_id, base_bundle)
    if existing:
        print(f"Dùng lại App ID: {team_id}.{base_bundle}")
        app_id_map[map_key] = {"effective_bundle": base_bundle, "app_id_id": existing}
        _save_state(state)
        return base_bundle, existing

    # 3) Tạo mới — tự xử lý 'unavailable' (đổi bundle id) và 'quota' (giải
    #    phóng App ID cũ) thay vì bỏ cuộc ngay ở lần thử đầu tiên.
    candidate = base_bundle
    attempt = 0
    quota_freed_once = False
    while True:
        print(f"Đang tạo App ID: {team_id}.{candidate}")
        result = dev_api.create_app_id(candidate, app_name)
        if result:
            app_id_id = result.get("appIdId") or result.get("id")
            print(f"✅ Tạo App ID thành công: {team_id}.{candidate}")
            app_id_map[map_key] = {"effective_bundle": candidate, "app_id_id": app_id_id}
            _save_state(state)
            return candidate, app_id_id

        kind = classify_app_id_error(dev_api.last_error)
        err_msg = (dev_api.last_error or {}).get("userString") or "lỗi không xác định"

        if kind == "unavailable" and attempt < _APP_ID_UNAVAILABLE_MAX_RETRIES:
            attempt += 1
            suffix = _random_bundle_suffix()
            print(
                f"⚠️  Bundle id '{team_id}.{candidate}' đã bị MỘT TÀI KHOẢN KHÁC đăng ký mất "
                f"(App ID là duy nhất TOÀN CẦU trên Apple Developer, không chỉ trong tài khoản "
                f"của bạn) — tự đổi bundle id và thử lại (lần {attempt}/{_APP_ID_UNAVAILABLE_MAX_RETRIES})..."
            )
            candidate = f"{base_bundle}-{suffix}"
            continue

        if kind == "quota" and not quota_freed_once:
            quota_freed_once = True
            if _free_one_own_app_id(dev_api, state, skip_map_key=map_key):
                print("Đã giải phóng 1 App ID cũ do tool này tạo trước đó — thử lại...")
                continue
            print(
                "❌ Tài khoản đã đạt giới hạn 10 App ID mới/7 ngày và không có App ID cũ nào "
                f"của tool này để tự giải phóng: {err_msg}"
            )
            return None, None

        print(f"❌ Không tạo được App ID '{team_id}.{candidate}': {err_msg}")
        return None, None


# ── Public API ────────────────────────────────────────────────────────────────

def do_sideload(
    ipa_path: str,
    apple_id: str,
    password: str,
    udid_override: str = "",
    anisette_url: str = "",
) -> bool:
    """Ký và cài đặt IPA lên thiết bị iOS đang kết nối USB."""
    try:
        print("══ Bắt đầu quá trình sideload ══")

        # ── Chuẩn bị thư mục làm việc ───────────────────────────────────────
        work_dir = os.path.join(str(AppPaths.filesDir()), "sideload_work")
        if os.path.exists(work_dir):
            shutil.rmtree(work_dir, ignore_errors=True)
        os.makedirs(work_dir, exist_ok=True)

        # ── Giải nén IPA ─────────────────────────────────────────────────────
        print("Đang giải nén IPA...")
        # BUGFIX v12: extract_ipa() trả về output_dir (work_dir), KHÔNG phải
        # đường dẫn tới .app bundle. Gọi trực tiếp app_dir = extract_ipa(...)
        # rồi get_bundle_id(app_dir) → tìm Info.plist trong sideload_work/ →
        # không tồn tại → "Info.plist không tìm thấy trong .../sideload_work".
        # Fix: tách thành 2 bước — giải nén xong, rồi find_app_bundle() để
        # lấy đường dẫn thật tới SomeApp.app/
        extract_ipa(ipa_path, work_dir)
        app_dir = find_app_bundle(work_dir)
        bundle_id = get_bundle_id(app_dir)
        app_name  = get_app_name(app_dir)
        print(f"Ứng dụng: {app_name} ({bundle_id})")

        # ── Xác thực Apple ID ─────────────────────────────────────────────────
        print("Đang đăng nhập Apple ID...")
        effective_anisette = anisette_url or config_manager.get_anisette_url()
        auth = AppleAuth(anisette_url=effective_anisette or None)
        # BUGFIX v11: AppleAuth dùng authenticate() không phải sign_in()
        session = auth.authenticate(apple_id, password)
        if not session or not session.get("authenticated"):
            print("❌ Đăng nhập Apple ID thất bại.")
            return False
        print("✅ Đăng nhập thành công.")

        # ── Lấy Development Team ─────────────────────────────────────────────
        # BUGFIX v11: DeveloperAPI cần (auth_object, dsid, session_token) —
        # không phải session dict. auth.session là requests.Session dùng để gọi HTTP.
        dev_api = DeveloperAPI(auth, session["dsid"], session["session_token"])
        teams = dev_api.list_teams()
        if not teams:
            print("❌ Không lấy được Development Team.")
            return False
        team_id = teams[0].get("teamId") or teams[0].get("teamID") or teams[0].get("id")
        dev_api.set_team(team_id)
        print(f"Team: {team_id}")

        # ── Certificate ───────────────────────────────────────────────────────
        state = _load_state()
        cert_id    = state.get("certificate_id")
        cert_pem   = state.get("certificate_pem")
        key_pem    = state.get("private_key_pem")
        reuse_cert = False

        if cert_id and cert_pem and key_pem:
            existing = [c for c in dev_api.list_certificates() if c.get("id") == cert_id]
            if existing:
                print(f"Dùng lại certificate hiện có: {cert_id}")
                reuse_cert = True

        if not reuse_cert:
            print("Đang tạo certificate mới...")
            # BUGFIX v11: create_certificate() tự sinh RSA key + CSR nội bộ.
            # Không gọi generate_csr() (không tồn tại) hay truyền arg vào.
            # Kết quả trả về: {"certificateId":..., "certContent":..., "_private_key_pem":...}
            cert_data = dev_api.create_certificate()
            if not cert_data:
                print("❌ Không tạo được certificate.")
                return False

            cert_id = cert_data.get("certificateId") or cert_data.get("id", "")
            # certContent là DER bytes (đã được decode_apple_data_field xử lý)
            raw_cert = cert_data.get("certContent") or cert_data.get("certificateContent") or b""
            cert_bytes = decode_apple_data_field(raw_cert)
            if not cert_bytes:
                print("❌ Không lấy được nội dung certificate (certContent rỗng).")
                return False
            # Chuyển DER → PEM để zsign đọc được
            import base64 as _b64
            if isinstance(cert_bytes, bytes) and not cert_bytes.startswith(b"-----"):
                cert_pem = (
                    "-----BEGIN CERTIFICATE-----\n"
                    + _b64.encodebytes(cert_bytes).decode("ascii")
                    + "-----END CERTIFICATE-----\n"
                )
            else:
                cert_pem = cert_bytes.decode("utf-8") if isinstance(cert_bytes, bytes) else cert_bytes

            # _private_key_pem đã là PEM (TraditionalOpenSSL) từ cryptography lib
            key_pem = cert_data.get("_private_key_pem", "")
            if not key_pem:
                print("❌ Không lấy được private key từ create_certificate().")
                return False

            state.update({"certificate_id": cert_id, "certificate_pem": cert_pem, "private_key_pem": key_pem})
            _save_state(state)
            print(f"✅ Tạo certificate thành công: {cert_id}")

        # Ghi cert và key ra file để zsign dùng
        cert_file = os.path.join(work_dir, "cert.pem")
        key_file  = os.path.join(work_dir, "key.pem")
        with open(cert_file, "w") as f: f.write(cert_pem)
        with open(key_file,  "w") as f: f.write(key_pem)

        # ── App ID ────────────────────────────────────────────────────────────
        # BUGFIX v13: trước đây, một lỗi 'unavailable' (resultCode 9401 — bundle
        # id đã bị tài khoản khác đăng ký mất, vd 'com.SideStore.SideStore')
        # khiến hàm bỏ cuộc ngay lập tức. Giờ dùng _resolve_app_id(), tự động
        # đổi bundle id (thêm hậu tố) khi bị trùng, và tự giải phóng App ID cũ
        # khi đạt giới hạn 10 App ID mới/7 ngày — xem chú thích đầy đủ ở định
        # nghĩa _resolve_app_id() phía trên.
        safe_bundle = bundle_id.replace("_", "-")
        effective_bundle, app_id_id = _resolve_app_id(dev_api, team_id, safe_bundle, app_name, state)
        if not app_id_id:
            print("❌ Không tạo được App ID — xem log phía trên để biết nguyên nhân cụ thể.")
            return False
        full_app_id = f"{team_id}.{effective_bundle}"
        if effective_bundle != safe_bundle:
            print(f"ℹ️  App ID hiệu lực: {full_app_id} (đã đổi từ '{safe_bundle}' do bị trùng).")

        # ── Provisioning Profile ──────────────────────────────────────────────
        print("Đang tạo Provisioning Profile...")
        udid = udid_override or _current_udid or config_manager.get_connected_udid() or str(AppPaths.filesDir())

        # Đăng ký UDID thiết bị nếu chưa có
        # BUGFIX v11: list_devices() dùng old plist API → device dict có
        # "deviceNumber" (UDID), không có lớp "attributes" như v1 API.
        devices = dev_api.list_devices()
        registered = any(
            (d.get("deviceNumber") or d.get("attributes", {}).get("udid", "")) == udid
            for d in devices
        )
        if not registered:
            print(f"Đang đăng ký thiết bị UDID: {udid}")
            # BUGFIX v12: register_device(device_name, device_udid) — args cũ bị
            # đảo ngược: udid được truyền vào tham số device_NAME → Apple nhận
            # deviceNumber="Android Sideload Device" (tên thiết bị thay vì UDID).
            #
            # BUGFIX v14: giá trị trả về CHƯA TỪNG được kiểm tra — nếu Apple từ
            # chối đăng ký (vd UDID sai định dạng, hoặc lỗi tạm thời), code cũ
            # vẫn đi tiếp bình thường tới bước tải Provisioning Profile như thể
            # thiết bị đã đăng ký thành công. Vì team chưa thực sự có thiết bị
            # nào, Apple sẽ báo resultCode 8220 "Your team has no devices from
            # which to generate a provisioning profile" ở bước SAU — rất khó
            # truy về đúng nguyên nhân gốc (đăng ký thiết bị thất bại) từ lỗi
            # đó. Giờ kiểm tra ngay và dừng lại với thông báo rõ ràng.
            if not dev_api.register_device("Android Sideload Device", udid):
                err_msg = (dev_api.last_error or {}).get("userString") or "lỗi không xác định"
                print(f"❌ Không đăng ký được thiết bị UDID {udid}: {err_msg}")
                return False
            print(f"✅ Đăng ký thiết bị thành công: {udid}")

        # BUGFIX v11: download_provisioning_profile(appIdId) là method đúng.
        # create_provisioning_profile() không tồn tại trong developer_api.py.
        # Old plist API trả về provisioningProfile.encodedProfile (base64 DER).
        profile_data = dev_api.download_provisioning_profile(app_id_id)
        if not profile_data:
            print("❌ Không tải được Provisioning Profile.")
            return False
        # encodedProfile từ old API (không phải attributes.profileContent từ v1)
        raw_profile = profile_data.get("encodedProfile") or profile_data.get("profileContent") or ""
        profile_bytes = decode_apple_data_field(raw_profile)
        if not profile_bytes:
            print("❌ Provisioning Profile rỗng sau decode.")
            return False
        profile_file  = os.path.join(work_dir, "profile.mobileprovision")
        with open(profile_file, "wb") as f:
            f.write(profile_bytes if isinstance(profile_bytes, bytes) else profile_bytes.encode())
        print("✅ Tải Provisioning Profile thành công.")

        # Đặt bundle ID hiệu lực vào app bundle.
        #
        # BUGFIX v13 [CRITICAL — nguyên nhân khiến cài đặt luôn thất bại dù ký
        # "thành công"]: bản cũ gọi set_bundle_id(app_dir, f"{team_id}.{safe_bundle}")
        # — TỰ Ý gắn thêm tiền tố Team ID vào CFBundleIdentifier. Đây SAI hoàn
        # toàn: CFBundleIdentifier trong Info.plist KHÔNG bao giờ chứa Team ID;
        # Team ID chỉ xuất hiện trong entitlement "application-identifier" của
        # provisioning profile dưới dạng "TEAMID.<CFBundleIdentifier>" — do
        # Apple tự ghép, không phải do tool này ghép vào Info.plist.
        # Hệ quả: App ID đăng ký với Apple là "<team_id>.{effective_bundle}"
        # (App ID = TeamID + bundle id gốc "com.SideStore.SideStore[-xxxxx]"),
        # nhưng CFBundleIdentifier bị ghi thành "{team_id}.com.SideStore.SideStore[-xxxxx]"
        # (có thêm TeamID lặp lại) → khi cài, installd so khớp entitlement
        # "application-identifier" = "TEAMID.com.SideStore.SideStore..." của
        # profile với CFBundleIdentifier thực tế của app và thấy KHÔNG khớp
        # → cài đặt bị từ chối (hoặc app crash ngay khi mở vì code signing
        # không hợp lệ), dù bước "Ký IPA bằng zsign" ở trên báo "thành công".
        # Fix: chỉ dùng effective_bundle (bundle id thật, có thể đã thêm hậu
        # tố ngẫu nhiên nếu bị trùng ở bước _resolve_app_id() — KHÔNG có Team
        # ID) làm CFBundleIdentifier, đúng như App ID đã đăng ký với Apple.
        set_bundle_id(app_dir, effective_bundle)

        # ── Ký IPA bằng zsign ─────────────────────────────────────────────────
        #
        # BUGFIX v14 [CRITICAL]: code cũ gọi run_command(["zsign", ...]) — tức
        # subprocess tìm một binary tên "zsign" trong PATH của tiến trình
        # Android. Android KHÔNG có PATH kiểu Linux desktop và app không được
        # phép exec() file tuỳ ý ngoài nativeLibraryDir(); "zsign" không tồn
        # tại ở đâu cả trên máy thật (chỉ tồn tại trong Termux, môi trường
        # gốc mà tool này được port từ đó) → subprocess raise
        # FileNotFoundError: [Errno 2] No such file or directory: 'zsign'
        # — đúng lỗi thấy trong log người dùng report ("zsign thất bại:
        # [Errno 2] No such file or directory: 'zsign'").
        #
        # AppPaths.zsignPath() (Kotlin, app/.../bridge/AppPaths.kt) đã trỏ
        # đúng tới binary zsign thật được đóng gói cùng APK
        # (jniLibs/arm64-v8a/libzsign.so) — nhưng chưa từng được dùng ở đây.
        # Đồng thời phải set LD_LIBRARY_PATH=AppPaths.nativeDepsDir() khi
        # spawn, vì libzsign.so cần libssl.so.3/libcrypto.so.3/libc++_shared.so
        # đã được AppPaths.nativeDepsDir() tự giải nén sẵn (xem chú thích chi
        # tiết trong AppPaths.kt) — nếu không, linker không tìm thấy 3 thư
        # viện này và zsign thoát ngay với "CANNOT LINK EXECUTABLE".
        #
        # BUGFIX v15 [CRITICAL]: sau khi v14 làm zsign thực sự chạy được, nó
        # thoát ngay với "Invalid temp folder! /tmp" (exit 255). Nguyên nhân:
        # zsign (src/zsign.cpp, hàm main) mặc định dùng ZFile::GetTempFolder()
        # làm nơi giải nén IPA tạm thời và ghi ipa output tạm — trên
        # Linux/Android hàm này trả cứng "/tmp" (src/common/fs.cpp) nếu không
        # truyền cờ -t/--temp_folder. Android sandbox của mỗi app KHÔNG có
        # thư mục /tmp nào cả (mỗi app chỉ thấy filesDir riêng của mình), nên
        # zsign tự kiểm tra IsFolder("/tmp") thất bại và thoát ngay trước khi
        # kịp đọc bất kỳ tham số ký nào khác. zsign CÓ hỗ trợ chỉ định thư mục
        # tạm qua "-t <path>" — chỉ cần trỏ vào một thư mục ghi được bên trong
        # sideload_work (đã tồn tại, đã ghi được vì cert.pem/key.pem ở trên
        # cũng nằm trong đó) là đủ.
        print("Đang ký IPA bằng zsign...")
        signed_ipa = os.path.join(work_dir, "signed.ipa")
        zsign_tmp_dir = os.path.join(work_dir, "zsign_tmp")
        os.makedirs(zsign_tmp_dir, exist_ok=True)
        zsign_bin = AppPaths.zsignPath()
        zsign_env = {"LD_LIBRARY_PATH": AppPaths.nativeDepsDir()}
        # BUGFIX v12: run_command() trả về str (stdout) hoặc raise CalledProcessError,
        # KHÔNG phải tuple (bool, str). Unpacking "ok_sign, sign_out = run_command(...)"
        # gây ValueError ngay cả khi zsign thành công.
        try:
            run_command([
                zsign_bin,
                "-k", key_file,
                "-c", cert_file,
                "-m", profile_file,
                "-o", signed_ipa,
                "-z", "9",
                "-t", zsign_tmp_dir,
                ipa_path,
            ], extra_env=zsign_env)
            print("✅ Ký IPA thành công.")
        except FileNotFoundError as e:
            print(f"❌ zsign thất bại: không tìm thấy binary tại '{zsign_bin}' ({e}). "
                  f"Kiểm tra APK có đúng ABI arm64-v8a và jniLibs/arm64-v8a/libzsign.so "
                  f"có được đóng gói vào bản build này không.")
            return False
        except Exception as e:
            print(f"❌ zsign thất bại: {e}")
            return False

        # ── Cài đặt lên thiết bị qua USB ─────────────────────────────────────
        # BUGFIX (video crash log): device_link không có hàm connect_and_pair() —
        # hàm đúng là pair_device()/pair_with_device(), trả về một pair_record
        # (dict) thay vì bool, và raise LockdownError khi thất bại. Đồng thời
        # install_ipa(pair_record, remote_ipa_path) cần 2 tham số, không phải 1 —
        # phải gọi afc_push_ipa() trước để "stage" đường dẫn IPA local.
        print("Đang kết nối với thiết bị iOS qua USB...")
        try:
            pair_record = device_link.pair_device()
        except device_link.LockdownError as e:
            print(f"❌ Không kết nối được với thiết bị iOS: {e}")
            return False

        print("Đang cài đặt IPA lên thiết bị...")
        try:
            remote_ipa_path = device_link.afc_push_ipa(
                pair_record, signed_ipa, os.path.basename(signed_ipa))
            device_link.install_ipa(pair_record, remote_ipa_path)
        except device_link.LockdownError as e:
            print(f"❌ Cài đặt thất bại: {e}")
            return False

        print("✅ Cài đặt ứng dụng thành công!")
        return True

    except Exception as e:
        import traceback
        print(f"❌ Lỗi không mong đợi trong do_sideload: {e}")
        traceback.print_exc()   # → sys.stderr → _StderrBridge → NativeLog UI
        return False


def do_register_device(
    apple_id: str,
    password: str,
    udid: str,
    device_name: str = "iPhone (Android Sideload)",
    anisette_url: str = "",
) -> bool:
    """
    Đăng ký UDID thiết bị iOS vào tài khoản Apple Developer, tách rời khỏi
    luồng ký & cài IPA.
    BUGFIX v15 [NEW]: trước đây việc đăng ký UDID chỉ xảy ra ngầm bên trong
    do_sideload() (phải có sẵn file IPA + kết nối USB mới đăng ký được, và
    lỗi đăng ký chỉ lộ ra sau khi đã đăng nhập + giải nén IPA). Hàm này cho
    phép đăng ký UDID độc lập — hữu ích khi muốn thêm thiết bị vào team
    trước, hoặc đăng ký một UDID không phải thiết bị đang cắm USB ngay lúc
    đó (vd lấy UDID từ Cài đặt > Chung > Giới thiệu trên iPhone rồi nhập tay).
    Dùng lại đúng logic chống trùng lặp (list_devices trước khi add) và kiểm
    tra lỗi (last_error) như trong do_sideload(), không tự ý bỏ qua bước nào.
    """
    try:
        print("══ Bắt đầu đăng ký UDID thiết bị ══")
        udid = (udid or "").strip()
        if not udid:
            print("❌ Chưa có UDID để đăng ký — kết nối USB hoặc nhập UDID tay.")
            return False

        print("Đang đăng nhập Apple ID...")
        effective_anisette = anisette_url or config_manager.get_anisette_url()
        auth = AppleAuth(anisette_url=effective_anisette or None)
        session = auth.authenticate(apple_id, password)
        if not session or not session.get("authenticated"):
            print("❌ Đăng nhập Apple ID thất bại.")
            return False
        print("✅ Đăng nhập thành công.")

        dev_api = DeveloperAPI(auth, session["dsid"], session["session_token"])
        teams = dev_api.list_teams()
        if not teams:
            print("❌ Không lấy được Development Team.")
            return False
        team_id = teams[0].get("teamId") or teams[0].get("teamID") or teams[0].get("id")
        dev_api.set_team(team_id)
        print(f"Team: {team_id}")

        devices = dev_api.list_devices()
        existing = next(
            (d for d in devices
             if (d.get("deviceNumber") or d.get("attributes", {}).get("udid", "")) == udid),
            None,
        )
        if existing:
            name = existing.get("name") or existing.get("attributes", {}).get("name", "?")
            print(f"ℹ️  UDID {udid} đã được đăng ký sẵn trên team này (tên: {name}) — không cần đăng ký lại.")
            return True

        print(f"Đang đăng ký thiết bị UDID: {udid} (tên: {device_name})")
        if not dev_api.register_device(device_name, udid):
            err_msg = (dev_api.last_error or {}).get("userString") or "lỗi không xác định"
            print(f"❌ Không đăng ký được thiết bị UDID {udid}: {err_msg}")
            return False

        print(f"✅ Đăng ký thiết bị thành công: {udid}")
        return True

    except Exception as e:
        import traceback
        print(f"❌ Lỗi không mong đợi trong do_register_device: {e}")
        traceback.print_exc()
        return False


def do_revoke_certs(
    apple_id: str,
    password: str,
    anisette_url: str = "",
    cert_selector: str = "",
) -> bool:
    """Thu hồi certificate Development trên tài khoản Apple ID."""
    try:
        print("Đang đăng nhập & tra cứu chứng chỉ...")
        effective_anisette = anisette_url or config_manager.get_anisette_url()
        auth = AppleAuth(anisette_url=effective_anisette or None)
        # BUGFIX v11: authenticate() — không phải sign_in()
        session = auth.authenticate(apple_id, password)
        if not session or not session.get("authenticated"):
            print("❌ Đăng nhập Apple ID thất bại.")
            return False

        # BUGFIX v11: DeveloperAPI(auth, dsid, session_token) — không phải DeveloperAPI(session)
        dev_api = DeveloperAPI(auth, session["dsid"], session["session_token"])
        teams = dev_api.list_teams()
        if not teams:
            print("❌ Không lấy được team.")
            return False
        team_id = teams[0].get("teamId") or teams[0].get("teamID") or teams[0].get("id")
        dev_api.set_team(team_id)

        certs = dev_api.list_certificates()
        if not certs:
            print("Không có certificate nào trên tài khoản — không có gì để thu hồi.")
            return True

        print(f"Tài khoản hiện có {len(certs)} certificate:")
        for i, cert in enumerate(certs, 1):
            attrs = cert.get("attributes", {})
            print(f"  [{i}] id={cert.get('id', '?')} name={attrs.get('name', '?')} expires={attrs.get('expirationDate', '?')}")

        selector = (cert_selector or "").strip().lower()
        if not selector:
            print("Chưa chọn certificate nào để thu hồi.")
            return False

        targets = certs if selector == "all" else None
        if targets is None:
            try:
                idx = int(selector) - 1
                if not (0 <= idx < len(certs)):
                    print("Số thứ tự không hợp lệ.")
                    return False
                targets = [certs[idx]]
            except ValueError:
                print("Lựa chọn không hợp lệ — dùng số thứ tự hoặc 'all'.")
                return False

        all_ok = True
        for cert in targets:
            cert_id = cert.get("id")
            name = cert.get("attributes", {}).get("name", "?")
            ok = dev_api.revoke_certificate(cert_id)
            print(f"  → {'✅ Đã revoke' if ok else '❌ Revoke thất bại'}: {name} (id={cert_id})")
            all_ok = all_ok and ok

        state = _load_state()
        if state.get("certificate_id") and any(
            str(c.get("id")) == str(state.get("certificate_id")) for c in targets
        ):
            _clear_cert_from_state(state)

        print("\nXong.")
        return all_ok

    except Exception as e:
        import traceback
        print(f"❌ Lỗi trong do_revoke_certs: {e}")
        traceback.print_exc()
        return False


def do_diagnostics() -> str:
    """Chẩn đoán kết nối iPhone và môi trường sideload.

    Học từ lệnh "termux-usbmuxd doctor" — tự kiểm tra mọi thành phần
    và báo cáo vấn đề với gợi ý khắc phục cụ thể.

    Kiểm tra:
      1. Kết nối USB (iPhone có nhận diện không)
      2. Pairing (iPhone đã tin cậy thiết bị Android chưa)
      3. UDID của thiết bị (có lấy được không)
      4. Apple account / session (đã đăng nhập chưa)
      5. Anisette server (URL có phản hồi không)
      6. Dung lượng lưu trữ tạm thời
      7. Kết quả native diagnostics từ C layer

    Returns:
        Chuỗi báo cáo đã định dạng để in ra console hoặc hiển thị trên UI.
    """
    import os
    lines = []
    lines.append("═══════════════════════════════════════════════════")
    lines.append("  CHẨN ĐOÁN KẾT NỐI iPHONE  (termux-usbmuxd doctor)")
    lines.append("═══════════════════════════════════════════════════")

    # 1. Kiểm tra USB + UDID
    try:
        from device_link import diagnose as native_diag
        udid = get_connected_udid()
    except Exception:
        udid = None

    if udid:
        lines.append(f"✅ USB + UDID : {udid}")
    else:
        lines.append("❌ USB / UDID : Không lấy được UDID — iPhone chưa kết nối hoặc chưa ghép nối")
        lines.append("   → Gợi ý: Cắm cáp Lightning/USB-C, bấm 'Kết nối', bấm 'Tin cậy' trên iPhone")

    # 2. Kiểm tra Pairing
    try:
        from com.superalpha.sideload.bridge import DeviceNative
        paired_diag = DeviceNative.diagnostics()
        is_paired = "paired: true" in (paired_diag or "").lower()
    except Exception:
        is_paired = False
        paired_diag = None

    lines.append(f"{'✅' if is_paired else '⚠️ '} Pairing    : {'đã ghép nối' if is_paired else 'chưa ghép nối — chạy bước Pair trước'}")

    # 3. Kiểm tra Apple Account / state
    apple_id = get_saved_apple_id()
    if apple_id:
        lines.append(f"✅ Apple ID  : {apple_id}")
    else:
        lines.append("⚠️  Apple ID  : chưa lưu — cần nhập Apple ID và mật khẩu")

    # 4. Kiểm tra Anisette URL
    anisette_url = get_saved_anisette_url()
    if anisette_url:
        lines.append(f"ℹ️  Anisette  : {anisette_url}")
        try:
            import urllib.request
            with urllib.request.urlopen(anisette_url + "/", timeout=3) as r:
                lines.append(f"✅ Anisette  : server phản hồi OK (status={r.status})")
        except Exception as e:
            lines.append(f"⚠️  Anisette  : server không phản hồi ({e})")
            lines.append("   → Gợi ý: Khởi động lại Anisette server hoặc chọn server khác")
    else:
        lines.append("⚠️  Anisette  : chưa cài đặt URL")

    # 5. Kiểm tra dung lượng lưu trữ tạm
    try:
        st = os.statvfs("/data/local/tmp")
        free_mb = st.f_bavail * st.f_frsize // (1024 * 1024)
        lines.append(f"{'✅' if free_mb > 50 else '⚠️ '} Dung lượng : {free_mb} MB trống (/data/local/tmp)")
        if free_mb <= 50:
            lines.append("   → Gợi ý: Giải phóng bộ nhớ trong thiết bị (xoá app/file không cần)")
    except Exception:
        lines.append("ℹ️  Dung lượng : không kiểm tra được")

    # 6. Native diagnostics (C layer)
    if paired_diag:
        lines.append("")
        lines.append("─── Native layer (C) ───")
        for ln in paired_diag.splitlines():
            lines.append(f"  {ln}")

    # 7. Installed apps count
    try:
        from device_link import list_installed_apps
        apps = list_installed_apps({})
        lines.append(f"ℹ️  Apps cài   : {len(apps)} user app(s) trên thiết bị")
    except Exception:
        pass

    lines.append("═══════════════════════════════════════════════════")
    report = "\n".join(lines)
    print(report)
    return report
