#!/usr/bin/env python3
"""v73 — Kiểm chứng file ghép nối (.mobiledevicepairing) TRƯỚC khi nhúng.

Lỗi #2 người dùng báo: "tạo file và nhúng pair vào SideStore/LiveContainer
thì app không dùng được, cứ lỗi không biết do đâu, mấy tool khác pair xong dùng ok".

Nguyên nhân đã xác minh (xem FIX_REPORT_v73.md):
  - SideStore/minimuxer parse file bằng struct thật, YÊU CẦU đủ bộ key:
    WiFiMACAddress, HostCertificate, UDID, HostID, HostPrivateKey, EscrowBag,
    DeviceCertificate, SystemBUID, RootPrivateKey, RootCertificate
    (SideStore#1532, ipaforge PR#26, SideInstaller NOTES.md).
  - libimobiledevice chỉ lưu WiFiMACAddress/EscrowBag KHI lockdownd trả value
    lúc pair (src/lockdown.c). iOS 26/27 có thể không trả → file thiếu key.
  - Bản cũ của tool chỉ thêm UDID rồi ghi ra, KHÔNG kiểm chứng gì.

Test này kiểm chứng lớp validator Python (chạy được trên host) + luồng
quyết định tên file. Phần bổ sung key trong C được kiểm chứng bằng
tests/host/check_native_syntax.sh (build thật với libplist 2.x).

Chạy: python3 tests/python/test_pairing_validation.py
"""
import os
import plistlib
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
PY_DIR = os.path.normpath(os.path.join(HERE, "..", "..", "app", "src", "main", "python"))
sys.path.insert(0, PY_DIR)

failures = []


def check(cond, msg):
    print(("  ✅ " if cond else "  ❌ ") + msg)
    if not cond:
        failures.append(msg)


# ── Stub Chaquopy / Kotlin / các module phụ thuộc ───────────────────────────
WORK = tempfile.mkdtemp(prefix="pairing_test_")


class _AppPaths:
    @staticmethod
    def filesDir():
        return WORK

    @staticmethod
    def zsignPath():
        return os.path.join(WORK, "zsign")

    @staticmethod
    def nativeDepsDir():
        return WORK


class _NativeLog:
    @staticmethod
    def log(tag, msg):
        pass

    @staticmethod
    def emit(msg):
        pass


class _UiPrompt:
    @staticmethod
    def requestInput(prompt):
        return ""


class _DeviceNative:
    xml = None
    report = ""
    ios = "26.6.1"

    @staticmethod
    def getPairingFileXml():
        return _DeviceNative.xml

    @staticmethod
    def getPairingFileReport():
        return _DeviceNative.report

    @staticmethod
    def getIosVersion():
        return _DeviceNative.ios


for name in ("com", "com.superalpha", "com.superalpha.sideload",
             "com.superalpha.sideload.bridge"):
    sys.modules[name] = types.ModuleType(name)
bridge = sys.modules["com.superalpha.sideload.bridge"]
bridge.AppPaths = _AppPaths
bridge.NativeLog = _NativeLog
bridge.UiPrompt = _UiPrompt
bridge.DeviceNative = _DeviceNative

stub_auth = types.ModuleType("apple_auth")
stub_auth.AppleAuth = object
stub_auth.fetch_official_servers = lambda: []
stub_auth.ANISETTE_SEED_SERVERS = ()
sys.modules["apple_auth"] = stub_auth

stub_dev = types.ModuleType("developer_api")
stub_dev.DeveloperAPI = object
stub_dev.classify_app_id_error = lambda e: "other"
sys.modules["developer_api"] = stub_dev

stub_dl = types.ModuleType("device_link")
for fn in ("pair_device", "afc_push_ipa", "install_ipa", "list_installed_apps",
           "reset_mux_device", "diagnose"):
    setattr(stub_dl, fn, lambda *a, **k: None)
sys.modules["device_link"] = stub_dl

stub_cfg = types.ModuleType("config_manager")
stub_cfg.get_apple_id = lambda: ""
stub_cfg.set_apple_id = lambda x: None
stub_cfg.get_connected_udid = lambda: ""
stub_cfg.get_anisette_url = lambda: ""
stub_cfg.set_anisette_url = lambda x: None
sys.modules["config_manager"] = stub_cfg

import sideload_core as core  # noqa: E402

UDID = "00008030-001A35E80C41802E"


def valid_plist(**overrides):
    d = {
        "DeviceCertificate": b"-----BEGIN CERTIFICATE-----dev",
        "HostCertificate": b"-----BEGIN CERTIFICATE-----host",
        "HostPrivateKey": b"-----BEGIN RSA PRIVATE KEY-----host",
        "RootCertificate": b"-----BEGIN CERTIFICATE-----root",
        "RootPrivateKey": b"-----BEGIN RSA PRIVATE KEY-----root",
        "HostID": "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE",
        "SystemBUID": "12345678-1234-1234-1234-123456789012",
        "UDID": UDID,
        "WiFiMACAddress": "a0:11:22:33:44:55",
        "EscrowBag": b"\x01\x02\x03",
    }
    d.update(overrides)
    for k, v in list(d.items()):
        if v is None:
            del d[k]
    return plistlib.dumps(d, fmt=plistlib.FMT_XML).decode("utf-8")


