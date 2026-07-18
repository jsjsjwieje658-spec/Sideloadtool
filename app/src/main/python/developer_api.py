import plistlib
import requests
import json
import base64
import time
import uuid
import re
from datetime import datetime


# ─────────────────────────────────────────────────────────────────────
# Phân loại lỗi khi tạo App ID thất bại.
#
# Apple trả 2 loại lỗi HOÀN TOÀN KHÁC NHAU cho addAppId.action nhưng bản cũ
# xử lý y hệt nhau (chỉ có 1 nhánh "limit" rồi coi mọi lỗi khác là chết hẳn):
#
#   (1) "unavailable" — resultCode 9401, userString dạng:
#       "An App ID with Identifier 'com.foo.bar' is not available.
#        Please enter a different string."
#       Đây KHÔNG phải do giới hạn tài khoản — App ID là một chuỗi DUY NHẤT
#       TOÀN CẦU trên toàn bộ Apple Developer (không chỉ trong team của bạn).
#       Rất hay gặp với các app phổ biến dùng bundle id "mặc định" chưa đổi
#       (vd 'com.SideStore.SideStore') vì HÀNG NGÀN người khác cũng từng thử
#       đăng ký đúng chuỗi đó bằng tài khoản của riêng họ. AltStore/SideStore/
#       iLoader và các tool sideload khác xử lý bằng cách: tự thêm một hậu
#       tố (suffix) vào bundle id rồi thử lại, KHÔNG báo lỗi chết ngay.
#
#   (2) "quota" — tài khoản Apple ID miễn phí bị giới hạn tối đa 10 App ID
#       MỚI mỗi 7 ngày (Apple: "You have reached the maximum number of App
#       IDs for today. You may create up to 10 App IDs every 7 days."). Đây
#       mới là giới hạn theo tài khoản — cách xử lý đúng là TÁI SỬ DỤNG một
#       App ID đã có (của chính app này nếu đã đăng ký trước đó trong vòng 7
#       ngày, hoặc giải phóng một App ID cũ do chính tool này tạo).
# ─────────────────────────────────────────────────────────────────────

_QUOTA_MARKERS = (
    "maximum number of app ids",
    "you may create up to",
    "app id limit",
    "every 7 days",
    "created too many app ids",
    "reached the maximum",
)


def classify_app_id_error(last_error):
    """Trả 'unavailable' | 'quota' | 'other' dựa trên resultCode/userString
    Apple trả về khi addAppId.action thất bại. Xem chú thích khối trên."""
    if not last_error:
        return "other"
    result_code = last_error.get("resultCode")
    user_string = str(last_error.get("userString", "") or "")
    lower = user_string.lower()

    if result_code == 9401 or "is not available" in lower or "enter a different string" in lower:
        return "unavailable"
    if any(marker in lower for marker in _QUOTA_MARKERS) or "limit" in lower:
        return "quota"
    return "other"


