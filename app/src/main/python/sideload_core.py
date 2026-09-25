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
    set_bundle_id, set_extension_bundle_id, find_extensions,
    save_certificate_as_pem, decode_apple_data_field,
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


# ── Nhập liệu qua UI ──────────────────────────────────────────────────────────
#
# BUGFIX v49: AppleAuth() trước đây được tạo KHÔNG có input_func, nên khi Apple
# hỏi mã 2FA, apple_auth gọi input() của Python — trên Android không có stdin →
# EOFError → 2FA luôn thất bại. Nay mọi câu hỏi đi qua UiPrompt (dialog).

def _ui_input(prompt) -> str:
    try:
        value = UiPrompt.requestInput(str(prompt))
        return "" if value is None else str(value)
    except Exception as e:
        print(f"[ui] Không hiện được hộp nhập liệu: {e}")
        return ""


def _new_auth(anisette_url: str = ""):
    effective = anisette_url or config_manager.get_anisette_url()
    return AppleAuth(anisette_url=effective or None, input_func=_ui_input)


def _login(apple_id: str, password: str, anisette_url: str = ""):
    """Đăng nhập Apple ID → (auth, dev_api, team_id) hoặc (None, None, None)."""
    print("Đang đăng nhập Apple ID...")
    auth = _new_auth(anisette_url)
    session = auth.authenticate(apple_id, password)
    if not session or not session.get("authenticated"):
        print("❌ Đăng nhập Apple ID thất bại.")
        return None, None, None
    if session.get("authenticated") == "2fa_completed":
        # chuỗi "2fa_completed" là truthy nhưng KHÔNG có session_token
        print("ℹ️  2FA đã xác nhận nhưng Apple chưa cấp phiên đầy đủ — bấm chạy lại một lần nữa.")
        return None, None, None
    print("✅ Đăng nhập thành công.")
    dev_api = DeveloperAPI(auth, session["dsid"], session["session_token"])
    teams = dev_api.list_teams()
    if not teams:
        print("❌ Không lấy được Development Team.")
        return None, None, None
    team_id = teams[0].get("teamId") or teams[0].get("teamID") or teams[0].get("id")
    dev_api.set_team(team_id)
    print(f"Team: {team_id}")
    return auth, dev_api, team_id


# ── UDID ──────────────────────────────────────────────────────────────────────

def _normalize_udid(value) -> str:
    s = "".join(str(value or "").split())
    if len(s) == 24 and "-" not in s:          # iPhone XS+: 8-16
        s = s[:8] + "-" + s[8:]
    return s


def _looks_like_udid(value) -> bool:
    s = _normalize_udid(value)
    hexchars = [c for c in s if c != "-"]
    return (24 <= len(s) <= 64 and len(hexchars) >= 24
            and all(c in "0123456789abcdefABCDEF" for c in hexchars)
            and any(c != "0" for c in hexchars))


# ── App ID + provisioning profile ─────────────────────────────────────────────
#
# BUGFIX v49 (port từ bản Termux đã chạy thật trên máy người dùng):
#   (1) IPA PHẢI mang đúng App ID đã nộp cho Apple. Bản cũ gọi set_bundle_id()
#       trên thư mục đã giải nén rồi lại đưa FILE IPA GỐC cho zsign → mọi thay
#       đổi bundle id bị bỏ qua, profile (application-identifier = TEAM.<id mới>)
#       lệch bundle id thật → iOS từ chối (ApplicationVerificationFailed).
#       Nay: đổi bundle id → tải profile → ký CHÍNH thư mục .app đã sửa.
#   (2) Mỗi .appex (widget, share extension…) cần App ID + profile RIÊNG khớp
#       bundle id của nó; zsign nhận nhiều -m (app chính trước). Bản cũ chỉ có
#       một profile cho cả gói → extension ký sai entitlements → không cài được.
#   (3) 9401 "not available" = bundle id bị TÀI KHOẢN KHÁC chiếm (App ID là duy
#       nhất toàn cầu) → dùng id phái sinh; quota 10 App ID/7 ngày → tái dùng
#       App ID sẵn có chưa bị app nào trên máy chiếm. Lựa chọn được ghi vào
#       state["app_id_map"] để lần sau cập nhật đúng app cũ, không tốn quota.

