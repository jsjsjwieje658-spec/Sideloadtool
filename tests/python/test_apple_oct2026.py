#!/usr/bin/env python3
"""v73 — Thay đổi Apple 10/2026: -80046 (apptokens) + nhất quán danh tính developer.

Nguồn đã đối chiếu (xem FIX_REPORT_v73.md):
  - blog.illixion.com/2026/10/sideloading-error-80046/  (Apparition 0.3.0)
  - pwnapplehat/iPASide#9, #10, CHANGELOG 1.2.9        (2026-10-22/23)
  - rileytestut/AltSign#61, altstoreio/AltStore#1834    (danh tính từ /v3/client_info)
  - Dadoum/anisette-v3-server#61                        (-80045 từ 12/10/2026)

Kiểm tra:
  1. `apptokens` LUÔN gửi danh tính Mac (dù server anisette khai Windows).
  2. Gặp ec -80046 → đổi danh tính Mac KHÁC và thử lại (không đăng nhập lại SRP).
  3. Header HTTP X-MMe-Client-Info và cpd TRÙNG NHAU khi dùng danh tính token.
  4. DeveloperAPI._auth_headers() dùng developer_client_info (danh tính Mac đã
     được cấp token), KHÔNG dùng danh tính Windows từ headers anisette.
  5. Bật TLS verify (không còn verify=False làm lộ mật khẩu Apple ID).

Chạy: python3 tests/python/test_apple_oct2026.py
"""
import os
import plistlib
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
PY_DIR = os.path.normpath(os.path.join(HERE, "..", "..", "app", "src", "main", "python"))
sys.path.insert(0, PY_DIR)

failures = []


def check(cond, msg):
    print(("  ✅ " if cond else "  ❌ ") + msg)
    if not cond:
        failures.append(msg)


WINDOWS_CI = "<PC> <Windows;6.2(0,0);9200> <com.apple.AuthKit/1 (com.apple.iTunes/12.12.4.1)>"
WINDOWS_UA = "akd/1.0 CFNetwork/808.1.4"

state = {"posts": []}


class FakeResp:
    def __init__(self, ec=0):
        self.status_code = 200
        self.headers = {"X-Apple-DSID": "1"}
        self.content = plistlib.dumps({"Response": {"Status": {"ec": ec, "hsc": 200}}})
        self.text = ""

    def raise_for_status(self):
        pass


class FakeSession:
    def __init__(self, net):
        self.net = net
        self.verify = True

    def request(self, method, url, **kw):
        return self.net.post(url, **kw)

    def get(self, url, **kw):
        return self.net.get(url, **kw)

    def post(self, url, **kw):
        return self.net.post(url, **kw)

    def close(self):
        pass


class FakeNet:
    def get(self, url, **kw):
        return FakeResp()

    def post(self, url, data=None, headers=None, **kw):
        headers = headers or {}
        body = plistlib.loads(data) if isinstance(data, (bytes, bytearray)) else {}
        cpd_ci = (body.get("Request", {}).get("cpd") or {}).get("X-MMe-Client-Info")
        state["posts"].append({"hdr_ci": headers.get("X-MMe-Client-Info"),
                               "cpd_ci": cpd_ci,
                               "ua": headers.get("User-Agent"),
                               "op": body.get("Request", {}).get("o")})
        return FakeResp(ec=0)

    def Session(self):
        return FakeSession(self)


import requests as _real  # noqa: E402
fake_net = FakeNet()
rq = types.ModuleType("requests")
rq.get = fake_net.get
rq.post = fake_net.post
rq.Session = fake_net.Session
rq.Session_ = FakeSession
sys.modules["requests"] = rq

import apple_auth  # noqa: E402
import developer_api  # noqa: E402

apple_auth.requests = rq
developer_api.requests = rq


def bare_auth(client_info=WINDOWS_CI):
    a = object.__new__(apple_auth.AppleAuth)
    a.client_info = client_info
    a.user_agent = WINDOWS_UA
    a._identity_variant = "raw"
    a._token_identity_next = False
    a._token_identity_index = 0
    a.last_error = ""
    a.session = FakeSession(fake_net)
    a.session.verify = True
    a._refresh_token_identity()
    return a


print("─" * 72)
print("1) Server Windows → danh tính apptokens vẫn là MAC")
auth = bare_auth()
check("<Mac" in auth.token_identity, f"token identity = {auth.token_identity}")
check(auth.developer_client_info == auth.token_identity,
      "developer services sẽ dùng ĐÚNG danh tính Mac đã cấp token")

