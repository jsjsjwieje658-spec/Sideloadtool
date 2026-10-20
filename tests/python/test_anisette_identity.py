#!/usr/bin/env python3
"""Kiểm thử lớp "danh tính anisette" của apple_auth (v72) — KHÔNG cần mạng.

Bối cảnh: từ 2026-10-12 GrandSlam trả ec=-80045 ở bước `o=init` khi machine-id
do anisette server sinh ra không khớp danh tính client khai báo; và 503 HTML
cho danh tính chứa token com.apple.dt.Xcode. Bộ test này mô phỏng Apple bằng
một fake HTTP layer để chứng minh:

  1. Server có danh tính bị Apple từ chối (-80045 / 503) bị loại.
  2. Server có danh tính hợp lệ được chọn, và client_info/user_agent được lấy
     NGUYÊN VĂN từ server (không bịa chuỗi Mac như bản cũ).
  3. Header HTTP và trường cpd gửi Apple TRÙNG NHAU (lệch → -80044/-80045).
  4. Khi server đang dùng chết giữa phiên, danh tính được cập nhật theo server
     mới (lỗi cũ: giữ client_info của server cũ).
  5. Khi Apple trả -80045 lúc đăng nhập thật, tool tự đổi server và chạy lại.
  6. hsc 409 (Apple tự gửi mã 2FA) → không gọi lại endpoint trigger.

Chạy: python3 tests/python/test_anisette_identity.py
"""
import json
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


# ── Fake HTTP layer ──────────────────────────────────────────────────────────
GOOD_CI = "<MacBookPro13,3> <Mac OS X;10.12.6;16G29> <com.apple.AuthKit/1 (com.apple.akd/1.0)>"
GOOD_UA = "akd/1.0 CFNetwork/811.5.4 Darwin/16.7.0"
GOOD2_CI = "<PC> <Windows;6.2(0,0);9200> <com.apple.AuthKit/1 (com.apple.iTunes/12.12.4.1)>"
GOOD2_UA = "akd/1.0 CFNetwork/808.1.4"
ANDROID_UNDER_MAC_CI = "<MacBookPro13,2> <macOS;13.1;22C65> <com.apple.AuthKit/1 (com.apple.akd/1.0)>"
XCODE_CI = "<MacBookPro13,2> <macOS;13.1;22C65> <com.apple.AuthKit/1 (com.apple.dt.Xcode/3594.4.19)>"

SERVERS = {
    "https://srv-xcode.test": {"ci": XCODE_CI, "ua": "akd/1.0 CFNetwork/808.1.4",
                               "init": {"http": 503, "ec": None}},
    "https://srv-android.test": {"ci": ANDROID_UNDER_MAC_CI, "ua": "akd/1.0 CFNetwork/808.1.4",
                                 "init": {"http": 200, "ec": -80045}},
    "https://srv-good.test": {"ci": GOOD_CI, "ua": GOOD_UA,
                              "init": {"http": 200, "ec": 0, "challenge": True}},
    "https://srv-good2.test": {"ci": GOOD2_CI, "ua": GOOD2_UA,
                               "init": {"http": 200, "ec": 0, "challenge": True}},
}

state = {
    "dead": set(),          # server trả lỗi mạng
    "posts": [],            # (client_info_header, cpd_client_info, ua, op)
    "init_calls": 0,
}


class FakeResp:
    def __init__(self, status_code=200, json_data=None, content=b"", headers=None):
        self.status_code = status_code
        self._json = json_data
        self.content = content if content else (
            json.dumps(json_data).encode() if json_data is not None else b"")
        self.headers = headers or {}
        self.text = self.content.decode("utf-8", "replace")

    @property
    def ok(self):
        return 200 <= self.status_code < 300

    def json(self):
        if self._json is None:
            raise ValueError("no json")
        return self._json

    def raise_for_status(self):
        if not self.ok:
            raise Exception(f"HTTP {self.status_code}")


def _gsa_plist(ec=None, challenge=False):
    resp = {"Status": {"hsc": 200, "ec": ec if ec is not None else 0, "em": "",
                       "ptxid": "fake"}}
    if challenge:
        resp.update({"sp": "s2k", "s": b"salt" * 8, "B": b"B" * 384,
                     "c": b"c" * 32, "i": 20000})
    return plistlib.dumps({"Response": resp})