def _first_present(d, keys, default=None):
    for k in keys:
        v = (d or {}).get(k)
        if v not in (None, ""):
            return v
    return default


def _app_id_identifier(app_id) -> str:
    return str((app_id or {}).get("identifier") or (app_id or {}).get("bundleId") or "").strip()


def _app_id_key(app_id):
    return _first_present(app_id or {}, ["appIdId", "id"])


def _find_app_id(app_ids, identifier, ignore_case=False):
    target = str(identifier or "")
    for a in app_ids or []:
        ident = _app_id_identifier(a)
        if not ident or "*" in ident:
            continue
        if ident == target or (ignore_case and ident.lower() == target.lower()):
            return a
    return None


def _error_text(dev_api) -> str:
    err = getattr(dev_api, "last_error", None)
    if not isinstance(err, dict):
        return str(err or "")
    parts = [str(err.get("resultCode", "")), str(err.get("userString", ""))]
    raw = err.get("raw") or {}
    for ve in (raw.get("validationErrors") or err.get("validationErrors") or []):
        parts.append(str(ve))
    return " | ".join(p for p in parts if p and p != "None")


def _is_identifier_taken(dev_api) -> bool:
    if classify_app_id_error(getattr(dev_api, "last_error", None)) == "unavailable":
        return True
    low = _error_text(dev_api).lower()
    return "already been registered" in low or "already exists" in low


def _is_app_id_limit(dev_api) -> bool:
    err = getattr(dev_api, "last_error", None) or {}
    if str(err.get("resultCode", "")) == "9120":
        return True
    return classify_app_id_error(err) == "quota"


def _suffixed_identifier(base: str) -> str:
    import re
    base = re.sub(r"[^A-Za-z0-9.\-]", "", str(base)).strip(".").strip("-")
    while ".." in base:
        base = base.replace("..", ".")
    suffix = "s" + uuid.uuid4().hex[:6]
    room = 128 - len(suffix) - 1
    if len(base) > room:
        base = base[:room].rstrip(".")
    return f"{base}.{suffix}"


def _installed_bundle_ids() -> set:
    try:
        return set(device_link.list_installed_apps({}) or [])
    except Exception as e:
        print(f"[appid] Không đọc được danh sách app trên iPhone: {e}")
        return set()


def _choose_replacement_app_id(app_ids, original_bundle_id, installed):
    exact, prefixed, other = [], [], []
    for a in app_ids or []:
        ident = _app_id_identifier(a)
        if not ident or "*" in ident or ident in installed:
            continue
        if ident == original_bundle_id:
            exact.append(a)
        elif ident.startswith(original_bundle_id + ".") or ident.startswith(original_bundle_id + "-"):
            prefixed.append(a)
        else:
            other.append(a)
    for bucket in (exact, prefixed, other):
        if bucket:
            return _app_id_identifier(bucket[0]), bucket[0]
    return None, None


def _remember_app_id(state, team_id, base_bundle, ident, app_id):
    state.setdefault("app_id_map", {})[f"{team_id}:{base_bundle}"] = {
        "effective_bundle": ident, "app_id_id": _app_id_key(app_id)}
    _save_state(state)


