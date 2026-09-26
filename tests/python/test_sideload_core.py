#!/usr/bin/env python3
"""Kiểm thử luồng do_sideload() của app Android mà KHÔNG cần Android/Chaquopy.

Stub: com.superalpha.sideload.bridge (AppPaths/NativeLog/UiPrompt/DeviceNative),
apple_auth (cần srp/cryptography), DeveloperAPI (giả lập Apple: bundle id gốc bị
tài khoản khác chiếm → 9401), zsign (script ghi lại tham số).

Chạy:  python3 tests/python/test_sideload_core.py
"""
import io
import json
import os
import plistlib
import shutil
import stat
import sys
import tempfile
import types
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
PY_DIR = os.path.normpath(os.path.join(HERE, "..", "..", "app", "src", "main", "python"))
sys.path.insert(0, PY_DIR)

UDID = "00008030-001A35E80C41802E"
MAIN_ID = "com.SideStore.SideStore"
APPEX_ID = "com.SideStore.SideStore.AltWidget"
TEAM = "R58DAC9MPN"

failures = []


def check(cond, msg):
    print(("  ✅ " if cond else "  ❌ ") + msg)
    if not cond:
        failures.append(msg)


# ── stub module Chaquopy/Kotlin ───────────────────────────────────────────────
WORK = tempfile.mkdtemp(prefix="sideload_core_test_")
ZSIGN_LOG = os.path.join(WORK, "zsign_calls.json")
FAKE_ZSIGN = os.path.join(WORK, "libzsign.so")
with open(FAKE_ZSIGN, "w") as f:
    f.write("""#!/usr/bin/env python3
import json, os, sys, zipfile
args = sys.argv[1:]
log = %r
calls = json.load(open(log)) if os.path.exists(log) else []
calls.append({"args": args, "LD_LIBRARY_PATH": os.environ.get("LD_LIBRARY_PATH")})
json.dump(calls, open(log, "w"))
out = args[args.index("-o") + 1]
src = args[-1]
with zipfile.ZipFile(out, "w") as z:
    base = os.path.dirname(src)
    for root, _d, files in os.walk(src):
        for fn in files:
            p = os.path.join(root, fn)
            z.write(p, os.path.join("Payload", os.path.relpath(p, base)))
print("Signed OK!")
""" % ZSIGN_LOG)
os.chmod(FAKE_ZSIGN, os.stat(FAKE_ZSIGN).st_mode | stat.S_IEXEC)

ui_answers = []
ui_prompts = []
native_calls = {"connectAndPair": 0, "sideloadIpa": [], "embedded": [], "prefs": []}


class AppPaths:
    @staticmethod
    def filesDir():
        return WORK

    @staticmethod
    def zsignPath():
        return FAKE_ZSIGN

    @staticmethod
    def nativeDepsDir():
        return os.path.join(WORK, "native_deps")


class NativeLog:
    @staticmethod
    def log(tag, msg):
        pass

    @staticmethod
    def emit(msg):
        pass


class UiPrompt:
    @staticmethod
    def requestInput(prompt):
        ui_prompts.append(prompt)
        return ui_answers.pop(0) if ui_answers else ""


class DeviceNative:
    connect_ok = True

    @staticmethod
    def connectAndPair():
        native_calls["connectAndPair"] += 1
        return DeviceNative.connect_ok

    @staticmethod
    def getUdid():
        return UDID

    @staticmethod
    def sideloadIpa(path):
        native_calls["sideloadIpa"].append(path)
        return True

    @staticmethod
    def writePairingFileToApp(bundle_id, rel_path):
        native_calls["embedded"].append((bundle_id, rel_path))
        return True

    @staticmethod
    def writeSideStorePrefs(bundle_id):
        native_calls["prefs"].append(bundle_id)
        return True

    # v60: list hoặc chuỗi "\n"-join — mô phỏng cả 2 dạng Chaquopy trả về
    installed_raw = []

    @staticmethod
    def listInstalledApps():
        return DeviceNative.installed_raw

    @staticmethod
    def reset():
        pass


bridge = types.ModuleType("com.superalpha.sideload.bridge")
bridge.AppPaths, bridge.NativeLog, bridge.UiPrompt, bridge.DeviceNative = AppPaths, NativeLog, UiPrompt, DeviceNative
for name in ("com", "com.superalpha", "com.superalpha.sideload"):
    sys.modules.setdefault(name, types.ModuleType(name))
sys.modules["com.superalpha.sideload.bridge"] = bridge

auth_instances = []


class FakeAppleAuth:
    result = {"authenticated": True, "dsid": "123", "session_token": "tok"}

    def __init__(self, anisette_url=None, input_func=None):
        self.anisette_url = anisette_url
        self.input_func = input_func
        self.session = None
        auth_instances.append(self)

    def authenticate(self, apple_id, password):
        return dict(FakeAppleAuth.result)

    def generate_anisette_headers(self):
        return {}


fake_auth_mod = types.ModuleType("apple_auth")
fake_auth_mod.AppleAuth = FakeAppleAuth
fake_auth_mod.fetch_official_servers = lambda: []
sys.modules["apple_auth"] = fake_auth_mod
try:
    import requests  # noqa: F401
