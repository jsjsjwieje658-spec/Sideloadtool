/**
 * jni_bridge_imd.c — JNI ↔ libimobiledevice (chạy trên usbmuxd nội bộ)
 *
 * ═══════════════════════════════════════════════════════════════════
 *  v49 — luồng giống hệt công cụ PC/Termux (idevicepair + ideviceinstaller):
 *
 *  nativeSetUsbFd : fd USB → libusb (đúng configuration) → usbmuxd nội bộ,
 *                   chờ iPhone bắt tay mux. Gọi lại với CÙNG fd = dùng lại.
 *  nativeConnect  : idevice (UDID thật) → lockdownd → nếu đã có pair record
 *                   hợp lệ thì mở phiên SSL (handshake) luôn.
 *  nativePair     : lockdownd_pair lặp tới khi người dùng bấm "Tin cậy";
 *                   pair record được usbmuxd nội bộ LƯU vào filesDir/lockdown
 *                   → mở lại app không phải Trust lại. Xác nhận bằng handshake.
 *  nativeSideload : afc_client_start_service + instproxy_client_start_service
 *                   (mỗi dịch vụ tự handshake SSL như ideviceinstaller),
 *                   upload PublicStaging/<tên>.ipa, instproxy_install KHÔNG
 *                   kèm PackageType=Developer (chỉ dành cho thư mục .app),
 *                   chờ "Complete" và báo lỗi cụ thể (ApplicationVerification…).
 *
 *  Lỗi của bản cũ đã bỏ: StartService trên client KHÔNG có phiên (luôn bị
 *  từ chối), PackageType=Developer cho file .ipa, đường dẫn "/PublicStaging"
 *  có dấu "/" đầu, USBMUXD_SOCKET_ADDRESS là đường dẫn trần, UDID giả
 *  "pending-device" dùng làm tên pair record, attach/detach JVM mỗi dòng log.
 * ═══════════════════════════════════════════════════════════════════
 */
#include <jni.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <android/log.h>

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/afc.h>
#include <libimobiledevice/installation_proxy.h>
#include <libimobiledevice/house_arrest.h>
#include <usbmuxd.h>
#include <plist/plist.h>

#include "usb_fd_bridge.h"
#include "usbmuxd_server.h"
#include "android_usbmuxd_fix.h"

#define LOG_TAG "jni_imd"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define CLIENT_LABEL        "sideloadtool"
#define PAIR_TIMEOUT_MS     (150 * 1000)
#define MUX_READY_TIMEOUT   14000
#define INSTALL_TIMEOUT_MS  (15 * 60 * 1000)
/*
 * v50.1 (fix lỗi "afc_file_write lỗi 30 sau 0 byte"):
 *
 * AFC_CHUNK phải NHỎ HƠN buffer socket unix (~208 KB mặc định, có thể thấp
 * hơn trên một số máy). Lý do: libusbmuxd 2.0.2 để socket client ở chế độ
 * O_NONBLOCK và usbmuxd_send() chỉ gọi MỘT syscall send() duy nhất — không
 * loop, không chia nhỏ. Nếu afc_file_write() nhận length lớn hơn buffer
 * socket thì send() ghi ĐỀN GÓI (~200 KB) và libusbmuxd chỉ in warning
 * "Did not send enough"; iPhone nhận gói AFC cụt (header vẫn khai toàn bộ
 * chiều dài), chờ phần còn lại mãi → không có reply → afc_receive_data()
 * timeout → AFC_E_MUX_ERROR (30). Đây chính là lỗi cài SideStore.ipa fail
 * ngay byte đầu tiên.
 *
 * 32 KB là an toàn: mỗi afc_file_write() (libimobiledevice 1.3.0) chờ reply
 * của chính nó trước khi trả về, nên giữa hai lần ghi socket đã được server
 * drain sạch → lần send() kế tiếp luôn có toàn bộ buffer trống. Tốc độ:
 * 26 MB ≈ 850 vòng lặp (mỗi vòng 1 RTT qua USB) ≈ 2–4 giây.
 * (ideviceinstaller upstream dùng 8 KB — cùng nguyên tắc.)
 */
#define AFC_CHUNK           (32 * 1024)

/* ── Trạng thái ─────────────────────────────────────────────────────────── */
static pthread_mutex_t    g_api = PTHREAD_MUTEX_INITIALIZER;
static idevice_t          g_device   = NULL;
static lockdownd_client_t g_lockdown = NULL;
static char               g_udid[128] = {0};
static char               g_files_dir[512] = {0};
static volatile bool      g_paired = false;
static int                g_product_id = 0;
static int                g_session_fd = -1;
static volatile int       g_cancel = 0;
static char               g_ios_version[32] = {0};
static char               g_device_name[128] = {0};

/* ── JNI helpers ────────────────────────────────────────────────────────── */
static JavaVM   *g_jvm = NULL;
static jobject   g_bridge_obj = NULL;
static jclass    g_bridge_cls = NULL;
static jmethodID g_mid_log = NULL;
static jmethodID g_mid_trust = NULL;
static jmethodID g_mid_dismiss = NULL;
static pthread_key_t  g_env_key;
static pthread_once_t g_env_once = PTHREAD_ONCE_INIT;

static void env_detach(void *unused) { (void)unused; if (g_jvm) (*g_jvm)->DetachCurrentThread(g_jvm); }
static void env_key_create(void) { pthread_key_create(&g_env_key, env_detach); }

static JNIEnv *get_env(void) {
    if (!g_jvm) return NULL;
    JNIEnv *env = NULL;
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) == JNI_OK) return env;
    pthread_once(&g_env_once, env_key_create);
    /* NDK khai báo JNIEnv**, JDK khai báo void** — void* hợp lệ cho cả hai */
    if ((*g_jvm)->AttachCurrentThread(g_jvm, (void *)&env, NULL) != JNI_OK) return NULL;
    pthread_setspecific(g_env_key, (void *)1);   /* tự detach khi luồng native kết thúc */
    return env;
}

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_jvm = vm;
    usb_bridge_set_jvm((void *)vm);
    return JNI_VERSION_1_6;
}

