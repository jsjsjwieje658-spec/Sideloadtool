"""Cổng Apple ID (GSA/SRP + 2FA) — port gần như nguyên bản từ công cụ Termux gốc.

KHÔNG có phụ thuộc Termux nào trong file này: toàn bộ giao tiếp là HTTP thuần
(requests) tới gsa.apple.com, dùng srp._pysrp (thuần Python, Chaquopy cài được)
và cryptography (Chaquopy có sẵn wheel Android). Vì vậy phần lõi xác thực được
giữ NGUYÊN so với bản gốc.

THAY ĐỔI DUY NHẤT so với bản gốc: các lời gọi input()/getpass() chặn luồng
(dùng cho nhập mã 2FA 6 số, hoặc DSID thủ công trong trường hợp hiếm) được thay
bằng self.input_func(prompt) — một callback có thể inject được. Trên Android,
sideload_core.py truyền vào một hàm gọi UiPrompt.requestInput(...) (xem
bridge/UiPrompt.kt), hàm này hiện dialog trên UI và BLOCK luồng nền cho tới khi
người dùng bấm gửi — về hành vi, tương đương input() chặn console gốc.
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
import urllib3
import traceback

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)


# Sửa lỗi quan trọng: bản gốc để hai hằng số này trỏ vào 127.0.0.1:6969 (một
# server anisette LOCAL — bản Termux gốc có thể tự chạy một server như vậy
# trên máy, nhưng ứng dụng Android này không đóng gói/chạy server đó). Kết
# quả là get_best_anisette_server() không bao giờ lấy được danh sách server
# thật, và fallback cuối cùng cũng trỏ vào một địa chỉ không tồn tại trên
# Android -> xác thực Apple ID/2FA luôn thất bại từ bước lấy anisette,
# bất kể lớp USB có hoạt động hay không. Giá trị đúng:
ANISETTE_URL = "https://ani.sidestore.io"
OFFICIAL_SERVERS_URL = "https://servers.sidestore.io/servers.json"


def fetch_official_servers():
    """Lấy danh sách server Anisette công khai từ SideStore, dạng
    list[dict(name=str, address=str, ...)]. Trả về [] nếu không lấy được
    (mất mạng, server đổi định dạng, v.v.) — KHÔNG raise, để lời gọi từ màn
    Cài đặt (qua sideload_core.list_anisette_servers) luôn có giá trị dùng
    được. Tách riêng khỏi get_best_anisette_server() để màn Cài đặt có thể
    hiện danh sách đầy đủ (kể cả server không phản hồi lúc đó) cho người
    dùng tự chọn, không chỉ server "tốt nhất" lúc kiểm tra."""
    try:
        resp = requests.get(OFFICIAL_SERVERS_URL, timeout=10, verify=False)
        resp.raise_for_status()
        return resp.json().get("servers", [])
    except Exception as e:
        print(f"[anisette] Không thể lấy danh sách server: {e}")
        return []


class AppleAuth:
    def __init__(self, anisette_url=None, input_func=None):
        if anisette_url:
            self.anisette_url = anisette_url
        else:
            self.anisette_url = self.get_best_anisette_server()
        self.input_func = input_func or input
        self.user_id = str(uuid.uuid4()).upper()
        self.device_id = str(uuid.uuid4()).upper()
        self.session = requests.Session()
        self.session.verify = False

        self.user_agent = "akd/1.0 CFNetwork/1408.0.4 Darwin/22.5.0"
        self.client_info = "<MacBookPro18,3> <Mac OS X;13.4.1;22F8> <com.apple.AOSKit/282 (com.apple.dt.Xcode/3594.4.19)>"
        self.xcode_ua = "com.apple.dt.Xcode/14.2 (14C18) akd/1.0 CFNetwork/1408.0.4 Darwin/22.5.0"

        srp.rfc5054_enable()
        srp.no_username_in_x()

    def get_best_anisette_server(self):
        """Lấy server Anisette hoạt động tốt nhất từ danh sách SideStore."""
        print("[anisette] Đang tìm kiếm server hoạt động...")
        servers = fetch_official_servers()
        for server in servers:
            addr = server.get("address")
            if not addr:
                continue
            try:
                test_resp = requests.get(addr, timeout=3, verify=False)
                if test_resp.ok:
                    print(f"[anisette] Sử dụng server: {server.get('name')} ({addr})")
                    return addr
            except Exception:
                continue

        print(f"[anisette] Quay lại server mặc định: {ANISETTE_URL}")
        return ANISETTE_URL

    def generate_anisette_headers(self):
        for attempt in range(3):
            try:
                print(f"[anisette] Fetching headers from {self.anisette_url} (lần {attempt+1}/3)...")
                response = self.session.get(self.anisette_url, timeout=10)
                response.raise_for_status()
                data = response.json()

                if "X-Apple-I-MD-M" not in data:
                    print("[anisette] Cảnh báo: Thiếu X-Apple-I-MD-M trong response.")

                return data
            except Exception as e:
                print(f"[anisette] Lỗi fetch anisette: {e}")
                if attempt < 2:
                    time.sleep(1)
                    self.anisette_url = self.get_best_anisette_server()
                else:
                    raise Exception("Không thể lấy dữ liệu Anisette sau 3 lần thử.")

    def generate_meta_headers(self):
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
        print("[cpd] Generating client platform data...")

        anisette_data = self.generate_anisette_headers()

        if "X-Mme-Device-Id" in anisette_data:
            self.device_id = anisette_data["X-Mme-Device-Id"]
        if "X-MMe-Client-Info" in anisette_data:
            self.client_info = anisette_data["X-MMe-Client-Info"]

        cpd = {
            "bootstrap": True,
            "icscrec": True,
            "pbe": False,
            "prkgen": True,
            "svct": "iCloud",
        }

        cpd.update(self.generate_meta_headers())
        cpd.update(anisette_data)

        print("[cpd] Client platform data generated successfully")
        return cpd

    def encrypt_password(self, password, salt, iterations, protocol):
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
        try:
            session_key = usr.get_session_key()
            if session_key is None:
                raise Exception("No session key available")
            return hmac.new(session_key, name.encode(), hashlib.sha256).digest()
        except Exception as e:
            print(f"[crypto] Error creating session key: {e}")
            raise

    def decrypt_cbc(self, usr, data):
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
        if not data.startswith(b"bplist") and not data.lstrip().startswith(b"<?xml"):
            PLISTHEADER = (
                b"<?xml version='1.0' encoding='UTF-8'?>\n"
                b"<!DOCTYPE plist PUBLIC '-//Apple//DTD PLIST 1.0//EN' "
                b"'http://www.apple.com/DTDs/PropertyList-1.0.dtd'>\n"
            )
            data = PLISTHEADER + data
        return plist.loads(data)

    def fetch_app_token(self, adsid, c, idms_token, sk, app="com.apple.gs.xcode.auth"):
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
            "X-MMe-Client-Info": self.client_info,
        }

        try:
            if debug:
                print(f"[gsa] Request operation: {parameters.get('o', 'unknown')}")

            print("[gsa] Sending request to Apple GSA service...")

            response = self.session.post(
                "https://gsa.apple.com/grandslam/GsService2",
                headers=headers,
                data=plist.dumps(body),
                timeout=30
            )

            print(f"[gsa] Response status: {response.status_code}")

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

        except Exception as e:
            print(f"[gsa] Request failed: {e}")
            raise

    def _build_2fa_base_headers(self, dsid, idms_token, force_new=True):
        identity_token = base64.b64encode(f"{dsid}:{idms_token}".encode()).decode()

        h = {
            "User-Agent": self.xcode_ua,
            "Accept": "text/x-xml-plist",
            "Accept-Language": "en-us",
            "X-Apple-Identity-Token": identity_token,
            "X-Apple-I-Identity-Token": identity_token,
            "X-Apple-App-Info": "com.apple.gs.xcode.auth",
            "X-Xcode-Version": "14.2 (14C18)",
            "X-Mme-Client-Info": self.client_info,
            "X-Apple-I-DSID": str(dsid),
        }

        h.update(self.generate_meta_headers())

        if force_new:
            try:
                anisette = self.generate_anisette_headers()
                h.update(anisette)
                print(f"[2fa] Fresh anisette fetched (MD-M present: {'X-Apple-I-MD-M' in anisette})")
            except Exception as e:
                print(f"[2fa] ⚠️  Anisette fetch thất bại: {e}")

        h["X-Apple-I-MD-LU"] = base64.b64encode(str(dsid).encode()).decode()
        if "X-Apple-I-MD-RINFO" not in h:
            h["X-Apple-I-MD-RINFO"] = "17106176"

        return h

    def handle_2fa_trusted_device(self, dsid, idms_token):
        print("[2fa] Triggering trusted device authentication...")

        try:
            trigger_url = "https://gsa.apple.com/auth/verify/trusteddevice"

            triggered = False
            for t_attempt in range(3):
                print(f"[2fa] Fetching fresh anisette for trigger (GET {trigger_url}, lần {t_attempt + 1}/3)...")
                trigger_headers = self._build_2fa_base_headers(dsid, idms_token)
                trigger_headers["Content-Type"] = "text/x-xml-plist"
                trigger_headers["Accept"] = "text/x-xml-plist"

                try:
                    trigger_resp = self.session.get(trigger_url, headers=trigger_headers, timeout=15)
                    print(f"[2fa] Trigger response: HTTP {trigger_resp.status_code}")

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

            print("[2fa] Fetching fresh anisette for validate request...")
            validate_headers = self._build_2fa_base_headers(dsid, idms_token)
            validate_headers["Security-Code"] = code
            validate_headers["security-code"] = code

            print("[2fa] Đang gửi mã xác thực tới Apple GSA (grandslam/GsService2/validate)...")
            response = self.session.get(
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
        print("[2fa-sms] Bắt đầu SMS 2FA...")

        try:
            print("[2fa-sms] Fetching phone list để lấy đúng phone_id...")
            list_headers = self._build_2fa_base_headers(dsid, idms_token)
            phone_id = 1
            phone_display = "số #1"

            try:
                list_resp = self.session.get(
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

            print(f"[2fa-sms] Gửi mã OTP qua SMS tới {phone_display}...")
            sms_headers = self._build_2fa_base_headers(dsid, idms_token)
            sms_headers["Content-Type"] = "application/json"

            sms_body = {"phoneNumber": {"id": phone_id}, "mode": "sms"}
            sms_resp = self.session.put(
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

            print("[2fa-sms] Fetching fresh anisette cho validate request...")
            val_headers = self._build_2fa_base_headers(dsid, idms_token)
            val_headers["Content-Type"] = "application/json"

            val_body = {
                "phoneNumber": {"id": phone_id},
                "mode": "sms",
                "securityCode": {"code": code}
            }

            print("[2fa-sms] Đang validate OTP với Apple...")
            val_resp = self.session.post(
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

    def authenticate(self, apple_id, password, _depth=0):
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

            m2_verified = False
            if "M2" in response:
                usr.verify_session(response["M2"])
                m2_verified = usr.authenticated()
                if not m2_verified:
                    print("[icloud] Cảnh báo: M2 từ Apple không khớp với session SRP cục bộ.")

            spd_data = {}
            if m2_verified and "spd" in response:
                try:
                    decrypted_spd = self.decrypt_cbc(usr, response["spd"])
                    spd_data = self._safe_plist_loads(decrypted_spd)
                    print(f"[debug] Giải mã SPD thành công. Các khoá có sẵn: {list(spd_data.keys())}")
                except Exception as e:
                    print(f"[debug] Không thể giải mã SPD: {e}")

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

                two_fa_ok = False
                if auth_type in ["trustedDeviceSecondaryAuth", "secondaryAuth"]:
                    two_fa_ok = self.handle_2fa_trusted_device(dsid, idms_token)
                    if not two_fa_ok:
                        print("[2fa] Trusted device thất bại. Thử SMS fallback...")
                        two_fa_ok = self.handle_2fa_sms(dsid, idms_token)
                else:
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
                dsid_input = self.input_func("\n[!] Không thể lấy DSID tự động. Nhập DSID (hoặc để trống để bỏ qua): ").strip()
                if dsid_input:
                    dsid = dsid_input

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

        except Exception as e:
            print(f"[icloud] Authentication error: {e}")
            return None
