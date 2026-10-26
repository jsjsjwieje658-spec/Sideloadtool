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
  3. [v72 — 2026-10-20] Apple GrandSlam (từ 12/10/2026) trả ec=-80045 ở bước
     `init` nếu machine-id anisette do Android CoreADI sinh ra nhưng request
     khai danh tính macOS/iPhone; và trả 503 HTML cho mọi client khai Android
     (từ 14/10/2026). Nguyên nhân trong code cũ: tự bịa danh tính Mac rồi ghi
     đè lên danh tính thật của server. Nay danh tính được lấy nguyên văn từ
     server (/v3/client_info) và KIỂM CHỨNG bằng một request init vô hại, chỉ
     dùng server vượt kiểm chứng. Xem khối "ANISETTE + DANH TÍNH CLIENT".
"""

import uuid
import json
import base64
import time
import secrets
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


class AnisetteIdentityRejected(Exception):
    """GSA trả -80045/-80009/-80010/-29004 ở bước `init`.

    Nghĩa là machine-id (X-Apple-I-MD-M) do server anisette sinh ra không
    khớp danh tính client khai báo, hoặc Apple đã chặn hẳn server đó. Mật
    khẩu CHƯA được gửi nên đây không phải lỗi tài khoản — phải đổi server.
    """
    pass


class AnisetteServerUnavailable(Exception):
    """Server anisette khong song / tra du lieu sai.

    Khac voi AnisetteIdentityRejected (Apple chap nhan request nhung tu choi
    machine-id), day la loi TANG SERVER. Phai doi server NGAY, khong duoc
    coi nhu loi dang nhap va dung ca chuoi (dung la bug nguoi dung bao:
    "server dau loi la dung luon, khong chuyen sang server duoi").
    """
    pass


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
GSA_URL = "https://gsa.apple.com/grandslam/GsService2"

# Server "seed" được thử TRƯỚC danh sách SideStore. Đây chỉ là thứ tự ưu tiên
# (thử nhanh hơn), KHÔNG phải danh sách tin cậy: mỗi server vẫn phải vượt
# _identity_accepted() thì mới được dùng — Apple có thể chặn bất kỳ lúc nào.
# Cập nhật 2026-10-20: chỉ mac-ani.jkcoxson.com còn được GSA chấp nhận.
# v73 (2026-10-26): do TRUC TIEP cac server cong khai. Ket qua:
#   mac-ani / ani.jkcoxson / ani3.stikstore / new.ani / ani2 / ani.sidestore = HTTP 200
#   win.ani.sidestore.io                                                  = HTTP 502 (chet)
# Nguon danh sach Mac-ADI: iPASide 1.2.9 (2026-10-23) va iloader 2.4.0.
# Day chi la THU TU UU TIEN; moi server van duoc kiem chung lai luc chay.
ANISETTE_SEED_SERVERS = (
    "https://mac-ani.jkcoxson.com",
    "https://ani.jkcoxson.com",
    "https://ani3.stikstore.app",
    "https://new.ani.sidestore.io",
    "https://ani2.stikstore.app",
    "https://ani.sidestore.io",
)

# Tu 22/10/2026 Apple tra -80046 o buoc `apptokens` neu request khai danh
# tinh khong phai Mac (Android/Windows) du SRP da thanh cong. Cach sua cua
# Apparition 0.3.0 / iPASide 1.2.9: gui request apptokens (va cac request
# developer services sau do) voi MOT danh tinh Mac nhat quan.
# Nguon: https://blog.illixion.com/2026/10/sideloading-error-80046/
#        https://github.com/pwnapplehat/iPASide/issues/9
MAC_TOKEN_IDENTITIES = (
    "<Mac15,7> <macOS;27.0;26A5378j> <com.apple.AuthKit/1 (com.apple.akd/1.0)>",
    "<MacBookPro18,3> <macOS;26.6;25F84> <com.apple.AuthKit/1 (com.apple.akd/1.0)>",
    "<iMacPro1,1> <macOS;26.6;25F84> <com.apple.AuthKit/1 (com.apple.akd/1.0)>",
)
MAC_TOKEN_USER_AGENTS = (
    "akd/1.0 CFNetwork/808.1.4",
    "AuthKit/1 (Macintosh; OS X 26.6) (com.apple.akd/1.0)",
    "akd/1.0 CFNetwork/811.5.4 Darwin/16.7.0",
)
APP_TOKEN_REJECT_EC = {-80046, -45061}

# Mã lỗi Apple dùng để nói "machine-id này không do client đang khai báo sinh ra"
# (hoặc thiếu/hỏng OTP) — danh tính bị từ chối, phải đổi server.
#   -80045 hành vi không hợp lệ / mismatch nguồn gốc machine-id
#   -80009 MID không hợp lệ      -80010 thiếu OTP
#   -29004 machine-id hỏng       -80044 lệch giữa header và cpd
IDENTITY_REJECT_EC = {-80045, -80009, -80010, -29004, -80044}
IDENTITY_REJECT_HTTP = {502, 503, 504}

# Số server tối đa được dò trong một lần chọn (tránh treo lâu khi mạng chậm).
ANISETTE_MAX_CANDIDATES = 24     # v73: duyet toan bo danh sach, khong bo server o duoi

DEFAULT_USER_AGENT = "akd/1.0 CFNetwork/1408.0.4 Darwin/22.5.0"

# GSA probe toi da 6 lan trong mot luot chon server. Neu Apple edge dang 429/503
# thi van phai chon duoc 1 server con song (khong duoc dung ca chuoi).
ANISETTE_MAX_GSA_PROBES = 6


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
ANISETTE_CACHE_TTL = 30 * 60  # v73: 30 phut (truoc 6h — server chet van duoc tin dung ca ngay)


def fetch_official_servers():
    """Danh sách server Anisette công khai của SideStore (list[dict]) — [] nếu lỗi.
    Dùng cho màn Cài đặt (sideload_core.list_anisette_servers)."""
    try:
        resp = requests.get(OFFICIAL_SERVERS_URL, timeout=10, verify=True)
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
        self.user_id = str(uuid.uuid4()).upper()
        self.device_id = str(uuid.uuid4()).upper()
        self.session = requests.Session()
        # v73: BAT LAI xac thuc TLS. Do truc tiep 2026-10-26: toan bo server
        # anisette cong khai + gsa.apple.com deu co cert hop le (curl ssl=0),
        # nen verify=False truoc day chi tao them nguy co MITM mat khau Apple ID
        # ma chang duoc loi gi. Server local http:// tu ky van duoc phep.
        self.session.verify = True
        # Danh tinh dung cho `apptokens` + developer services (xem -80046).
        self.developer_client_info = None
        self.developer_user_agent = None
        self.token_identity = None
        self.token_user_agent = None
        self._token_identity_next = False
        self._token_identity_index = 0
        self._server_switches = 0

        # Danh tính mặc định CHỈ dùng khi chưa lấy được từ server (chưa từng
        # được gửi tới Apple nếu selection thành công — xem _set_identity).
        self.user_agent = DEFAULT_USER_AGENT
        self.xcode_ua = DEFAULT_USER_AGENT
        self.client_info = ""
        self._identity_variant = "raw"
        self._identity_user_agent = None
        self._identity_retries = 0
        # Lý do thất bại gần nhất, dạng người dùng đọc được. UI (PythonBridge
        # → sideload_core.get_last_error) hiển thị nguyên nhân thật thay vì
        # câu chung "xem nhật ký".
        self.last_error = ""

        if self._anisette_pinned:
            self.anisette_url = str(anisette_url).strip().rstrip("/")
            print(f"[anisette] Server chỉ định: {self.anisette_url} (vẫn tự đổi nếu server chết)")
            ident = self._fetch_identity(self.anisette_url)
            if not ident:
                # v73: server ghim chet ngay luc khoi tao -> chon server song
                # khac ngay, thay vi doi toi luc generate_cpd() roi vang exception.
                print(f"[anisette] ⚠️  Server chỉ định {self.anisette_url} không phản hồi — tự chọn server khác.")
                self._failed_servers.add(self.anisette_url)
                alt = self._pick_live_server(exclude=set(self._failed_servers))
                if alt:
                    self._apply_server_identity(alt)
                    ident = alt[1]
            if ident and ident.get("client_info"):
                # Server do người dùng tự chọn: KHÔNG kiểm chứng (tôn trọng lựa
                # chọn), nhưng nếu danh tính còn token Xcode thì tự dùng biến
                # thể akd — token Xcode bị edge trả 503 từ 09/2026.
                variant = ("akd" if "com.apple.dt.Xcode" in ident["client_info"]
                           else "raw")
                if variant == "akd":
                    print("[anisette] ⚠️  Server trả danh tính còn token "
                          "com.apple.dt.Xcode (edge chặn từ 09/2026) — dùng akd.")
                self._set_identity(self.anisette_url, ident["client_info"],
                                   ident.get("user_agent"), variant)
                print(f"[anisette] Danh tính client: {self.client_info}")
                print(f"[anisette] User-Agent       : {self.user_agent}")
            else:
                print("[anisette] ⚠️  Không lấy được danh tính client từ server này "
                      "(có thể vẫn chạy, sẽ thử lại khi lấy headers).")
        else:
            self.anisette_url = self.get_best_anisette_server()
        if not self.client_info:
            # Không có danh tính server: dùng chuỗi akd, nhưng vẫn giữ UA akd
            # (KHÔNG dùng token com.apple.dt.Xcode — edge trả 503 từ 09/2026).
            self.client_info = ("<MacBookPro18,3> <macOS;13.4.1;22F8> "
                                "<com.apple.AuthKit/1 (com.apple.akd/1.0)>")

        # Danh tinh Mac cho apptokens: neu server da tra danh tinh Mac thi dung
        # lai chinh no (khop machine-id), neu khong thi dung Mac fallback.
        self._refresh_token_identity()

        # Configure SRP properly
        srp.rfc5054_enable()
        srp.no_username_in_x()

    # ══════════════════════════════════════════════════════════════════════════
    # ANISETTE + DANH TÍNH CLIENT (v72 — 2026-10-20)
    #
    # SỰ CỐ THẬT (đã kiểm chứng trực tiếp với gsa.apple.com, 2026-10-20):
    #   Từ 2026-10-12 GrandSlam so *nguồn gốc* của machine-id (X-Apple-I-MD-M)
    #   với *danh tính client khai báo* (X-MMe-Client-Info). Machine-id do
    #   Android CoreADI / anisette-v3 sinh ra mà request lại khai là Mac →
    #   HTTP 200 + Status.ec = -80045 "This action cannot be completed at this
    #   time." ngay ở bước `o=init`, TRƯỚC khi kiểm tra mật khẩu.
    #   Từ 2026-10-14 ~20:50 UTC: mọi request khai client Android bị edge trả
    #   503 HTML. Client-info chứa com.apple.dt.Xcode bị 503 từ 09/2026.
    #
    # LỖI CỦA BẢN CŨ (chính là nguyên nhân log -80045):
    #   Code tự BỊA danh tính "<MacBookPro18,3> <macOS;13.4.1;22F8> ..." rồi
    #   ghi đè lên X-MMe-Client-Info mà server anisette trả về. Server trả
    #   machine-id Android nhưng request khai Mac → mismatch → -80045.
    #   Ngoài ra khi xoay server, client_info cũ vẫn được giữ lại (thêm một
    #   nguồn mismatch nữa), và chỉ "probe" server bằng HTTP 200 chứ không
    #   kiểm tra Apple có chấp nhận machine-id đó hay không.
    #
    # CÁCH SỬA (không phụ thuộc tên server, tự thích ứng khi Apple đổi tiếp):
    #   1. Lấy danh tính THẬT từ server: GET /v3/client_info →
    #      {client_info, user_agent}. Dùng ĐÚNG chuỗi đó cho cả header HTTP
    #      lẫn trường "cpd" trong body, và dùng đúng user_agent của server.
    #   2. Kiểm chứng danh tính bằng một lần `o=init` với Apple ID NGẪU NHIÊN
    #      KHÔNG TỒN TẠI (chỉ init, không gửi mật khẩu, không đụng tài khoản
    #      người dùng). Apple trả ec = -80045/-80009/-80010/-29004 (hoặc 503)
    #      = danh tính bị từ chối; trả -20970/-22421/0/challenge = danh tính
    #      ĐƯỢC CHẤP NHẬN (lỗi còn lại chỉ là "tài khoản không tồn tại").
    #   3. Chỉ chọn server vượt bước 2, cache 6 giờ kèm client_info/user_agent.
    #   4. Khi GSA trả -80045 lúc đăng nhập thật → tự động bỏ server đó, chọn
    #      server đã kiểm chứng khác và chạy lại SRP một lần (provider chết
    #      giữa phiên vẫn tự phục hồi).
    #   5. hsc 434 → Apple yêu cầu đồng bộ lại anisette: lấy OTP mới, thử lại.
    #
    # Nguồn: Dadoum/anisette-v3-server#61, rileytestut/AltSign#61,
    #        altstoreio/AltStore#1834, xtool-org/xtool#337, nab138/iloader#859.
    # ══════════════════════════════════════════════════════════════════════════

    # ── v73: tach biet "server con song" va "Apple chap nhan danh tinh" ──────
    @staticmethod
    def _tls_verify(addr):
        """Chi bo qua xac thuc TLS cho server local do nguoi dung tu host."""
        a = (addr or "").lower()
        return not (a.startswith("http://") or "localhost" in a or "127.0.0.1" in a)

    @staticmethod
    def _identity_score(client_info):
        """Diem uu tien khi KHONG kiem chung duoc voi Apple (Apple edge 429/503).

        0 = Mac-ADI (dang duoc Apple chap nhan tu 22/10/2026)
        1 = Windows/PC iTunes identity    2 = Android (503 tu 14/10/2026)
        3 = con token com.apple.dt.Xcode (503 tu 09/2026)
        """
        ci = client_info or ""
        if "com.apple.dt.Xcode" in ci:
            return 3
        if "<PC>" in ci or "Windows" in ci:
            return 1
        if "<Mac" in ci or "Mac OS X" in ci or "macOS;" in ci:
            return 0
        if "<" in ci and ("Android" in ci or "<Pixel" in ci or "<PC" in ci):
            return 2
        return 2

    def _refresh_token_identity(self):
        """Dat danh tinh Mac cho `apptokens` va developer services (-80046).

        Lan 0: neu server da tra danh tinh Mac thi dung lai chinh no (khop
        machine-id). Lan 1/2 (retry sau -80046): bat buoc doi sang Mac khac,
        neu khong se gui lai dung request vua bi tu choi.
        """
        ci = self._apply_identity_variant(self.client_info)
        retry = getattr(self, "_token_identity_index", 0)
        server_is_mac = (self._identity_score(ci) == 0)
        if retry == 0 and server_is_mac:
            self.token_identity = ci
            self.token_user_agent = self.user_agent
        else:
            # Server Mac: retry 0 = danh tinh server, 1 = fallback[0], 2 = fallback[1].
            # Server khong phai Mac: retry 0 = fallback[0], 1 = [1], 2 = [2].
            # MOI lan retry phai KHAC danh tinh lan truoc, neu khong Apple lai
            # tra -80046 y het (da bi test_apple_oct2026 bat duoc).
            base = 1 if server_is_mac else 0
            idx = min(max(retry - base, 0), len(MAC_TOKEN_IDENTITIES) - 1)
            self.token_identity = MAC_TOKEN_IDENTITIES[idx]
            self.token_user_agent = MAC_TOKEN_USER_AGENTS[idx]
        # Apple "tie" developer token vao danh tinh ma no cap: moi request
        # developerservices2 phai dung DUNG danh tinh nay, neu khong se bao
        # "session has expired" (iPASide 1.2.8 / AltSign #61).
        self.developer_client_info = self.token_identity
        self.developer_user_agent = self.token_user_agent

    def _pick_live_server(self, exclude=None):
        """Tim server CON SONG (chi GET, khong can Apple). Tra (url, ident) hoac None.

        Day la diem mau chot cua loi "server dau chet la dung luon": viec chon
        server KHONG BAO GIO duoc phu thuoc vao viec gsa.apple.com co tra loi
        hay khong. Neu Apple dang 429/503, ta van chon duoc server tot nhat.
        """
        exclude = set(exclude or ())
        for addr in self._candidate_servers()[:ANISETTE_MAX_CANDIDATES]:
            if addr in exclude or addr in self._failed_servers:
                continue
            try:
                ident = self._fetch_identity(addr, timeout=5)
            except Exception:
                ident = None
            if not ident:
                self._failed_servers.add(addr)
                print(f"[anisette]   ✗ {addr} — server không phản hồi / sai dữ liệu")
                continue
            return addr, ident
        return None

    def _apply_server_identity(self, picked):
        """Ap dung server moi + danh tinh cua chinh server do (khong bi giu lai ci cu)."""
        addr, ident = picked
        ci = ident.get("client_info") or ""
        variant = "akd" if "com.apple.dt.Xcode" in ci else "raw"
        self._set_identity(addr, ci, ident.get("user_agent"), variant)
        self._server_switches += 1
        print(f"[anisette] 🔄 Đổi sang server #{self._server_switches}: {addr}")
        print(f"[anisette]    Danh tính mới: {self.client_info}")
        return addr

    def get_best_anisette_server(self):
        """Chọn server Anisette có danh tính được Apple CHẤP NHẬN (đã kiểm chứng).

        Trả về URL server; đồng thời đặt self._identity / self.client_info /
        self.user_agent khớp với machine-id của server đó.
        """
        return self._select_anisette()

    @staticmethod
    def _candidate_servers():
        """Danh sách URL ứng viên: seed đã biết tốt trước, rồi servers.json."""
        seen = []
        for url in ANISETTE_SEED_SERVERS:
            u = (url or "").rstrip("/")
            if u and u not in seen:
                seen.append(u)
        for s in AppleAuth._fetch_server_list():
            u = (s.get("address") or "").rstrip("/")
            if u and u not in seen:
                seen.append(u)
        if ANISETTE_URL.rstrip("/") not in seen:
            seen.append(ANISETTE_URL.rstrip("/"))
        return seen

    def _select_anisette(self, ignore_cache=False):
        """v73: chon server theo 2 lop DOC LAP.

        Lop 1 (bat buoc): server con song va tra headers anisette hop le.
        Lop 2 (tot co thi co): Apple chap nhan danh tinh cua server do.

        LOI CU: chi can mot server dau tien chet, hoac GSA probe khong tra loi,
        thi _select_anisette roi vao nhanh "khong co server nao hoat dong" va
        tra ve ANISETTE_URL co dinh -> tool DUNG LUON, khong thu cac server o
        duoi. Bay gio lop 1 luon duoc quet toan bo danh sach (toi 24 server),
        va that bai cua lop 2 KHONG BAO GIO xoa ket qua cua lop 1.
        """
        if not ignore_cache:
            cached = self._load_anisette_cache()
            if cached and cached.get("url") and cached["url"] not in self._failed_servers:
                # v73: cache phai duoc kiem chung lai con song. TTL 6h cua ban
                # cu khien server da 502 van duoc tin dung ca ngay.
                try:
                    live = self._fetch_identity(cached["url"], timeout=5)
                except Exception:
                    live = None
                if live and cached.get("client_info"):
                    ci = live.get("client_info") or cached["client_info"]
                    self._set_identity(cached["url"], ci,
                                       live.get("user_agent") or cached.get("user_agent"),
                                       cached.get("variant", "raw"))
                    print(f"[anisette] Dùng server đã kiểm chứng (cache, còn sống): {cached['url']}")
                    return cached["url"]
                print("[anisette] ⚠️  Server trong cache đã chết / đổi danh tính — dò lại.")
                self._failed_servers.add(cached["url"])
                self._invalidate_anisette_cache()

        print("[anisette] Dò server Anisette: (1) còn sống? (2) Apple có chấp nhận danh tính?")
        candidates = self._candidate_servers()[:ANISETTE_MAX_CANDIDATES]
        print(f"[anisette] Có {len(candidates)} server ứng viên.")

        # ── Lop 1: quet TAT CA, khong break sau 3 server chet ──────────────
        live_list = []
        for addr in candidates:
            if addr in self._failed_servers:
                continue
            try:
                ident = self._fetch_identity(addr, timeout=6)
            except Exception:
                ident = None
            if not ident:
                self._failed_servers.add(addr)
                print(f"[anisette]   ✗ {addr} — server không phản hồi / headers hỏng")
                continue
            live_list.append((addr, ident))
            print(f"[anisette]   ✓ {addr} — sống (điểm danh tính {self._identity_score(ident.get('client_info'))})")

        if not live_list:
            # Khong con gi de thu: bao loi RO RANG thay vi am tham tra ve 1 URL chet.
            print("[anisette] ❌ Không còn server anisette nào sống trong danh sách.")
            self.last_error = ("Không kết nối được server Anisette nào (mạng/VPN hoặc toàn bộ "
                               "server công khai đang chết). Kiểm tra mạng rồi thử lại.")
            return ANISETTE_URL

        # ── Lop 2: hoi Apple (co ngan sach, co the that bai hoan toan) ─────
        gsa_probes = 0
        probe_unknown = set()
        for addr, ident in live_list:
            if gsa_probes >= ANISETTE_MAX_GSA_PROBES:
                print("[anisette] ⚠️  Đã đạt giới hạn probe GSA — dùng server sống tốt nhất.")
                break
            gsa_probes += 1
            try:
                ci_ok, variant, detail = self._identity_accepted(addr, ident)
            except Exception as e:
                print(f"[anisette]   ? {addr} — không kiểm chứng được với Apple: {e}")
                probe_unknown.add(addr)
                continue
            if ci_ok:
                self._set_identity(addr, ci_ok, ident.get("user_agent"), variant)
                self._save_anisette_cache(addr, ci_ok, ident.get("user_agent"), variant)
                print(f"[anisette] ✅ Chọn server: {addr} — danh tính được GSA chấp nhận ({detail})")
                return addr
            if ("không kiểm chứng" in str(detail) or "429" in str(detail)
                    or "lỗi mạng" in str(detail) or detail == "unknown"
                    or str(detail).startswith("HTTP 5")):
                print("[anisette] ⚠️  Apple edge không trả lời — bỏ qua kiểm chứng, "
                      "chọn server sống theo danh tính Mac-ADI.")
                probe_unknown.add(addr)
                break
            print(f"[anisette]   ✗ {addr} — Apple từ chối danh tính ({detail})")

        # Apple khong tra loi (429/503/mat mang) -> van chon 1 server SONG.
        # Uu tien server ma Apple CHUA kip tu choi (probe "unknown") hon la
        # server da bi tra -80045 ro rang.
        ordered = ([it for it in live_list if it[0] in probe_unknown] + live_list)
        ordered.sort(key=lambda it: self._identity_score(it[1].get("client_info")))
        addr, ident = ordered[0]
        ci = ident.get("client_info") or ""
        self._set_identity(addr, ci, ident.get("user_agent"),
                           "akd" if "com.apple.dt.Xcode" in ci else "raw")
        # KHONG luu cache "da kiem chung" khi chua kiem chung duoc.
        print(f"[anisette] ⚠️  Chưa kiểm chứng được với Apple — tạm dùng server sống: {addr}")
        return addr

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
    def _fetch_identity(addr, timeout=8):
        """Lấy danh tính THẬT + headers của một server anisette.

        v73: (1) TLS verify ON cho server cong khai; (2) chap nhan ca
        X-Apple-I-MD-M viet thuong/khac hoa; (3) 429/5xx bi coi la chet de
        co the failover; (4) tra None ngay neu headers khong hop le thay vi
        de generate_cpd() nhan dict thieu OTP roi bi Apple tra -80010.
        """
        verify = AppleAuth._tls_verify(addr)
        client_info, user_agent = None, None
        try:
            r = requests.get(addr + "/v3/client_info", timeout=timeout, verify=verify)
            if r.ok:
                j = r.json()
                client_info = j.get("client_info") or j.get("clientInfo")
                user_agent = j.get("user_agent") or j.get("userAgent")
        except Exception:
            pass

        headers = None
        for path in ("/", "/v3/get_headers"):
            try:
                r = requests.get(addr + path, timeout=timeout, verify=verify)
                if not r.ok or r.status_code in (429, 502, 503, 504):
                    continue
                j = r.json()
                if isinstance(j, dict) and any(
                        k in j for k in ("X-Apple-I-MD-M", "x-apple-i-md-m", "X-Apple-I-MD")):
                    headers = j
                    break
            except Exception:
                continue
        if not headers:
            return None

        # Chuan hoa key: mot so server tra X-Apple-I-MD-M, so khac chi co
        # X-Apple-I-MD (local provision). Can ca hai de GSA khong tu choi.
        if "X-Apple-I-MD-M" not in headers:
            for k in list(headers.keys()):
                if k.lower() == "x-apple-i-md-m":
                    headers["X-Apple-I-MD-M"] = headers[k]
                    break
        if not client_info:
            client_info = (headers.get("X-MMe-Client-Info")
                           or headers.get("X-Mme-Client-Info")
                           or headers.get("X-MMe-CLIENT-INFO"))
        if not user_agent:
            user_agent = headers.get("User-Agent") or DEFAULT_USER_AGENT
        return {"headers": headers, "client_info": client_info,
                "user_agent": user_agent}

    @staticmethod
    def _identity_variants(client_info):
        """Các biến thể danh tính có thể gửi cho Apple.

        (raw, "raw")  — đúng nguyên văn server trả về (ưu tiên: khớp machine-id)
        (akd, "akd")  — chỉ khi chuỗi gốc còn token com.apple.dt.Xcode, vốn bị
                        edge trả 503 từ 09/2026 → thay bằng com.apple.akd như
                        bản vá cũ. Đây chỉ là phương án dự phòng, KHÔNG phải
                        mặc định, vì sửa danh tính là tạo mismatch mới.
        """
        if not client_info:
            return []
        out = [(client_info, "raw")]
        if "com.apple.dt.Xcode" in client_info:
            out.append((client_info.replace("com.apple.dt.Xcode", "com.apple.akd"), "akd"))
        return out

    def _identity_accepted(self, addr, ident):
        """Hỏi Apple xem machine-id của `addr` có được chấp nhận không.

        Dùng một Apple ID NGẪU NHIÊN KHÔNG TỒN TẠI và chỉ gửi `o=init` (không
        mật khẩu, không đụng tài khoản người dùng) — như vậy kiểm chứng được
        đúng điều Apple đang kiểm: nguồn gốc machine-id so với danh tính khai
        báo. Trả về (client_info_nên_dùng, variant, detail).
        """
        user = "probe%s@icloud.com" % secrets.token_hex(5)
        last_detail = "unknown"
        for client_info, variant in self._identity_variants(ident.get("client_info")):
            try:
                res = self._gsa_init_probe(user, ident["headers"], client_info,
                                           ident.get("user_agent"))
            except Exception as e:
                last_detail = f"lỗi mạng: {e}"
                continue
            http = res.get("http")
            ec = res.get("ec")
            if http in IDENTITY_REJECT_HTTP:
                last_detail = f"HTTP {http}"
                continue
            if ec in IDENTITY_REJECT_EC:
                last_detail = f"ec {ec}"
                continue
            if http == 429:
                last_detail = "unknown"
                continue
            if ec is None:
                last_detail = f"HTTP {http} không phải plist"
                continue
            detail = "ec 0 + SRP challenge" if res.get("has_challenge") else f"ec {ec}"
            return client_info, variant, detail
        return None, None, last_detail

    def _gsa_init_probe(self, user, anisette_headers, client_info, user_agent):
        """Một request GsService2 `o=init` duy nhất (dùng cho kiểm chứng)."""
        cpd = self._build_cpd(anisette_headers, client_info, variant="raw")
        body = {
            "Header": {"Version": "1.0.1"},
            "Request": {"cpd": cpd, "A2k": secrets.token_bytes(384),
                        "ps": ["s2k", "s2k_fo"], "u": user, "o": "init"},
        }
        headers = {
            "Content-Type": "text/x-xml-plist",
            "Accept": "text/x-xml-plist",
            "User-Agent": user_agent or DEFAULT_USER_AGENT,
            "X-MMe-Client-Info": client_info or "",
            "X-Apple-App-Info": "com.apple.gs.xcode.auth",
            "X-Xcode-Version": "14.2 (14C18)",
            "Connection": "close",
        }
        r = requests.post(GSA_URL, data=plist.dumps(body), headers=headers,
                          timeout=20, verify=True)
        out = {"http": r.status_code, "ec": None, "has_challenge": False}
        try:
            res = plist.loads(r.content).get("Response", {})
            out["ec"] = (res.get("Status") or {}).get("ec", 0)
            out["has_challenge"] = "sp" in res
        except Exception:
            pass
        return out

    # ── Cache ────────────────────────────────────────────────────────────────
    @staticmethod
    def _load_anisette_cache():
        try:
            if not os.path.exists(ANISETTE_CACHE_FILE):
                return None
            with open(ANISETTE_CACHE_FILE) as f:
                data = json.load(f)
            if data.get("url") and time.time() - data.get("ts", 0) < ANISETTE_CACHE_TTL:
                return data
        except Exception:
            pass
        return None

    @staticmethod
    def _save_anisette_cache(url, client_info, user_agent, variant="raw"):
        try:
            with open(ANISETTE_CACHE_FILE, "w") as f:
                json.dump({"url": url, "client_info": client_info,
                           "user_agent": user_agent, "variant": variant,
                           "ts": time.time()}, f)
        except Exception:
            pass

    @staticmethod
    def _invalidate_anisette_cache():
        try:
            if os.path.exists(ANISETTE_CACHE_FILE):
                os.remove(ANISETTE_CACHE_FILE)
        except Exception:
            pass

    # ── Danh tính đang dùng ─────────────────────────────────────────────────
    def _set_identity(self, url, client_info, user_agent=None, variant="raw"):
        """Đặt danh tính client khớp ĐÚNG server đang dùng (không bịa)."""
        self.anisette_url = url.rstrip("/")
        if client_info:
            if variant == "akd":
                client_info = client_info.replace("com.apple.dt.Xcode", "com.apple.akd")
            self.client_info = client_info
        self.user_agent = user_agent or self.user_agent or DEFAULT_USER_AGENT
        self._identity_user_agent = self.user_agent
        self._identity_variant = variant
        # Danh tinh apptokens/developer phai doi theo server moi.
        if hasattr(self, "developer_client_info"):
            self._token_identity_index = 0
            self._refresh_token_identity()

    def _fetch_headers_once(self, addr):
        """MOT lan GET headers tu server. Moi loi deu la AnisetteServerUnavailable."""
        verify = self._tls_verify(addr)
        response = self.session.get(addr, timeout=10, verify=verify)
        if response.status_code in (429, 502, 503, 504):
            raise AnisetteServerUnavailable(f"HTTP {response.status_code}")
        response.raise_for_status()
        try:
            data = response.json()
        except Exception as e:
            raise AnisetteServerUnavailable(f"JSON hỏng: {e}")
        if not isinstance(data, dict):
            raise AnisetteServerUnavailable("response không phải object")
        if not any(k in data for k in ("X-Apple-I-MD-M", "x-apple-i-md-m", "X-Apple-I-MD")):
            raise AnisetteServerUnavailable("thiếu X-Apple-I-MD-M (server trả sai định dạng)")
        if "X-Mme-Device-Id" in data:
            self.device_id = data["X-Mme-Device-Id"]
        server_ci = data.get("X-MMe-Client-Info") or data.get("X-Mme-Client-Info")
        if server_ci:
            self._set_identity(addr, server_ci,
                               getattr(self, "_identity_user_agent", None) or self.user_agent,
                               getattr(self, "_identity_variant", "raw"))
        return data

    def generate_anisette_headers(self):
        """Lấy headers anisette — TỰ ĐỔI SANG SERVER KHÁC khi server hiện tại lỗi.

        v73 (root cause bug #1): ban cu chi xoay server o lan thu 2 cua 3 lan
        thu tren CUNG MOT URL, va _select_anisette() co the tra ve mot URL chet
        (hoac URL mac dinh) neu GSA probe that bai. Moi loi khac (timeout, 502,
        JSON hong, thieu OTP) deu lot vao `except Exception -> raise` roi
        authenticate() bat lay va return None -> DUNG LUON.

        Bay gio: moi lan lay headers that bai = loai server do ngay lap tuc,
        chon server tiep theo CON SONG (da kiem chung bang chinh GET headers),
        cap nhat lai client_info/user_agent theo server moi, va thu lai trong
        CUNG lan goi. Chi dung lai khi da thu het danh sach.
        """
        max_servers = max(1, min(len(self._candidate_servers()), ANISETTE_MAX_CANDIDATES))
        tried = set()
        last_exc = None

        for step in range(max_servers):
            addr = (self.anisette_url or "").rstrip("/")
            if not addr:
                picked = self._pick_live_server(exclude=tried)
                if not picked:
                    break
                self._apply_server_identity(picked)
                addr = self.anisette_url.rstrip("/")
            try:
                print(f"[anisette] Fetching headers từ {addr} (server #{step + 1}/{max_servers})...")
                return self._fetch_headers_once(addr)
            except Exception as e:
                last_exc = e
                print(f"[anisette] ❌ {addr} lỗi: {e}")
                self._failed_servers.add(addr)
                tried.add(addr)
                self._invalidate_anisette_cache()
                # Chon server tiep theo CON SONG (khong goi GSA o day — doi khi
                # Apple dang 503 thi viec doi server van phai chay duoc).
                picked = self._pick_live_server(exclude=tried)
                if not picked:
                    break
                self._apply_server_identity(picked)

        self.last_error = ("Không lấy được dữ liệu Anisette từ bất kỳ server nào "
                           f"(đã thử {len(tried)} server). Lỗi cuối: {last_exc}")
        raise AnisetteServerUnavailable(self.last_error)

    def _build_cpd(self, anisette_data, client_info=None, variant=None):
        """Dựng trường `cpd` của GSA.

        QUAN TRỌNG: X-MMe-Client-Info trong body phải TRÙNG KHỚP với header
        HTTP và với machine-id. Bản cũ chỉ sửa header, hoặc sửa cả hai thành
        một chuỗi Mac bịa ra → Apple trả -80044/-80045.
        """
        cpd = {
            "bootstrap": True,
            "icscrec": True,
            "pbe": False,
            "prkgen": True,
            "svct": "iCloud",
        }
        cpd.update(self.generate_meta_headers())
        cpd.update(anisette_data or {})

        ci = client_info or self._apply_identity_variant(self.client_info)
        effective_variant = variant if variant is not None else \
            getattr(self, "_identity_variant", "raw")
        if effective_variant == "akd" and ci:
            ci = ci.replace("com.apple.dt.Xcode", "com.apple.akd")
        if ci:
            cpd["X-MMe-Client-Info"] = ci
        if not cpd.get("X-Mme-Device-Id"):
            cpd["X-Mme-Device-Id"] = self.device_id
        return cpd

    def _apply_identity_variant(self, info):
        """Áp biến thể danh tính đã chọn (raw/akd) lên chuỗi client-info."""
        if not info:
            return info
        if getattr(self, "_identity_variant", "raw") == "akd":
            return info.replace("com.apple.dt.Xcode", "com.apple.akd")
        return info

    @staticmethod
    def _sanitize_client_info(info):
        """(Giữ lại cho tương thích) thay token Xcode bằng akd.

        Từ v72 hàm này KHÔNG còn được dùng ở đường chính: sửa danh tính là
        tạo ra mismatch mới với machine-id (chính là lỗi -80045). Danh tính
        được lấy nguyên văn từ server và kiểm chứng bằng _identity_accepted().
        """
        if not info:
            return info
        return info.replace("com.apple.dt.Xcode", "com.apple.akd")

    @staticmethod
    def _locale_id():
        """Locale dạng en_US. locale.getdefaultlocale() bị deprecated (Python
        3.15 sẽ bỏ) — log người dùng đầy DeprecationWarning vì hàm này được
        gọi 2 lần cho mỗi CPD. Dùng getlocale() trước, chỉ fallback khi rỗng."""
        value = None
        try:
            value = locale.getlocale()[0]
        except Exception:
            value = None
        if not value:
            try:
                value = locale.getdefaultlocale()[0]
            except Exception:
                value = None
        return value or "en_US"

    def generate_meta_headers(self):
        """Generate meta headers for Apple requests."""
        loc = self._locale_id()
        return {
            "X-Apple-I-Client-Time": datetime.utcnow().replace(microsecond=0).isoformat() + "Z",
            "X-Apple-I-TimeZone": str(datetime.utcnow().astimezone().tzinfo),
            "loc": loc,
            "X-Apple-Locale": loc,
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

        # Update internal device info to match anisette server (machine-id của
        # server phải khớp device-id gửi đi, nếu không Apple trả ec=-80045).
        if "X-Mme-Device-Id" in anisette_data:
            self.device_id = anisette_data["X-Mme-Device-Id"]

        # v72: KHÔNG tự bịa/thay danh tính ở đây nữa — _build_cpd() dùng
        # nguyên văn danh tính đã kiểm chứng của server (self.client_info).
        cpd = self._build_cpd(anisette_data)

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
            sess.verify = True
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

            params = {
                "u": adsid,
                "app": [app],
                "c": c,
                "t": idms_token,
                "checksum": checksum,
                "o": "apptokens",
            }
            response = None
            # Thu toi da 3 danh tinh Mac (server Mac -> Mac 27 -> Mac 26.6).
            # Moi lan that bai bang -80046/-45061 thi doi danh tinh, KHONG
            # dang nhap lai SRP (session con nguyen) — theo probe cua Apparition.
            for attempt in range(3):
                self._token_identity_index = min(attempt, len(MAC_TOKEN_IDENTITIES) - 1)
                self._refresh_token_identity()
                self._token_identity_next = True
                try:
                    response = self.gsa_request(params)
                finally:
                    self._token_identity_next = False
                status = (response or {}).get("Status") or {}
                ec = status.get("ec")
                if ec in APP_TOKEN_REJECT_EC:
                    print(f"[apptoken] ⚠️  Apple từ chối cấp developer token (ec {ec}) — "
                          f"thử danh tính Mac khác (lần {attempt + 1}/3)...")
                    self._explain_gsa_status(status)
                    continue
                if ec not in (None, 0):
                    self._explain_gsa_status(status)
                break

            if not response:
                return None

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

        except AnisetteServerUnavailable:
            raise
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

        # v73: request `apptokens` (va cac request developer services sau do)
        # PHAI khai mot danh tinh Mac, neu khong Apple tra -80046 tu 22/10/2026.
        # Cac buoc init/complete van dung danh tinh cua server sinh machine-id
        # (chong -80045). Day la 2 lop kiem tra KHAC NHAU cua Apple.
        use_token = bool(getattr(self, "_token_identity_next", False))
        ci = (self.token_identity if use_token else self.client_info) or ""
        ua = (self.token_user_agent if use_token else self.user_agent) or DEFAULT_USER_AGENT
        if not use_token and getattr(self, "_identity_variant", "raw") == "akd" and ci:
            ci = ci.replace("com.apple.dt.Xcode", "com.apple.akd")
        cpd_data["X-MMe-Client-Info"] = ci      # header va cpd PHAI trung nhau (-80044)
        headers = {
            "Content-Type": "text/x-xml-plist",
            "Accept": "text/x-xml-plist",
            "User-Agent": ua,
            "X-MMe-Client-Info": ci,
            "X-Apple-App-Info": "com.apple.gs.xcode.auth",
            "X-Xcode-Version": "14.2 (14C18)",
        }
        if use_token:
            print(f"[gsa] apptokens dùng danh tính Mac: {ci}")
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

                # v72: hết 3 lần vẫn là 5xx HTML → nhiều khả năng edge chặn
                # DANH TÍNH (vd. server anisette đổi client-info sang token
                # com.apple.dt.Xcode, bị chặn từ 09/2026) chứ không phải
                # "server anisette chết". Phân loại đúng để luồng tự phục hồi
                # (_recover_identity) xử lý thay vì báo lỗi 429 sai đường.
                if response.status_code in (502, 503, 504):
                    snippet = response.content[:120].decode("utf-8", "replace").replace("\n", " ")
                    raise AnisetteIdentityRejected(
                        f"Apple edge trả HTTP {response.status_code} cho danh tính "
                        f"'{self._apply_identity_variant(self.client_info)}' ({snippet})"
                    )

                response.raise_for_status()

                response_data = plist.loads(response.content)

                if debug and "Response" in response_data:
                    resp_keys = list(response_data["Response"].keys())
                    print(f"[gsa] Response keys: {resp_keys}")

                if "Response" not in response_data:
                    raise Exception("Invalid response format from GSA service")

                result = response_data["Response"]
                result["_headers"] = response.headers

                status = result.get("Status") or {}
                ec = status.get("ec")
                if ec in IDENTITY_REJECT_EC and op in ("init", "complete"):
                    # Apple từ chối NGAY ở bước init vì machine-id không khớp
                    # danh tính khai báo → đổi server anisette, không phải
                    # sai mật khẩu (mật khẩu chưa hề được gửi).
                    raise AnisetteIdentityRejected(
                        f"Apple từ chối danh tính anisette (ec {ec}) ở bước init — "
                        f"server {self.anisette_url} không còn dùng được với GSA."
                    )
                return result

            except (GsaRateLimitedError, AnisetteIdentityRejected, AnisetteServerUnavailable):
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
            # v72: UA phải là UA của client đã sinh machine-id (server trả ở
            # /v3/client_info), nếu không Apple coi là hai client khác nhau.
            "User-Agent": self.user_agent,
            "Accept": "text/x-xml-plist",
            "Accept-Language": "en-us",
            "X-Apple-Identity-Token": identity_token,
            "X-Apple-I-Identity-Token": identity_token,
            "X-Apple-App-Info": "com.apple.gs.xcode.auth",
            "X-Xcode-Version": "14.2 (14C18)",
            "X-MMe-Client-Info": self._apply_identity_variant(self.client_info),
            "X-Apple-I-DSID": str(dsid),
        }

        h.update(self.generate_meta_headers())

        if force_new:
            try:
                anisette = self.generate_anisette_headers()
                h.update(anisette)
                # anisette server co the tra ve client-info chua token Xcode -> sanitize
                if "X-MMe-Client-Info" in h:
                    h["X-MMe-Client-Info"] = self._apply_identity_variant(h["X-MMe-Client-Info"])
                print(f"[2fa] Fresh anisette fetched (MD-M present: {'X-Apple-I-MD-M' in anisette})")
            except Exception as e:
                print(f"[2fa] ⚠️  Anisette fetch thất bại: {e}")

        # X-Apple-I-MD-LU: pypush đặt base64(dsid), NHƯNG từ 10/2026 Apple so
        # khớp chặt header với machine-id đã cấp OTP. LU là "local user" được
        # dùng lúc provision — ghi đè bằng dsid có thể làm OTP không còn hợp lệ
        # với LU đó. Nay giữ LU do server cấp, chỉ fallback base64(dsid) khi
        # server không trả (server cũ).
        if not h.get("X-Apple-I-MD-LU"):
            h["X-Apple-I-MD-LU"] = base64.b64encode(str(dsid).encode()).decode()
        # Đảm bảo X-Apple-I-MD-RINFO được cập nhật từ anisette nếu có
        if "X-Apple-I-MD-RINFO" not in h:
            h["X-Apple-I-MD-RINFO"] = "17106176"
            
        return h

    # =========================================================================
    # handle_2fa_trusted_device — đã fix stale-anisette + POST fallback
    # =========================================================================
    def handle_2fa_trusted_device(self, dsid, idms_token, already_pushed=False):
        """Handle trusted device 2FA.

        already_pushed=True: Apple vừa tự gửi mã (hsc 409) — KHÔNG gọi endpoint
        trigger nữa, vì gọi lại sẽ vô hiệu hoá mã người dùng vừa nhận được và
        khiến họ nhập mã đúng mà vẫn bị từ chối.
        """
        if already_pushed:
            print("[2fa] Mã đã được Apple gửi sẵn — bỏ qua bước trigger.")
        else:
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

            triggered = bool(already_pushed)
            for t_attempt in range(0 if already_pushed else 3):
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

    def authenticate(self, apple_id, password, _depth=0, _gsa_retries=0,
                     _identity_retries=0, _resync_retries=0):
        """Main authentication method - GSA SRP + 2FA.

        _gsa_retries: so lan thu lai ca chuoi SRP sau khi edge van tra 429
        tren socket moi. Khong xoay anisette server vi 429 nay khong phu thuoc
        vao server anisette (init da 200 voi cung header).

        _identity_retries: [v72] so lan đã tự đổi server anisette sau khi
        Apple trả -80045 (danh tính bị từ chối). Tối đa 2 để không vòng lặp.

        _resync_retries: [v72] so lan đã đồng bộ lại anisette sau hsc 434.
        """
        self._gsa_init_ok = False
        self._cpd_pin = None
        self._pin_cpd = True
        try:
            print(f"[icloud] Starting authentication for {apple_id}")
            print(f"[icloud] Anisette: {self.anisette_url}")
            print(f"[icloud] Client -Info: {self._apply_identity_variant(self.client_info)}")
            print(f"[icloud] User-Agent   : {self.user_agent}")

            usr = srp.User(apple_id, bytes(), hash_alg=srp.SHA256, ng_type=srp.NG_2048)
            _, A = usr.start_authentication()

            print("[srp] SRP initialized successfully")

            try:
                response = self.gsa_request({
                    "A2k": A,
                    "ps": ["s2k", "s2k_fo"],
                    "u": apple_id,
                    "o": "init"
                })
            except AnisetteIdentityRejected as e:
                return self._recover_identity(apple_id, password, _depth,
                                              _identity_retries, e)
            # init da toi GrandSlam: profile anisette dang dung khong bi edge chan.
            self._gsa_init_ok = True

            status = response.get("Status") or {}
            hsc = status.get("hsc")

            # [v72] hsc 434: Apple yêu cầu đồng bộ lại anisette (machine-id
            # được cấp cho danh tính khác). Lấy OTP mới và thử lại 1 lần.
            if hsc == 434 and _resync_retries < 1:
                print("[icloud] Apple yêu cầu đồng bộ lại anisette (hsc 434) — "
                      "lấy dữ liệu anisette mới và thử lại...")
                self._pin_cpd = False
                self._cpd_pin = None
                try:
                    self.generate_anisette_headers()
                except Exception as exc:
                    print(f"[icloud] Không lấy được anisette mới: {exc}")
                return self.authenticate(apple_id, password, _depth=_depth,
                                         _gsa_retries=_gsa_retries,
                                         _identity_retries=_identity_retries,
                                         _resync_retries=_resync_retries + 1)

            if "sp" not in response:
                self._explain_gsa_status(status, response)
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

            try:
                response = self.gsa_request({
                    "c": c,
                    "M1": M,
                    "u": apple_id,
                    "o": "complete"
                })
            except AnisetteIdentityRejected as e:
                # Machine-id bị Apple từ chối ngay ở bước complete (đổi giữa
                # phiên / edge chặn danh tính) → coi như lỗi danh tính.
                return self._recover_identity(apple_id, password, _depth,
                                              _identity_retries, e)

            status = response.get("Status", {})
            auth_type = status.get("au")
            hsc = status.get("hsc")
            already_pushed = False

            # [v72] Từ 13/10/2026 Apple thường TỰ gửi mã xác thực tới thiết bị
            # tin cậy ngay sau bước mật khẩu: response `complete` có hsc=409 và
            # KHÔNG có "au". Bản cũ coi đây là "không cần 2FA" → đi thẳng xin
            # app token → Apple trả -22411. Nay coi 409 như trustedDevice, chỉ
            # khác là KHÔNG xin mã mới (xin lại sẽ làm mã vừa gửi mất hiệu lực).
            if hsc == 409 and not auth_type:
                print("[2fa] Apple đã tự gửi mã xác thực tới thiết bị tin cậy "
                      "(hsc 409, không có \"au\") — nhập mã đã nhận, không xin lại.")
                auth_type = "trustedDeviceSecondaryAuth"
                already_pushed = True

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
                    # hsc 409 (Apple tự gửi mã) không kèm spd giải mã được:
                    # token phiên nằm trong header trả về.
                    or headers_dict.get("x-apple-idms-token")
                    or headers_dict.get("x-apple-i-idms-token")
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
                    two_fa_ok = self.handle_2fa_trusted_device(
                        dsid, idms_token, already_pushed=already_pushed)
                    if not two_fa_ok:
                        print("[2fa] Trusted device thất bại. Thử SMS fallback...")
                        two_fa_ok = self.handle_2fa_sms(dsid, idms_token)
                else:
                    # smsSecondaryAuth
                    two_fa_ok = self.handle_2fa_sms(dsid, idms_token)
                    if not two_fa_ok:
                        print("[2fa] SMS thất bại. Thử trusted device...")
                        two_fa_ok = self.handle_2fa_trusted_device(
                            dsid, idms_token, already_pushed=already_pushed)

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
                    self.last_error = "Xác thực 2FA thất bại hoặc bị huỷ."
                    return None

            # ---- Không cần 2FA ----
            if not m2_verified:
                print(f"[icloud] Authentication failed: {response}")
                if not self.last_error:
                    self.last_error = "Apple không xác nhận phiên SRP (M2 sai) — thử lại."
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
        except (AnisetteIdentityRejected, AnisetteServerUnavailable) as e:
            # v73: loi server chet / Apple tu choi danh tinh khong duoc ket
            # thuc ca chuoi dang nhap. Doi server da kiem chung roi chay lai SRP.
            return self._recover_identity(apple_id, password, _depth,
                                          _identity_retries, e)
        except Exception as e:
            print(f"[icloud] Authentication error: {e}")
            return None
        finally:
            self._pin_cpd = False
            self._cpd_pin = None

    # ══════════════════════════════════════════════════════════════════════════
    # v72: tự phục hồi khi Apple từ chối danh tính anisette (-80045) và
    # giải thích mã lỗi GSA cho người dùng thay vì chỉ in dictionary thô.
    # ══════════════════════════════════════════════════════════════════════════

    GSA_ERROR_HINTS = {
        -80045: ("Apple từ chối danh tính anisette (machine-id không khớp client "
                 "khai báo). Tool sẽ tự đổi server anisette."),
        -80009: "Apple không nhận machine-id (X-Apple-I-MD-M) — anisette server trả thiếu/hỏng.",
        -80010: "Thiếu one-time password (X-Apple-I-MD-M) trong request.",
        -29004: "machine-id anisette bị Apple coi là hỏng (malformed).",
        -80044: "Danh tính trong header HTTP và trong body (cpd) không khớp nhau.",
        -80046: ("Apple TỪ CHỐI CẤP DEVELOPER TOKEN (từ 22/10/2026) vì request khai "
                 "client không phải Mac. Tool tự gửi lại apptokens với danh tính Mac. "
                 "Nếu vẫn lỗi: đổi sang server Mac-ADI (mac-ani.jkcoxson.com)."),
        -45061: ("Dữ liệu provisioning anisette bị Apple từ chối — cần server Mac-ADI "
                 "mới (xoá .anisette_cache.json, chọn 'Tự động')."),
        -20101: "Sai Apple ID hoặc mật khẩu.",
        -20102: "Tài khoản Apple ID không tồn tại hoặc sai định dạng.",
        -20209: ("TÀI KHOẢN BỊ KHOÁ vì lý do an ninh (Apple khoá khi thấy nhiều lần "
                 "đăng nhập bất thường). Mở https://iforgot.apple.com để mở khoá, "
                 "đừng thử lại liên tục."),
        -22411: ("Apple không cấp token phiên ở bước lấy app token — thường do 2FA "
                 "chưa hoàn tất (hsc 409) hoặc phiên đã bị thay thế."),
        -22421: "Apple tạm từ chối (thử lại sau ít phút; thường do rate-limit theo IP).",
        -20970: "Apple nói thông tin tài khoản không đúng (kiểm tra lại Apple ID).",
    }

    def _explain_gsa_status(self, status, response=None):
        """In ra lời giải thích dễ hiểu cho Status.ec/hsc của GSA."""
        ec = (status or {}).get("ec")
        hsc = (status or {}).get("hsc")
        if ec in self.GSA_ERROR_HINTS:
            print(f"[gsa] ⚠️  ec {ec}: {self.GSA_ERROR_HINTS[ec]}")
            self.last_error = f"Apple trả lỗi {ec}: {self.GSA_ERROR_HINTS[ec]}"
        elif ec not in (None, 0):
            msg = (status or {}).get("em") or "Apple từ chối request."
            print(f"[gsa] ⚠️  ec {ec}: {msg}")
            self.last_error = f"Apple trả lỗi {ec}: {msg}"
        if hsc not in (None, 200):
            print(f"[gsa] ⚠️  hsc {hsc}")

    def _recover_identity(self, apple_id, password, _depth, attempts, exc):
        """Apple từ chối machine-id của server đang dùng → đổi server và thử lại.

        Chỉ đổi khi người dùng KHÔNG ghim server (ghim = họ tự chọn, tôn trọng
        lựa chọn nhưng báo rõ). Tối đa 2 lần để không vòng lặp vô hạn.
        """
        print(f"[icloud] ❌ {exc}")
        server_dead = isinstance(exc, AnisetteServerUnavailable)
        if server_dead:
            self.last_error = f"Server anisette không dùng được: {exc}"
            print(f"[icloud] → Lỗi TẦNG SERVER ({exc}) — đổi server ngay, không thử lại server này.")
        else:
            self._explain_gsa_status({"ec": -80045})

        # v73: server do nguoi dung ghim VAN DUOC PHEP failover khi no chet
        # (yeu cau truc tiep cua nguoi dung). Chi giung ghim khi Apple van
        # nhan request nhung tu choi danh tinh (-80045) — luc do can nguoi
        # dung chon server khac co y thuc.
        if getattr(self, "_anisette_pinned", False) and not server_dead:
            self.last_error = (
                f"Server anisette đang ghim ({self.anisette_url}) bị Apple từ chối "
                "(-80045). Vào Cài đặt → Server Anisette → chọn 'Tự động'.")
            print(f"[icloud] Server đang ghim ({self.anisette_url}) bị Apple từ chối.")
            print("[icloud] → Vào Cài đặt > Server Anisette, chọn 'Tự động' hoặc một "
                  "server khác (ví dụ mac-ani.jkcoxson.com).")
            return None

        self._pin_cpd = False
        self._cpd_pin = None

        if server_dead:
            # Khong xin OTP lai tren server da chet. Loai no, chon server song
            # tiep theo (khong can GSA probe), roi chay lai SRP.
            self._failed_servers.add((self.anisette_url or "").rstrip("/"))
            self._invalidate_anisette_cache()
            picked = self._pick_live_server(exclude=set(self._failed_servers))
            if not picked:
                self.last_error = ("Mọi server anisette đều không phản hồi — kiểm tra mạng/VPN "
                                   "rồi thử lại (không tự thử lại vô hạn).")
                print(f"[icloud] ❌ {self.last_error}")
                return None
            self._apply_server_identity(picked)
            self._save_anisette_cache(picked[0], picked[1].get("client_info") or "",
                                      picked[1].get("user_agent"), "raw")
            if attempts >= 4:
                self.last_error = "Đã đổi server nhiều lần nhưng đăng nhập vẫn lỗi — dừng để tránh khoá tài khoản."
                print(f"[icloud] ❌ {self.last_error}")
                return None
            return self.authenticate(apple_id, password, _depth=_depth,
                                     _identity_retries=attempts + 1)

        # Lần 1: cùng server nhưng xin OTP/machine-id mới. Apple từ chối
        # thường vì OTP đã cũ hoặc machine-id vừa bị luân chuyển — chưa cần
        # đổi server (tránh mất server đang tốt).
        if attempts == 0:
            print("[icloud] → Thử lại với dữ liệu anisette mới trên cùng server...")
            try:
                self.generate_anisette_headers()
            except Exception as e:
                print(f"[icloud] Không lấy được anisette mới: {e}")
            return self.authenticate(apple_id, password, _depth=_depth,
                                     _identity_retries=1)

        # Lần 2: server này thật sự bị chặn → loại và chọn server đã kiểm chứng khác.
        if 1 <= attempts < 4:
            print(f"[icloud] → Server này bị chặn: tự động chọn server anisette khác "
                  f"(lần {attempts}) và thử lại...")
            self._failed_servers.add(self.anisette_url.rstrip("/"))
            self._invalidate_anisette_cache()
            picked = self._pick_live_server(exclude=set(self._failed_servers))
            if picked:
                self._apply_server_identity(picked)
                self._save_anisette_cache(picked[0], picked[1].get("client_info") or "",
                                          picked[1].get("user_agent"), "raw")
            else:
                self._select_anisette(ignore_cache=True)
            return self.authenticate(apple_id, password, _depth=_depth,
                                     _identity_retries=attempts + 1)

        self.last_error = (
            "Apple từ chối mọi danh tính anisette đã thử (-80045). Thử: "
            "(1) chọn server khác trong Cài đặt, (2) đổi mạng/VPN, "
            "(3) tự host anisette-v3-server.")
        print("[icloud] Đã thử OTP mới + đổi server mà Apple vẫn từ chối. "
              "Nguyên nhân có thể: (1) mọi server công khai đang bị chặn, "
              "(2) mạng/VPN đang bị Apple đánh dấu, (3) cần tự host "
              "anisette-v3-server. Thử lại sau vài phút.")
        return None

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