static void emit_log(const char *msg) {
    LOGI("%s", msg);
    JNIEnv *env = get_env();
    if (!env || !g_bridge_cls || !g_mid_log) return;
    jstring jmsg = (*env)->NewStringUTF(env, msg);
    if (!jmsg) { if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env); return; }
    (*env)->CallStaticVoidMethod(env, g_bridge_cls, g_mid_log, jmsg);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, jmsg);
}

static void emitf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void emitf(const char *fmt, ...) {
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    emit_log(buf);
}

static void server_log_callback(const char *msg) { emit_log(msg); }

static void notify_trust(bool show) {
    JNIEnv *env = get_env();
    jmethodID mid = show ? g_mid_trust : g_mid_dismiss;
    if (!env || !g_bridge_cls || !mid) return;
    (*env)->CallStaticVoidMethod(env, g_bridge_cls, mid);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }

/* ── UDID ───────────────────────────────────────────────────────────────── */
static void normalize_udid(const char *in, char *out, size_t n) {
    out[0] = 0;
    if (!in) return;
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < n; i++) {
        char c = in[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        out[j++] = c;
    }
    out[j] = 0;
    if (j == 24 && !strchr(out, '-') && n > 25) {   /* UDID mới: XXXXXXXX-XXXXXXXXXXXXXXXX */
        memmove(out + 9, out + 8, 17);
        out[8] = '-';
    }
}

static bool is_real_udid(const char *v) {
    if (!v || !v[0] || strcmp(v, "pending-device") == 0) return false;
    size_t n = strlen(v), hex = 0;
    if (n < 24 || n > 64) return false;
    bool nonzero = false;
    for (size_t i = 0; i < n; i++) {
        char c = v[i];
        bool is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (is_hex) { hex++; if (c != '0') nonzero = true; }
        else if (c != '-') return false;
    }
    return nonzero && hex >= 24;
}

static const char *ld_err(lockdownd_error_t e) {
    switch (e) {
        case LOCKDOWN_E_SUCCESS: return "SUCCESS";
        case LOCKDOWN_E_INVALID_ARG: return "INVALID_ARG";
        case LOCKDOWN_E_INVALID_CONF: return "INVALID_CONF (không đọc/ghi được pair record)";
        case LOCKDOWN_E_PLIST_ERROR: return "PLIST_ERROR";
        case LOCKDOWN_E_PAIRING_FAILED: return "PAIRING_FAILED";
        case LOCKDOWN_E_SSL_ERROR: return "SSL_ERROR";
        case LOCKDOWN_E_RECEIVE_TIMEOUT: return "RECEIVE_TIMEOUT";
        case LOCKDOWN_E_MUX_ERROR: return "MUX_ERROR (không kết nối được qua usbmuxd)";
        case LOCKDOWN_E_NO_RUNNING_SESSION: return "NO_RUNNING_SESSION";
        case LOCKDOWN_E_PASSWORD_PROTECTED: return "PASSWORD_PROTECTED (iPhone đang khoá)";
        case LOCKDOWN_E_USER_DENIED_PAIRING: return "USER_DENIED_PAIRING (đã bấm Không tin cậy)";
        case LOCKDOWN_E_PAIRING_DIALOG_RESPONSE_PENDING: return "PAIRING_DIALOG_RESPONSE_PENDING";
        case LOCKDOWN_E_INVALID_HOST_ID: return "INVALID_HOST_ID (pair record cũ không còn hợp lệ)";
        case LOCKDOWN_E_INVALID_PAIR_RECORD: return "INVALID_PAIR_RECORD";
        case LOCKDOWN_E_MISSING_PAIR_RECORD: return "MISSING_PAIR_RECORD";
        case LOCKDOWN_E_SESSION_INACTIVE: return "SESSION_INACTIVE";
        case LOCKDOWN_E_INVALID_SERVICE: return "INVALID_SERVICE";
        case LOCKDOWN_E_SERVICE_PROHIBITED: return "SERVICE_PROHIBITED";
        case LOCKDOWN_E_PAIRING_PROHIBITED_OVER_THIS_CONNECTION: return "PAIRING_PROHIBITED_OVER_THIS_CONNECTION";
        default: return "UNKNOWN";
    }
}

static bool have_pair_record(const char *udid) {
    char *data = NULL;
    uint32_t size = 0;
    int r = usbmuxd_read_pair_record(udid, &data, &size);
    free(data);
    return r == 0 && size > 0;
}

static void free_ld_device(void) {
    if (g_lockdown) { lockdownd_client_free(g_lockdown); g_lockdown = NULL; }
    if (g_device)   { idevice_free(g_device); g_device = NULL; }
    g_paired = false;
}

static char *ld_get_string(lockdownd_client_t ld, const char *key) {
    plist_t v = NULL;
    char *s = NULL;
    if (lockdownd_get_value(ld, NULL, key, &v) == LOCKDOWN_E_SUCCESS && v &&
        plist_get_node_type(v) == PLIST_STRING)
        plist_get_string_val(v, &s);
    if (v) plist_free(v);
    return s;
}

/* ── nativeInit ─────────────────────────────────────────────────────────── */
JNIEXPORT void JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeInit(JNIEnv *env, jobject obj, jstring filesDir) {
    if (g_bridge_obj) (*env)->DeleteGlobalRef(env, g_bridge_obj);
    if (g_bridge_cls) (*env)->DeleteGlobalRef(env, g_bridge_cls);
    g_bridge_obj = (*env)->NewGlobalRef(env, obj);
    jclass cls = (*env)->GetObjectClass(env, obj);
    g_bridge_cls = (jclass)(*env)->NewGlobalRef(env, cls);
    (*env)->DeleteLocalRef(env, cls);
    g_mid_log     = (*env)->GetStaticMethodID(env, g_bridge_cls, "onNativeLog", "(Ljava/lang/String;)V");
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); g_mid_log = NULL; }
    g_mid_trust   = (*env)->GetStaticMethodID(env, g_bridge_cls, "onTrustRequired", "()V");
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); g_mid_trust = NULL; }
    g_mid_dismiss = (*env)->GetStaticMethodID(env, g_bridge_cls, "dismissTrust", "()V");
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); g_mid_dismiss = NULL; }

    const char *dir = (*env)->GetStringUTFChars(env, filesDir, NULL);
    snprintf(g_files_dir, sizeof(g_files_dir), "%s", dir ? dir : "");
    if (dir) (*env)->ReleaseStringUTFChars(env, filesDir, dir);

    android_usbmuxd_fix_set_log_callback(server_log_callback);
    usb_bridge_set_bridge_ref((void *)g_bridge_obj);
    emit_log("[jni] SideloadTool native v49 — usbmuxd chuẩn (port upstream) + libimobiledevice");
}

