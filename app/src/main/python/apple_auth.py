"""Cổng Apple ID (GSA/SRP + 2FA) cho bản Android (Chaquopy).

Dùng CHÍNH bản đã sửa của công cụ Termux (đã chạy thật: đăng nhập, 2FA,
đăng ký thiết bị, App ID, profile, ký IPA đều OK), chỉ thích ứng Android:
  - input()/getpass() → self.input_func(prompt): sideload_core truyền vào hàm
    gọi UiPrompt.requestInput() (dialog trên UI, chặn luồng nền tới khi gửi).
  - file cache anisette ghi vào filesDir của app (thư mục hiện hành của tiến
    trình Android không ghi được).
  - giữ hàm fetch_official_servers() cho màn Cài đặt.

HAI LỖI GIỐNG HỆT bản Termux lúc đầu đã được sửa ở đây:
  1. HTTP 429 từ gsa.apple.com dù anisette riêng hoạt động: requests.Session
     dùng lại một socket keep-alive; edge của Apple trả trang HTML 429 cho mọi
     request sau request đầu tiên trên cùng socket → `init` 200 nhưng
     `complete` 429. Nay mỗi request tới gsa.apple.com đi một kết nối riêng
     (Connection: close) và gửi lại tối đa 3 lần trên socket mới.
  2. X-MMe-Client-Info mang token com.apple.dt.Xcode bị edge chặn (503 HTML)
     từ 09/2026 → nay định danh là com.apple.akd như AltServer mới.
"""

import uuid
import json
import base64
import time
from datetime import datetime, timezone
import locale
import requests
import plistlib as plist
import srp._pysrp as srp
import hashlib
import hmac
import binascii
from cryptography.hazmat.primitives import padding, hashes
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC
from cryptography.hazmat.backends import default_backend
import os
import urllib3
import traceback

# Disable SSL warnings
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)


class GsaRateLimitedError(Exception):
    """Apple edge tra HTML 429 cho POST GsService2.

    Khong phai anisette server bi chan. Doi live 2026-09-23: request dau tren
    mot socket keep-alive vao duoc GrandSlam, moi request sau tren cung socket
    bi edge tra trang HTML 429 (pooled [404, 429, 429, 429]; moi request mot
    socket moi [404, 404, 404, 404]). requests.Session tai su dung socket do,
    nen `init` 200 va `complete` 429 du anisette private tra header hop le.
    """
    pass

# ---------------------- Configuration ----------------------
ANISETTE_URL = "https://ani.sidestore.io"  # fallback khi không lấy được danh sách
OFFICIAL_SERVERS_URL = "https://servers.sidestore.io/servers.json"  # Danh sách server SideStore
def _writable_dir():
    try:
        from com.superalpha.sideload.bridge import AppPaths
        d = str(AppPaths.filesDir())
        if d and os.path.isdir(d):
            return d
    except Exception:
        pass
    for d in (os.environ.get("HOME"), os.getcwd()):
        if d and os.path.isdir(d) and os.access(d, os.W_OK):
            return d
    import tempfile
    return tempfile.gettempdir()


ANISETTE_CACHE_FILE = os.path.join(_writable_dir(), ".anisette_cache.json")
ANISETTE_CACHE_TTL = 6 * 3600  # Cache server đã chọn trong 6 giờ


def fetch_official_servers():
    """Danh sách server Anisette công khai của SideStore (list[dict]) — [] nếu lỗi.
    Dùng cho màn Cài đặt (sideload_core.list_anisette_servers)."""
    try:
        resp = requests.get(OFFICIAL_SERVERS_URL, timeout=10, verify=False)
        resp.raise_for_status()
        return resp.json().get("servers", [])
    except Exception as e:
        print(f"[anisette] Không thể lấy danh sách server: {e}")
        return []


