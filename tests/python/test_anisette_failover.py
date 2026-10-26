#!/usr/bin/env python3
"""v73 — Kiểm thử TỰ ĐỔI SERVER khi server Anisette lỗi (không cần mạng).

Bối cảnh thực tế (đo trực tiếp 2026-10-26):
  https://mac-ani.jkcoxson.com        → HTTP 200
  https://ani.jkcoxson.com            → HTTP 200
  https://win.ani.sidestore.io        → HTTP 502  (server chết)

Bug người dùng báo: "server mac-ani lỗi là tool DỪNG LUÔN, không chuyển sang
ani.jkcoxson.com hay các server ở dưới". Test này tái hiện 5 tình huống:

  1. Server ĐẦU TIÊN trả 502 lúc khởi tạo → phải chọn server thứ 2.
  2. Server đang dùng CHẾT GIỮA PHIÊN → đổi server ngay trong cùng lần gọi
     generate_anisette_headers(), đồng thời đổi client_info/user_agent theo server mới.
  3. Apple GSA probe HOÀN TOÀN KHÔNG TRẢ LỜI (503/timeout) → vẫn phải chọn
     được một server CÒN SỐNG (trước đây rơi vào nhánh "dùng mặc định" chết).
  4. Server do người dùng GHIM bị chết → vẫn tự failover (yêu cầu trực tiếp).
  5. authenticate() gặp AnisetteServerUnavailable → tự đổi server và chạy lại SRP.

Chạy: python3 tests/python/test_anisette_failover.py
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


MAC = "https://mac-ani.jkcoxson.com"
ANI = "https://ani.jkcoxson.com"
ANI3 = "https://ani3.stikstore.app"

CI_MAC = "<MacBookPro13,3> <Mac OS X;10.12.6;16G29> <com.apple.AuthKit/1 (com.apple.akd/1.0)>"
UA_MAC = "akd/1.0 CFNetwork/811.5.4 Darwin/16.7.0"
CI_ANI = "<MacBookPro13,2> <macOS;13.1;22C65> <com.apple.AuthKit/1 (com.apple.akd/1.0)>"
UA_ANI = "akd/1.0 CFNetwork/808.1.4"

SERVERS = {
    MAC: {"ci": CI_MAC, "ua": UA_MAC},
    ANI: {"ci": CI_ANI, "ua": UA_ANI},
    ANI3: {"ci": CI_MAC, "ua": UA_MAC},
}

state = {
    "dead": set(),          # server không phản hồi
    "http_502": set(),      # server trả 502 rõ ràng
    "gsa_down": False,      # Apple edge không trả lời
    "posts": [],            # các request GSA (header + cpd + op)
    "gets": [],             # các GET tới anisette server
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


class FakeSession:
    def __init__(self, net):
        self.net = net
        self.verify = True

    def get(self, url, timeout=10, **kw):
        return self.net.get(url, timeout=timeout, **kw)

    def request(self, method, url, **kw):
        return self.net.post(url, **kw)

    def post(self, url, **kw):
        return self.net.post(url, **kw)

    def close(self):
        pass


def _gsa_plist(ec=None, challenge=False):
    resp = {"Status": {"hsc": 200, "ec": ec if ec is not None else 0, "em": "", "ptxid": "x"}}
    if challenge:
        resp.update({"sp": "s2k", "s": b"salt" * 8, "B": b"B" * 384,
                     "c": b"c" * 32, "i": 20000})
    return plistlib.dumps({"Response": resp})


class FakeNet:
    def get(self, url, timeout=10, **kw):
        base = url.split("/v3/")[0].rstrip("/")
        state["gets"].append((base, url))
        if base in state["dead"]:
            raise Exception("connection refused (fake)")
        if base in state["http_502"]:
            return FakeResp(502, content=b"<html>502 Bad Gateway</html>")
        cfg = SERVERS.get(base)
        if cfg is None:
            return FakeResp(404, content=b"")
        if url.endswith("/v3/client_info"):
            return FakeResp(200, json_data={"client_info": cfg["ci"], "user_agent": cfg["ua"]})
        if url.endswith("/v3/get_headers"):
            return FakeResp(405, content=b"")
        return FakeResp(200, json_data={
            "X-Apple-I-Client-Time": "2026-10-26T12:00:00Z",
            "X-Apple-I-MD": "AAAABQAAAA==",
            "X-Apple-I-MD-M": "otp_" + base[-6:],
            "X-Apple-I-MD-LU": "LU_" + base[-6:],
            "X-MMe-Client-Info": cfg["ci"],
            "X-Mme-Device-Id": "DEVICE-" + base[-6:],
            "X-Apple-I-TimeZone": "UTC",
            "X-Apple-Locale": "en_US",
        })

    def post(self, url, data=None, headers=None, **kw):
        headers = headers or {}
        body = plistlib.loads(data) if isinstance(data, (bytes, bytearray)) else {}
        req = body.get("Request", {})
        cpd_ci = (req.get("cpd") or {}).get("X-MMe-Client-Info")
        state["posts"].append({"hdr_ci": headers.get("X-MMe-Client-Info"),
                               "cpd_ci": cpd_ci,
                               "ua": headers.get("User-Agent"),
                               "op": req.get("o")})
        if url.endswith("/GsService2") or "grandslam" in url:
            if state["gsa_down"]:
                return FakeResp(503, content=b"<html>503</html>")
            return FakeResp(200, content=_gsa_plist(ec=0, challenge=True))
        return FakeResp(200, content=b"{}")

    def Session(self):
        return FakeSession(self)


import requests as _real_requests  # noqa: E402
fake_net = FakeNet()
requests_mod = types.ModuleType("requests")
requests_mod.get = fake_net.get
requests_mod.post = fake_net.post
requests_mod.Session = fake_net.Session
requests_mod.Session_ = FakeSession
sys.modules["requests"] = requests_mod

import apple_auth  # noqa: E402

apple_auth.requests = requests_mod
apple_auth.ANISETTE_CACHE_FILE = os.path.join(tempfile.mkdtemp(prefix="failover_test_"),
                                              ".anisette_cache.json")
apple_auth.ANISETTE_SEED_SERVERS = (MAC, ANI, ANI3)
apple_auth.ANISETTE_URL = ANI3     # fallback cũng phải là server sống
apple_auth.OFFICIAL_SERVERS_URL = "https://servers.invalid/servers.json"
apple_auth.AppleAuth._fetch_server_list = staticmethod(lambda: [])


def reset(dead=(), http_502=(), gsa_down=False, seeds=(MAC, ANI, ANI3)):
    if os.path.exists(apple_auth.ANISETTE_CACHE_FILE):
        os.remove(apple_auth.ANISETTE_CACHE_FILE)
    state["dead"] = set(dead)
    state["http_502"] = set(http_502)
    state["gsa_down"] = gsa_down
    state["posts"].clear()
    state["gets"].clear()
    apple_auth.ANISETTE_SEED_SERVERS = seeds
    return apple_auth.AppleAuth(input_func=lambda p: "")


print("─" * 72)
print("1) Server ĐẦU TIÊN 502 → tự chọn server thứ 2 (ani.jkcoxson.com)")
auth = reset(http_502={MAC})
check(auth.anisette_url == ANI, f"đã chuyển sang {ANI} (nhận {auth.anisette_url})")
check(auth.client_info == CI_ANI, "client_info lấy đúng của server MỚI")
check(auth.user_agent == UA_ANI, "user_agent lấy đúng của server MỚI")
hdr = auth.generate_anisette_headers()
check(hdr.get("X-Apple-I-MD-M", "").endswith(ANI[-6:]),
      "headers anisette lấy từ server MỚI (OTP/machine-id khớp)")

print()
print("2) Server đang dùng CHẾT GIỮA PHIÊN → đổi ngay trong cùng lần gọi")
auth = reset()
first = auth.generate_anisette_headers()
check(auth.anisette_url == MAC, "ban đầu dùng mac-ani")
state["dead"].add(MAC)
try:
    second = auth.generate_anisette_headers()
    check(auth.anisette_url == ANI, f"đổi sang {ANI} (nhận {auth.anisette_url})")
    check(auth.client_info == CI_ANI, "danh tính ĐỔI theo server mới")
    check(auth.user_agent == UA_ANI, "user_agent ĐỔI theo server mới")
    check(second.get("X-Apple-I-MD-M", "").endswith(ANI[-6:]),
          "trả headers hợp lệ ngay, KHÔNG raise rồi dừng cả luồng")
    check(MAC in auth._failed_servers, "server chết bị đưa vào danh sách loại")
except Exception as e:
    check(False, f"generate_anisette_headers() vẫn raise: {e}")

print()
print("3) Apple GSA probe KHÔNG TRẢ LỜI (503) → vẫn chọn được server sống")
auth = reset(gsa_down=True)
check(auth.anisette_url in (MAC, ANI, ANI3),
      f"vẫn có server sống được chọn: {auth.anisette_url}")
check(auth.anisette_url not in state["dead"], "server được chọn không phải server chết")
check(len(auth.client_info) > 0, "vẫn có client_info để gửi Apple")
check(auth.session.verify is True, "TLS verification được BẬT (không còn verify=False)")

print()
print("4) Server do người dùng GHIM bị chết → vẫn tự failover")
if os.path.exists(apple_auth.ANISETTE_CACHE_FILE):
    os.remove(apple_auth.ANISETTE_CACHE_FILE)
state["dead"] = {MAC}
state["http_502"] = set()
state["gsa_down"] = False
state["posts"].clear()
pinned = apple_auth.AppleAuth(anisette_url=MAC, input_func=lambda p: "")
check(pinned.anisette_url != MAC, f"không giữ server ghim đã chết: {pinned.anisette_url}")
try:
    got = pinned.generate_anisette_headers()
    check("X-Apple-I-MD-M" in got, "lấy được headers từ server dự phòng")
except Exception as e:
    check(False, f"server ghim chết vẫn làm đứt luồng: {e}")

print()
print("5) authenticate() gặp AnisetteServerUnavailable → đổi server + chạy lại SRP")
auth = reset()
auth._gsa_init_ok = False
calls = {"n": 0, "servers": []}


def flaky_gsa(params, debug=True):
    calls["n"] += 1
    calls["servers"].append(auth.anisette_url)
    if calls["n"] == 1:
        raise apple_auth.AnisetteServerUnavailable("server mac-ani vừa 502 (fake)")
    # Lần 2: trả response không có "sp" để authenticate kết thúc sạch sẽ.
    return {"Status": {"ec": 0, "hsc": 200}}


auth.gsa_request = flaky_gsa
auth.authenticate("user@example.com", "pw")
check(calls["n"] >= 2, f"đã tự chạy lại SRP sau khi server chết ({calls['n']} lần)")
check(len(set(calls["servers"])) >= 2,
      f"đã đổi server giữa 2 lần thử: {calls['servers']}")
check(auth.anisette_url != MAC, f"không quay lại server chết: {auth.anisette_url}")

print()
print("6) MỌI server chết → báo lỗi rõ ràng, không treo vô hạn")
auth = reset(dead={MAC, ANI, ANI3})
try:
    auth.generate_anisette_headers()
    check(False, "đáng lẽ phải raise AnisetteServerUnavailable")
except apple_auth.AnisetteServerUnavailable as e:
    check("thử" in str(e) or "server" in str(e).lower(),
          f"lỗi có nguyên nhân rõ ràng cho UI: {str(e)[:100]}")
except Exception as e:
    check(False, f"sai loại exception: {type(e).__name__}: {e}")

print()
print("=" * 72)
if failures:
    print(f"❌ {len(failures)} kiểm tra thất bại")
    for f in failures:
        print("   -", f)
    sys.exit(1)
print("✅ TẤT CẢ KIỂM TRA ĐỀU ĐẠT")