/* ── nativeSetUsbFd ─────────────────────────────────────────────────────── */
JNIEXPORT jboolean JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeSetUsbFd(
        JNIEnv *env, jobject obj, jint fd, jint vendorId, jint productId,
        jstring udidHint, jint epIn, jint epOut, jint ifaceNum) {
    (void)obj;
    pthread_mutex_lock(&g_api);

    /* Cùng fd, usbmuxd còn chạy và iPhone còn bắt tay → giữ nguyên phiên. Bản
     * cũ luôn phá phiên rồi wrap lại, làm đứt mọi kết nối đang mở. */
    if (fd == g_session_fd && usbmuxd_server_socket_path() && usbmuxd_server_device_ready(0)) {
        pthread_mutex_unlock(&g_api);
        return JNI_TRUE;
    }

    free_ld_device();
    usbmuxd_server_stop();
    usb_bridge_close();
    g_session_fd = -1;
    g_udid[0] = 0;
    g_ios_version[0] = 0;
    g_device_name[0] = 0;
    g_product_id = (int)productId;

    if (udidHint) {
        const char *hint = (*env)->GetStringUTFChars(env, udidHint, NULL);
        char norm[128];
        normalize_udid(hint, norm, sizeof(norm));
        if (is_real_udid(norm)) snprintf(g_udid, sizeof(g_udid), "%s", norm);
        if (hint) (*env)->ReleaseStringUTFChars(env, udidHint, hint);
    }

    emitf("[usb] Mở USB fd=%d (vid=0x%04x pid=0x%04x)", (int)fd, (int)vendorId, (int)productId);
    if (!usb_bridge_init_from_fd2((int)fd, (int)vendorId, (int)productId, (int)epIn, (int)epOut, (int)ifaceNum)) {
        emit_log("[usb] ❌ Không khởi tạo được USB — rút cáp cắm lại, cấp quyền USB cho app");
        pthread_mutex_unlock(&g_api);
        return JNI_FALSE;
    }
    if (!usb_bridge_iface_claimed() && !usb_bridge_set_android_mode()) {
        emit_log("[usb] ❌ Không claim được interface usbmux bằng cả libusb lẫn Android");
        usb_bridge_close();
        pthread_mutex_unlock(&g_api);
        return JNI_FALSE;
    }
    if (!g_udid[0]) {
        char norm[128];
        normalize_udid(usb_bridge_serial(), norm, sizeof(norm));
        if (is_real_udid(norm)) snprintf(g_udid, sizeof(g_udid), "%s", norm);
    }

    if (!usbmuxd_server_start(g_files_dir, g_udid[0] ? g_udid : NULL, (int)productId)) {
        emit_log("[usbmux] ❌ Không khởi động được usbmuxd nội bộ (kiểm tra quyền ghi filesDir)");
        usb_bridge_close();
        pthread_mutex_unlock(&g_api);
        return JNI_FALSE;
    }
    const char *addr = usbmuxd_server_socket_address();
    setenv("USBMUXD_SOCKET_ADDRESS", addr, 1);   /* "UNIX:/…" — libusbmuxd 2.0.2 hiểu */

    if (!usbmuxd_server_device_ready(MUX_READY_TIMEOUT)) {
        emit_log("[usbmux] ❌ iPhone không trả lời bắt tay mux. Thử: dùng cáp DATA (không phải cáp "
                 "chỉ sạc), mở khoá iPhone, rút cáp cắm lại rồi bấm Kết nối lại.");
        usbmuxd_server_stop();
        usb_bridge_close();
        pthread_mutex_unlock(&g_api);
        return JNI_FALSE;
    }
    g_session_fd = (int)fd;
    emitf("[usbmux] ✅ usbmuxd nội bộ sẵn sàng (%s)", addr);
    pthread_mutex_unlock(&g_api);
    return JNI_TRUE;
}

/* ── nativeConnect ──────────────────────────────────────────────────────── */
static lockdownd_error_t open_plain_lockdown(lockdownd_client_t *out) {
    lockdownd_error_t e = LOCKDOWN_E_UNKNOWN_ERROR;
    for (int i = 0; i < 4 && !g_cancel; i++) {
        e = lockdownd_client_new(g_device, out, CLIENT_LABEL);
        if (e == LOCKDOWN_E_SUCCESS) return e;
        *out = NULL;
        sleep_ms(400 + 300 * i);
    }
    return e;
}