except Exception:
    sys.modules["requests"] = types.ModuleType("requests")

import sideload_core as core  # noqa: E402


# ── Apple giả lập ─────────────────────────────────────────────────────────────
class FakeDevAPI:
    accept_device = True
    instances = []

    # v60: mô phỏng giới hạn 10 App ID / 7 ngày + wildcard tuỳ chọn
    app_id_limit = False
    has_wildcard = True
    extra_app_ids = []

    def __init__(self, auth, dsid, token):
        self.last_error = None
        self.team_id = None
        self.devices = []
        self.app_ids = []
        if FakeDevAPI.has_wildcard:
            # Wildcard toàn team của Apple = App ID identifier "*" → profile
            # application-identifier "<TeamID>.*" che phủ mọi bundle id.
            self.app_ids.append({"identifier": "*", "appIdId": "WILD"})
        self.app_ids.extend(dict(a) for a in FakeDevAPI.extra_app_ids)
        self.registered = []
        self.created_ids = []
        self.profiles_for = []
        FakeDevAPI.instances.append(self)

    def list_teams(self):
        return [{"teamId": TEAM}]

    def set_team(self, t):
        self.team_id = t

    def list_devices(self):
        return list(self.devices)

    def register_device(self, name, udid):
        self.registered.append((name, udid))
        if not FakeDevAPI.accept_device:
            self.last_error = {"resultCode": 35, "userString": "Invalid device number"}
            return None
        d = {"deviceNumber": udid, "name": name}
        self.devices.append(d)
        return d

    # v51: hỗ trợ test tự động thu hồi cert khi hết chỗ
    certs = []            # các certificate đang có trên "tài khoản"
    cert_fail_first = False   # create_certificate() thất bại 1 lần đầu rồi OK
    revoked = []           # các id đã revoke

    def list_certificates(self):
        return list(self.certs)

    def revoke_certificate(self, certificate_id):
        self.revoked.append(certificate_id)
        self.certs = [c for c in self.certs if c.get("id") != certificate_id]
        return True

    def create_certificate(self):
        if self.cert_fail_first:
            self.cert_fail_first = False
            self.last_error = {"resultCode": -1, "userString": "403 Forbidden (limit)"}
            return None
        return {"certificateId": "CERT1", "certContent": b"\x30\x82\x01\x00fake-der",
                "_private_key_pem": "-----BEGIN RSA PRIVATE KEY-----\nMII\n-----END RSA PRIVATE KEY-----\n"}

    def list_app_ids(self):
        return [dict(a) for a in self.app_ids]

    def create_app_id(self, identifier, name):
        if FakeDevAPI.app_id_limit:
            self.last_error = {"resultCode": 9120, "userString":
                               "Your maximum App ID limit has been reached. You may create up to "
                               "10 App IDs every 7 days."}
            return None
        if identifier == MAIN_ID:
            self.last_error = {"resultCode": 9401, "userString":
                               f"An App ID with Identifier '{identifier}' is not available. "
                               "Please enter a different string."}
            return None
        a = {"identifier": identifier, "appIdId": f"ID{len(self.app_ids)}", "name": name}
        self.app_ids.append(a)
        self.created_ids.append(identifier)
        self.last_error = None
        return dict(a)

    def download_provisioning_profile(self, app_id_id):
        ident = next(a["identifier"] for a in self.app_ids if a["appIdId"] == app_id_id)
        self.profiles_for.append(ident)
        body = plistlib.dumps({"Entitlements": {"application-identifier": f"{TEAM}.{ident}"},
                               "ProvisionedDevices": [d["deviceNumber"] for d in self.devices]})
        return {"encodedProfile": body}


core.DeveloperAPI = FakeDevAPI


def make_ipa(path):
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("Payload/SideStore.app/Info.plist", plistlib.dumps({
            "CFBundleIdentifier": MAIN_ID, "CFBundleDisplayName": "SideStore", "CFBundleExecutable": "SideStore"}))
        z.writestr("Payload/SideStore.app/SideStore", b"\xcf\xfa\xed\xfe binary")
        z.writestr("Payload/SideStore.app/PlugIns/AltWidgetExtension.appex/Info.plist", plistlib.dumps({
            "CFBundleIdentifier": APPEX_ID, "CFBundleExecutable": "AltWidgetExtension",
            "NSExtension": {"NSExtensionPointIdentifier": "com.apple.widgetkit-extension"}}))
        z.writestr("Payload/SideStore.app/PlugIns/AltWidgetExtension.appex/AltWidgetExtension", b"bin")


def reset_run():
    if os.path.exists(ZSIGN_LOG):
        os.remove(ZSIGN_LOG)
    native_calls["connectAndPair"] = 0
    native_calls["sideloadIpa"] = []
    native_calls["embedded"] = []
    native_calls["prefs"] = []
    DeviceNative.installed_raw = []
    FakeDevAPI.app_id_limit = False
    FakeDevAPI.has_wildcard = True
    FakeDevAPI.extra_app_ids = []
    FakeDevAPI.instances.clear()
    auth_instances.clear()
    state = os.path.join(WORK, "sideload_state.json")
    if os.path.exists(state):
        os.remove(state)