class FakeSession:
    def __init__(self, net):
        self.net = net
        self.verify = False

    def get(self, url, timeout=10, **kw):
        return self.net.get(url, timeout=timeout, **kw)

    def request(self, method, url, **kw):
        return self.net.post(url, **kw)

    def post(self, url, **kw):
        return self.net.post(url, **kw)

    def close(self):
        pass


class FakeNet:
    def get(self, url, timeout=10, **kw):
        base = url.split("/v3/")[0].rstrip("/")
        if base in state["dead"]:
            raise Exception("connection refused (fake)")
        cfg = SERVERS.get(base)
        if cfg is None:
            return FakeResp(404, content=b"")
        if url.endswith("/v3/client_info"):
            return FakeResp(200, json_data={"client_info": cfg["ci"],
                                            "user_agent": cfg["ua"]})
        # GET "/" → headers anisette
        return FakeResp(200, json_data={
            "X-Apple-I-MD": "AAAABQAAABCekzZWRHVxds5Avb8gE1oKAAAABA==",
            "X-Apple-I-MD-M": "otp" + base[-8:],
            "X-MMe-Client-Info": cfg["ci"],
            "X-Apple-I-TimeZone": "UTC",
            "X-Apple-Locale": "en_US",
            "X-Apple-I-MD-LU": "C3C06B73AE04B8F6",
            "X-Apple-I-Client-Time": "2026-10-20T12:00:00Z",
            "X-Apple-I-MD-RINFO": "17106176",
            "X-Apple-I-SRL-NO": "0",
            "X-Mme-Device-Id": "37B05E5F-5E3A-8FFB-0B95-00DEC93F1423",
        })

    def post(self, url, data=None, headers=None, **kw):
        headers = headers or {}
        body = plistlib.loads(data) if isinstance(data, (bytes, bytearray)) else {}
        req = body.get("Request", {})
        cpd_ci = (req.get("cpd") or {}).get("X-MMe-Client-Info")
        hdr_ci = headers.get("X-MMe-Client-Info")
        state["posts"].append({
            "hdr_ci": hdr_ci, "cpd_ci": cpd_ci,
            "ua": headers.get("User-Agent"), "op": req.get("o"),
        })
        if url.endswith("/GsService2"):
            state["init_calls"] += 1
            for addr, cfg in SERVERS.items():
                if cfg["ci"] == hdr_ci:
                    sc = cfg["init"]
                    if sc["http"] != 200:
                        return FakeResp(sc["http"], content=b"<html>503</html>")
                    return FakeResp(200, content=_gsa_plist(ec=sc.get("ec"),
                                                            challenge=sc.get("challenge", False)))
            return FakeResp(200, content=_gsa_plist(ec=-80045))
        return FakeResp(200, content=b"{}")

    def Session(self):
        return FakeSession(self)

    # requests.get / requests.post dùng ở module level
    def _get(self, url, **kw):
        return self.get(url, **kw)


# ── Nạp module với fake requests ─────────────────────────────────────────────
import requests as real_requests  # noqa: E402
fake_net = FakeNet()
requests_mod = types.ModuleType("requests")
requests_mod.get = fake_net.get
requests_mod.post = fake_net.post
requests_mod.Session = fake_net.Session
requests_mod.Session_ = FakeSession
sys.modules["requests"] = requests_mod

import apple_auth  # noqa: E402

apple_auth.requests = requests_mod
apple_auth.ANISETTE_CACHE_FILE = os.path.join(tempfile.mkdtemp(prefix="anisette_test_"),
                                              ".anisette_cache.json")
apple_auth.ANISETTE_SEED_SERVERS = ("https://srv-xcode.test", "https://srv-android.test",
                                    "https://srv-good.test")
apple_auth.AppleAuth._fetch_server_list = staticmethod(lambda: [])


def new_auth(pinned=None):
    if os.path.exists(apple_auth.ANISETTE_CACHE_FILE):
        os.remove(apple_auth.ANISETTE_CACHE_FILE)
    state["posts"].clear()
    state["init_calls"] = 0
    state["dead"].clear()
    return apple_auth.AppleAuth(anisette_url=pinned, input_func=lambda p: "")


print("─" * 70)
print("1) Chọn server: chỉ dùng server có danh tính Apple CHẤP NHẬN")
auth = new_auth()
check(auth.anisette_url == "https://srv-good.test",
      f"chọn srv-good (nhận được {auth.anisette_url})")