JNIEXPORT jboolean JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeConnect(JNIEnv *env, jobject obj) {
    (void)env; (void)obj;
    pthread_mutex_lock(&g_api);
    g_cancel = 0;

    if (!usbmuxd_server_socket_path()) {
        emit_log("[imd] ❌ Chưa có phiên USB — cắm iPhone và cấp quyền USB trước");
        pthread_mutex_unlock(&g_api);
        return JNI_FALSE;
    }
    if (!usbmuxd_server_device_ready(5000)) {
        emit_log("[imd] ❌ iPhone chưa sẵn sàng trên usbmuxd (mất kết nối USB?) — rút cáp cắm lại");
        pthread_mutex_unlock(&g_api);
        return JNI_FALSE;
    }
    free_ld_device();

    idevice_error_t ie = IDEVICE_E_UNKNOWN_ERROR;
    for (int i = 0; i < 3; i++) {
        ie = idevice_new_with_options(&g_device, is_real_udid(g_udid) ? g_udid : NULL, IDEVICE_LOOKUP_USBMUX);
        if (ie == IDEVICE_E_SUCCESS) break;
        g_device = NULL;
        sleep_ms(300);
    }
    if (ie != IDEVICE_E_SUCCESS) {
        emitf("[imd] ❌ idevice_new_with_options lỗi %d — usbmuxd nội bộ không liệt kê được iPhone", (int)ie);
        pthread_mutex_unlock(&g_api);
        return JNI_FALSE;
    }

    lockdownd_client_t ld = NULL;
    lockdownd_error_t le = open_plain_lockdown(&ld);
    if (le != LOCKDOWN_E_SUCCESS) {
        emitf("[lockdown] ❌ Không mở được lockdownd (cổng 62078): %s", ld_err(le));
        free_ld_device();
        pthread_mutex_unlock(&g_api);
        return JNI_FALSE;
    }

    /* UDID thật từ lockdown — dùng làm tên pair record và để đăng ký với Apple. */
    char *real = ld_get_string(ld, "UniqueDeviceID");
    if (real && is_real_udid(real) && strcmp(real, g_udid) != 0) {
        bool had = is_real_udid(g_udid);
        snprintf(g_udid, sizeof(g_udid), "%s", real);
        usbmuxd_server_update_udid(g_udid);
        if (!had) {
            /* idevice cũ mang UDID tạm → tạo lại để libimobiledevice dùng UDID thật */
            lockdownd_client_free(ld); ld = NULL;
            idevice_free(g_device); g_device = NULL;
            ie = idevice_new_with_options(&g_device, g_udid, IDEVICE_LOOKUP_USBMUX);
            if (ie != IDEVICE_E_SUCCESS || open_plain_lockdown(&ld) != LOCKDOWN_E_SUCCESS) {
                emit_log("[lockdown] ❌ Mở lại lockdownd với UDID thật thất bại");
                free(real);
                free_ld_device();
                pthread_mutex_unlock(&g_api);
                return JNI_FALSE;
            }
        }
    }
    free(real);
    char *ver = ld_get_string(ld, "ProductVersion");
    char *name = ld_get_string(ld, "DeviceName");
    snprintf(g_ios_version, sizeof(g_ios_version), "%s", ver ? ver : "?");
    snprintf(g_device_name, sizeof(g_device_name), "%s", name ? name : "iPhone");
    free(ver); free(name);
    emitf("[imd] ✅ %s — iOS %s — UDID %s", g_device_name, g_ios_version, g_udid[0] ? g_udid : "?");

    /* Đã pair từ trước? Thử mở phiên SSL bằng pair record đã lưu. */
    if (have_pair_record(g_udid)) {
        lockdownd_client_t hs = NULL;
        lockdownd_error_t he = lockdownd_client_new_with_handshake(g_device, &hs, CLIENT_LABEL);
        if (he == LOCKDOWN_E_SUCCESS) {
            lockdownd_client_free(ld);
            g_lockdown = hs;
            g_paired = true;
            emit_log("[pair] ✅ Đã ghép nối từ trước — phiên SSL lockdownd OK");
            pthread_mutex_unlock(&g_api);
            return JNI_TRUE;
        }
        emitf("[pair] Pair record đã lưu không dùng được (%s) — sẽ ghép nối lại", ld_err(he));
        if (he == LOCKDOWN_E_INVALID_HOST_ID || he == LOCKDOWN_E_INVALID_PAIR_RECORD ||
            he == LOCKDOWN_E_SSL_ERROR || he == LOCKDOWN_E_INVALID_CONF) {
            usbmuxd_delete_pair_record(g_udid);
        }
    }
    g_lockdown = ld;
    g_paired = false;
    emit_log("[lockdown] ✅ lockdownd OK — cần ghép nối (Tin cậy) trước khi cài app");
    pthread_mutex_unlock(&g_api);
    return JNI_TRUE;
}

/* ── nativePair ─────────────────────────────────────────────────────────── */
JNIEXPORT jboolean JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativePair(JNIEnv *env, jobject obj) {
    (void)env; (void)obj;
    pthread_mutex_lock(&g_api);
    g_cancel = 0;
    if (!g_device) {
        emit_log("[pair] ❌ Chưa kết nối — gọi Kết nối trước");
        pthread_mutex_unlock(&g_api);
        return JNI_FALSE;
    }
    if (g_paired && g_lockdown) {
        pthread_mutex_unlock(&g_api);
        return JNI_TRUE;
    }

    emit_log("[pair] Bắt đầu ghép nối — mở khoá iPhone, bấm \"Tin cậy\" rồi nhập mật mã khi được hỏi");
    uint64_t deadline = now_ms() + PAIR_TIMEOUT_MS;
    bool asked = false, locked_told = false, ok = false;
    lockdownd_error_t err = LOCKDOWN_E_UNKNOWN_ERROR;

    while (!g_cancel && now_ms() < deadline) {
        if (!g_lockdown && open_plain_lockdown(&g_lockdown) != LOCKDOWN_E_SUCCESS) {
            g_lockdown = NULL;
            sleep_ms(1000);
            continue;
        }
        err = lockdownd_pair(g_lockdown, NULL);
        if (err == LOCKDOWN_E_SUCCESS) { ok = true; break; }
        switch (err) {
            case LOCKDOWN_E_PAIRING_DIALOG_RESPONSE_PENDING:
                if (!asked) {
                    emit_log("[pair] ⏳ iPhone đang hiện \"Tin cậy máy tính này?\" — bấm Tin cậy + nhập mật mã");
                    notify_trust(true);
                    asked = true;
                }
                sleep_ms(1500);
                break;
            case LOCKDOWN_E_PASSWORD_PROTECTED:
                if (!locked_told) {
                    emit_log("[pair] 🔒 iPhone đang khoá — mở khoá màn hình để hiện hộp thoại Tin cậy");
                    notify_trust(true);
                    locked_told = true;
                }
                sleep_ms(1500);
                break;
            case LOCKDOWN_E_USER_DENIED_PAIRING:
                emit_log("[pair] ❌ Bạn đã bấm \"Không tin cậy\". Vào Cài đặt > Cài đặt chung > Chuyển "
                         "hoặc đặt lại > Đặt lại > Đặt lại Vị trí & Quyền riêng tư, rồi cắm lại.");
                goto fail;
            case LOCKDOWN_E_INVALID_HOST_ID:
            case LOCKDOWN_E_INVALID_PAIR_RECORD:
                usbmuxd_delete_pair_record(g_udid);
                sleep_ms(500);
                break;
            case LOCKDOWN_E_MUX_ERROR:
            case LOCKDOWN_E_RECEIVE_TIMEOUT:
            case LOCKDOWN_E_SSL_ERROR:
            case LOCKDOWN_E_PLIST_ERROR:
            case LOCKDOWN_E_UNKNOWN_ERROR:
                /* iPhone đóng kết nối lockdown khi hiện hộp thoại — mở lại */
                lockdownd_client_free(g_lockdown);
                g_lockdown = NULL;
                sleep_ms(1000);
                break;
            default:
                emitf("[pair] ❌ lockdownd_pair: %s (%d)", ld_err(err), (int)err);
                goto fail;
        }
    }
    if (!ok) {
        emitf("[pair] ❌ Hết thời gian chờ \"Tin cậy\" (lỗi cuối: %s)", ld_err(err));
        goto fail;
    }

    emit_log("[pair] ✅ iPhone đã chấp nhận ghép nối — đang mở phiên SSL...");
    lockdownd_client_free(g_lockdown);
    g_lockdown = NULL;
    for (int i = 0; i < 4; i++) {
        err = lockdownd_client_new_with_handshake(g_device, &g_lockdown, CLIENT_LABEL);
        if (err == LOCKDOWN_E_SUCCESS) break;
        g_lockdown = NULL;
        sleep_ms(700);
    }
    if (err != LOCKDOWN_E_SUCCESS) {
        emitf("[pair] ❌ Ghép nối xong nhưng mở phiên SSL thất bại: %s", ld_err(err));
        goto fail;
    }
    g_paired = true;
    notify_trust(false);
    emit_log("[pair] ✅ Ghép nối + phiên SSL OK (pair record đã lưu, lần sau không cần Tin cậy lại)");
    pthread_mutex_unlock(&g_api);
    return JNI_TRUE;

fail:
    notify_trust(false);
    pthread_mutex_unlock(&g_api);
    return JNI_FALSE;
}