ipa = os.path.join(WORK, "SideStore.ipa")
make_ipa(ipa)

print("=== CASE A: bundle id gốc bị tài khoản khác chiếm (9401), IPA có extension ===")
reset_run()
ok = core.do_sideload(ipa, "user@example.com", "pw", udid_override="", anisette_url="")
check(ok is True, "do_sideload trả True")
check(native_calls["connectAndPair"] >= 1, "kết nối + ghép nối iPhone TRƯỚC khi làm việc với Apple")
dev = FakeDevAPI.instances[-1]
check(dev.registered == [(f"iPhone-{UDID.replace('-', '')[:8]}", UDID)],
      f"đăng ký thiết bị bằng UDID thật từ lockdown: {dev.registered}")
check(auth_instances and auth_instances[-1].input_func is core._ui_input,
      "AppleAuth nhận input_func=UiPrompt (2FA không còn gọi input() → EOFError)")
main_created = [i for i in dev.created_ids if i.startswith(MAIN_ID + ".s") and not i.endswith(".AltWidget")]
check(len(main_created) == 1, f"tạo App ID phái sinh cho app chính: {main_created}")
new_main = main_created[0] if main_created else "?"
check(f"{new_main}.AltWidget" in dev.created_ids, f"tạo App ID riêng cho extension: {new_main}.AltWidget")
check(dev.profiles_for == [new_main, f"{new_main}.AltWidget"],
      f"tải profile cho TỪNG App ID, app chính trước: {dev.profiles_for}")

calls = json.load(open(ZSIGN_LOG)) if os.path.exists(ZSIGN_LOG) else []
check(len(calls) == 1, "gọi zsign đúng 1 lần")
if calls:
    args = calls[0]["args"]
    app_dir = args[-1]
    print("     zsign " + " ".join(args))
    check(app_dir.endswith("SideStore.app") and os.path.isdir(app_dir),
          "zsign ký THƯ MỤC .app đã sửa (bản cũ đưa FILE IPA GỐC → mất bundle id mới)")
    check(ipa not in args, "không đưa IPA gốc cho zsign")
    ms = [args[i + 1] for i, a in enumerate(args) if a == "-m"]
    check(len(ms) == 2 and ms[0].endswith("SideStore.app/embedded.mobileprovision")
          and ms[1].endswith("AltWidgetExtension.appex/embedded.mobileprovision"),
          f"2 cờ -m, app chính trước: {[os.path.relpath(m, WORK) for m in ms]}")
    check("-b" not in args, "không dùng -b (bundle id đã sửa bằng Python)")
    check("-t" in args and "-f" in args, "có -t (Android không có /tmp) và -f")
    check(calls[0]["LD_LIBRARY_PATH"] == AppPaths.nativeDepsDir(), "LD_LIBRARY_PATH trỏ tới native deps")
    with open(os.path.join(app_dir, "Info.plist"), "rb") as f:
        main_plist = plistlib.load(f)
    with open(os.path.join(app_dir, "PlugIns", "AltWidgetExtension.appex", "Info.plist"), "rb") as f:
        appex_plist = plistlib.load(f)
    check(main_plist["CFBundleIdentifier"] == new_main, f"bundle id app == App ID đã nộp ({new_main})")
    check(appex_plist["CFBundleIdentifier"] == f"{new_main}.AltWidget", "bundle id extension == App ID đã nộp")
    for bundle, want in ((app_dir, new_main),
                         (os.path.join(app_dir, "PlugIns", "AltWidgetExtension.appex"), f"{new_main}.AltWidget")):
        with open(os.path.join(bundle, "embedded.mobileprovision"), "rb") as f:
            prof = plistlib.load(f)
        check(prof["Entitlements"]["application-identifier"] == f"{TEAM}.{want}"
              and UDID in prof["ProvisionedDevices"],
              f"profile nhúng trong {os.path.basename(bundle)} khớp App ID + có UDID máy")
check(len(native_calls["sideloadIpa"]) == 1 and native_calls["sideloadIpa"][0].endswith("_signed.ipa"),
      f"cài qua USB đúng file đã ký: {[os.path.basename(p) for p in native_calls['sideloadIpa']]}")
check(native_calls["embedded"] == [(new_main, "ALTPairingFile.mobiledevicepairing"),
                                    (new_main, "PairingFile_Lockdown.plist")],
      f"v58: nhúng CẢ 2 file ghép nối cho SideStore (legacy + 0.7+): {native_calls['embedded']}")
check(native_calls["prefs"] == [new_main],
      f"v59: ghi UserDefaults tự kích hoạt pairing cho SideStore: {native_calls['prefs']}")

print("=== CASE B: chạy lại — dùng lại App ID đã chọn, không tạo thêm (không tốn quota) ===")
native_calls["sideloadIpa"] = []
if os.path.exists(ZSIGN_LOG):
    os.remove(ZSIGN_LOG)
prev_ids = list(dev.app_ids)
prev_devices = list(dev.devices)
orig_init = FakeDevAPI.__init__


def init_with_existing(self, auth, dsid, token):
    orig_init(self, auth, dsid, token)
    self.app_ids = [dict(a) for a in prev_ids]
    self.devices = list(prev_devices)