print()
print("2) Apple trả -80046 → thử lại với danh tính Mac KHÁC")
claims = []


def flaky_apptokens(params, debug=True):
    claims.append({"identity": auth.token_identity, "op": params.get("o")})
    if len(claims) < 3:
        return {"Status": {"ec": -80046, "hsc": 400}}
    return {"Status": {"ec": 0, "hsc": 200}}   # không có 'et' → kết thúc sạch


auth.gsa_request = flaky_apptokens
auth.fetch_app_token("adsid", b"c" * 16, "idms-token", b"s" * 32)
check(len(claims) == 3, f"đã thử lại apptokens 3 lần (nhận {len(claims)})")
check(all(c["op"] == "apptokens" for c in claims), "cả 3 lần đều là op=apptokens")
check(all(apple_auth.AppleAuth._identity_score(c["identity"]) == 0 for c in claims),
      "mọi lần thử đều khai Mac (không gửi lại danh tính Windows)")
check(len({c["identity"] for c in claims}) >= 2,
      f"đã ĐỔI danh tính Mac giữa các lần thử: {len({c['identity'] for c in claims})} kiểu")
check("-80046" in auth.last_error,
      "UI nhận được lý do thật (-80046) thay vì câu chung 'xem nhật ký'")
check(-80046 in apple_auth.AppleAuth.GSA_ERROR_HINTS, "-80046 có hint hành động cho UI")

print()
print("3) gsa_request (apptokens): header HTTP và cpd PHẢI trùng danh tính Mac")
auth2 = bare_auth()
auth2.generate_cpd = lambda: {"X-MMe-Client-Info": WINDOWS_CI,
                              "X-Apple-I-MD-M": b"otp", "bootstrap": True}
auth2._token_identity_next = True
state["posts"].clear()
try:
    auth2.gsa_request({"u": "x", "o": "apptokens"})
except Exception as e:
    print("   (gsa_request:", e, ")")
check(len(state["posts"]) == 1, "đã gửi 1 request GSA")
if state["posts"]:
    post = state["posts"][0]
    check(post["hdr_ci"] == auth2.token_identity,
          f"header X-MMe-Client-Info = danh tính Mac ({post['hdr_ci']})")
    check(post["cpd_ci"] == post["hdr_ci"],
          "cpd X-MMe-Client-Info TRÙNG header (tránh -80044/-80045)")
    check("Windows" not in str(post["hdr_ci"]),
          "không gửi danh tính Windows ở bước apptokens")

print()
print("4) DeveloperAPI dùng danh tính Mac đã cấp token (chống 'session expired')")
MAC_DEV = "<Mac15,7> <macOS;27.0;26A5378j> <com.apple.AuthKit/1 (com.apple.akd/1.0)>"


class FakeAuth:
    session = FakeSession(fake_net)
    developer_client_info = MAC_DEV
    developer_user_agent = "akd/1.0 CFNetwork/808.1.4"
    client_info = WINDOWS_CI
    user_agent = WINDOWS_UA

    def generate_anisette_headers(self):
        return {"X-MMe-Client-Info": WINDOWS_CI, "X-Apple-I-MD-M": b"otp"}


api = developer_api.DeveloperAPI(FakeAuth(), "123", "token")
h = api._auth_headers("text/x-xml-plist", "text/x-xml-plist")
check(h.get("X-MMe-Client-Info") == MAC_DEV,
      f"developer header = danh tính Mac ({h.get('X-MMe-Client-Info')})")
check("Windows" not in h.get("X-MMe-Client-Info", ""),
      "KHÔNG ghi đè ngược lại bằng danh tính Windows của server anisette")
check(h.get("User-Agent") == "Xcode",
      "giữ User-Agent Xcode của developerservices2 (AltSign), không đổi protocol")

print()
print("5) TLS verification BẬT cho mọi kết nối Apple")
src = open(os.path.join(PY_DIR, "apple_auth.py"), encoding="utf-8").read()
check("self.session.verify = True" in src, "AppleAuth.session.verify = True")
check("sess.verify = True" in src, "socket mới tới gsa.apple.com cũng verify=True")
check("resp = requests.get(OFFICIAL_SERVERS_URL, timeout=10, verify=True)" in src,
      "tải danh sách server SideStore verify=True")

print()
print("=" * 72)
if failures:
    print(f"❌ {len(failures)} kiểm tra thất bại")
    for f in failures:
        print("   -", f)
    sys.exit(1)
print("✅ TẤT CẢ KIỂM TRA ĐỀU ĐẠT")