/* ── nativeSideload ─────────────────────────────────────────────────────── */
struct install_ctx {
    pthread_mutex_t mtx;
    int done, ok, percent;
    char status[64];
    char err_name[128];
    char err_desc[512];
};

static void install_status_cb(plist_t command, plist_t status, void *user) {
    (void)command;
    struct install_ctx *c = (struct install_ctx *)user;
    if (!status || !c) return;
    char *sname = NULL, *ename = NULL, *edesc = NULL;
    uint64_t ecode = 0;
    int pct = -1;
    instproxy_status_get_name(status, &sname);
    instproxy_status_get_error(status, &ename, &edesc, &ecode);
    instproxy_status_get_percent_complete(status, &pct);
    pthread_mutex_lock(&c->mtx);
    if (ename) {
        snprintf(c->err_name, sizeof(c->err_name), "%s", ename);
        snprintf(c->err_desc, sizeof(c->err_desc), "%s", edesc ? edesc : "");
        c->done = 1;
        c->ok = 0;
    } else if (sname) {
        snprintf(c->status, sizeof(c->status), "%s", sname);
        if (pct >= 0) c->percent = pct;
        if (!strcmp(sname, "Complete")) { c->done = 1; c->ok = 1; c->percent = 100; }
    }
    pthread_mutex_unlock(&c->mtx);
    free(sname); free(ename); free(edesc);
}

static const char *install_hint(const char *err) {
    if (!err) return "";
    if (strstr(err, "ApplicationVerificationFailed"))
        return "→ iOS từ chối chữ ký/profile: UDID máy chưa có trong provisioning profile, "
               "bundle id lệch App ID, hoặc chứng chỉ đã bị thu hồi.";
    if (strstr(err, "DeviceOSVersionTooLow")) return "→ App yêu cầu iOS mới hơn máy đang dùng.";
    if (strstr(err, "IncorrectArchitecture")) return "→ IPA không có kiến trúc arm64 cho máy này.";
    if (strstr(err, "PackageExtractionFailed") || strstr(err, "PackageInspectionFailed"))
        return "→ File IPA hỏng hoặc không đúng cấu trúc Payload/*.app.";
    if (strstr(err, "MismatchedApplicationIdentifierEntitlement"))
        return "→ Đã có bản app cùng bundle id nhưng ký bởi team khác — gỡ bản cũ trên iPhone rồi cài lại.";
    if (strstr(err, "ApplicationSandbox") || strstr(err, "Entitlement"))
        return "→ Entitlements trong app không khớp provisioning profile.";
    if (strstr(err, "DeveloperMode")) return "→ Bật Chế độ nhà phát triển (Cài đặt > Quyền riêng tư & Bảo mật).";
    return "";
}

static bool afc_upload(afc_client_t afc, const char *local, const char *remote) {
    FILE *f = fopen(local, "rb");
    if (!f) { emitf("[afc] ❌ Không mở được %s: %s", local, strerror(errno)); return false; }
    struct stat st;
    uint64_t total = (fstat(fileno(f), &st) == 0) ? (uint64_t)st.st_size : 0;
    uint64_t handle = 0;
    if (afc_file_open(afc, remote, AFC_FOPEN_WRONLY, &handle) != AFC_E_SUCCESS || !handle) {
        emitf("[afc] ❌ afc_file_open(%s) thất bại", remote);
        fclose(f);
        return false;
    }
    char *buf = malloc(AFC_CHUNK);
    if (!buf) { afc_file_close(afc, handle); fclose(f); return false; }
    uint64_t sent = 0;
    int last_pct = -1;
    bool ok = true;
    size_t n;
    uint64_t t0 = now_ms();
    while (ok && (n = fread(buf, 1, AFC_CHUNK, f)) > 0) {
        uint32_t done = 0;
        while (done < n) {
            uint32_t w = 0;
            afc_error_t ae = afc_file_write(afc, handle, buf + done, (uint32_t)(n - done), &w);
            if (ae != AFC_E_SUCCESS || w == 0) {
                emitf("[afc] ❌ afc_file_write lỗi %d sau %llu byte", (int)ae, (unsigned long long)sent);
                ok = false;
                break;
            }
            done += w;
        }
        sent += done;
        if (total > 0) {
            int pct = (int)(sent * 100 / total);
            if (pct / 10 != last_pct / 10) {
                last_pct = pct;
                emitf("[afc] Đang chép IPA... %d%% (%.1f/%.1f MB)", pct, sent / 1048576.0, total / 1048576.0);
            }
        }
    }
    free(buf);
    afc_file_close(afc, handle);
    fclose(f);
    if (ok) {
        double secs = (now_ms() - t0) / 1000.0;
        emitf("[afc] ✅ Đã chép %.1f MB trong %.1f s", sent / 1048576.0, secs);
    }
    return ok;
}