FakeDevAPI.__init__ = init_with_existing
ok = core.do_sideload(ipa, "user@example.com", "pw")
dev2 = FakeDevAPI.instances[-1]
FakeDevAPI.__init__ = orig_init
check(ok is True and dev2.created_ids == [], f"không tạo App ID mới nào ({dev2.created_ids})")
check(dev2.registered == [], "thiết bị đã có trong team → không đăng ký lại")
calls = json.load(open(ZSIGN_LOG)) if os.path.exists(ZSIGN_LOG) else []
if calls:
    with open(os.path.join(calls[0]["args"][-1], "Info.plist"), "rb") as f:
        check(plistlib.load(f)["CFBundleIdentifier"] == new_main, "vẫn ký với đúng App ID của lần trước")

print("=== CASE C: Apple từ chối đăng ký thiết bị → DỪNG trước khi ký ===")
reset_run()
FakeDevAPI.accept_device = False
ok = core.do_sideload(ipa, "user@example.com", "pw")
FakeDevAPI.accept_device = True
check(ok is False, "do_sideload trả False")
check(not os.path.exists(ZSIGN_LOG), "không chạy zsign")
check(native_calls["sideloadIpa"] == [], "không cài đặt")

print("=== CASE D: 2FA xong nhưng Apple chưa cấp phiên (\"2fa_completed\") ===")
reset_run()
FakeAppleAuth.result = {"authenticated": "2fa_completed", "dsid": "123"}
try:
    ok = core.do_sideload(ipa, "user@example.com", "pw")
    crashed = False
except KeyError:
    crashed = True
FakeAppleAuth.result = {"authenticated": True, "dsid": "123", "session_token": "tok"}
check(not crashed and ok is False, "không KeyError 'session_token'; báo chạy lại")

print("=== CASE E: không kết nối được iPhone → dừng NGAY, không đụng tới Apple ===")
reset_run()
DeviceNative.connect_ok = False
ok = core.do_sideload(ipa, "user@example.com", "pw")
DeviceNative.connect_ok = True
check(ok is False and not FakeDevAPI.instances, "dừng trước khi đăng nhập Apple / tạo App ID")

print("=== CASE G: hết chỗ certificate → tự động thu hồi cert của tool rồi tạo lại ===")
reset_run()
FakeDevAPI.cert_fail_first = True
FakeDevAPI.certs = [
    {"id": "C-TOOL", "attributes": {"name": "ios-sideload-tool", "status": "ACTIVE"}},
    {"id": "C-XCODE", "attributes": {"name": "Xcode: Macbook", "status": "ACTIVE"}},
]
ok = core.do_sideload(ipa, "user@example.com", "pw")
check(ok is True, "do_sideload vẫn thành công sau khi tự thu hồi cert")
check(FakeDevAPI.revoked == ["C-TOOL"], f"chỉ thu hồi cert của tool: {FakeDevAPI.revoked}")
FakeDevAPI.cert_fail_first = False
FakeDevAPI.certs = []
FakeDevAPI.revoked = []

print("=== CASE H: tạo cert lỗi + 0 cert → không có gì để thu hồi, báo thất bại ===")
reset_run()
FakeDevAPI.cert_fail_first = True   # fail lần đầu, không có cert nào để thu hồi
ok = core.do_sideload(ipa, "user@example.com", "pw")
check(ok is False, "do_sideload báo thất bại")
check(FakeDevAPI.revoked == [], f"không thu hồi cert nào: {FakeDevAPI.revoked}")
FakeDevAPI.cert_fail_first = False

print("=== CASE H2 (v55): tạo cert lỗi + CHỈ 1 cert vẫn tự thu hồi rồi tạo lại ===")
reset_run()
FakeDevAPI.cert_fail_first = True   # create fail lần đầu
FakeDevAPI.certs = [
    {"id": "C-ONLY", "attributes": {"name": "ios-sideload-tool", "status": "ACTIVE"}},
]
ok = core.do_sideload(ipa, "user@example.com", "pw")
check(ok is True, "do_sideload thành công sau khi thu hồi cert duy nhất")
check(FakeDevAPI.revoked == ["C-ONLY"], f"đã thu hồi cert duy nhất: {FakeDevAPI.revoked}")
FakeDevAPI.certs = []
FakeDevAPI.revoked = []

print("=== CASE H3 (v55): 1 cert KHÔNG phải của tool vẫn bị thu hồi (fallback tất cả) ===")
reset_run()
FakeDevAPI.cert_fail_first = True
FakeDevAPI.certs = [
    {"id": "C-XC", "attributes": {"name": "Xcode: Macbook", "status": "ACTIVE"}},
]
ok = core.do_sideload(ipa, "user@example.com", "pw")
check(ok is True, "do_sideload thành công sau khi thu hồi cert không phải của tool")
check(FakeDevAPI.revoked == ["C-XC"], f"fallback thu hồi tất cả: {FakeDevAPI.revoked}")
FakeDevAPI.certs = []
FakeDevAPI.revoked = []