def _resolve_main_app_id(dev_api, app_ids, bundle_id, app_name, state, team_id):
    """→ (app_id_dict, final_identifier) hoặc (None, None)."""
    remembered = (state.get("app_id_map") or {}).get(f"{team_id}:{bundle_id}") or {}
    rem_ident = remembered.get("effective_bundle")
    if rem_ident:
        found = _find_app_id(app_ids, rem_ident)
        if found:
            print(f"[appid] ♻️  Dùng lại App ID đã chọn lần trước: {rem_ident}")
            return found, rem_ident

    existing = _find_app_id(app_ids, bundle_id) or _find_app_id(app_ids, bundle_id, ignore_case=True)
    if existing:
        ident = _app_id_identifier(existing)
        print(f"[appid] ✅ Đã có App ID '{ident}' — dùng lại.")
        _remember_app_id(state, team_id, bundle_id, ident, existing)
        return existing, ident

    print(f"[appid] Chưa có App ID cho '{bundle_id}' — đang tạo...")
    created = dev_api.create_app_id(bundle_id, app_name)
    if created:
        print(f"[appid] ✅ Đã tạo App ID '{bundle_id}'.")
        app_ids.append(created)
        _remember_app_id(state, team_id, bundle_id, bundle_id, created)
        return created, bundle_id

    refreshed = dev_api.list_app_ids()
    again = _find_app_id(refreshed, bundle_id) or _find_app_id(refreshed, bundle_id, ignore_case=True)
    if again:
        ident = _app_id_identifier(again)
        print(f"[appid] ✅ Liệt kê lại thì thấy App ID '{ident}' — dùng lại.")
        app_ids[:] = refreshed
        _remember_app_id(state, team_id, bundle_id, ident, again)
        return again, ident

    taken, limit = _is_identifier_taken(dev_api), _is_app_id_limit(dev_api)
    print(f"[appid] ❌ Apple từ chối App ID '{bundle_id}': {_error_text(dev_api)}")
    if not (taken or limit):
        return None, None
    if taken:
        print("[appid]    → bundle id này đã bị MỘT TÀI KHOẢN APPLE KHÁC đăng ký (App ID là duy nhất"
              " toàn cầu — rất hay gặp với SideStore/AltStore). Sẽ đổi bundle id của IPA.")
    if limit:
        print("[appid]    → tài khoản đã hết lượt tạo App ID (10 / 7 ngày). Sẽ tái dùng App ID sẵn có.")

    for a in app_ids:                          # App ID phái sinh từ lần chạy trước
        ident = _app_id_identifier(a)
        if "*" not in ident and (ident.startswith(bundle_id + ".") or ident.startswith(bundle_id + "-")):
            print(f"[appid] ♻️  Tái dùng App ID phái sinh có sẵn: {ident}")
            _remember_app_id(state, team_id, bundle_id, ident, a)
            return a, ident

    if not limit:
        for _ in range(4):
            candidate = _suffixed_identifier(bundle_id)
            print(f"[appid] Đang tạo App ID thay thế: {candidate}...")
            new_app_id = dev_api.create_app_id(candidate, app_name)
            if new_app_id:
                print(f"[appid] ✅ Đã tạo App ID thay thế: {candidate}")
                app_ids.append(new_app_id)
                _remember_app_id(state, team_id, bundle_id, candidate, new_app_id)
                return new_app_id, candidate
            if _is_identifier_taken(dev_api):
                continue
            if _is_app_id_limit(dev_api):
                print("[appid] ⚠️  Đã chạm giới hạn App ID — chuyển sang tái dùng App ID sẵn có.")
            else:
                print(f"[appid] ❌ Không tạo được '{candidate}': {_error_text(dev_api)}")
            break

    new_id, app_id = _choose_replacement_app_id(app_ids, bundle_id, _installed_bundle_ids())
    if new_id:
        print(f"[appid] ♻️  Dùng App ID sẵn có chưa bị app nào trên iPhone chiếm: {new_id}")
        _remember_app_id(state, team_id, bundle_id, new_id, app_id)
        return app_id, new_id
    print("[appid] ❌ Hết cách: bundle id gốc bị chiếm, hết quota tạo App ID và không còn App ID"
          " trống. Chờ hết chu kỳ 7 ngày hoặc dọn App ID cũ trên developer.apple.com.")
    return None, None


def _ensure_app_id(dev_api, app_ids, identifier, display_name):
    """App ID cho một .appex → (app_id_dict, final_identifier) hoặc (None, None)."""
    found = _find_app_id(app_ids, identifier) or _find_app_id(app_ids, identifier, ignore_case=True)
    if found:
        print(f"[appid] ✅ Đã có App ID cho extension '{_app_id_identifier(found)}' — dùng lại.")
        return found, _app_id_identifier(found)
    for _ in range(4):
        created = dev_api.create_app_id(identifier, display_name)
        if created:
            print(f"[appid] ✅ Đã tạo App ID '{identifier}'.")
            app_ids.append(created)
            return created, identifier
        if _is_identifier_taken(dev_api):
            new_identifier = _suffixed_identifier(identifier)
            print(f"[appid] ⚠️  '{identifier}' đã bị tài khoản khác đăng ký → thử '{new_identifier}'")
            identifier = new_identifier
            continue
        print(f"[appid] ❌ Không tạo được App ID '{identifier}': {_error_text(dev_api)}")
        return None, None
    return None, None