JNIEXPORT jboolean JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeSideload(JNIEnv *env, jobject obj, jstring jipaPath) {
    (void)obj;
    const char *ipa = (*env)->GetStringUTFChars(env, jipaPath, NULL);
    char ipa_path[1024];
    snprintf(ipa_path, sizeof(ipa_path), "%s", ipa ? ipa : "");
    if (ipa) (*env)->ReleaseStringUTFChars(env, jipaPath, ipa);

    pthread_mutex_lock(&g_api);
    bool success = false;
    afc_client_t afc = NULL;
    instproxy_client_t ipc = NULL;
    char remote[600];

    emitf("[sideload] IPA: %s", ipa_path);
    if (!g_device) {
        emit_log("[sideload] ❌ Chưa kết nối iPhone");
        goto done;
    }
    if (!g_paired) {
        lockdownd_client_t hs = NULL;
        if (lockdownd_client_new_with_handshake(g_device, &hs, CLIENT_LABEL) == LOCKDOWN_E_SUCCESS) {
            if (g_lockdown) lockdownd_client_free(g_lockdown);
            g_lockdown = hs;
            g_paired = true;
        } else {
            emit_log("[sideload] ❌ iPhone chưa ghép nối (Tin cậy) — chạy Ghép nối trước");
            goto done;
        }
    }

    afc_error_t ae = afc_client_start_service(g_device, &afc, CLIENT_LABEL);
    if (ae != AFC_E_SUCCESS || !afc) {
        emitf("[afc] ❌ Không mở được dịch vụ AFC (lỗi %d)", (int)ae);
        afc = NULL;
        goto done;
    }
    {
        char **info = NULL;
        if (afc_get_file_info(afc, "PublicStaging", &info) != AFC_E_SUCCESS)
            afc_make_directory(afc, "PublicStaging");
        if (info) afc_dictionary_free(info);
    }
    {
        const char *base = strrchr(ipa_path, '/');
        base = base ? base + 1 : ipa_path;
        snprintf(remote, sizeof(remote), "PublicStaging/%s", base);   /* không có '/' đầu, như ideviceinstaller */
    }
    if (!afc_upload(afc, ipa_path, remote)) goto done;

    instproxy_error_t pe = instproxy_client_start_service(g_device, &ipc, CLIENT_LABEL);
    if (pe != INSTPROXY_E_SUCCESS || !ipc) {
        emitf("[instproxy] ❌ Không mở được installation_proxy (lỗi %d)", (int)pe);
        ipc = NULL;
        goto done;
    }

    struct install_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    pthread_mutex_init(&ctx.mtx, NULL);
    plist_t opts = instproxy_client_options_new();   /* .ipa: KHÔNG PackageType=Developer */
    emit_log("[instproxy] Đang cài đặt...");
    pe = instproxy_install(ipc, remote, opts, install_status_cb, &ctx);
    instproxy_client_options_free(opts);
    if (pe != INSTPROXY_E_SUCCESS) {
        emitf("[instproxy] ❌ instproxy_install lỗi %d", (int)pe);
        pthread_mutex_destroy(&ctx.mtx);
        goto done;
    }
    uint64_t deadline = now_ms() + INSTALL_TIMEOUT_MS;
    int last_pct = -1;
    char last_status[64] = {0};
    for (;;) {
        pthread_mutex_lock(&ctx.mtx);
        int d = ctx.done, pct = ctx.percent;
        char st[64];
        snprintf(st, sizeof(st), "%s", ctx.status);
        pthread_mutex_unlock(&ctx.mtx);
        if (st[0] && (strcmp(st, last_status) != 0 || pct / 20 != last_pct / 20)) {
            emitf("[instproxy] %s (%d%%)", st, pct);
            snprintf(last_status, sizeof(last_status), "%s", st);
            last_pct = pct;
        }
        if (d) break;
        if (now_ms() > deadline) { emit_log("[instproxy] ❌ Quá thời gian chờ cài đặt"); break; }
        sleep_ms(300);
    }
    instproxy_client_free(ipc);   /* join luồng trạng thái của libimobiledevice */
    ipc = NULL;
    if (ctx.done && ctx.ok) {
        emit_log("[instproxy] ✅ Cài đặt thành công!");
        success = true;
    } else if (ctx.err_name[0]) {
        emitf("[instproxy] ❌ Cài đặt thất bại: %s — %s", ctx.err_name, ctx.err_desc);
        const char *hint = install_hint(ctx.err_name);
        if (!hint[0]) hint = install_hint(ctx.err_desc);
        if (hint[0]) emit_log(hint);
    }
    pthread_mutex_destroy(&ctx.mtx);

done:
    if (ipc) instproxy_client_free(ipc);
    if (afc) {
        if (remote[0] && success) afc_remove_path(afc, remote);
        afc_client_free(afc);
    }
    pthread_mutex_unlock(&g_api);
    return success ? JNI_TRUE : JNI_FALSE;
}

/* ── Getters / tiện ích ─────────────────────────────────────────────────── */
JNIEXPORT jstring JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeGetUdid(JNIEnv *env, jobject obj) {
    (void)obj;
    return is_real_udid(g_udid) ? (*env)->NewStringUTF(env, g_udid) : NULL;
}