print("=== CASE I: do_login (màn đăng nhập lần đầu) ===")
check(core.do_login("user@example.com", "pw") is True, "do_login thành công với tài khoản hợp lệ")
FakeAppleAuth.result = {"authenticated": False}
check(core.do_login("user@example.com", "sai-mat-khau") is False, "do_login thất bại khi Apple từ chối")
FakeAppleAuth.result = {"authenticated": True, "dsid": "123", "session_token": "tok"}
try:
    core.do_login("user@example.com", "pw")
    no_crash = True
except Exception:
    no_crash = False
check(no_crash, "do_login không crash với tài khoản lỗi đột xuất")

print("=== CASE F: UDID ===")
check(core._normalize_udid("00008030001A35E80C41802E") == UDID, "UDID 24 ký tự được chèn '-'")
check(core._looks_like_udid("102e03e0d56583407853e9518f945642c72298d3"), "UDID 40 hex hợp lệ")
check(not core._looks_like_udid(WORK), "đường dẫn thư mục KHÔNG bị coi là UDID (bản cũ fallback filesDir)")

def make_jit_ipa(path):
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("Payload/Jitterbug.app/Info.plist", plistlib.dumps({
            "CFBundleIdentifier": "com.osy86.Jitterbug", "CFBundleDisplayName": "Jitterbug",
            "CFBundleExecutable": "Jitterbug"}))
        z.writestr("Payload/Jitterbug.app/Jitterbug", b"\xcf\xfa\xed\xfe binary")
        z.writestr("Payload/Jitterbug.app/PlugIns/JitterbugTunnel.appex/Info.plist", plistlib.dumps({
            "CFBundleIdentifier": "com.osy86.Jitterbug.JitterbugTunnel",
            "CFBundleExecutable": "JitterbugTunnel",
            "NSExtension": {"NSExtensionPointIdentifier": "com.apple.network-extension.packet-tunnel"}}))
        z.writestr("Payload/Jitterbug.app/PlugIns/JitterbugTunnel.appex/JitterbugTunnel", b"bin")


print("=== CASE J (v60): hết hạn mức App ID + có wildcard → giữ nguyên bundle id, extension che phủ ===")
reset_run()
FakeDevAPI.cert_fail_first = False  # H2/H3 để sót True ở class attr
make_jit_ipa(ipa)
# iPhone đang cài SideStore (bundle phái sinh) —stub trả CHUỖI như Chaquopy thật
DeviceNative.installed_raw = "com.SideStore.SideStore.sa9b733\ncom.other.InstalledApp"
FakeDevAPI.app_id_limit = True
FakeDevAPI.extra_app_ids = [
    {"identifier": "com.osy86.*", "appIdId": "WC1"},
    {"identifier": "com.SideStore.SideStore.sa9b733.AltWidget", "appIdId": "ALTW"},
]
ok = core.do_sideload(ipa, "user@example.com", "pw")
check(ok is True, "cài thành công qua wildcard khi hết hạn mức App ID")
dev = FakeDevAPI.instances[-1]
check(dev.created_ids == [], f"KHÔNG tạo App ID mới nào: {dev.created_ids}")
calls = json.load(open(ZSIGN_LOG)) if os.path.exists(ZSIGN_LOG) else []
app_dir_j = calls[0]["args"][-1] if calls else ""
with open(os.path.join(app_dir_j, "Info.plist"), "rb") as f:
    jit_plist = plistlib.load(f)
check(jit_plist["CFBundleIdentifier"] == "com.osy86.Jitterbug",
      f"wildcard → KHÔNG đổi bundle id: {jit_plist['CFBundleIdentifier']}")
check(os.path.isdir(os.path.join(app_dir_j, "PlugIns", "JitterbugTunnel.appex")),
      "extension KHÔNG bị bỏ (wildcard che phủ)")
ms = [calls[0]["args"][i + 1] for i, a in enumerate(calls[0]["args"]) if a == "-m"] if calls else []
check(len(ms) == 1, f"v62: wildcard dedupe — 1 cờ -m cho app + extension: {ms}")

print("=== CASE K (v64): hết hạn mức + KHÔNG wildcard → dừng SỚM với hướng dẫn (không ký hụt) ===")
reset_run()
FakeDevAPI.cert_fail_first = False
make_jit_ipa(ipa)
DeviceNative.installed_raw = ["com.SideStore.SideStore.sa9b733"]
FakeDevAPI.app_id_limit = True
FakeDevAPI.has_wildcard = False
FakeDevAPI.extra_app_ids = [
    {"identifier": "com.old.Uninstalled", "appIdId": "OLD1"},
    {"identifier": "com.SideStore.SideStore.sa9b733.AltWidget", "appIdId": "ALTW"},
]
# Ghi nhớ App ID đang bị SideStore chiếm — tool phải nhận ra và KHÔNG dùng lại
_state_path = os.path.join(WORK, "sideload_state.json")
with open(_state_path, "w") as f:
    json.dump({"app_id_map": {f"{TEAM}:com.osy86.Jitterbug": {
        "effective_bundle": "com.SideStore.SideStore.sa9b733.AltWidget", "app_id_id": "ALTW"}}}, f)