def _remove_extensions(app_bundle_path):
    removed = []
    for sub in ("PlugIns", "Extensions"):
        d = os.path.join(app_bundle_path, sub)
        if os.path.isdir(d):
            removed.extend(sorted(os.listdir(d)))
            shutil.rmtree(d, ignore_errors=True)
    print(f"[appid] 🗑  Đã bỏ {len(removed)} extension: {', '.join(removed) or '(không có)'}")


def _ask_drop_extensions(failed_id) -> bool:
    answer = _ui_input(
        f"Không đăng ký được App ID cho extension '{failed_id}' (thường do hết 10 App ID/7 ngày).\n"
        "App chính vẫn cài được nếu bỏ extension (mất widget/share extension…).\n"
        "Gõ C để bỏ extension và tiếp tục, bỏ trống để huỷ:")
    return answer.strip().lower() in ("c", "co", "có", "y", "yes")


def _prepare_app_ids_and_profiles(dev_api, app_bundle_path, bundle_id, app_name, state, team_id):
    """→ list[(bundle_path, bundle_id, profile_path)] (phần tử đầu = app chính)."""
    app_ids = dev_api.list_app_ids()
    print(f"[appid] Tài khoản đang có {len(app_ids)} App ID.")
    main_app_id, final_bundle_id = _resolve_main_app_id(dev_api, app_ids, bundle_id, app_name, state, team_id)
    if not main_app_id:
        return None
    final_bundle_id = _app_id_identifier(main_app_id) or final_bundle_id
    if final_bundle_id != bundle_id:
        print(f"[appid] Ghi đè bundle id trong IPA cho khớp App ID đã nộp: {bundle_id} → {final_bundle_id}")
        set_bundle_id(app_bundle_path, final_bundle_id)

    targets = [(app_bundle_path, final_bundle_id, main_app_id)]
    for appex_path, appex_id in find_extensions(app_bundle_path):     # đọc SAU khi đã đổi
        label = f"{app_name} {os.path.splitext(os.path.basename(appex_path))[0]}"
        appex_app_id, final_appex_id = _ensure_app_id(dev_api, app_ids, appex_id, label)
        if not appex_app_id:
            if _ask_drop_extensions(appex_id):
                _remove_extensions(app_bundle_path)
                targets = targets[:1]
                break
            return None
        if final_appex_id != appex_id:
            print(f"[appid] Ghi đè bundle id của extension: {appex_id} → {final_appex_id}")
            set_extension_bundle_id(appex_path, final_appex_id)
        targets.append((appex_path, final_appex_id, appex_app_id))

    result = []
    for bundle_path, ident, app_id_obj in targets:
        app_id_id = _app_id_key(app_id_obj)
        if not app_id_id:
            print(f"[profile] ❌ App ID '{ident}' không có appIdId.")
            return None
        print(f"[profile] Tải provisioning profile cho {ident}...")
        profile = dev_api.download_provisioning_profile(app_id_id)
        raw = _first_present(profile or {}, ["encodedProfile", "profileContent", "content"])
        if not raw:
            print(f"[profile] ❌ Không tải được profile cho {ident} (thiết bị đã vào team chưa?).")
            return None
        data = decode_apple_data_field(raw)
        out_path = os.path.join(bundle_path, "embedded.mobileprovision")
        with open(out_path, "wb") as f:
            f.write(data if isinstance(data, bytes) else data.encode())
        print(f"[profile] ✅ Nhúng {len(data)} byte vào {os.path.basename(bundle_path)}")
        result.append((bundle_path, ident, out_path))
    return result


# ── Public API ────────────────────────────────────────────────────────────────