print("─" * 72)
print("1) File đủ bộ key → hợp lệ")
r = core.validate_pairing_file_xml(valid_plist())
check(r["ok"] is True, "ok = True")
check(r["missing"] == [], f"không thiếu key nào: {r['missing']}")
check(r["udid"] == UDID, "đọc đúng UDID")
check(r["has_rp"] is False, "chưa có RP pairing")
check(r["warnings"] == [], f"không cảnh báo: {r['warnings']}")

print()
print("2) Thiếu WiFiMACAddress (iOS 26/27 không trả WiFiAddress) → CẢNH BÁO")
r = core.validate_pairing_file_xml(valid_plist(WiFiMACAddress=None))
check(r["ok"] is True, "vẫn ok (key này không bắt buộc ở mọi reader)")
check(any("WiFiMACAddress" in w for w in r["warnings"]),
      "có cảnh báo rõ ràng thay vì im lặng ghi file hỏng")
r2 = core.validate_pairing_file_xml(valid_plist(EscrowBag=None))
check(any("EscrowBag" in w for w in r2["warnings"]), "cảnh báo cả EscrowBag")

print()
print("3) Thiếu RootPrivateKey → TỪ CHỐI (fail-closed)")
r = core.validate_pairing_file_xml(valid_plist(RootPrivateKey=None))
check(r["ok"] is False, "ok = False")
check("RootPrivateKey" in r["missing"], "liệt kê đúng key thiếu cho UI/log")

print()
print("4) Thiếu nhiều key / plist hỏng")
r = core.validate_pairing_file_xml(valid_plist(HostID=None, SystemBUID=None, UDID=None))
check(len(r["missing"]) == 3, f"liệt kê đủ 3 key: {r['missing']}")
check(core.validate_pairing_file_xml("không phải plist")["ok"] is False,
      "plist rác không được coi là hợp lệ")
check(core.validate_pairing_file_xml("")["ok"] is False, "xml rỗng không hợp lệ")

print()
print("5) Có RPPairing → ghi thêm PairingFile_RemoteRP.plist")
rp_xml = plistlib.dumps({
    "UDID": UDID, "alt_irk": b"irk", "private_key": b"pk",
    "public_key": b"pub", "identifier": "host-1",
}, fmt=plistlib.FMT_XML).decode("utf-8")
r = core.validate_pairing_file_xml(rp_xml)
check(r["has_rp"] is True, "phát hiện khoá RP")
rels = core._pairing_rel_paths_for("SideStore", "com.SideStore.SideStore", has_rp=r["has_rp"])
check("PairingFile_RemoteRP.plist" in rels, f"SideStore: ghi cả 3 tên file: {rels}")
rels_lc = core._pairing_rel_paths_for("LiveContainer", "com.kdt.LiveContainer", has_rp=True)
check(any("SideStore/Documents/PairingFile_RemoteRP.plist" == x for x in rels_lc),
      f"LiveContainer: RP file nằm trong guest Documents: {rels_lc}")

print()
print("6) KHÔNG có RP keys → không ghi file tên RP (tránh SideStore đọc nhầm)")
rels = core._pairing_rel_paths_for("SideStore", "com.SideStore.SideStore", has_rp=False)
check("ALTPairingFile.mobiledevicepairing" in rels, "vẫn ghi file legacy")
check("PairingFile_Lockdown.plist" in rels, "vẫn ghi file lockdown 0.7+")
check("PairingFile_RemoteRP.plist" not in rels,
      "KHÔNG ghi lockdown data vào tên file RemoteRP")

print()
print("7) _pairing_preflight() đọc báo cáo native + cảnh báo iOS 17.4+")
_DeviceNative.xml = valid_plist()
_DeviceNative.report = "ios=26.6.1;status=OK;udid=%s;keys=DeviceCertificate,..." % UDID
_DeviceNative.ios = "26.6.1"
pre = core._pairing_preflight("SideStore")
check(pre["ok"] is True, "preflight hợp lệ với file đủ key")
check(pre["ios_note"] == "26.6.1", "lấy được phiên bản iOS")

_DeviceNative.xml = valid_plist(RootPrivateKey=None)
pre_bad = core._pairing_preflight("SideStore")
check(pre_bad["ok"] is False, "preflight báo invalid khi thiếu key")
check("RootPrivateKey" in pre_bad["missing"], "nêu đúng key thiếu")

print()
print("8) Native layer CŨ (không có method mới) → không làm gãy luồng")
del _DeviceNative.getPairingFileXml
pre_old = core._pairing_preflight("SideStore")
check(pre_old["ok"] is True, "bỏ qua kiểm chứng, vẫn cho phép nhúng (tương thích ngược)")

print()
print("=" * 72)
if failures:
    print(f"❌ {len(failures)} kiểm tra thất bại")
    for f in failures:
        print("   -", f)
    sys.exit(1)
print("✅ TẤT CẢ KIỂM TRA ĐỀU ĐẠT")