ok = core.do_sideload(ipa, "user@example.com", "pw")
check(ok is False, "v64: hết App ID + không wildcard → dừng SỚM (trả False) thay vì ký xong chết ở installd")
check(not os.path.exists(ZSIGN_LOG), "KHÔNG gọi zsign (không ký hụt)")
check(native_calls["sideloadIpa"] == [], "KHÔNG cài đặt gì")
dev = FakeDevAPI.instances[-1]
check(dev.created_ids == [], f"KHÔNG tạo App ID mới: {dev.created_ids}")

print("=== CASE K2 (v64): wildcard TOÀN TEAM (TEAM.*) che phủ cả extension + App ID nhớ bị chiếm ===")
reset_run()
make_jit_ipa(ipa)
DeviceNative.installed_raw = ["com.SideStore.SideStore.sa9b733"]
FakeDevAPI.app_id_limit = True
FakeDevAPI.has_wildcard = True     # App ID f"{TEAM}.*" — wildcard toàn team
FakeDevAPI.extra_app_ids = [
    {"identifier": "com.SideStore.SideStore.sa9b733.AltWidget", "appIdId": "ALTW"},
]
_state_path = os.path.join(WORK, "sideload_state.json")
with open(_state_path, "w") as f:
    json.dump({"app_id_map": {f"{TEAM}:com.osy86.Jitterbug": {
        "effective_bundle": "com.SideStore.SideStore.sa9b733.AltWidget", "app_id_id": "ALTW"}}}, f)
ok = core.do_sideload(ipa, "user@example.com", "pw")
check(ok is True, "cài thành công qua wildcard toàn team (kể cả extension)")
dev = FakeDevAPI.instances[-1]
check(dev.created_ids == [], f"KHÔNG tạo App ID mới: {dev.created_ids}")
check(dev.profiles_for == ["*"], f"chỉ tải 1 profile wildcard (cache dùng lại): {dev.profiles_for}")
calls = json.load(open(ZSIGN_LOG)) if os.path.exists(ZSIGN_LOG) else []
app_dir_k = calls[0]["args"][-1] if calls else ""
with open(os.path.join(app_dir_k, "Info.plist"), "rb") as f:
    k_plist = plistlib.load(f)
check(k_plist["CFBundleIdentifier"] == "com.osy86.Jitterbug",
      f"giữ NGUYÊN bundle id gốc (wildcard che phủ): {k_plist['CFBundleIdentifier']}")
check(k_plist["CFBundleIdentifier"] != "com.SideStore.SideStore.sa9b733.AltWidget",
      "v64: KHÔNG dùng lại App ID đang bị SideStore chiếm (sẽ đè app đó)")
appex_k = os.path.join(app_dir_k, "PlugIns", "JitterbugTunnel.appex")
check(os.path.isdir(appex_k), "extension KHÔNG bị bỏ (VPN/tunnel cần nó)")
with open(os.path.join(appex_k, "Info.plist"), "rb") as f:
    k_appex = plistlib.load(f)
check(k_appex["CFBundleIdentifier"] == "com.osy86.Jitterbug.JitterbugTunnel",
      f"bundle id extension giữ nguyên tiền tố app chính: {k_appex['CFBundleIdentifier']}")
ms = [calls[0]["args"][i + 1] for i, a in enumerate(calls[0]["args"]) if a == "-m"] if calls else []
check(len(ms) == 1, f"1 profile wildcard cho cả app + extension (dedupe): {ms}")
for bundle_k in (app_dir_k, appex_k):
    with open(os.path.join(bundle_k, "embedded.mobileprovision"), "rb") as f:
        prof_k = plistlib.load(f)
    check(prof_k["Entitlements"]["application-identifier"] == f"{TEAM}.*",
          f"profile trong {os.path.basename(bundle_k)} là wildcard toàn team")

print("=== CASE O (v67): cài lại app cũ (App ID nhớ bị CHÍNH app đó chiếm) → CÀI ĐÈ refresh + extension dùng App ID con trống ===")
reset_run()
FakeDevAPI.cert_fail_first = False
make_jit_ipa(ipa)
# App cũ do tool cài lần trước vẫn đang trên máy — chiếm ĐÚNG bundle id App ID nhớ
DeviceNative.installed_raw = "com.old.Pair\ncom.other.App"
FakeDevAPI.app_id_limit = True
FakeDevAPI.has_wildcard = False
FakeDevAPI.extra_app_ids = [
    {"identifier": "com.old.Pair", "appIdId": "PR1"},         # bị chính app cũ (cùng app) chiếm
    {"identifier": "com.old.Pair.Widget", "appIdId": "PR2"},  # App ID CON trống
    {"identifier": "com.old.Pair.Widget2", "appIdId": "PR3"}, # con trống thứ 2
]
_state_path = os.path.join(WORK, "sideload_state.json")
with open(_state_path, "w") as f:
    json.dump({"app_id_map": {f"{TEAM}:com.osy86.Jitterbug": {
        "effective_bundle": "com.old.Pair", "app_id_id": "PR1"}}}, f)
