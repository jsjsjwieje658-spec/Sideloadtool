
import subprocess
import os
import shutil
import zipfile
import plistlib
import base64

def decode_apple_data_field(raw):
    """[FIX MỚI — nguyên nhân gốc của lỗi binascii.Error vừa gặp]

    Apple trả response của developerservices2.apple.com dưới dạng XML
    plist. Field như 'encodedProfile' (provisioning profile) có thể được
    Apple đóng gói dưới dạng phần tử <data> HOẶC <string> trong XML — và
    plistlib.loads() xử lý 2 dạng này KHÁC NHAU HOÀN TOÀN:

        <data>...</data>      -> plistlib TỰ ĐỘNG base64-decode, trả về
                                  Python `bytes` — đây đã là dữ liệu
                                  mobileprovision THẬT (nhị phân CMS/DER),
                                  dùng trực tiếp, KHÔNG decode thêm.
        <string>...</string>  -> plistlib giữ nguyên `str` — vẫn là TEXT
                                  base64, cần gọi base64.b64decode().

    Code cũ luôn gọi base64.b64decode() VÔ ĐIỀU KIỆN. Khi field là <data>
    (đã là bytes nhị phân ngẫu nhiên), b64decode lại sẽ:
      - hoặc "thành công" về mặt kỹ thuật nhưng cho ra dữ liệu RÁC, ghi
        thành file .mobileprovision hỏng mà không có exception nào cả
        (đây là khả năng cao đã xảy ra ở lần chạy có lỗi zsign exit 255
        trước đó — file profile bị hỏng nhưng không ai biết);
      - hoặc raise binascii.Error khi số "data characters" sau khi lọc
        ký tự base64 hợp lệ tình cờ không chia hết cho 4 — đúng như lỗi
        'number of data characters (9265) cannot be 1 more than a
        multiple of 4' vừa gặp.

    Vì plistlib LUÔN trả bytes cho <data> và str cho <string>, chỉ cần
    kiểm tra type() là biết chắc chắn có cần decode hay không — không cần
    đoán hay try/except.
    """
    if isinstance(raw, bytes):
        return raw
    return base64.b64decode(raw)

def run_command(command, cwd=None, extra_env=None):
    """Chạy lệnh shell và trả về output.

    extra_env: dict các biến môi trường THÊM VÀO (không thay thế) os.environ
    hiện tại của tiến trình Python — dùng để set LD_LIBRARY_PATH khi chạy
    zsign (xem sideload_core.py, bước "Ký IPA bằng zsign"), vì zsign cần tìm
    libssl.so.3/libcrypto.so.3/libc++_shared.so đã giải nén sẵn ở một thư mục
    KHÔNG nằm trong PATH tìm thư viện mặc định của Android."""
    env = None
    if extra_env:
        env = dict(os.environ)
        env.update(extra_env)
    try:
        result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True, check=True)
        print(f"[CMD] {command}\n{result.stdout}")
        return result.stdout.strip()
    except subprocess.CalledProcessError as e:
        # [FIX] BUG GỐC: code cũ chỉ in e.stderr.
        # zsign (và nhiều CLI tool khác như ideviceinstaller) ghi TOÀN BỘ
        # thông báo lỗi thật (vd "Error: ...", lý do ký thất bại) ra STDOUT
        # qua ZLog::Print, không phải stderr. Vì capture_output=True chặn cả
        # hai luồng, chỉ in e.stderr khiến lý do lỗi thật bị NUỐT MẤT hoàn
        # toàn — đây chính là lý do log chỉ thấy "exit status 255" mà không
        # có thông tin gì hữu ích để debug. Giờ in cả hai, có nhãn rõ ràng.
        print(f"[CMD ERROR] {command}")
        if e.stdout:
            print(f"--- stdout (exit {e.returncode}) ---\n{e.stdout}")
        if e.stderr:
            print(f"--- stderr (exit {e.returncode}) ---\n{e.stderr}")
        if not e.stdout and not e.stderr:
            print(f"(exit {e.returncode}, không có output nào được capture — có thể binary "
                  f"thiếu shared library hoặc bị kill. Thử chạy lệnh thủ công trong terminal "
                  f"để xem lỗi đầy đủ, hoặc thêm cờ -d/--debug khi gọi zsign.)")
        raise

def extract_ipa(ipa_path, output_dir):
    """Giải nén file IPA."""
    print(f"[IPA] Giải nén {ipa_path} vào {output_dir}...")
    if os.path.exists(output_dir):
        shutil.rmtree(output_dir)
    os.makedirs(output_dir)
    with zipfile.ZipFile(ipa_path, 'r') as zip_ref:
        zip_ref.extractall(output_dir)
    print(f"[IPA] Giải nén hoàn tất.")
    return output_dir