class AppleAuth:
    def __init__(self, anisette_url=None, input_func=None):
        self.input_func = input_func or input
        # URL truyen tu config (server private) khong duoc bo khi Apple tra 429.
        # 429 do la filter theo socket, khong phai profile anisette chet.
        self._anisette_pinned = bool(anisette_url and str(anisette_url).strip())
        self._failed_servers = set()
        self._cpd_pin = None
        self._pin_cpd = False
        self._gsa_init_ok = False
        if self._anisette_pinned:
            self.anisette_url = str(anisette_url).strip().rstrip("/")
            print(f"[anisette] Dùng server chỉ định, không xoay: {self.anisette_url}")
        else:
            self.anisette_url = self.get_best_anisette_server()
        self.user_id = str(uuid.uuid4()).upper()
        self.device_id = str(uuid.uuid4()).upper()
        self.session = requests.Session()
        self.session.verify = False

        # User agent and client info that Apple expects
        self.user_agent = "akd/1.0 CFNetwork/1408.0.4 Darwin/22.5.0"
        # [FIX 2026-09] Apple edge chan moi POST GsService2 co token com.apple.dt.Xcode
        # trong X-MMe-Client-Info (tra ve 503 HTML thay plist GSA). Fix theo
        # AltServer 1.7.6 / anisette-v3-server #60: identify as com.apple.akd.
        self.client_info = "<MacBookPro18,3> <macOS;13.4.1;22F8> <com.apple.AuthKit/1 (com.apple.akd/1.0)>"
        # UA giong akd that (khong con token Xcode)
        self.xcode_ua = "akd/1.0 CFNetwork/1408.0.4 Darwin/22.5.0"

        # Configure SRP properly
        srp.rfc5054_enable()
        srp.no_username_in_x()

    def get_best_anisette_server(self):
        """Tự động chọn server Anisette từ danh sách SideStore.

        - Tải danh sách từ https://servers.sidestore.io/servers.json
        - Duyệt từ server CUỐI danh sách LÊN TRÊN (bottom-up), lấy server ĐẦU TIÊN
          phản hồi hợp lệ (HTTP 200 qua /v3/client_info hoặc /)
        - Nếu server đang dùng lỗi khi fetch anisette, nó bị thêm vào
          self._failed_servers và lần chọn tiếp theo sẽ bỏ qua, nhảy lên server
          phía trên trong danh sách
        - Cache server đã chọn 6 giờ (.anisette_cache.json)
        - Không server nào sống -> fallback ANISETTE_URL (local)
        """
        failed = getattr(self, "_failed_servers", set())

        cached = self._load_anisette_cache()
        if cached and cached not in failed:
            print(f"[anisette] Dùng server đã cache: {cached}")
            return cached
        if cached and cached in failed:
            # Server cache đã chết -> bỏ cache để dò lại
            self._invalidate_anisette_cache()

        print("[anisette] Đang tìm server từ danh sách SideStore (duyệt từ dưới lên)...")
        servers = self._fetch_server_list()
        if not servers:
            print(f"[anisette] ⚠️  Không tải được danh sách server — dùng mặc định: {ANISETTE_URL}")
            return ANISETTE_URL

        # Duyệt từ server dưới đáy danh sách lên trên
        for server in reversed(servers):
            addr = (server.get("address") or "").rstrip("/")
            if not addr or addr in failed:
                continue
            name = server.get("name", addr)
            latency = self._probe_anisette_server(addr)
            if latency is not None:
                print(f"[anisette] ✅ Chọn server: {name} ({addr}) — {latency*1000:.0f}ms")
                self._save_anisette_cache(addr)
                return addr
            print(f"[anisette]   ✗ {name} ({addr}) — không phản hồi, thử server phía trên...")

        print(f"[anisette] ⚠️  Không có server nào hoạt động — dùng mặc định: {ANISETTE_URL}")
        return ANISETTE_URL

    @staticmethod
    def _fetch_server_list():
        """Tải danh sách server từ SideStore."""
        try:
            resp = requests.get(OFFICIAL_SERVERS_URL, timeout=10)
            resp.raise_for_status()
            servers = resp.json().get("servers", [])
            print(f"[anisette] Nhận được {len(servers)} server từ danh sách SideStore")
            return servers
        except Exception as e:
            print(f"[anisette] Lỗi tải danh sách server: {e}")
            return []

    @staticmethod
    def _probe_anisette_server(addr, timeout=6):
        """Kiểm tra server có sống không. Trả về latency (giây) nếu HTTP 200, ngược lại None."""
        for path in ("/v3/client_info", ""):
            url = addr + path
            try:
                start = time.monotonic()
                resp = requests.get(url, timeout=timeout, verify=False)
                latency = time.monotonic() - start
                if resp.ok:
                    return latency
            except Exception:
                continue
        return None

    @staticmethod
    def _load_anisette_cache():
        try:
            if not os.path.exists(ANISETTE_CACHE_FILE):
                return None
            with open(ANISETTE_CACHE_FILE) as f:
                data = json.load(f)
            if data.get("url") and time.time() - data.get("ts", 0) < ANISETTE_CACHE_TTL:
                return data["url"]
        except Exception:
            pass
        return None

    @staticmethod
    def _save_anisette_cache(url):
        try:
            with open(ANISETTE_CACHE_FILE, "w") as f:
                json.dump({"url": url, "ts": time.time()}, f)
        except Exception:
            pass

    @staticmethod
    def _invalidate_anisette_cache():
        try:
            if os.path.exists(ANISETTE_CACHE_FILE):
                os.remove(ANISETTE_CACHE_FILE)
        except Exception:
            pass

    def generate_anisette_headers(self):
        """Generate Anisette headers required for Apple authentication."""
        # Thử lại tối đa 3 lần nếu fetch anisette lỗi
        for attempt in range(3):
            try:
                print(f"[anisette] Fetching headers from {self.anisette_url} (lần {attempt+1}/3)...")
                response = self.session.get(self.anisette_url, timeout=10)
                response.raise_for_status()
                data = response.json()
                
                # Kiểm tra xem có đủ key quan trọng không
                if "X-Apple-I-MD-M" not in data:
                    print("[anisette] Cảnh báo: Thiếu X-Apple-I-MD-M trong response.")
                
                return data
            except Exception as e:
                print(f"[anisette] Lỗi fetch anisette từ {self.anisette_url}: {e}")
                if not hasattr(self, "_failed_servers"):
                    self._failed_servers = set()
                if attempt < 2:
                    time.sleep(1)
                    # Server private do nguoi dung chi dinh: loi fetch la loi mang,
                    # khong duoc nhay sang server cong cong.
                    if getattr(self, "_anisette_pinned", False):
                        print(f"[anisette] Giữ server private {self.anisette_url}, thử lại...")
                    else:
                        self._failed_servers.add(self.anisette_url.rstrip("/"))
                        self.anisette_url = self.get_best_anisette_server()
                else:
                    raise Exception("Không thể lấy dữ liệu Anisette sau 3 lần thử.")

    @staticmethod
    def _sanitize_client_info(info):
        """[FIX 2026-09] Apple edge (tu ~08/09/2026) drop ngay moi POST toi
        https://gsa.apple.com/grandslam/GsService2 neu header X-MMe-Client-Info
        chua token 'com.apple.dt.Xcode' — tra ve trang HTML 190 byte (Server: Apple)
        thay vi plist GSA, client doc thanh '503 Service Temporarily Unavailable'.
        Day la block o EDGE truoc khi xu ly credential nen doi Apple ID/server/
        mang deu khong anh huong. Fix theo AltStore (altstoreio/AltStore#1790,
        AltServer 1.7.6) va Dadoum/anisette-v3-server #60: thay token bang
        'com.apple.akd' — daemon thuc su thuc hien request tren macOS.
        Luu y: phai sanitize O MOI DIEM GUI, ke ca khi anisette server tu tra
        ve client-info chua token Xcode (nhieu server con cu)."""
        if not info:
            return info
        return info.replace("com.apple.dt.Xcode", "com.apple.akd")

    def generate_meta_headers(self):
        """Generate meta headers for Apple requests."""
        return {
            "X-Apple-I-Client-Time": datetime.utcnow().replace(microsecond=0).isoformat() + "Z",
            "X-Apple-I-TimeZone": str(datetime.utcnow().astimezone().tzinfo),
            "loc": locale.getdefaultlocale()[0] or "en_US",
            "X-Apple-Locale": locale.getdefaultlocale()[0] or "en_US",
            "X-Apple-I-MD-RINFO": "17106176",
            "X-Apple-I-MD-LU": base64.b64encode(self.user_id.encode()).decode(),
            "X-Mme-Device-Id": self.device_id,
            "X-Apple-I-SRL-NO": "0",
        }

    def generate_cpd(self):
        """Generate CPD (Client Platform Data) for GSA requests.

        Trong mot lan SRP, init va complete dung cung mot CPD. Doi OTP/device-id
        giua hai buoc khong gay ra 429 (edge chan truoc khi doc body) nhung lam
        GrandSlam thay hai thiet bi khac nhau neu buoc complete lot qua edge.
        """
        if getattr(self, "_pin_cpd", False) and self._cpd_pin is not None:
            print("[cpd] Dùng lại client platform data của phiên SRP (cùng machine-id/OTP).")
            return self._cpd_pin

        print("[cpd] Generating client platform data...")

        anisette_data = self.generate_anisette_headers()

        # Update internal device info to match anisette server
        if "X-Mme-Device-Id" in anisette_data:
            self.device_id = anisette_data["X-Mme-Device-Id"]
        if "X-MMe-Client-Info" in anisette_data:
            self.client_info = self._sanitize_client_info(anisette_data["X-MMe-Client-Info"])

        cpd = {
            "bootstrap": True,
            "icscrec": True,
            "pbe": False,
            "prkgen": True,
            "svct": "iCloud",
        }

        cpd.update(self.generate_meta_headers())
        cpd.update(anisette_data)
        if "X-MMe-Client-Info" in cpd:
            cpd["X-MMe-Client-Info"] = self._sanitize_client_info(cpd["X-MMe-Client-Info"])

        print("[cpd] Client platform data generated successfully")
        if getattr(self, "_pin_cpd", False):
            self._cpd_pin = cpd
        return cpd

    def _http(self, method, url, **kwargs):
        """HTTP helper.

        gsa.apple.com ghim socket keep-alive va tra HTML 429 cho moi request
        sau request dau tien tren socket do. requests.Session tai su dung socket,
        nen moi request toi host nay di qua Session mot lan roi dong ngay.
        """
        headers = dict(kwargs.pop("headers", None) or {})
        timeout = kwargs.pop("timeout", 30)
        if "gsa.apple.com" in url:
            headers["Connection"] = "close"
            sess = requests.Session()
            sess.verify = False
            try:
                return sess.request(method, url, headers=headers, timeout=timeout, **kwargs)
            finally:
                # Body da duoc doc (stream=False) truoc khi post() tra ve.
                sess.close()
        return self.session.request(method, url, headers=headers, timeout=timeout, **kwargs)

    def encrypt_password(self, password, salt, iterations, protocol):
        """Encrypt password using Apple's protocol."""
        try:
            print(f"[crypto] Encrypting password with protocol: {protocol}")

            p = hashlib.sha256(password.encode("utf-8")).digest()

            if protocol == "s2k_fo":
                p = p.hex().encode("utf-8")

            return hashlib.pbkdf2_hmac("sha256", p, salt, iterations, 32)

        except Exception as e:
            print(f"[crypto] Error encrypting password: {e}")
            raise

    def create_session_key(self, usr, name):
        """Create session key for decryption."""
        try:
            session_key = usr.get_session_key()
            if session_key is None:
                raise Exception("No session key available")
            return hmac.new(session_key, name.encode(), hashlib.sha256).digest()
        except Exception as e:
            print(f"[crypto] Error creating session key: {e}")
            raise

    def decrypt_cbc(self, usr, data):
        """Decrypt CBC encrypted data."""
        try:
            extra_data_key = self.create_session_key(usr, "extra data key:")
            extra_data_iv = self.create_session_key(usr, "extra data iv:")
            extra_data_iv = extra_data_iv[:16]

            cipher = Cipher(algorithms.AES(extra_data_key), modes.CBC(extra_data_iv))
            decryptor = cipher.decryptor()
            data = decryptor.update(data) + decryptor.finalize()

            unpadder = padding.PKCS7(128).unpadder()
            return unpadder.update(data) + unpadder.finalize()

        except Exception as e:
            print(f"[crypto] Error decrypting data: {e}")
            raise

    def decrypt_gcm(self, sk, encrypted_data):
        """Giải mã AES-GCM cho trường 'et' (encrypted token) từ bước apptokens.
        
        Cấu trúc: [3 byte 'XYZ' (AAD)] + [16 byte IV] + [ciphertext] + [16 byte tag]
        """
        if len(encrypted_data) < 35:
            raise Exception("Encrypted token quá ngắn.")
        if encrypted_data[:3] != b"XYZ":
            raise Exception("Encrypted token có version không đúng (mong đợi b'XYZ').")

        aad = encrypted_data[:3]
        iv = encrypted_data[3:19]
        ciphertext = encrypted_data[19:-16]
        tag = encrypted_data[-16:]

        decryptor = Cipher(
            algorithms.AES(sk), modes.GCM(iv, tag), backend=default_backend()
        ).decryptor()
        decryptor.authenticate_additional_data(aad)
        return decryptor.update(ciphertext) + decryptor.finalize()

    def _safe_plist_loads(self, data):
        """Parse plist an toàn - Apple trả về XML không có phần header khai báo."""
        if not data.startswith(b"bplist") and not data.lstrip().startswith(b"<?xml"):
            PLISTHEADER = (
                b"<?xml version='1.0' encoding='UTF-8'?>\n"
                b"<!DOCTYPE plist PUBLIC '-//Apple//DTD PLIST 1.0//EN' "
                b"'http://www.apple.com/DTDs/PropertyList-1.0.dtd'>\n"
            )
            data = PLISTHEADER + data
        return plist.loads(data)

    def fetch_app_token(self, adsid, c, idms_token, sk, app="com.apple.gs.xcode.auth"):
        """Đổi GsIdmsToken lấy app token thực sự dùng cho Developer API."""
        print(f"[apptoken] Đang đổi GsIdmsToken lấy app token cho '{app}'...")

        try:
            checksum_hmac = hmac.new(sk, digestmod=hashlib.sha256)
            checksum_hmac.update(b"apptokens")
            checksum_hmac.update(adsid.encode("utf-8"))
            checksum_hmac.update(app.encode("utf-8"))
            checksum = checksum_hmac.digest()

            response = self.gsa_request({
                "u": adsid,
                "app": [app],
                "c": c,
                "t": idms_token,
                "checksum": checksum,
                "o": "apptokens",
            })

            encrypted_token = response.get("et")
            if not encrypted_token:
                print(f"[apptoken] Không nhận được 'et' trong response: {response}")
                return None

            decrypted = self.decrypt_gcm(sk, encrypted_token)
            token_plist = self._safe_plist_loads(decrypted)

            app_tokens = token_plist.get("t", {})
            token_info = app_tokens.get(app)
            if not token_info or "token" not in token_info:
                print(f"[apptoken] Không tìm thấy token cho app '{app}' trong: {list(app_tokens.keys())}")
                return None

            print(f"[apptoken] Lấy app token cho '{app}' thành công.")
            return token_info["token"]

        except Exception as e:
            print(f"[apptoken] Lỗi khi lấy app token: {e}")
            return None

    def gsa_request(self, parameters, debug=True):
        """Make GSA request to Apple's authentication service.

        Moi lan goi mo mot TCP connection moi. Neu edge van tra 429/5xx, gui
        lai DUNG body do tren socket moi: edge chan truoc khi GrandSlam doc
        request, nen SRP challenge da nhan o buoc init van con hieu luc.
        """
        cpd_data = self.generate_cpd()

        body = {
            "Header": {"Version": "1.0.1"},
            "Request": {"cpd": cpd_data},
        }
        body["Request"].update(parameters)

        headers = {
            "Content-Type": "text/x-xml-plist",
            "Accept": "*/*",
            "User-Agent": self.user_agent,
            "X-MMe-Client-Info": self._sanitize_client_info(self.client_info),
        }
        payload = plist.dumps(body)
        op = parameters.get("o", "unknown")
        # 429 HTML va 5xx HTML deu la edge, khong phai plist GSA.
        edge_statuses = {429, 502, 503, 504}

        last_status = None
        for attempt in range(3):
            try:
                if debug:
                    extra = f" (kết nối mới {attempt + 1}/3)" if attempt else ""
                    print(f"[gsa] Request operation: {op}{extra}")

                print("[gsa] Sending request to Apple GSA service...")

                response = self._http(
                    "POST",
                    "https://gsa.apple.com/grandslam/GsService2",
                    headers=headers,
                    data=payload,
                    timeout=30,
                )

                print(f"[gsa] Response status: {response.status_code}")
                last_status = response.status_code

                if response.status_code in edge_statuses and attempt < 2:
                    snippet = response.content[:140].decode("utf-8", "replace").replace("\n", " ")
                    print(
                        f"[gsa] Apple edge {response.status_code} (HTML, không phải anisette bị chặn). "
                        f"Gửi lại trên socket mới... {snippet}"
                    )
                    time.sleep(0.25)
                    continue

                if response.status_code == 429:
                    snippet = response.content[:160].decode("utf-8", "replace").replace("\n", " ")
                    retry_after = response.headers.get("Retry-After", "")
                    raise GsaRateLimitedError(retry_after or f"edge 429: {snippet}")

                if response.status_code == 404:
                    raise Exception("GSA endpoint not found (404)")

                response.raise_for_status()

                response_data = plist.loads(response.content)

                if debug and "Response" in response_data:
                    resp_keys = list(response_data["Response"].keys())
                    print(f"[gsa] Response keys: {resp_keys}")

                if "Response" not in response_data:
                    raise Exception("Invalid response format from GSA service")

                result = response_data["Response"]
                result["_headers"] = response.headers
                return result

            except GsaRateLimitedError:
                raise
            except Exception as e:
                print(f"[gsa] Request failed: {e}")
                raise

        raise GsaRateLimitedError(f"edge {last_status} sau 3 kết nối mới")

    # =========================================================================
    # HELPER: _build_2fa_base_headers — PHẢI gọi riêng cho mỗi HTTP request
    # =========================================================================
    def _build_2fa_base_headers(self, dsid, idms_token, force_new=True):
        """Tạo headers 2FA với anisette MỚI mỗi lần gọi."""
        identity_token = base64.b64encode(f"{dsid}:{idms_token}".encode()).decode()

        h = {
            "User-Agent": self.xcode_ua,
            "Accept": "text/x-xml-plist",
            "Accept-Language": "en-us",
            "X-Apple-Identity-Token": identity_token,
            "X-Apple-I-Identity-Token": identity_token,
            "X-Apple-App-Info": "com.apple.gs.xcode.auth",
            "X-Xcode-Version": "14.2 (14C18)",
            "X-Mme-Client-Info": self._sanitize_client_info(self.client_info),
            "X-Apple-I-DSID": str(dsid),
        }

        h.update(self.generate_meta_headers())

        if force_new:
            try:
                anisette = self.generate_anisette_headers()
                h.update(anisette)
                # anisette server co the tra ve client-info chua token Xcode -> sanitize
                if "X-MMe-Client-Info" in h:
                    h["X-MMe-Client-Info"] = self._sanitize_client_info(h["X-MMe-Client-Info"])
                print(f"[2fa] Fresh anisette fetched (MD-M present: {'X-Apple-I-MD-M' in anisette})")
            except Exception as e:
                print(f"[2fa] ⚠️  Anisette fetch thất bại: {e}")

        h["X-Apple-I-MD-LU"] = base64.b64encode(str(dsid).encode()).decode()
        # Đảm bảo X-Apple-I-MD-RINFO được cập nhật từ anisette nếu có
        if "X-Apple-I-MD-RINFO" not in h:
            h["X-Apple-I-MD-RINFO"] = "17106176"
            
        return h

    # =========================================================================
    # handle_2fa_trusted_device — đã fix stale-anisette + POST fallback
    # =========================================================================
    def handle_2fa_trusted_device(self, dsid, idms_token):
        """Handle trusted device 2FA."""
        print("[2fa] Triggering trusted device authentication...")

        try:
            # ── BƯỚC 1: Gửi trigger ──────────────────────
            # Theo protocol GSA (TheAppleWiki), endpoint trigger thật chỉ có MỘT:
            # GET /auth/verify/trusteddevice. Bản cũ có thêm một "endpoint" thứ hai
            # là POST tới .../trusteddevice/securitycode KHÔNG kèm mã — đó chính là
            # endpoint validate (bước 3), gọi nó ở bước trigger mà không có mã là vô
            # nghĩa và chỉ tạo thêm request rác tới Apple. Đã bỏ, chỉ retry endpoint
            # trigger thật với anisette mới mỗi lần.
            trigger_url = "https://gsa.apple.com/auth/verify/trusteddevice"

            triggered = False
            for t_attempt in range(3):
                print(f"[2fa] Fetching fresh anisette for trigger (GET {trigger_url}, lần {t_attempt + 1}/3)...")
                trigger_headers = self._build_2fa_base_headers(dsid, idms_token)
                trigger_headers["Content-Type"] = "text/x-xml-plist"
                trigger_headers["Accept"] = "text/x-xml-plist"

                try:
                    trigger_resp = self._http("GET", trigger_url, headers=trigger_headers, timeout=15)
                    print(f"[2fa] Trigger response: HTTP {trigger_resp.status_code}")

                    # CHỈ 200 (đã gửi) và 412 (đã gửi gần đây, mã cũ vẫn còn dùng được)
                    # mới thực sự đồng nghĩa với "push đã tới thiết bị".
                    # HTTP 401 nghĩa là Apple TỪ CHỐI request (identity-token hoặc
                    # anisette không hợp lệ) — KHÔNG có push nào được gửi cả. Coi 401
                    # là "đã trigger" (như code cũ) sẽ làm bạn ngồi nhập mã cho một
                    # push không tồn tại, dẫn đến nhập rỗng / sai 3 lần liên tục.
                    if trigger_resp.status_code in [200, 412]:
                        triggered = True
                        break
                    elif trigger_resp.status_code == 401:
                        print("[2fa] ⚠️  HTTP 401: Apple từ chối yêu cầu trigger (identity-token / "
                              "anisette không hợp lệ với tài khoản này). Thử lại với anisette mới...")

                    time.sleep(1)
                except Exception as trigger_exc:
                    print(f"[2fa] Trigger error: {trigger_exc}")

            if not triggered:
                print()
                print("[2fa] ❌ Không thể kích hoạt push 2FA — Apple liên tục trả về HTTP 401 ở "
                      "bước trigger, nghĩa là KHÔNG có mã nào được gửi tới thiết bị của bạn.")
                print("[2fa] Nguyên nhân phổ biến nhất: Anisette server công khai (ví dụ ani.sidestore.io) "
                      "đang cấp một machine-id/anisette không được Apple chấp nhận cho đúng tài khoản này "
                      "vào đúng thời điểm đó (server công khai bị quá tải / machine-id bị Apple đánh dấu).")
                print("[2fa] Hãy thử: (1) đổi sang anisette_url khác hoặc tự host anisette server riêng "
                      "(ví dụ SideStore/anisette-v3-server), (2) chạy lại tool sau vài phút.")
                return False

            # ── BƯỚC 2: Hỏi mã từ người dùng ───────────────────────────────
            print()
            print("[2fa] ✅ Đã gửi yêu cầu lên Apple.")
            print("[2fa] 👉 Kiểm tra iPhone / Mac / iPad của bạn để nhận mã 6 số.")
            print("[2fa]    (Nếu không thấy push notification, thử mở Settings > Apple ID trên thiết bị)")
            print()

            code = None
            for attempt in range(3):
                raw = self.input_func(f"[2fa] Nhập mã xác thực 6 số (lần {attempt + 1}/3): ").strip()
                cleaned = raw.replace(" ", "").replace("-", "")
                if len(cleaned) == 6 and cleaned.isdigit():
                    code = cleaned
                    break
                print(f"[2fa] Mã không hợp lệ (nhận được: {raw!r}). Vui lòng thử lại.")

            if not code:
                print("[2fa] Đã thử 3 lần nhưng không nhận được mã hợp lệ.")
                return False

            # ── BƯỚC 3: Validate với FRESH anisette (quan trọng nhất!) ───────
            # Gọi _build_2fa_base_headers() LẠI ngay tại đây — không dùng lại
            # trigger_headers — để đảm bảo X-Apple-I-MD-M còn hiệu lực.
            #
            # *** ROOT CAUSE CỦA "nhập đúng mã vẫn lỗi 2FA" ***
            # Endpoint validate ĐÚNG theo protocol GrandSlam (GSA) — protocol mà
            # Xcode/AuthKit dùng và toàn bộ flow SRP phía trên đang implement — là:
            #     GET https://gsa.apple.com/grandslam/GsService2/validate
            # KHÔNG PHẢI https://gsa.apple.com/auth/verify/trusteddevice/securitycode.
            # Path "/auth/verify/trusteddevice/securitycode" thuộc về flow web Apple
            # ID (host idmsa.apple.com, dùng cookie + header "scnt") — một protocol
            # đăng nhập HOÀN TOÀN KHÁC, không tương thích với phiên SRP/GSA đang chạy
            # ở trên. Gọi sai endpoint này khiến Apple trả về 500 (hoặc 401) ngay cả
            # khi mã 6 số bạn nhập là chính xác.
            # Nguồn tham chiếu: TheAppleWiki "Grand Slam Authentication" (liệt kê rõ
            # "Fourth HTTP Request" là GET /grandslam/GsService2/validate với header
            # Security-Code) và implementation pypush_gsa_icloud.py (project
            # Send-My-Python) — cả hai đều validate ở cùng endpoint /grandslam/GsService2/validate.
            print("[2fa] Fetching fresh anisette for validate request...")
            validate_headers = self._build_2fa_base_headers(dsid, idms_token)
            validate_headers["Security-Code"] = code   # tên header theo Apple Wiki
            validate_headers["security-code"] = code   # giữ thêm bản lowercase cho an toàn
            # Content-Type PHẢI giữ lại — Apple Wiki liệt kê Content-Type là một trong
            # các header có mặt ở request validate thật (không nên pop bỏ như bản cũ).

            print("[2fa] Đang gửi mã xác thực tới Apple GSA (grandslam/GsService2/validate)...")
            response = self._http(
                "GET",
                "https://gsa.apple.com/grandslam/GsService2/validate",
                headers=validate_headers,
                timeout=15,
            )
            print(f"[2fa] Validate GET: HTTP {response.status_code}")

            if response.ok:
                print("[2fa] ✅ Xác thực 2FA thành công!")
                return True

            body_text = response.text if response.text else "(rỗng)"
            print(f"[2fa] ❌ Validate thất bại. Body: {body_text[:500]}")
            if response.status_code == 401:
                print("[2fa] (401 ở bước validate thường nghĩa là mã sai/đã hết hạn, hoặc "
                      "anisette dùng để validate khác với anisette lúc trigger — đã fetch "
                      "fresh anisette riêng cho bước này nên trường hợp này ít xảy ra hơn.)")
            return False

        except Exception as e:
            print(f"[2fa] Exception: {e}")
            traceback.print_exc()
            return False

    def handle_2fa_sms(self, dsid, idms_token):
        """Handle SMS 2FA (fallback khi không có trusted device).

        CÁC FIX SO VỚI BẢN CŨ:

        FIX A [ROOT CAUSE - HTTP 401 validate]:
            Giống trusted device, headers/anisette được fetch 1 lần rồi dùng cho
            cả put-send lẫn post-validate. Fresh anisette fix vấn đề này.

        FIX B [BUG - phone_id hardcoded]:
            Bản cũ hardcode phone_id=1, nếu số đầu tiên trên tài khoản không phải id=1
            thì Apple từ chối. Fix: GET /auth/verify/phone trước để lấy danh sách số
            thực tế, dùng id của số đầu tiên.

        FIX C [HTTP 401 khi gửi SMS]:
            SMS PUT 401 sau khi trusted device thất bại cho thấy session bị invalidate.
            Fresh anisette cho mỗi request giúp giảm thiểu vấn đề này.
        """
        print("[2fa-sms] Bắt đầu SMS 2FA...")

        try:
            # ── BƯỚC 1: Lấy danh sách số điện thoại thực tế ─────────────────
            print("[2fa-sms] Fetching phone list để lấy đúng phone_id...")
            list_headers = self._build_2fa_base_headers(dsid, idms_token)
            phone_id = 1           # fallback mặc định
            phone_display = "số #1"

            try:
                list_resp = self._http(
                    "GET",
                    "https://gsa.apple.com/auth/verify/phone",
                    headers=list_headers,
                    timeout=10,
                )
                print(f"[2fa-sms] Phone list: HTTP {list_resp.status_code}")

                if list_resp.ok and list_resp.text:
                    try:
                        phone_data = list_resp.json()
                        phones = phone_data.get("trustedPhoneNumbers", [])
                        if phones:
                            phone_id = phones[0].get("id", 1)
                            phone_display = phones[0].get("numberWithDialCode", f"id={phone_id}")
                            print(f"[2fa-sms] Tìm thấy {len(phones)} số. Dùng: {phone_display} (id={phone_id})")
                        else:
                            print(f"[2fa-sms] Không có trustedPhoneNumbers trong response, dùng id=1")
                            print(f"[2fa-sms] Raw response: {list_resp.text[:300]}")
                    except Exception as parse_e:
                        print(f"[2fa-sms] Không parse được phone list: {parse_e}")
                        print(f"[2fa-sms] Raw response: {list_resp.text[:300]}")
                elif not list_resp.ok:
                    print(f"[2fa-sms] Phone list thất bại ({list_resp.status_code}), dùng id=1 fallback")
            except Exception as list_e:
                print(f"[2fa-sms] Không lấy được phone list: {list_e} — dùng id=1")

            # ── BƯỚC 2: Gửi SMS với FRESH anisette ───────────────────────────
            print(f"[2fa-sms] Gửi mã OTP qua SMS tới {phone_display}...")
            sms_headers = self._build_2fa_base_headers(dsid, idms_token)
            sms_headers["Content-Type"] = "application/json"

            sms_body = {"phoneNumber": {"id": phone_id}, "mode": "sms"}
            sms_resp = self._http(
                "PUT",
                "https://gsa.apple.com/auth/verify/phone",
                json=sms_body,
                headers=sms_headers,
                timeout=10,
            )
            print(f"[2fa-sms] Gửi SMS: HTTP {sms_resp.status_code}")
            if not sms_resp.ok and sms_resp.status_code not in (200, 405):
                print(f"[2fa-sms] Cảnh báo gửi SMS: {sms_resp.text[:300]}")

            print()
            print(f"[2fa-sms] 📱 Mã OTP đã được gửi tới {phone_display}.")

            # ── BƯỚC 3: Hỏi mã OTP từ người dùng ────────────────────────────
            code = None
            for attempt in range(3):
                raw = self.input_func(f"[2fa-sms] Nhập mã OTP SMS 6 số (lần {attempt + 1}/3): ").strip()
                cleaned = raw.replace(" ", "").replace("-", "")
                if len(cleaned) == 6 and cleaned.isdigit():
                    code = cleaned
                    break
                print(f"[2fa-sms] Mã không hợp lệ (nhận được: {raw!r}). Vui lòng thử lại.")

            if not code:
                print("[2fa-sms] Đã thử 3 lần nhưng không nhận được mã hợp lệ.")
                return False

            # ── BƯỚC 4: Validate với FRESH anisette ──────────────────────────
            print("[2fa-sms] Fetching fresh anisette cho validate request...")
            val_headers = self._build_2fa_base_headers(dsid, idms_token)
            val_headers["Content-Type"] = "application/json"

            val_body = {
                "phoneNumber": {"id": phone_id},
                "mode": "sms",
                "securityCode": {"code": code}
            }

            print("[2fa-sms] Đang validate OTP với Apple...")
            val_resp = self._http(
                "POST",
                "https://gsa.apple.com/auth/verify/phone/securitycode",
                json=val_body,
                headers=val_headers,
                timeout=10,
            )
            print(f"[2fa-sms] Validate: HTTP {val_resp.status_code}")

            if val_resp.ok:
                print("[2fa-sms] ✅ Xác thực SMS 2FA thành công!")
                return True

            body_text = val_resp.text if val_resp.text else "(rỗng)"
            print(f"[2fa-sms] ❌ Thất bại. Body: {body_text[:500]}")
            return False

        except Exception as e:
            print(f"[2fa-sms] Exception: {e}")
            traceback.print_exc()
            return False

    def authenticate(self, apple_id, password, _depth=0, _gsa_retries=0):
        """Main authentication method - GSA SRP + 2FA.

        _gsa_retries: so lan thu lai ca chuoi SRP sau khi edge van tra 429
        tren socket moi. Khong xoay anisette server vi 429 nay khong phu thuoc
        vao server anisette (init da 200 voi cung header).
        """
        self._gsa_init_ok = False
        self._cpd_pin = None
        self._pin_cpd = True
        try:
            print(f"[icloud] Starting authentication for {apple_id}")

            usr = srp.User(apple_id, bytes(), hash_alg=srp.SHA256, ng_type=srp.NG_2048)
            _, A = usr.start_authentication()

            print("[srp] SRP initialized successfully")

            response = self.gsa_request({
                "A2k": A,
                "ps": ["s2k", "s2k_fo"],
                "u": apple_id,
                "o": "init"
            })
            # init da toi GrandSlam: profile anisette dang dung khong bi edge chan.
            self._gsa_init_ok = True

            if "sp" not in response:
                print(f"[icloud] Failed to authenticate: {response}")
                return None

            if response["sp"] not in ["s2k", "s2k_fo"]:
                print(f"[icloud] Unsupported protocol: {response['sp']}")
                return None

            print(f"[icloud] Using protocol: {response['sp']}")

            protocol = response["sp"]
            salt = response["s"]
            B = response["B"]
            c = response["c"]
            iterations = response["i"]

            print(f"[srp] Salt length: {len(salt)}, B length: {len(B)}, iterations: {iterations}")

            usr.p = self.encrypt_password(password, salt, iterations, protocol)
            M = usr.process_challenge(salt, B)

            if M is None:
                print("[srp] Failed to process SRP challenge")
                return None

            print("[srp] SRP challenge processed successfully")

            response = self.gsa_request({
                "c": c,
                "M1": M,
                "u": apple_id,
                "o": "complete"
            })

            status = response.get("Status", {})
            auth_type = status.get("au")

            # Xác minh M2 từ Apple
            m2_verified = False
            if "M2" in response:
                usr.verify_session(response["M2"])
                m2_verified = usr.authenticated()
                if not m2_verified:
                    print("[icloud] Cảnh báo: M2 từ Apple không khớp với session SRP cục bộ.")

            # Giải mã SPD để lấy dsid / idms token
            spd_data = {}
            if m2_verified and "spd" in response:
                try:
                    decrypted_spd = self.decrypt_cbc(usr, response["spd"])
                    spd_data = self._safe_plist_loads(decrypted_spd)
                    print(f"[debug] Giải mã SPD thành công. Các khoá có sẵn: {list(spd_data.keys())}")
                except Exception as e:
                    print(f"[debug] Không thể giải mã SPD: {e}")

            # ---- Xử lý 2FA ----
            if auth_type in ["trustedDeviceSecondaryAuth", "secondaryAuth", "smsSecondaryAuth"]:
                print(f"[2fa] Yêu cầu xác thực 2FA: {auth_type}")

                headers_dict = {k.lower(): v for k, v in response.get("_headers", {}).items()}

                dsid = (
                    spd_data.get("adsid")
                    or spd_data.get("dsid")
                    or spd_data.get("DsPrsID")
                    or status.get("dsid")
                    or headers_dict.get("x-apple-dsid")
                )
                idms_token = (
                    spd_data.get("GsIdmsToken")
                    or spd_data.get("idmsToken")
                    or status.get("idmsToken")
                )

                if not dsid or not idms_token:
                    print(f"[2fa] Thiếu dsid hoặc idms_token để kích hoạt 2FA "
                          f"(dsid={dsid}, idms_token={'có' if idms_token else None}).")
                    print(f"[debug] Status: {status}")
                    if spd_data:
                        print(f"[debug] Các khoá trong SPD: {list(spd_data.keys())}")
                    return None

                # Thử trusted device trước, SMS làm fallback
                two_fa_ok = False
                if auth_type in ["trustedDeviceSecondaryAuth", "secondaryAuth"]:
                    two_fa_ok = self.handle_2fa_trusted_device(dsid, idms_token)
                    if not two_fa_ok:
                        print("[2fa] Trusted device thất bại. Thử SMS fallback...")
                        two_fa_ok = self.handle_2fa_sms(dsid, idms_token)
                else:
                    # smsSecondaryAuth
                    two_fa_ok = self.handle_2fa_sms(dsid, idms_token)
                    if not two_fa_ok:
                        print("[2fa] SMS thất bại. Thử trusted device...")
                        two_fa_ok = self.handle_2fa_trusted_device(dsid, idms_token)

                if two_fa_ok:
                    if _depth >= 1:
                        print("[2fa] 2FA đã hoàn tất nhưng phiên chưa được cấp đầy đủ. Hãy chạy lại.")
                        return {
                            "user_id": apple_id,
                            "authenticated": "2fa_completed",
                            "dsid": dsid,
                            "message": "2FA completed, re-run tool to get full session"
                        }
                    print("[icloud] 2FA thành công! Đang xác thực lại để lấy token phiên đầy đủ...")
                    return self.authenticate(apple_id, password, _depth=_depth + 1)
                else:
                    return None

            # ---- Không cần 2FA ----
            if not m2_verified:
                print(f"[icloud] Authentication failed: {response}")
                return None

            print("[icloud] Authentication successful!")

            dsid = (
                spd_data.get("adsid")
                or spd_data.get("dsid")
                or response.get("dsid")
                or status.get("dsid")
            )

            if not dsid and "_headers" in response:
                h = {k.lower(): v for k, v in response["_headers"].items()}
                dsid = h.get("x-apple-dsid") or h.get("dsid")

            if not dsid:
                print(f"[debug] Toàn bộ keys trong response: {list(response.keys())}")
                print(f"[debug] Status content: {status}")
                if spd_data:
                    print(f"[debug] Các khoá trong SPD: {list(spd_data.keys())}")
                dsid_input = self.input_func("\n[!] Không thể lấy DSID tự động. Nhập DSID (hoặc Enter để bỏ qua): ").strip()
                if dsid_input:
                    dsid = dsid_input

            # Đổi GsIdmsToken -> app token thực sự cho Developer API
            app_session_token = None
            apptoken_adsid = spd_data.get("adsid") or dsid
            apptoken_c = spd_data.get("c")
            apptoken_sk = spd_data.get("sk")
            apptoken_idms = spd_data.get("GsIdmsToken")
            if apptoken_adsid and apptoken_c and apptoken_sk and apptoken_idms:
                app_session_token = self.fetch_app_token(
                    apptoken_adsid, apptoken_c, apptoken_idms, apptoken_sk
                )
            else:
                print("[apptoken] Thiếu một số trường trong SPD, dùng GsIdmsToken tạm thời.")

            return {
                "user_id": apple_id,
                "authenticated": True,
                "dsid": dsid,
                "session_token": (
                    app_session_token
                    or spd_data.get("GsIdmsToken")
                    or response.get("sessionToken")
                    or status.get("idmsToken")
                ),
                "m2": response.get("M2"),
                "srp_user": usr
            }

        except GsaRateLimitedError as e:
            return self._handle_gsa_429(apple_id, password, _depth, _gsa_retries, e)
        except Exception as e:
            print(f"[icloud] Authentication error: {e}")
            return None
        finally:
            self._pin_cpd = False
            self._cpd_pin = None

    def _handle_gsa_429(self, apple_id, password, _depth, _gsa_retries, exc):
        """429 con lai sau khi gsa_request da thu 3 socket moi.

        Khong xoay anisette. Log: private server tra header, init 200 (co salt/B),
        chi complete bi 429 — dung pattern keep-alive. Xoay sang server SideStore
        cong cong bo mot profile dang song va dot them ngan sach 429 theo IP.
        """
        pinned = getattr(self, "_anisette_pinned", False)
        init_ok = getattr(self, "_gsa_init_ok", False)

        wait = 0
        try:
            wait = int(str(exc).strip())
        except (TypeError, ValueError):
            pass

        if _gsa_retries < 1:
            print("[icloud] ⚠️  Apple vẫn trả 429 sau kết nối mới. "
                  "Giữ nguyên anisette, thử lại SRP một lần...")
            if wait > 0:
                print(f"[icloud] Chờ {min(wait, 30)}s theo Retry-After...")
                time.sleep(min(wait, 30))
            else:
                time.sleep(1)
            return self.authenticate(apple_id, password, _depth=_depth,
                                     _gsa_retries=_gsa_retries + 1)

        print("[icloud] ❌ Apple edge vẫn trả 429 sau khi mỗi request đã đi socket riêng.")
        if pinned:
            print(f"[icloud] Giữ server private {self.anisette_url} — không xoay sang server công cộng.")
        elif init_ok:
            print("[icloud] init đã được GrandSlam nhận (200), nên anisette không bị chặn.")
        print("[icloud] Đây là filter edge theo socket/IP, không phải server anisette hỏng.")
        print("[icloud] Đợi 2-3 phút rồi chạy lại. Xoá .anisette_cache.json nếu tool bỏ server private.")
        return None