ok = core.do_sideload(ipa, "user@example.com", "pw")
check(ok is True, "v67: cài lại app cũ → CÀI ĐÈ (refresh) thành công, KHÔNG cần xoá app")
dev = FakeDevAPI.instances[-1]
check(dev.created_ids == [], f"KHÔNG tạo App ID mới (0 lượt): {dev.created_ids}")
calls = json.load(open(ZSIGN_LOG)) if os.path.exists(ZSIGN_LOG) else []
app_dir_o = calls[0]["args"][-1] if calls else ""
with open(os.path.join(app_dir_o, "Info.plist"), "rb") as f:
    o_main = plistlib.load(f)
check(o_main["CFBundleIdentifier"] == "com.old.Pair",
      f"main GIỮ NGUYÊN bundle id cũ (cài đè tại chỗ, giữ data): {o_main['CFBundleIdentifier']}")
appex_o = os.path.join(app_dir_o, "PlugIns", "JitterbugTunnel.appex")
with open(os.path.join(appex_o, "Info.plist"), "rb") as f:
    o_appex = plistlib.load(f)
check(o_appex["CFBundleIdentifier"] == "com.old.Pair.Widget",
      f"extension tái dùng App ID CON trống của cha (đúng luật tiền tố): {o_appex['CFBundleIdentifier']}")
check(o_appex["CFBundleIdentifier"].startswith(o_main["CFBundleIdentifier"] + "."),
      "extension = '<app chính>.<hậu tố>' — đúng luật installd")
check(dev.profiles_for == ["com.old.Pair", "com.old.Pair.Widget"],
      f"2 profile riêng (cha + con): {dev.profiles_for}")
for bundle_o, want_o in ((app_dir_o, "com.old.Pair"), (appex_o, "com.old.Pair.Widget")):
    with open(os.path.join(bundle_o, "embedded.mobileprovision"), "rb") as f:
        prof_o = plistlib.load(f)
    check(prof_o["Entitlements"]["application-identifier"] == f"{TEAM}.{want_o}",
          f"profile trong {os.path.basename(bundle_o)} khớp ĐÚNG bundle id của nó")
check(len(native_calls["sideloadIpa"]) == 1, "cài đúng file đã ký (đè lên app cũ)")
st = json.load(open(_state_path))
check(st["app_id_map"].get(f"{TEAM}:com.osy86.Jitterbug", {}).get("effective_bundle") == "com.old.Pair",
      "memory KHÔNG bị xoá (lần sau vẫn refresh được)")

print("=== CASE N (v66): hết lượt tạo App ID → tái dùng CẶP App ID cha-con trống ===")
reset_run()
FakeDevAPI.cert_fail_first = False
make_jit_ipa(ipa)
DeviceNative.installed_raw = "com.SideStore.SideStore.sa9b733\ncom.other.App"
FakeDevAPI.app_id_limit = True
FakeDevAPI.has_wildcard = False
FakeDevAPI.extra_app_ids = [
    {"identifier": "com.old.Uninstalled", "appIdId": "OLD1"},   # mồi: không phải cha của ai
    {"identifier": "com.old.Pair", "appIdId": "PR1"},           # CHA trống
    {"identifier": "com.old.Pair.Widget", "appIdId": "PR2"},    # CON trống (tiền tố cha)
    {"identifier": "com.SideStore.SideStore.sa9b733.AltWidget", "appIdId": "ALTW"},  # bị chiếm
]
ok = core.do_sideload(ipa, "user@example.com", "pw")
check(ok is True, "cài thành công: tái dùng CẶP App ID cha-con (app chính + extension)")
dev = FakeDevAPI.instances[-1]
check(dev.created_ids == [], f"KHÔNG tạo App ID mới nào (không tốn lượt): {dev.created_ids}")
calls = json.load(open(ZSIGN_LOG)) if os.path.exists(ZSIGN_LOG) else []
app_dir_n = calls[0]["args"][-1] if calls else ""
with open(os.path.join(app_dir_n, "Info.plist"), "rb") as f:
    n_main = plistlib.load(f)
check(n_main["CFBundleIdentifier"] == "com.old.Pair",
      f"app chính bị ĐỔI sang App ID cha trống: {n_main['CFBundleIdentifier']}")
appex_n = os.path.join(app_dir_n, "PlugIns", "JitterbugTunnel.appex")
with open(os.path.join(appex_n, "Info.plist"), "rb") as f:
    n_appex = plistlib.load(f)
check(n_appex["CFBundleIdentifier"] == "com.old.Pair.Widget",
      f"extension dùng App ID con: {n_appex['CFBundleIdentifier']}")
check(n_appex["CFBundleIdentifier"].startswith(n_main["CFBundleIdentifier"] + "."),
      "extension = '<app chính>.<hậu tố>' — đúng luật installd (prefix bắt buộc)")
check(dev.profiles_for == ["com.old.Pair", "com.old.Pair.Widget"],
      f"2 profile riêng cho cặp cha-con: {dev.profiles_for}")
ms = [calls[0]["args"][i + 1] for i, a in enumerate(calls[0]["args"]) if a == "-m"] if calls else []
check(len(ms) == 2, f"2 cờ -m (2 profile khác nhau, không dedupe): {len(ms)}")
for bundle_n, want_n in ((app_dir_n, "com.old.Pair"), (appex_n, "com.old.Pair.Widget")):
    with open(os.path.join(bundle_n, "embedded.mobileprovision"), "rb") as f:
        prof_n = plistlib.load(f)
    check(prof_n["Entitlements"]["application-identifier"] == f"{TEAM}.{want_n}",
          f"profile trong {os.path.basename(bundle_n)} khớp ĐÚNG bundle id của nó")