def find_app_bundle(extracted_ipa_path):
    """Tìm thư mục .app trong IPA đã giải nén."""
    payload_path = os.path.join(extracted_ipa_path, "Payload")
    if not os.path.exists(payload_path):
        raise Exception(f"Thư mục Payload không tìm thấy trong {extracted_ipa_path}")
    
    for item in os.listdir(payload_path):
        if item.endswith(".app"):
            app_bundle_path = os.path.join(payload_path, item)
            print(f"[IPA] Tìm thấy thư mục .app: {app_bundle_path}")
            return app_bundle_path
    raise Exception(f"Không tìm thấy thư mục .app trong {payload_path}")

def get_bundle_id(app_bundle_path):
    """Lấy Bundle ID từ Info.plist."""
    info_plist_path = os.path.join(app_bundle_path, "Info.plist")
    if not os.path.exists(info_plist_path):
        raise Exception(f"Info.plist không tìm thấy trong {app_bundle_path}")
    
    with open(info_plist_path, 'rb') as f:
        plist_data = plistlib.load(f)
    bundle_id = plist_data.get("CFBundleIdentifier")
    if not bundle_id:
        raise Exception(f"Không tìm thấy CFBundleIdentifier trong Info.plist của {app_bundle_path}")
    print(f"[IPA] Bundle ID: {bundle_id}")
    return bundle_id

def get_app_name(app_bundle_path):
    """Lấy tên ứng dụng từ Info.plist."""
    info_plist_path = os.path.join(app_bundle_path, "Info.plist")
    if not os.path.exists(info_plist_path):
        raise Exception(f"Info.plist không tìm thấy trong {app_bundle_path}")
    
    with open(info_plist_path, 'rb') as f:
        plist_data = plistlib.load(f)
    app_name = plist_data.get("CFBundleDisplayName") or plist_data.get("CFBundleName")
    if not app_name:
        raise Exception(f"Không tìm thấy tên ứng dụng trong Info.plist của {app_bundle_path}")
    print(f"[IPA] Tên ứng dụng: {app_name}")
    return app_name

def save_certificate_as_pem(cert_content_raw, output_path):
    """Lưu nội dung certificate Apple trả về thành file .pem, tự nhận diện đúng
    định dạng. Apple có thể trả certContent ở MỘT TRONG 4 dạng khác nhau tuỳ
    endpoint/thời điểm:
      (a) base64 TEXT của DER thô (str — cần b64decode)
      (b) base64 TEXT của TOÀN BỘ PEM (str, đã có -----BEGIN CERTIFICATE-----)
      (c) text PEM trần (str, không base64)
      (d) bytes DER THẬT — khi field này là <data> trong XML plist, plistlib
          đã TỰ ĐỘNG base64-decode rồi, nên cert_content_raw ở đây là DER
          nhị phân thật, KHÔNG phải text cần decode thêm.
    [FIX] Trước đây code luôn .decode('utf-8','ignore') trước khi xét — nếu
    rơi vào case (d), DER nhị phân (không phải UTF-8 hợp lệ) sẽ bị làm hỏng
    ÂM THẦM (mất byte) ngay tại bước decode này, giống hệt lỗi vừa gặp với
    provisioning profile. Giờ kiểm tra dấu hiệu DER (ASN.1 SEQUENCE, byte
    đầu = 0x30) TRƯỚC, để dùng trực tiếp bytes nhị phân khi đúng là case (d)."""
    if isinstance(cert_content_raw, bytes) and cert_content_raw[:1] == b"\x30":
        # (d) <data> trong plist -> plistlib đã decode sẵn, đây là DER thật.
        with open(output_path, "wb") as f:
            f.write(b"-----BEGIN CERTIFICATE-----\n")
            f.write(base64.encodebytes(cert_content_raw))
            f.write(b"-----END CERTIFICATE-----\n")
        return

    if isinstance(cert_content_raw, bytes):
        raw_str = cert_content_raw.decode("utf-8", "ignore")
    else:
        raw_str = cert_content_raw

    if "-----BEGIN CERTIFICATE-----" in raw_str:
        # (c) Đã là PEM text sẵn
        with open(output_path, "w") as f:
            f.write(raw_str)
        return

    try:
        decoded = base64.b64decode(raw_str, validate=False)
    except Exception:
        decoded = None

    if decoded and b"-----BEGIN CERTIFICATE-----" in decoded[:60]:
        # (b) base64 của PEM text
        with open(output_path, "wb") as f:
            f.write(decoded)
        return

    if decoded:
        # (a) base64 của DER thô -> tự bọc PEM
        with open(output_path, "wb") as f:
            f.write(b"-----BEGIN CERTIFICATE-----\n")
            f.write(base64.encodebytes(decoded))
            f.write(b"-----END CERTIFICATE-----\n")
        return

    raise Exception("Không thể nhận diện định dạng certContent trả về từ Apple.")