class DeveloperAPI:
    """
    Client cho Apple Developer Services (developerservices2.apple.com).
    Viết lại dựa trên AltSign (ALTAppleAPI.m) — rileytestut/AltSign.

    CHANGELOG so với bản cũ:
      FIX-1  Thêm revoke_certificate() — DELETE services/v1/certificates/{id}
             (AltSign: revokeCertificate: URL = certificates/{identifier} relative
             to servicesBaseURL, HTTPMethod = DELETE)
      FIX-2  _get_anisette(): cache 300 s → 60 s
             300 giây quá dài: một session dài (tạo cert + 3 lần poll nội dung,
             mỗi lần sleep 3s) dễ vượt 60 s, lúc đó anisette cũ → Apple trả
             resultCode 1100 "Your session has expired." dù DSID/token OK.
      FIX-3  _make_developer_request(): phát hiện resultCode 1100 (session
             expired) → tự invalidate cache + retry 1 lần với anisette mới
             thay vì cứng là ném Exception ngay.
      FIX-4  Thêm classify_app_id_error() + delete_app_id() — nền tảng cho
             chức năng tự động đổi bundle id khi bị trùng ('unavailable') và
             tự động giải phóng/tái sử dụng App ID khi đạt giới hạn 10/7 ngày
             ('quota'). Xem sideload_core.py::_resolve_app_id().
    """

    def __init__(self, apple_auth_instance, dsid, session_token):
        self.auth = apple_auth_instance
        self.dsid = dsid
        self.session_token = session_token
        self.session = self.auth.session

        self.base_url = "https://developerservices2.apple.com/services/QH65B2"
        self.services_base_url = "https://developerservices2.apple.com/services/v1"
        self.client_id = "XABBG36SBA"
        self.protocol_version = "QH65B2"
        self.xcode_version = "11.2 (11B41)"

        self.team_id = None
        self.last_error = None

        self._cached_anisette = None
        self._last_anisette_time = 0

    # ─────────────────────────────────────────────────────────────────
    # Helpers
    # ─────────────────────────────────────────────────────────────────

    def _get_anisette(self, force=False):
        """FIX-2: cache 60 s thay vì 300 s để tránh stale anisette."""
        if force or self._cached_anisette is None or (time.time() - self._last_anisette_time > 60):
            self._cached_anisette = self.auth.generate_anisette_headers()
            self._last_anisette_time = time.time()
        return self._cached_anisette

    def _auth_headers(self, content_type, accept):
        """Header xác thực chuẩn theo AltSign."""
        headers = {
            "Content-Type": content_type,
            "User-Agent": "Xcode",
            "Accept": accept,
            "Accept-Language": "en-us",
            "X-Apple-App-Info": "com.apple.gs.xcode.auth",
            "X-Xcode-Version": self.xcode_version,
            "X-Apple-I-Identity-Id": str(self.dsid) if self.dsid else "",
            "X-Apple-GS-Token": str(self.session_token) if self.session_token else "",
        }
        headers.update(self._get_anisette())
        return headers

    def _make_developer_request(self, action_path, extra_params=None, require_team=True):
        """
        Gọi 1 action plist trên developerservices2.
        FIX-3: phát hiện resultCode 1100 (session expired) → invalidate
        anisette cache + retry 1 lần trước khi báo lỗi.
        """
        for attempt in range(2):
            headers = self._auth_headers("text/x-xml-plist", "text/x-xml-plist")

            params = {
                "clientId": self.client_id,
                "protocolVersion": self.protocol_version,
                "requestId": str(uuid.uuid4()).upper(),
            }
            if require_team:
                if not self.team_id:
                    raise Exception(
                        "Chưa có team_id. Hãy gọi list_teams() rồi set_team() trước."
                    )
                params["teamId"] = self.team_id
            if extra_params:
                params.update(extra_params)

            url = f"{self.base_url}/{action_path}?clientId={self.client_id}"
            payload = plistlib.dumps(params, fmt=plistlib.FMT_XML)

            try:
                response = self.session.post(url, headers=headers, data=payload, timeout=30)
                if response.status_code != 200:
                    print(f"[developer_api] Error {response.status_code} on {action_path}")
                    print(f"[developer_api] Raw: {response.content[:400]}")
                response.raise_for_status()

                content = response.content
                if not content.lstrip().startswith(b"<?xml") and not content.startswith(b"bplist"):
                    raise Exception(
                        f"Response không phải plist hợp lệ: {content[:300]!r}"
                    )
                result = plistlib.loads(content)

                # FIX-3: phát hiện "session expired" → retry với fresh anisette
                result_code = result.get("resultCode") or result.get("resultcode")
                if result_code == 1100 and attempt == 0:
                    print(f"[developer_api] resultCode 1100 (session expired) trên "
                          f"'{action_path}' — làm mới anisette và thử lại...")
                    self._get_anisette(force=True)
                    continue  # retry

                return result

            except Exception as e:
                if attempt == 0 and "session" not in str(e).lower():
                    # Retry một lần với fresh anisette cho lỗi network/timeout
                    print(f"[developer_api] Lỗi lần 1 trên '{action_path}': {e} — thử lại...")
                    self._get_anisette(force=True)
                    continue
                print(f"[developer_api] Request '{action_path}' thất bại sau retry: {e}")
                raise

        raise Exception(f"Request '{action_path}' thất bại sau {2} lần thử.")

    # ─────────────────────────────────────────────────────────────────
    # Teams
    # ─────────────────────────────────────────────────────────────────

    def list_teams(self):
        print("[developer_api] Listing teams...")
        try:
            response = self._make_developer_request("listTeams.action", require_team=False)
            teams = response.get("teams", [])
            print(f"[developer_api] Found {len(teams)} teams.")
            return teams
        except Exception as e:
            print(f"[developer_api] Failed to list teams: {e}")
            return []

    def set_team(self, team_id):
        self.team_id = team_id
        print(f"[developer_api] Đã chọn team: {team_id}")

    # ─────────────────────────────────────────────────────────────────
    # Devices
    # ─────────────────────────────────────────────────────────────────

    def list_devices(self):
        print("[developer_api] Listing registered devices...")
        try:
            response = self._make_developer_request("ios/listDevices.action")
            devices = response.get("devices", [])
            print(f"[developer_api] Found {len(devices)} devices.")
            return devices
        except Exception as e:
            print(f"[developer_api] Failed to list devices: {e}")
            return []

    def register_device(self, device_name, device_udid):
        print(f"[developer_api] Registering device {device_name} ({device_udid})...")
        self.last_error = None
        try:
            response = self._make_developer_request("ios/addDevice.action", extra_params={
                "deviceNumber": device_udid,
                "name": device_name,
            })
            device = response.get("device")
            if device:
                print(f"[developer_api] Device {device_name} registered successfully.")
                return device
            # [FIX] Trước đây lỗi ở đây bị "nuốt" hoàn toàn — không lưu vào
            # last_error nên caller (sideload_core.py) không có cách nào biết
            # đăng ký thất bại, và (bug riêng) trước đây caller cũng không
            # thèm kiểm tra giá trị trả về, nên UDID không đăng ký được vẫn
            # đi tiếp bình thường tới bước tạo Provisioning Profile — nơi lỗi
            # thật (resultCode 8220 "Your team has no devices...") mới lộ ra,
            # rất khó truy về đúng nguyên nhân. Giờ lưu lại để log rõ ràng.
            self.last_error = {
                "resultCode": response.get("resultCode"),
                "userString": response.get("userString") or response.get("resultString") or "",
                "raw": response,
            }
            print(f"[developer_api] Failed to register device: {response}")
            return None
        except Exception as e:
            self.last_error = {"resultCode": -1, "userString": str(e)}
            print(f"[developer_api] Error registering device: {e}")
            return None

    # ─────────────────────────────────────────────────────────────────
    # App IDs
    # ─────────────────────────────────────────────────────────────────

    def list_app_ids(self):
        print("[developer_api] Listing App IDs...")
        try:
            response = self._make_developer_request("ios/listAppIds.action")
            app_ids = response.get("appIds", [])
            print(f"[developer_api] Found {len(app_ids)} App IDs.")
            return app_ids
        except Exception as e:
            print(f"[developer_api] Failed to list App IDs: {e}")
            return []

    def create_app_id(self, bundle_id, name):
        print(f"[developer_api] Creating App ID {bundle_id} ({name})...")
        sanitized_name = re.sub(r"[^A-Za-z0-9 ]", "", name) or "App"
        self.last_error = None
        try:
            response = self._make_developer_request("ios/addAppId.action", extra_params={
                "identifier": bundle_id,
                "name": sanitized_name,
            })
            app_id = response.get("appId")
            if app_id:
                print(f"[developer_api] App ID {bundle_id} created successfully.")
                return app_id
            # [FIX-4] Trước đây self.last_error = response (nguyên cả plist trả
            # về) — resultCode/userString vẫn có trong đó nên vẫn đọc được,
            # nhưng để chắc chắn classify_app_id_error() luôn có đủ 2 khoá này
            # (đề phòng response thiếu 1 trong 2), chuẩn hoá lại tại đây.
            self.last_error = {
                "resultCode": response.get("resultCode"),
                "userString": response.get("userString") or response.get("resultString") or "",
                "raw": response,
            }
            print(f"[developer_api] Failed to create App ID: {response}")
            return None
        except Exception as e:
            self.last_error = {"resultCode": -1, "userString": str(e)}
            print(f"[developer_api] Error creating App ID: {e}")
            return None

    def delete_app_id(self, app_id_id):
        """Xoá một App ID để giải phóng chỗ trong giới hạn 10 App ID/7 ngày.

        [FIX-4] AltSign (ALTAppleAPI removeAppID:) gọi action
        'ios/deleteAppId.action' với tham số 'appIdId' — giống cấu trúc mọi
        action khác của DeveloperAPI này (list/add đều dùng plist qua
        _make_developer_request), khác với certificates (dùng JSON v1 API).

        CHỈ gọi hàm này với appIdId của App ID do CHÍNH TOOL NÀY tạo và
        không còn app nào đang cài dùng nó — xem _resolve_app_id() trong
        sideload_core.py, nơi quyết định app_id_id nào là "an toàn để xoá"
        dựa trên registry cục bộ, KHÔNG xoá App ID ngẫu nhiên tìm thấy trên
        tài khoản (App ID đó có thể đang được Xcode hoặc một app khác của
        bạn trên thiết bị khác dùng).
        """
        print(f"[developer_api] Đang xoá App ID (appIdId={app_id_id}) để giải phóng hạn mức...")
        try:
            response = self._make_developer_request("ios/deleteAppId.action", extra_params={
                "appIdId": app_id_id,
            })
            result_code = response.get("resultCode")
            if result_code in (0, None):
                print(f"[developer_api] ✅ Đã xoá App ID {app_id_id}.")
                return True
            print(f"[developer_api] Xoá App ID thất bại: {response}")
            return False
        except Exception as e:
            print(f"[developer_api] Lỗi khi xoá App ID {app_id_id}: {e}")
            return False

    # ─────────────────────────────────────────────────────────────────
    # Certificates — JSON v1 API
    # ─────────────────────────────────────────────────────────────────

    def list_certificates(self):
        """Liệt kê certificate iOS Development qua v1 JSON API.
        Response trả về list object với cấu trúc:
            { "id": "...", "attributes": { "name": "...", "certificateContent": "...",
              "expirationDate": "...", "status": "ACTIVE", ... } }
        """
        print("[developer_api] Listing certificates (JSON v1 API)...")
        if not self.team_id:
            print("[developer_api] Chưa có team_id.")
            return []
        url = f"{self.services_base_url}/certificates"
        query = f"teamId={self.team_id}&filter[certificateType]=IOS_DEVELOPMENT"
        # FIX-2 ảnh hưởng: mỗi lần list_certificates là một request mới,
        # invalidate cache để luôn dùng anisette mới cho request quan trọng này.
        self._get_anisette(force=True)
        headers = self._auth_headers("application/vnd.api+json", "application/vnd.api+json")
        headers["X-HTTP-Method-Override"] = "GET"
        try:
            response = self.session.post(
                url, headers=headers,
                json={"urlEncodedQueryParams": query},
                timeout=30,
            )
            if response.status_code != 200:
                print(f"[developer_api] Error {response.status_code}: {response.text[:400]}")
            response.raise_for_status()
            data = response.json()
            certs = data.get("data", [])
            print(f"[developer_api] Found {len(certs)} certificates.")
            return certs
        except Exception as e:
            print(f"[developer_api] Failed to list certificates: {e}")
            return []

    # ─────────────────────────────────────────────────────────────────
    # FIX-1: THÊM MỚI revoke_certificate()
    # Theo AltSign ALTAppleAPI.m:
    #   NSURL *URL = [NSURL URLWithString:[NSString stringWithFormat:@"certificates/%@",
    #                 certificate.identifier] relativeToURL:self.servicesBaseURL];
    #   [request setHTTPMethod:@"DELETE"];
    # ─────────────────────────────────────────────────────────────────
    def revoke_certificate(self, certificate_id):
        """Revoke certificate qua services/v1/certificates/{id}.

        [FIX QUAN TRỌNG] Bản cũ dùng self.session.delete(...) — tức gửi HTTP
        DELETE THẬT. Apple đặt một lớp edge/WAF (Akamai) trước cụm endpoint
        services/v1 này, và lớp đó CHẶN THẲNG các verb DELETE/PUT thật, trả
        về một trang lỗi thô:

            403 Forbidden
            <center>Apple</center>

        — đây KHÔNG phải lỗi JSON có ý nghĩa từ ứng dụng (sẽ có dạng
        {"errors": [...]}) mà là bị edge chặn trước khi tới được logic xử lý.
        Đây cũng chính là lý do list_certificates() ở trên phải "giả" GET
        bằng POST + header X-HTTP-Method-Override: GET (xem AltSign — cách
        ALTAppleAPI lách qua lớp edge này). revoke_certificate() cần áp
        dụng đúng kỹ thuật tương tự với DELETE.

        Quan trọng:
        - CHỈ revoke certificate do CHÍNH TOOL NÀY tạo (machine name bắt đầu
          bằng 'sideload-'). ĐỪNG revoke certificate của Xcode — Xcode sẽ mất
          quyền ký cho đến khi bạn đăng nhập lại.
        - Sau khi revoke thành công, chờ ~2-3 giây trước khi tạo cert mới
          để Apple xử lý xong việc thu hồi.
        """
        if not self.team_id:
            print("[developer_api] Chưa có team_id — không thể revoke.")
            return False

        url = f"{self.services_base_url}/certificates/{certificate_id}"
        query = f"teamId={self.team_id}"
        # Luôn dùng fresh anisette cho thao tác có tác dụng phụ (revoke)
        self._get_anisette(force=True)
        headers = self._auth_headers("application/vnd.api+json", "application/vnd.api+json")
        headers["X-HTTP-Method-Override"] = "DELETE"  # [FIX] giả DELETE bằng POST, né edge-block
        try:
            response = self.session.post(
                url, headers=headers,
                json={"urlEncodedQueryParams": query},
                timeout=30,
            )
            if response.status_code in [200, 204]:
                print(f"[developer_api] ✅ Certificate {certificate_id} revoked thành công.")
                return True
            print(f"[developer_api] Revoke thất bại: HTTP {response.status_code}")
            try:
                print(f"[developer_api] Response: {response.text[:400]}")
            except Exception:
                pass
            return False
        except Exception as e:
            print(f"[developer_api] Lỗi khi revoke certificate {certificate_id}: {e}")
            return False

    def _fetch_certificate_content(self, certificate_id):
        """Poll v1 API để lấy certificateContent cho cert vừa tạo.
        Retry tối đa 5 lần với backoff tăng dần vì Apple cần thời gian đồng bộ.
        """
        delays = [2, 4, 6, 8, 10]
        for attempt, delay in enumerate(delays):
            print(f"[developer_api] Đang lấy nội dung cert {certificate_id} (lần {attempt + 1}/{len(delays)})...")
            certs = self.list_certificates()
            for cert in certs:
                cert_id = cert.get("id") or cert.get("attributes", {}).get("certificateId")
                if str(cert_id) == str(certificate_id):
                    content = cert.get("attributes", {}).get("certificateContent")
                    if content:
                        return content
            if attempt < len(delays) - 1:
                print(f"[developer_api] certificateContent chưa sẵn sàng — thử lại sau {delay}s...")
                time.sleep(delay)
        return None

    def create_certificate(self, machine_name="ios-sideload-tool"):
        """Sinh CSR + private key mới, nộp lên Apple, trả về cert + private key.

        LƯU Ý: free account giới hạn 2 certificate iOS Development cùng lúc.
        Kiểm tra và revoke cert cũ của tool TRƯỚC khi gọi hàm này (xem main.py).
        """
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import rsa
        from cryptography import x509
        from cryptography.x509.oid import NameOID

        print("[developer_api] Đang sinh RSA private key + CSR mới...")
        private_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        csr = (
            x509.CertificateSigningRequestBuilder()
            .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, machine_name)]))
            .sign(private_key, hashes.SHA256())
        )
        csr_pem = csr.public_bytes(serialization.Encoding.PEM).decode("utf-8")
        machine_id = str(uuid.uuid4()).upper()

        try:
            response = self._make_developer_request("ios/submitDevelopmentCSR.action", extra_params={
                "csrContent": csr_pem,
                "machineId": machine_id,
                "machineName": machine_name,
            })
            cert_request = response.get("certRequest")
            if not cert_request:
                print(f"[developer_api] Không tạo được certificate: {response}")
                return None

            private_key_pem = private_key.private_bytes(
                encoding=serialization.Encoding.PEM,
                format=serialization.PrivateFormat.TraditionalOpenSSL,
                encryption_algorithm=serialization.NoEncryption(),
            ).decode("utf-8")
            cert_request["_private_key_pem"] = private_key_pem

            certificate_id = cert_request.get("certificateId")
            cert_content = self._fetch_certificate_content(certificate_id) if certificate_id else None

            if cert_content:
                cert_request["certContent"] = cert_content
                print("[developer_api] ✅ Tạo certificate mới thành công (cert + private key).")
            else:
                print(f"[developer_api] ⚠️  Certificate đã tạo (id={certificate_id}) "
                      "nhưng KHÔNG lấy được certificateContent sau 3 lần thử.")

            return cert_request

        except Exception as e:
            print(f"[developer_api] Lỗi khi tạo certificate: {e}")
            return None

    # ─────────────────────────────────────────────────────────────────
    # Provisioning Profile
    # ─────────────────────────────────────────────────────────────────

    def list_provisioning_profiles(self):
        print("[developer_api] Listing provisioning profiles...")
        try:
            response = self._make_developer_request("ios/listProvisioningProfiles.action")
            profiles = response.get("provisioningProfiles", [])
            print(f"[developer_api] Found {len(profiles)} provisioning profiles.")
            return profiles
        except Exception as e:
            print(f"[developer_api] Failed to list provisioning profiles: {e}")
            return []

    def download_provisioning_profile(self, app_id_id, retries=3):
        """Download Team Provisioning Profile cho appIdId.

        Thêm tham số retries vì sau khi tạo cert mới, Apple cần vài giây
        để tích hợp cert vào Team Profile. Nếu profile download thất bại,
        retry tự động với delay ngắn.
        """
        print("[developer_api] Downloading team provisioning profile...")
        delay = 3
        for attempt in range(retries):
            try:
                response = self._make_developer_request(
                    "ios/downloadTeamProvisioningProfile.action",
                    extra_params={"appIdId": app_id_id},
                )
                profile = response.get("provisioningProfile")
                if profile:
                    print("[developer_api] ✅ Provisioning profile downloaded successfully.")
                    return profile
                print(f"[developer_api] Profile chưa sẵn sàng (lần {attempt + 1}/{retries}): {response}")
            except Exception as e:
                print(f"[developer_api] Lỗi download profile (lần {attempt + 1}/{retries}): {e}")

            if attempt < retries - 1:
                print(f"[developer_api] Thử lại sau {delay}s...")
                time.sleep(delay)
                delay += 2

        print("[developer_api] ❌ Không thể tải provisioning profile sau tất cả lần thử.")
        return None


if __name__ == "__main__":
    print("Module này không chạy trực tiếp. Dùng qua main.py.")