check(len(native_calls["sideloadIpa"]) == 1, "cài đúng file đã ký")

print("=== CASE L (v62): IPA không có Payload/ (.app nằm ở gốc) vẫn xử lý được ===")
reset_run()
FakeDevAPI.cert_fail_first = False
ipa_bare = os.path.join(WORK, "Bare.ipa")
with zipfile.ZipFile(ipa_bare, "w") as z:
    z.writestr("BareApp.app/Info.plist", plistlib.dumps({
        "CFBundleIdentifier": "com.example.BareApp", "CFBundleDisplayName": "BareApp",
        "CFBundleExecutable": "BareApp"}))
    z.writestr("BareApp.app/BareApp", b"\xcf\xfa\xed\xfe binary")
ok = core.do_sideload(ipa_bare, "user@example.com", "pw")
check(ok is True, "IPA zip lại từ .app trần (thiếu Payload/) vẫn cài được")
check(len(native_calls["sideloadIpa"]) == 1 and native_calls["sideloadIpa"][0].endswith("_signed.ipa"),
      "vẫn cài đúng file đã ký")

import utils as _utils
bad_dir = os.path.join(WORK, "bad_extracted")
os.makedirs(bad_dir, exist_ok=True)
with open(os.path.join(bad_dir, "hello.txt"), "w") as f:
    f.write("not an ipa")
try:
    _utils.find_app_bundle(bad_dir)
    check(False, "file không phải IPA phải raise")
except Exception as e:
    check("hello.txt" in str(e), f"lỗi liệt kê nội dung để chẩn đoán: {str(e)[:60]}…")

print("=== CASE M (v63): file ZIP chứa .ipa lồng bên trong — tự giải nén tiếp ===")
reset_run()
FakeDevAPI.cert_fail_first = False
inner_ipa = os.path.join(WORK, "Real.ipa")
with zipfile.ZipFile(inner_ipa, "w") as z:
    z.writestr("Payload/RealApp.app/Info.plist", plistlib.dumps({
        "CFBundleIdentifier": "com.example.RealApp", "CFBundleDisplayName": "RealApp",
        "CFBundleExecutable": "RealApp"}))
    z.writestr("Payload/RealApp.app/RealApp", b"\xcf\xfa\xed\xfe binary")
wrapped_ipa = os.path.join(WORK, "MiniStore.ipa")
with zipfile.ZipFile(wrapped_ipa, "w") as z:
    z.write(inner_ipa, "MiniStore.ipa")
ok = core.do_sideload(wrapped_ipa, "user@example.com", "pw")
check(ok is True, "ZIP chứa .ipa lồng vẫn cài được (tự giải nén tầng trong)")
calls = json.load(open(ZSIGN_LOG)) if os.path.exists(ZSIGN_LOG) else []
check(ok and calls and calls[0]["args"][-1].endswith("RealApp.app"),
      "ký đúng app bên trong (RealApp.app)")

print("=== CASE I (v56): nhúng file ghép nối — tắt cờ / app ngoài danh sách ===")
reset_run()
FakeDevAPI.cert_fail_first = False  # H2/H3 để sót True ở class attr (instance shadow)
make_ipa(ipa)  # các case trước đã tiêu thụ file ipa gốc
ok = core.do_sideload(ipa, "user@example.com", "pw", embed_pairing=False)
check(ok is True, "do_sideload thành công khi embed_pairing=False")
check(native_calls["embedded"] == [], f"embed_pairing=False → KHÔNG nhúng file ghép nối: {native_calls['embedded']}")
check(native_calls["prefs"] == [], f"embed_pairing=False → KHÔNG ghi UserDefaults: {native_calls['prefs']}")

reset_run()
ipa_other = os.path.join(WORK, "Other.ipa")
with zipfile.ZipFile(ipa_other, "w") as z:
    z.writestr("Payload/Other.app/Info.plist", plistlib.dumps({
        "CFBundleIdentifier": "com.example.OtherApp", "CFBundleDisplayName": "OtherApp",
        "CFBundleExecutable": "OtherApp"}))
    z.writestr("Payload/Other.app/OtherApp", b"\xcf\xfa\xed\xfe binary")
ok = core.do_sideload(ipa_other, "user@example.com", "pw")
check(ok is True, "cài app ngoài danh sách ghép nối vẫn thành công")
check(native_calls["embedded"] == [], f"app không cần ghép nối → không nhúng: {native_calls['embedded']}")
check(native_calls["prefs"] == [], f"app ngoài danh sách → KHÔNG ghi UserDefaults: {native_calls['prefs']}")

shutil.rmtree(WORK, ignore_errors=True)
print("=" * 70)
if failures:
    print(f"❌ {len(failures)} kiểm tra THẤT BẠI")
    sys.exit(1)
print("✅ TẤT CẢ KIỂM TRA ĐỀU ĐẠT")