def set_bundle_id(app_bundle_path, new_bundle_id):
    """Ghi đè CFBundleIdentifier trong Info.plist của app (và mọi extension .appex
    bên trong PlugIns/ nếu có, giữ đúng tiền tố — iOS yêu cầu bundle ID của
    extension PHẢI có tiền tố trùng app chính, nếu không sẽ bị từ chối cài đặt).

    Cần dùng khi App ID 'tường minh' với bundle ID gốc của IPA đã bị MỘT TÀI
    KHOẢN FREE KHÁC đăng ký mất (rất hay gặp với app phổ biến như SideStore,
    vì App ID phải duy nhất TOÀN CẦU trong thời gian còn hiệu lực — không phải
    chỉ duy nhất trong tài khoản của bạn)."""
    info_plist_path = os.path.join(app_bundle_path, "Info.plist")
    with open(info_plist_path, 'rb') as f:
        plist_data = plistlib.load(f)
    old_bundle_id = plist_data.get("CFBundleIdentifier", "")
    plist_data["CFBundleIdentifier"] = new_bundle_id
    with open(info_plist_path, 'wb') as f:
        plistlib.dump(plist_data, f)
    print(f"[IPA] Đã đổi CFBundleIdentifier: {old_bundle_id} -> {new_bundle_id}")

    plugins_dir = os.path.join(app_bundle_path, "PlugIns")
    if old_bundle_id and os.path.isdir(plugins_dir):
        for item in os.listdir(plugins_dir):
            if not item.endswith(".appex"):
                continue
            appex_plist_path = os.path.join(plugins_dir, item, "Info.plist")
            if not os.path.exists(appex_plist_path):
                continue
            with open(appex_plist_path, 'rb') as f:
                appex_plist = plistlib.load(f)
            appex_old_id = appex_plist.get("CFBundleIdentifier", "")
            if appex_old_id.startswith(old_bundle_id):
                appex_new_id = new_bundle_id + appex_old_id[len(old_bundle_id):]
                appex_plist["CFBundleIdentifier"] = appex_new_id
                with open(appex_plist_path, 'wb') as f:
                    plistlib.dump(appex_plist, f)
                print(f"[IPA] Đã đổi extension '{item}': {appex_old_id} -> {appex_new_id}")
            else:
                print(f"[IPA] ⚠️  Extension '{item}' có CFBundleIdentifier '{appex_old_id}' "
                      f"không bắt đầu bằng '{old_bundle_id}' — bỏ qua, kiểm tra thủ công nếu cần.")

    return new_bundle_id


def create_zip_archive(source_dir, output_zip_path):
    """Tạo file ZIP từ một thư mục."""
    print(f"[ZIP] Tạo file ZIP từ {source_dir} thành {output_zip_path}...")
    shutil.make_archive(os.path.splitext(output_zip_path)[0], 'zip', source_dir)
    print(f"[ZIP] Tạo file ZIP hoàn tất.")


def find_extensions(app_bundle_path):
    """Tìm tất cả app extension (.appex) trong PlugIns/ của app bundle.

    Trả về list[(appex_path, bundle_id)].

    [FIX QUAN TRỌNG] Cần dùng hàm này khi ký IPA có extension (widget,
    share extension, notification service, v.v.):

    Mỗi extension có Bundle ID RIÊNG (vd com.foo.App.Widget, là con của
    bundle id app chính) và theo cơ chế provisioning của Apple, nó cần
    MỘT App ID + MỘT Provisioning Profile RIÊNG khớp đúng bundle id đó —
    KHÔNG thể dùng chung profile của app chính (vì entitlement
    'application-identifier' trong profile phải khớp chính xác bundle id
    của từng Mach-O được ký).

    zsign hỗ trợ điều này qua nhiều cờ `-m` trong một lệnh ký (xem README
    zsign, mục "Multi-profile signing — per-extension provisioning
    profiles for apps with extensions"). Nếu tool chỉ tạo App ID/profile
    cho app chính rồi truyền MỘT `-m` duy nhất cho cả bundle có extension,
    zsign có thể không tìm được entitlements hợp lệ cho phần extension và
    thoát với lỗi (thường gặp: exit code -1 / 255).
    """
    extensions = []
    plugins_dir = os.path.join(app_bundle_path, "PlugIns")
    if not os.path.isdir(plugins_dir):
        return extensions

    for item in sorted(os.listdir(plugins_dir)):
        if not item.endswith(".appex"):
            continue
        appex_path = os.path.join(plugins_dir, item)
        info_plist_path = os.path.join(appex_path, "Info.plist")
        if not os.path.exists(info_plist_path):
            print(f"[IPA] ⚠️  Extension '{item}' không có Info.plist — bỏ qua.")
            continue
        with open(info_plist_path, 'rb') as f:
            plist_data = plistlib.load(f)
        bundle_id = plist_data.get("CFBundleIdentifier")
        if not bundle_id:
            print(f"[IPA] ⚠️  Extension '{item}' không có CFBundleIdentifier — bỏ qua.")
            continue
        print(f"[IPA] Tìm thấy extension: {item} (Bundle ID: {bundle_id})")
        extensions.append((appex_path, bundle_id))

    return extensions