JNIEXPORT jboolean JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeIsPaired(JNIEnv *env, jobject obj) {
    (void)env; (void)obj;
    return g_paired ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeIsConnected(JNIEnv *env, jobject obj) {
    (void)env; (void)obj;
    return (g_device && g_lockdown && usbmuxd_server_device_state() == 2) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeGetConnectionState(JNIEnv *env, jobject obj) {
    (void)env; (void)obj;
    if (!g_device) return 0;
    if (!g_lockdown) return 1;
    if (!g_paired) return 2;
    return 3;
}

JNIEXPORT jstring JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeGetPairingPlist(JNIEnv *env, jobject obj) {
    (void)obj;
    if (!is_real_udid(g_udid)) return NULL;
    char *data = NULL;
    uint32_t size = 0;
    if (usbmuxd_read_pair_record(g_udid, &data, &size) != 0 || !data || !size) { free(data); return NULL; }
    char *z = malloc(size + 1);
    if (!z) { free(data); return NULL; }
    memcpy(z, data, size);
    z[size] = 0;
    free(data);
    jstring r = (*env)->NewStringUTF(env, z);
    free(z);
    return r;
}

/* ═════════════════════════════════════════════════════════════════════════
 * v56 — Quản lý file ghép nối (.mobiledevicepairing) cho SideStore /
 * LiveContainer… (học từ iLoader — github.com/nab138/iloader, src/pairing.rs)
 *
 * Định dạng file: XML plist của pair record HIỆN CÓ (SystemBUID, HostID,
 * RootCertificate, RootPrivateKey, DeviceCertificate…) + khóa UDID của máy
 * (iLoader: pairing_file.udid = udid) — đúng định dạng AltStore/SideStore
 * dùng (ALTPairingFile.mobiledevicepairing).
 *
 * Nhúng vào app đã cài: house_arrest → VendDocuments(bundle_id) → AFC ghi
 * file vào Documents của app (iLoader place_file()).
 * ═════════════════════════════════════════════════════════════════════════ */

static char *build_pairing_file_xml(uint32_t *out_len) {
    if (!is_real_udid(g_udid)) return NULL;
    char *data = NULL;
    uint32_t size = 0;
    if (usbmuxd_read_pair_record(g_udid, &data, &size) != 0 || !data || !size) {
        free(data);
        return NULL;
    }
    plist_t pl = NULL;
    plist_format_t fmt = PLIST_FORMAT_XML;
    if (plist_from_memory(data, size, &pl, &fmt) != PLIST_ERR_SUCCESS || !pl) {
        free(data);
        return NULL;
    }
    free(data);
    if (plist_dict_get_item(pl, "UDID") == NULL) {
        plist_dict_set_item(pl, "UDID", plist_new_string(g_udid));
    }
    char *xml = NULL;
    uint32_t len = 0;
    if (plist_to_xml(pl, &xml, &len) != PLIST_ERR_SUCCESS || !xml) {
        plist_free(pl);
        return NULL;
    }
    plist_free(pl);
    if (out_len) *out_len = len;
    return xml;
}

/* Nội dung file ghép nối hiện tại (XML plist + UDID) — cho nút Xuất file. */
JNIEXPORT jstring JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeGetPairingFile(JNIEnv *env, jobject obj) {
    (void)obj;
    char *xml = build_pairing_file_xml(NULL);
    if (!xml) return NULL;
    jstring r = (*env)->NewStringUTF(env, xml);
    free(xml);
    return r;
}

/*
 * Ghi file ghép nối vào Documents của một app ĐÃ CÀI (SideStore,
 * LiveContainer…). rel_path tính từ gốc Documents của app, vd:
 *   SideStore            → "ALTPairingFile.mobiledevicepairing"
 *   LiveContainer        → "SideStore/Documents/ALTPairingFile.mobiledevicepairing"
 * Gọi sau khi instproxy_install thành công, khi app đã chịu chữ ký
 * development (house_arrest chỉ hoạt động với app development-signed).
 */
JNIEXPORT jboolean JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeWritePairingFileToApp(
        JNIEnv *env, jobject obj, jstring j_bundle, jstring j_rel) {
    (void)obj;
    const char *bundle = j_bundle ? (*env)->GetStringUTFChars(env, j_bundle, NULL) : NULL;
    const char *rel    = j_rel    ? (*env)->GetStringUTFChars(env, j_rel, NULL)    : NULL;
    jboolean ok = JNI_FALSE;
    house_arrest_client_t ha = NULL;
    afc_client_t afc = NULL;
    char *xml = NULL;

    pthread_mutex_lock(&g_api);
    do {
        if (!bundle || !rel || !*bundle || !*rel) {
            emit_log("[pairing] ❌ Thiếu bundle id / đường dẫn file");
            break;
        }
        if (!g_device || !g_paired) {
            emit_log("[pairing] ❌ iPhone chưa kết nối / chưa ghép nối");
            break;
        }
        xml = build_pairing_file_xml(NULL);
        if (!xml) {
            emit_log("[pairing] ❌ Chưa có pair record — ghép nối iPhone trước");
            break;
        }

        house_arrest_error_t he = house_arrest_client_start_service(g_device, &ha, CLIENT_LABEL);
        if (he != HOUSE_ARREST_E_SUCCESS || !ha) {
            emitf("[pairing] ❌ Không mở được house_arrest (lỗi %d)", (int)he);
            break;
        }

        he = house_arrest_send_command(ha, "VendDocuments", bundle);
        plist_t res = NULL;
        if (he == HOUSE_ARREST_E_SUCCESS) he = house_arrest_get_result(ha, &res);
        if (he == HOUSE_ARREST_E_SUCCESS && res) {
            plist_t err_item = plist_dict_get_item(res, "Error");
            if (err_item) {
                char *es = NULL;
                plist_get_string_val(err_item, &es);
                emitf("[pairing] ❌ iPhone từ chối truy cập Documents của %s (%s)",
                      bundle, es ? es : "?");
                free(es);
                he = HOUSE_ARREST_E_UNKNOWN_ERROR;
            }
        }
        if (res) plist_free(res);
        if (he != HOUSE_ARREST_E_SUCCESS) {
            emitf("[pairing] ❌ VendDocuments(%s) thất bại (lỗi %d)", bundle, (int)he);
            break;
        }

        if (afc_client_new_from_house_arrest_client(ha, &afc) != AFC_E_SUCCESS || !afc) {
            emit_log("[pairing] ❌ Không chuyển sang chế độ AFC được");
            break;
        }

        /*
         * iLoader place_file(): gốc AFC sau VendDocuments là DATA CONTAINER
         * của app — file phải ghi vào /Documents/<rel_path>, không phải "/"
         * (v56 ghi nhầm ở gốc → afc_file_open bị từ chối). Luôn tạo sẵn
         * /Documents + các thư mục cha, bỏ qua lỗi "đã tồn tại".
         */
        char remote[512];
        snprintf(remote, sizeof(remote), "/Documents/%s", rel);
        for (char *p = remote + 1; *p; p++) {
            if (*p == '/') {
                *p = 0;
                afc_make_directory(afc, remote);
                *p = '/';
            }
        }
        afc_make_directory(afc, "/Documents");

        uint64_t handle = 0;
        afc_error_t afe = afc_file_open(afc, remote, AFC_FOPEN_WR, &handle);
        if (afe != AFC_E_SUCCESS || !handle) {
            emitf("[pairing] ❌ afc_file_open(%s) thất bại (AFC lỗi %d)", remote, (int)afe);
            break;
        }
        uint32_t total = (uint32_t)strlen(xml);
        uint32_t done = 0;
        bool werr = false;
        while (done < total) {
            uint32_t w = 0;
            if (afc_file_write(afc, handle, xml + done, total - done, &w) != AFC_E_SUCCESS || w == 0) {
                emitf("[pairing] ❌ afc_file_write thất bại (đã ghi %u/%u)", done, total);
                werr = true;
                break;
            }
            done += w;
        }
        afc_file_close(afc, handle);
        if (!werr && done == total) {
            emitf("[pairing] ✅ Đã ghi file ghép nối vào Documents của %s", bundle);
            ok = JNI_TRUE;
        }
    } while (0);

    /*
     * LƯU Ý QUAN TRỌNG (tránh double-free): afc_client được tạo từ house_arrest
     * DÙNG CHUNG service connection với house_arrest client. Chỉ được free
     * MỘT trong hai — ở đây free house_arrest (sạch chuỗi service + connection).
     * Struct afc (~vài chục byte) không free — chấp nhận cho lần gọi hiếm.
     */
    if (ha) house_arrest_client_free(ha);
    free(xml);
    pthread_mutex_unlock(&g_api);
    if (bundle) (*env)->ReleaseStringUTFChars(env, j_bundle, bundle);
    if (rel) (*env)->ReleaseStringUTFChars(env, j_rel, rel);
    return ok;
}

JNIEXPORT void JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeReset(JNIEnv *env, jobject obj) {
    (void)env; (void)obj;
    g_cancel = 1;
    pthread_mutex_lock(&g_api);
    free_ld_device();
    usbmuxd_server_stop();
    usb_bridge_close();
    g_session_fd = -1;
    g_udid[0] = 0;
    g_product_id = 0;
    pthread_mutex_unlock(&g_api);
    emit_log("[jni] Đã đóng phiên USB/usbmuxd");
}

JNIEXPORT jstring JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeListInstalledApps(JNIEnv *env, jobject obj) {
    (void)obj;
    pthread_mutex_lock(&g_api);
    jstring result = NULL;
    instproxy_client_t ip = NULL;
    if (!g_device || !g_paired) {
        emit_log("[jni] nativeListInstalledApps: iPhone chưa kết nối/ghép nối");
        goto out;
    }
    if (instproxy_client_start_service(g_device, &ip, CLIENT_LABEL) != INSTPROXY_E_SUCCESS || !ip) {
        emit_log("[jni] nativeListInstalledApps: không mở được installation_proxy");
        goto out;
    }
    plist_t opts = instproxy_client_options_new();
    instproxy_client_options_add(opts, "ApplicationType", "User", NULL);
    instproxy_client_options_set_return_attributes(opts, "CFBundleIdentifier", "CFBundleDisplayName", NULL);
    plist_t apps = NULL;
    instproxy_error_t ie = instproxy_browse(ip, opts, &apps);
    instproxy_client_options_free(opts);
    if (ie == INSTPROXY_E_SUCCESS && apps) {
        char *xml = NULL; uint32_t len = 0;
        plist_to_xml(apps, &xml, &len);
        if (xml && len) result = (*env)->NewStringUTF(env, xml);
        free(xml);
    }
    if (apps) plist_free(apps);
out:
    if (ip) instproxy_client_free(ip);
    pthread_mutex_unlock(&g_api);
    return result;
}

JNIEXPORT jstring JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeDiagnostics(JNIEnv *env, jobject obj) {
    (void)obj;
    static const char *states[] = { "dừng", "đang bắt tay", "SẴN SÀNG", "mất kết nối" };
    int st = usbmuxd_server_device_state();
    const char *addr = getenv("USBMUXD_SOCKET_ADDRESS");
    char buf[1400];
    snprintf(buf, sizeof(buf),
        "=== Chẩn đoán native (v49) ===\n"
        "usbmuxd nội bộ: %s (%s)\n"
        "USB: config=%d iface=%d ep_in=0x%02x ep_out=0x%02x maxpkt=%d %s\n"
        "serial USB: %s\n"
        "iPhone: %s — iOS %s\n"
        "UDID: %s\n"
        "idevice: %s | lockdown: %s | đã ghép nối: %s\n"
        "pair record: %s/%s.plist\n",
        (st >= 0 && st <= 3) ? states[st] : "?", addr ? addr : "(chưa đặt)",
        usb_bridge_active_config(), usb_bridge_interface(), usb_bridge_ep_in(), usb_bridge_ep_out(),
        usb_bridge_max_packet_size(),
        usb_bridge_using_android_mode() ? "(bulkTransfer Android)" : (usb_bridge_iface_claimed() ? "(libusb)" : "(chưa claim)"),
        usb_bridge_serial() ? usb_bridge_serial() : "(không đọc được)",
        g_device_name[0] ? g_device_name : "?", g_ios_version[0] ? g_ios_version : "?",
        g_udid[0] ? g_udid : "(chưa biết)",
        g_device ? "OK" : "—", g_lockdown ? "OK" : "—", g_paired ? "có" : "chưa",
        usbmuxd_server_config_dir() ? usbmuxd_server_config_dir() : "?", g_udid[0] ? g_udid : "<udid>");
    return (*env)->NewStringUTF(env, buf);
}