def do_sideload(
    ipa_path: str,
    apple_id: str,
    password: str,
    udid_override: str = "",
    anisette_url: str = "",
) -> bool:
    """Ký và cài đặt IPA lên iPhone đang cắm USB."""
    try:
        print("══ Bắt đầu quá trình sideload ══")
        if not ipa_path or not os.path.isfile(ipa_path):
            print(f"❌ Không tìm thấy file IPA: {ipa_path}")
            return False

        # ── Bước 0: kết nối + ghép nối iPhone TRƯỚC (lấy UDID thật) ──────────
        # Bản cũ lấy UDID từ nhiều nguồn, fallback cuối là... đường dẫn filesDir,
        # rồi đăng ký chuỗi đó với Apple. UDID đúng nhất là UniqueDeviceID do
        # lockdownd của chính iPhone trả về; kiểm tra USB trước cũng giúp không
        # tốn lượt App ID khi cáp/ghép nối còn lỗi.
        print("[Bước 0/5] Kết nối và ghép nối iPhone qua USB...")
        try:
            pair_record = device_link.pair_device()
        except device_link.LockdownError as e:
            print(f"❌ Không kết nối/ghép nối được iPhone: {e}")
            return False
        udid = _normalize_udid(pair_record.get("udid"))
        if not _looks_like_udid(udid):
            for cand in (udid_override, _current_udid):
                if _looks_like_udid(cand):
                    udid = _normalize_udid(cand)
                    break
        if not _looks_like_udid(udid):
            print("❌ Không đọc được UDID của iPhone — không thể đăng ký thiết bị với Apple.")
            return False
        print(f"UDID: {udid}")

        # ── Bước 1: Apple ID ────────────────────────────────────────────────
        print("[Bước 1/5] Đăng nhập Apple ID...")
        auth, dev_api, team_id = _login(apple_id, password, anisette_url)
        if not dev_api:
            return False

        devices = dev_api.list_devices()
        registered = any(
            str(d.get("deviceNumber") or d.get("attributes", {}).get("udid", "")).lower() == udid.lower()
            for d in devices
        )
        if not registered:
            name = f"iPhone-{udid.replace('-', '')[:8]}"
            print(f"Đang đăng ký thiết bị (name='{name}', UDID={udid})...")
            if not dev_api.register_device(name, udid):
                err = (dev_api.last_error or {}).get("userString") or "lỗi không xác định"
                print(f"❌ Apple từ chối đăng ký thiết bị {udid}: {err}")
                print("   Dừng lại: profile tải về sẽ không chứa máy này → cài chắc chắn thất bại.")
                return False
            print(f"✅ Thiết bị đã vào team: {udid}")
        else:
            print("Thiết bị đã có trong team.")

        # ── Bước 2: Certificate ─────────────────────────────────────────────
        print("[Bước 2/5] Chuẩn bị certificate...")
        work_dir = os.path.join(str(AppPaths.filesDir()), "sideload_work")
        if os.path.exists(work_dir):
            shutil.rmtree(work_dir, ignore_errors=True)
        os.makedirs(work_dir, exist_ok=True)

        state = _load_state()
        cert_id, cert_pem, key_pem = state.get("certificate_id"), state.get("certificate_pem"), state.get("private_key_pem")
        reuse = False
        if cert_id and cert_pem and key_pem:
            if any(str(c.get("id")) == str(cert_id) for c in dev_api.list_certificates()):
                print(f"Dùng lại certificate: {cert_id}")
                reuse = True
        if not reuse:
            print("Đang tạo certificate mới...")
            cert_data = dev_api.create_certificate()
            if not cert_data:
                print("❌ Không tạo được certificate. Nếu tài khoản đã đủ số certificate, vào mục"
                      " \"Thu hồi chứng chỉ\" để revoke cái cũ rồi chạy lại.")
                return False
            cert_id = cert_data.get("certificateId") or cert_data.get("id", "")
            raw_cert = cert_data.get("certContent") or cert_data.get("certificateContent") or b""
            cert_bytes = decode_apple_data_field(raw_cert)
            if not cert_bytes:
                print("❌ Apple không trả nội dung certificate.")
                return False
            import base64 as _b64
            if isinstance(cert_bytes, bytes) and not cert_bytes.startswith(b"-----"):
                cert_pem = ("-----BEGIN CERTIFICATE-----\n" + _b64.encodebytes(cert_bytes).decode("ascii")
                            + "-----END CERTIFICATE-----\n")
            else:
                cert_pem = cert_bytes.decode("utf-8") if isinstance(cert_bytes, bytes) else cert_bytes
            key_pem = cert_data.get("_private_key_pem", "")
            if not key_pem:
                print("❌ Không có private key cho certificate mới.")
                return False
            state.update({"certificate_id": cert_id, "certificate_pem": cert_pem, "private_key_pem": key_pem})
            _save_state(state)
            print(f"✅ Tạo certificate thành công: {cert_id}")
        cert_file = os.path.join(work_dir, "cert.pem")
        key_file = os.path.join(work_dir, "key.pem")
        with open(cert_file, "w") as f:
            f.write(cert_pem)
        with open(key_file, "w") as f:
            f.write(key_pem)

        # ── Bước 3: App ID + profile (IPA đổi theo App ID TRƯỚC khi ký) ──────
        print("[Bước 3/5] App ID + provisioning profile...")
        extracted = os.path.join(work_dir, "extracted")
        extract_ipa(ipa_path, extracted)
        app_dir = find_app_bundle(extracted)
        bundle_id = get_bundle_id(app_dir)
        app_name = get_app_name(app_dir)
        print(f"Ứng dụng: {app_name} ({bundle_id})")
        targets = _prepare_app_ids_and_profiles(dev_api, app_dir, bundle_id, app_name, state, team_id)
        if not targets:
            print("❌ Không chuẩn bị được App ID / provisioning profile — dừng.")
            return False
        print(f"[appid] Bundle id trong IPA: {targets[0][1]}"
              + "".join(f"\n[appid]   + extension: {t[1]}" for t in targets[1:]))

        # ── Bước 4: ký bằng zsign ───────────────────────────────────────────
        # Ký THƯ MỤC .app đã sửa (không phải IPA gốc), một -m cho mỗi bundle
        # (app chính trước), KHÔNG -b (bundle id đã sửa bằng Python). zsign cần
        # -t (Android không có /tmp) và LD_LIBRARY_PATH cho libssl/libcrypto.
        print("[Bước 4/5] Ký IPA bằng zsign...")
        import re as _re
        safe_name = _re.sub(r"[^\w.\-]", "_", app_name).strip("._") or "app"
        signed_ipa = os.path.join(work_dir, f"{safe_name}_signed.ipa")
        zsign_tmp_dir = os.path.join(work_dir, "zsign_tmp")
        os.makedirs(zsign_tmp_dir, exist_ok=True)
        zsign_bin = AppPaths.zsignPath()
        cmd = [zsign_bin, "-f", "-k", key_file, "-c", cert_file]
        for _bp, _ident, prof in targets:
            cmd += ["-m", prof]
        cmd += ["-o", signed_ipa, "-z", "9", "-t", zsign_tmp_dir, app_dir]
        try:
            run_command(cmd, extra_env={"LD_LIBRARY_PATH": AppPaths.nativeDepsDir()})
        except FileNotFoundError as e:
            print(f"❌ Không chạy được zsign tại '{zsign_bin}' ({e}).")
            return False
        except Exception as e:
            print(f"❌ zsign thất bại: {e}")
            return False
        if not os.path.isfile(signed_ipa):
            print("❌ zsign không tạo ra file IPA đã ký.")
            return False
        print(f"✅ Đã ký: {os.path.basename(signed_ipa)} ({os.path.getsize(signed_ipa) / 1048576:.2f} MB)")

        # ── Bước 5: cài qua USB (AFC + installation_proxy) ──────────────────
        print("[Bước 5/5] Cài đặt lên iPhone...")
        try:
            remote = device_link.afc_push_ipa(pair_record, signed_ipa, os.path.basename(signed_ipa))
            device_link.install_ipa(pair_record, remote)
        except device_link.LockdownError as e:
            print(f"❌ Cài đặt thất bại: {e}")
            print(f"   File đã ký vẫn còn ở: {signed_ipa}")
            return False
        print("✅ Cài đặt ứng dụng thành công! (Lần đầu mở app: Cài đặt > Cài đặt chung > "
              "Quản lý VPN & Thiết bị > tin cậy Apple ID của bạn.)")
        return True

    except Exception as e:
        import traceback
        print(f"❌ Lỗi không mong đợi trong do_sideload: {e}")
        traceback.print_exc()
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

        udid = _normalize_udid(udid)
        if not _looks_like_udid(udid):
            print(f"❌ '{udid}' không phải UDID hợp lệ (40 ký tự hex, hoặc dạng 00008xxx-xxxxxxxxxxxxxxxx).")
            return False
        _auth, dev_api, _team = _login(apple_id, password, anisette_url)
        if not dev_api:
            return False

        devices = dev_api.list_devices()
        existing = next(
            (d for d in devices
             if str(d.get("deviceNumber") or d.get("attributes", {}).get("udid", "")).lower() == udid.lower()),
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
        _auth, dev_api, _team = _login(apple_id, password, anisette_url)
        if not dev_api:
            return False

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