check(auth.client_info == GOOD_CI, "client_info lấy NGUYÊN VĂN từ server (không bịa Mac)")
check(auth.user_agent == GOOD_UA, "user_agent lấy từ server")
check("com.apple.dt.Xcode" not in auth.client_info, "không gửi token Xcode bị edge chặn 503")

print()
print("2) Header HTTP và cpd phải TRÙNG nhau")
cpd = auth.generate_cpd()
check(cpd["X-MMe-Client-Info"] == auth.client_info,
      "cpd X-MMe-Client-Info == client_info của server")
check(cpd["X-Mme-Device-Id"] == "37B05E5F-5E3A-8FFB-0B95-00DEC93F1423",
      "device-id khớp machine-id của anisette server")

print()
print("3) Cache: lần sau dùng lại server đã kiểm chứng (không dò lại)")
state["posts"].clear()
state["init_calls"] = 0
auth2 = apple_auth.AppleAuth(input_func=lambda p: "")
check(auth2.anisette_url == "https://srv-good.test", "server lấy từ cache")
check(auth2.client_info == GOOD_CI, "danh tính trong cache được áp lại")
check(state["init_calls"] == 0, "không tốn request kiểm chứng nào khi đã có cache")

print()
print("4) Server chết giữa phiên → đổi server VÀ đổi luôn danh tính")
saved_seeds = apple_auth.ANISETTE_SEED_SERVERS
apple_auth.ANISETTE_SEED_SERVERS = ("https://srv-good.test", "https://srv-good2.test")
auth3 = new_auth()
try:
    auth3.generate_anisette_headers()
    check(auth3.client_info == GOOD_CI, "đang dùng danh tính của srv-good")
    state["dead"].add("https://srv-good.test")
    auth3.generate_anisette_headers()
    check(auth3.anisette_url == "https://srv-good2.test",
          f"đã xoay sang server dự phòng ({auth3.anisette_url})")
    check(auth3.client_info == GOOD2_CI,
          "danh tính ĐỔI theo server mới (lỗi cũ: giữ client_info của server cũ)")
    check(auth3.user_agent == GOOD2_UA, "user_agent cũng đổi theo server mới")
except Exception as e:  # pragma: no cover
    check(False, f"xoay server thất bại: {e}")
finally:
    apple_auth.ANISETTE_SEED_SERVERS = saved_seeds
    state["dead"].discard("https://srv-good.test")

print()
print("5) -80045 lúc đăng nhập thật → tự đổi server và thử lại")
auth4 = new_auth()
attempts = {"n": 0}
real_gsa = auth4.gsa_request


def flaky_gsa(params, debug=True):
    if params.get("o") == "init" and attempts["n"] == 0:
        attempts["n"] += 1
        raise apple_auth.AnisetteIdentityRejected("ec -80045 (fake)")
    return {"Status": {"ec": 0}, "sp": "s2k", "s": b"salt" * 8, "B": b"B" * 384,
            "c": b"c" * 32, "i": 20000, "M2": b"m2"}


auth4.gsa_request = flaky_gsa
res = auth4._recover_identity("user@example.com", "pw", 0, 0,
                             apple_auth.AnisetteIdentityRejected("ec -80045 (fake)"))
check(attempts["n"] == 1, "đã tự chạy lại SRP sau khi Apple từ chối danh tính")
check(auth4.last_error != "", "có ghi lại lý do cho UI (get_last_error)")

print()
print("6) hsc 409 (Apple tự gửi mã 2FA) → không gọi lại endpoint trigger")
called = {"trigger": 0}


def no_trigger(dsid, idms, already_pushed=False):
    called["trigger"] = 0 if already_pushed else 1
    return False


auth5 = new_auth()
auth5.handle_2fa_trusted_device = no_trigger
code_requests = []
auth5.input_func = lambda prompt: (code_requests.append(prompt), "123456")[1]
auth5.handle_2fa_trusted_device(1, "tok", already_pushed=True)
check(called["trigger"] == 0, "already_pushed=True → bỏ qua trigger")

print()
print("=" * 70)
if failures:
    print(f"❌ {len(failures)} kiểm tra thất bại")
    for f in failures:
        print("   -", f)
    sys.exit(1)
print("✅ TẤT CẢ KIỂM TRA ĐỀU ĐẠT")
