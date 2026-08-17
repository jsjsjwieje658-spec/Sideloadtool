/**
 * jni_bridge_imd.c — JNI bridge dùng libimobiledevice API thật (Mode 1)
 *
 * ═══════════════════════════════════════════════════════════════════
 *  FIX v20 — Các lỗi quan trọng đã sửa:
 * ═══════════════════════════════════════════════════════════════════
 *  1. RACE CONDITION: usbmuxd_server_start() tạo thread, socket chưa
 *     bind xong khi idevice_new_with_options() gọi ngay sau đó.
 *     Fix: thêm retry loop trong nativeConnect() chờ socket sẵn sàng.
 *
 *  2. SERVER KHÔNG START: usbmuxd_server.c/h bị THIẾU hoàn toàn →
 *     usbmuxd_server_socket_path() luôn trả NULL → warning + fail.
 *     Fix: thêm file usbmuxd_server.c/h đầy đủ.
 *
 *  3. nativeConnect() không kiểm tra usbmuxd_server_socket_path()
 *     kỹ đủ → vẫn tiếp tục gọi idevice_new_with_options() dù server
 *     chưa ready → err=-3 (IDEVICE_E_NO_DEVICE).
 *     Fix: Wait-for-socket loop + bail nếu không có fd bridge.
 *
 *  4. usb_fd_bridge.c/h bị THIẾU hoàn toàn → Mode 1 không compile.
 *     Fix: thêm usb_fd_bridge.c/h đầy đủ.
 *
 *  5. nativeConnect(): idevice_new_with_options retry khi server chưa
 *     có UDID thật → restart server sau idevice_get_udid() gây race.
 *     Fix: cập nhật UDID qua usbmuxd_server_update_udid() (không restart).
 * ═══════════════════════════════════════════════════════════════════
 *
 * ═══════════════════════════════════════════════════════════════════
 * FIX runtime Bug B: nativeSetUsbFd() chỉ khởi tạo libusb và server;
 * version exchange được trì hoãn đến Connect đầu tiên tới lockdown, đúng
 * thứ tự mà libimobiledevice/usbmuxd upstream dùng cho một USB session.
 * ═══════════════════════════════════════════════════════════════════
 */
#include <jni.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <android/log.h>

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/afc.h>
#include <libimobiledevice/installation_proxy.h>
#include <plist/plist.h>

#include "usb_fd_bridge.h"
#include "usbmuxd_server.h"
#include "android_usbmuxd_fix.h"

#define LOG_TAG "jni_imd"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Kích thước chunk khi ghi file qua AFC */
#define AFC_CHUNK_SIZE (256 * 1024)

/* Timeout ghép nối — iPhone cần thời gian hiện trust popup */
#define PAIR_RETRY_MAX       20
#define PAIR_RETRY_SLEEP_MS  2000

/*
 * FIX: Thời gian chờ usbmuxd server socket sẵn sàng.
 * Server start() chỉ tạo thread — socket có thể chưa bind xong.
 * Chờ tối đa 3 giây (30 × 100ms).
 */
/* FIX BONUS: Tăng timeout chờ server socket sẵn sàng từ 3s → 6s.
 * Trên Android low-end (RAM thấp, I/O chậm), thread server cần đến 4-5s
 * để bind socket và bắt đầu accept. 30 retries × 100ms = 3s là không đủ. */
#define SOCKET_READY_RETRIES  60   /* tăng từ 30 → 60 (6s tổng) */
#define SOCKET_READY_SLEEP_MS 100

/* ── Global state ────────────────────────────────────────────────────────── */
static idevice_t          g_device   = NULL;
static lockdownd_client_t g_lockdown = NULL;
static char               g_udid[64] = {0};
static char               g_files_dir[512] = {0};
static bool               g_paired   = false;
static int                g_product_id = 0;

/* A USB mux handle can be used before lockdown reveals the real UDID, but
 * neither the all-zero UUID nor a synthetic label is a device UDID. */
static bool is_real_udid(const char *value) {
    if (!value || !value[0]) return false;
    size_t n = strlen(value);
    if (n < 16 || n >= sizeof(g_udid)) return false;
    bool has_nonzero = false;
    for (size_t i = 0; i < n; i++) {
        char c = value[i];
        if (c != '0' && c != '-') has_nonzero = true;
    }
    return has_nonzero && strcmp(value, "pending-device") != 0;
}

/* ── JNI helpers ──
───────────────────────────────────────────────────────── */
static JavaVM *g_jvm = NULL;
static jobject g_bridge_obj = NULL;

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

static void emit_log(const char *msg) {
    LOGI("%s", msg);
    if (!g_jvm || !g_bridge_obj) return;
    JNIEnv *env = NULL;
    bool detach = false;
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        (*g_jvm)->AttachCurrentThread(g_jvm, (void **)&env, NULL);
        detach = true;
    }
    /* Worker threads cannot reliably use FindClass with the app class
     * loader. Resolve the class from the global bridge object instead. */
    jclass cls = g_bridge_obj ? (*env)->GetObjectClass(env, g_bridge_obj) : NULL;
    if (cls) {
        jmethodID mid = (*env)->GetStaticMethodID(env, cls, "onNativeLog",
                                                   "(Ljava/lang/String;)V");
        if (mid) {
            jstring jmsg = (*env)->NewStringUTF(env, msg);
            if (jmsg) {
                (*env)->CallStaticVoidMethod(env, cls, mid, jmsg);
                (*env)->DeleteLocalRef(env, jmsg);
            }
        }
        (*env)->DeleteLocalRef(env, cls);
    }
    if (detach) (*g_jvm)->DetachCurrentThread(g_jvm);
}

/* Forward logs emitted by usbmuxd_server worker threads to the same UI sink. */
static void server_log_callback(const char *msg) {
    emit_log(msg);
}

/* ── nativeInit ─────────────────────────────────────────────────────────── */
JNIEXPORT void JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeInit(
        JNIEnv *env, jobject obj, jstring filesDir) {
    if (g_bridge_obj) {
        (*env)->DeleteGlobalRef(env, g_bridge_obj);
    }
    g_bridge_obj = (*env)->NewGlobalRef(env, obj);
    const char *dir = (*env)->GetStringUTFChars(env, filesDir, NULL);
    strncpy(g_files_dir, dir, sizeof(g_files_dir) - 1);
    (*env)->ReleaseStringUTFChars(env, filesDir, dir);
    android_usbmuxd_fix_set_log_callback(server_log_callback);
    emit_log("[jni] Mode 1: libimobiledevice thật + usbmuxd server nội bộ");
    /*
     * FIX v36: Banner phiên bản để user xác nhận đang chạy APK mới nhất.
     * Khi gặp lỗi, user nhìn lên đầu log sẽ biết ngay phiên bản nào đang chạy.
     */
    emit_log("[jni] ═══════════════════════════════════════════════════════");
    emit_log("[jni]   SideloadTool native v36 (2026-08-17)");
    emit_log("[jni]   Fixes: v33 eager+clear, v34 log_hex overflow,");
    emit_log("[jni]          v35 libusb error codes, v36 UI log forwarding");
    emit_log("[jni] ═══════════════════════════════════════════════════════");
    LOGI("nativeInit: files_dir=%s", g_files_dir);
}

/* ── nativeSetUsbFd ─────────────────────────────────────────────────────── */
/**
 * Nhận Android USB fd, khởi tạo libusb, discover endpoints,
 * rồi khởi động usbmuxd server nội bộ.
 *
 * FIX v20: Thứ tự đúng:
 *   1. usb_bridge_init_from_fd() → libusb handle + ep_in/ep_out
 *   2. usbmuxd_server_start()    → Unix socket + server thread
 *   (Không restart server sau khi biết UDID — dùng update_udid() thay thế)
 */
JNIEXPORT jboolean JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeSetUsbFd(
        JNIEnv *env, jobject obj, jint fd, jint vendorId, jint productId,
        jstring udidHint) {
    (void)obj;

    /* nativeSetUsbFd() bắt đầu một USB session mới. Dọn toàn bộ state cũ
     * trước khi libusb nhận fd mới; nếu không, server/tunnel cũ có thể giữ
     * packet và làm lockdownd trả LOCKDOWN_E_MUX_ERROR (-8). */
    if (g_lockdown) { lockdownd_client_free(g_lockdown); g_lockdown = NULL; }
    if (g_device)   { idevice_free(g_device); g_device = NULL; }
    usbmuxd_server_stop();
    usb_bridge_close();
    g_paired = false;
    g_udid[0] = '\0';

    if (udidHint) {
        const char *hint = (*env)->GetStringUTFChars(env, udidHint, NULL);
        if (hint && is_real_udid(hint)) {
            strncpy(g_udid, hint, sizeof(g_udid) - 1);
            g_udid[sizeof(g_udid) - 1] = '\0';
        }
        if (hint) (*env)->ReleaseStringUTFChars(env, udidHint, hint);
    }
    g_product_id = (int)productId;

    char buf[256];
    snprintf(buf, sizeof(buf),
             "[usb] libusb_wrap_sys_device(fd=%d, vid=0x%04x, pid=0x%04x)",
             (int)fd, (int)vendorId, (int)productId);
    emit_log(buf);

    /* Bước 1: Khởi tạo libusb với Android USB fd */
    bool ok = usb_bridge_init_from_fd((int)fd, (int)vendorId, (int)productId);
    if (!ok) {
        emit_log("[usb] \u274c libusb_wrap_sys_device() thất bại — kiểm tra quyền USB Host");
        return JNI_FALSE;
    }

    snprintf(buf, sizeof(buf),
             "[usb] \u2705 libusb ready: ep_in=0x%02x ep_out=0x%02x",
             usb_bridge_ep_in(), usb_bridge_ep_out());
    emit_log(buf);

    /*
     * Không thực hiện version exchange tại thời điểm nhận fd.
     *
     * termux-usbmuxd giữ raw Android USB fd và để usbmuxd thực hiện
     * handshake khi client gửi yêu cầu Connect tới service thực tế. Nếu
     * gửi version packet ngay tại đây, iPhone chưa có một session mux
     * được mở và có thể bỏ qua/stall bulk endpoint; kết quả là app chỉ
     * retry version exchange và không bao giờ tới lockdown để hiện Trust.
     *
     * usbmuxd_server_start() sẽ phục vụ ListDevices/Connect. Lần Connect
     * đầu tiên đi qua do_usb_v1_connect(), nơi usbmux_version_exchange()
     * được gọi đúng thời điểm trước SYN tới port lockdown 62078.
     */
    usbmuxd_server_reset_version_state();
    emit_log("[usbmux] Đã giữ raw USB fd; trì hoãn version exchange đến lúc Connect lockdown...");

    /*
     * FIX v33: Eager version exchange "best-effort" ngay sau khi USB bridge
     * sẵn sàng — KHÔNG bắt buộc phải thành công ở đây.
     *
     * Lý do thêm bước eager này:
     *   - Upstream usbmuxd gửi VERSION NGAY khi device_add() được gọi (ngay
     *     sau USB enumeration). iPhone có "mux session window" — nếu không
     *     nhận được VERSION trong vài giây đầu, iPhone có thể vào trạng thái
     *     "idle" và bỏ qua VERSION packet gửi sau đó.
     *   - Code cũ CHỈ gửi VERSION khi Connect tới socket (lazy) → quá muộn
     *     → iPhone đã idle → VERSION không được phản hồi → fail 5/5.
     *   - Eager attempt ở đây cho iPhone cơ hội nhận VERSION sớm. Nếu fail,
     *     không sao — lazy attempt trong do_usb_v1_connect() sẽ retry với
     *     clear_halt đã được apply từ Fix v33.
     *
     * Quan trọng:KHÔNG return false nếu eager fail — vẫn cho phép server
     * start và để lazy attempt xử lý.
     */
    {
        emit_log("[usbmux] Eager version exchange (best-effort, non-blocking)...");
        bool eager_ok = usbmux_version_exchange();
        if (eager_ok) {
            emit_log("[usbmux] ✅ Eager version exchange OK — iPhone đã sẵn sàng mux session");
        } else {
            emit_log("[usbmux] ⚠️ Eager version exchange fail — sẽ retry lazily khi Connect tới");
            /* Reset state để lazy attempt trong do_usb_v1_connect() có cơ hội retry */
            usbmuxd_server_reset_version_state();
        }
    }

    /*
     * Bước 2: Khởi động usbmuxd server nội bộ.
     *
     * Khi chưa mở lockdown, chỉ dùng nhãn transport `pending-device`;
     * UDID thật sẽ được đọc từ khóa UniqueDeviceID sau khi lockdown mở.
     * Khi có UDID thật, cập nhật server mà không restart để tránh race.
     */
    const char *server_identity = g_udid[0] ? g_udid : "pending-device";
    bool srv = usbmuxd_server_start(
        g_files_dir,
        server_identity,
        (int)productId
    );

    if (srv) {
        snprintf(buf, sizeof(buf),
                 "[usbmuxd_srv] \u2705 Server listening: %s (identity: %s)",
                 usbmuxd_server_socket_path(), g_udid[0] ? g_udid : "pending; chưa lấy từ lockdown");
        emit_log(buf);
        /* Set env var ngay lập tức */
        setenv("USBMUXD_SOCKET_ADDRESS", usbmuxd_server_socket_path(), 1);
    } else {
        emit_log("[usbmuxd_srv] \u274c Không khởi động được server — kiểm tra filesDir và quyền ghi");
        return JNI_FALSE;
    }

    return JNI_TRUE;
}

/* ── nativeConnect ──────────────────────────────────────────────────────── */
/**
 * FIX v20 race condition: Sau khi server start(), socket bind có thể
 * chưa hoàn tất. Chờ socket accessible trước khi gọi libimobiledevice.
 *
 * Cũng đảm bảo USBMUXD_SOCKET_ADDRESS đã set đúng.
 */
JNIEXPORT jboolean JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeConnect(
        JNIEnv *env, jobject obj) {
    (void)env; (void)obj;

    // Giải phóng các tài nguyên cũ trước khi kết nối lại
    if (g_lockdown) { lockdownd_client_free(g_lockdown);   g_lockdown = NULL; }
    if (g_device)   { idevice_free(g_device);              g_device   = NULL; }

    /* ── FIX: Kiểm tra usb_bridge đã init chưa ── */
    if (!usb_bridge_ep_in() || !usb_bridge_ep_out()) {
        emit_log("[imd] \u274c USB bridge chưa khởi tạo — gọi nativeSetUsbFd() trước");
        return JNI_FALSE;
    }

    /* ── FIX: Đảm bảo usbmuxd server đang chạy ── */
    const char *sock_path = usbmuxd_server_socket_path();
    if (!sock_path) {
        emit_log("[imd] \u274c usbmuxd server chưa chạy — gọi nativeSetUsbFd() trước");
        return JNI_FALSE;
    }

    /* Luôn set lại env var (có thể bị xóa bởi system) */
    setenv("USBMUXD_SOCKET_ADDRESS", sock_path, 1);
    {
        char buf[300];
        snprintf(buf, sizeof(buf), "[imd] USBMUXD_SOCKET_ADDRESS=%s", sock_path);
        emit_log(buf);
    }

    /*
     * ── FIX RACE CONDITION: Chờ socket sẵn sàng ──
     *
     * usbmuxd_server_start() tạo thread và return. Thread đó mới bind socket.
     * Có thể có race nếu idevice_new_with_options() gọi trước khi bind xong.
     * Thử kết nối thực tế vào socket để xác nhận ready.
     */
    {
        char buf[128];
        int ready = 0;
        for (int i = 0; i < SOCKET_READY_RETRIES && !ready; i++) {
            int test_fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (test_fd >= 0) {
                struct sockaddr_un sa;
                memset(&sa, 0, sizeof(sa));
                sa.sun_family = AF_UNIX;
                strncpy(sa.sun_path, sock_path, sizeof(sa.sun_path) - 1);
                if (connect(test_fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
                    ready = 1;
                }
                close(test_fd);
            }
            if (!ready) {
                snprintf(buf, sizeof(buf),
                         "[imd] Chờ usbmuxd socket ready... (%d/%d)",
                         i+1, SOCKET_READY_RETRIES);
                emit_log(buf);
                usleep(SOCKET_READY_SLEEP_MS * 1000);
            }
        }
        if (!ready) {
            emit_log("[imd] \u274c usbmuxd socket không accessible sau 3 giây");
            return JNI_FALSE;
        }
        emit_log("[imd] usbmuxd socket ready \u2705");
    }

    emit_log("[imd] idevice_new_with_options(USBMUX)...");

    /*
     * Thử idevice_new_with_options() với retry nếu server mới start.
     * Lỗi -3 (IDEVICE_E_NO_DEVICE) có thể xảy ra lần đầu nếu server
     * chưa kịp process ListDevices request.
     */
    idevice_error_t err = IDEVICE_E_UNKNOWN_ERROR;
    for (int attempt = 0; attempt < 3; attempt++) {
        enum idevice_options opts = IDEVICE_LOOKUP_USBMUX;
        err = idevice_new_with_options(&g_device, NULL, opts);
        if (err == IDEVICE_E_SUCCESS) break;
        {
            char msg[200];
            snprintf(msg, sizeof(msg),
                     "[imd] idevice_new_with_options() err=%d (lần %d/3)",
                     (int)err, attempt + 1);
            emit_log(msg);
        }
        if (attempt < 2) usleep(500000);  /* chờ 500ms */
    }

    if (err != IDEVICE_E_SUCCESS) {
        char msg[200];
        snprintf(msg, sizeof(msg),
                 "[imd] \u274c idevice_new_with_options() err=%d "
                 "(màn hình iPhone đã mở khoá? cáp USB kết nối chắc chưa?)",
                 (int)err);
        emit_log(msg);
        return JNI_FALSE;
    }

    emit_log("[imd] \u2705 idevice OK");

    /* Không gọi idevice_get_udid() ở đây: upstream chỉ trả lại chuỗi đã
     * copy từ usbmuxd_get_device(), trước đó chỉ là identity pending. UDID
     * thật chỉ được đọc từ lockdown sau khi transport mở thành công. */
    if (!g_udid[0]) {
        emit_log("[imd] UDID đang pending; chưa coi identity tạm là UDID thật");
    }

    /* Chỉ mở lockdownd client thuần, chưa handshake/TLS.
     * lockdownd_client_new_with_handshake() cố validate pair record ngay
     * trong lần connect đầu; khi record cũ/stale hoặc chưa tồn tại, thư viện
     * quy đổi lỗi transport thành LOCKDOWN_E_MUX_ERROR (-8), khiến UI không
     * bao giờ đi tới bước Pair/Trust. Pairing được thực hiện riêng trong
     * nativePair() trên chính client này. */
    emit_log("[lockdown] Mở lockdownd client (no-TLS, chờ Pair)...");
    lockdownd_error_t ld_err = lockdownd_client_new(
            g_device, &g_lockdown, "sideloadtool");
    if (ld_err != LOCKDOWN_E_SUCCESS || !g_lockdown) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "[lockdown] ❌ lockdownd_client_new() err=%d",
                 (int)ld_err);
        emit_log(msg);
        g_lockdown = NULL;
        idevice_free(g_device);
        g_device = NULL;
        return JNI_FALSE;
    }

    /* Lấy UniqueDeviceID thật từ lockdown, không lấy từ placeholder trong
     * usbmuxd_device_info_t. Nếu iPhone chưa cho đọc, vẫn giữ session để
     * nativePair() có thể yêu cầu Trust. */
    plist_t unique_value = NULL;
    lockdownd_error_t value_err = lockdownd_get_value(
            g_lockdown, NULL, "UniqueDeviceID", &unique_value);
    char *real_udid = NULL;
    if (value_err == LOCKDOWN_E_SUCCESS && unique_value) {
        plist_get_string_val(unique_value, &real_udid);
    }
    if (real_udid && is_real_udid(real_udid)) {
        strncpy(g_udid, real_udid, sizeof(g_udid) - 1);
        g_udid[sizeof(g_udid) - 1] = '\0';
        usbmuxd_server_update_udid(g_udid);
        char msg[128];
        snprintf(msg, sizeof(msg), "[imd] ✅ iPhone UDID thật: %s", g_udid);
        emit_log(msg);
    } else {
        emit_log("[imd] UDID thật chưa đọc được từ lockdown; không dùng UDID giả");
    }
    if (real_udid) free(real_udid);
    if (unique_value) plist_free(unique_value);

    emit_log("[lockdown] ✅ lockdownd client OK (no-TLS; sẵn sàng Pair/Trust)");
    return JNI_TRUE;
}

/* ── nativePair ──────────────────────────────────────────────────────────── */
JNIEXPORT jboolean JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativePair(
        JNIEnv *env, jobject obj) {
    (void)env; (void)obj;

    if (!g_device || !g_lockdown) {
        emit_log("[pair] \u274c Chưa kết nối — gọi nativeConnect() trước");
        return JNI_FALSE;
    }
    emit_log("[pair] Bắt đầu ghép nối...");

    for (int i = 0; i < PAIR_RETRY_MAX; i++) {
        lockdownd_error_t err = lockdownd_pair(g_lockdown, NULL);
        switch (err) {
            case LOCKDOWN_E_SUCCESS:
                emit_log("[pair] \u2705 Ghép nối thành công!");
                g_paired = true;
                /* Giữ nguyên client no-TLS sau khi pair. Không mở lại bằng
                 * client_new_with_handshake(): thao tác đó có thể tạo session
                 * mux thứ hai và tái phát err=-8 ngay sau khi Trust. */
                emit_log("[pair] ✅ Pair thành công; giữ lockdownd session hiện tại");
                return JNI_TRUE;

            case LOCKDOWN_E_PAIRING_DIALOG_RESPONSE_PENDING: {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "[pair] \u23f3 Chờ \"Tin cậy\" trên iPhone... (%d/%d)",
                         i + 1, PAIR_RETRY_MAX);
                emit_log(msg);
                /* Hiện Trust popup notification */
                if (g_jvm) {
                    JNIEnv *e = NULL; bool dt = false;
                    if ((*g_jvm)->GetEnv(g_jvm, (void **)&e, JNI_VERSION_1_6) != JNI_OK) {
                        (*g_jvm)->AttachCurrentThread(g_jvm, (void **)&e, NULL); dt = true;
                    }
                    jclass cls = (*e)->FindClass(e,
                        "com/superalpha/sideload/bridge/NativeBridge");
                    if (cls) {
                        jmethodID mid = (*e)->GetStaticMethodID(e, cls,
                            "onTrustRequired", "()V");
                        if (mid) (*e)->CallStaticVoidMethod(e, cls, mid);
                        (*e)->DeleteLocalRef(e, cls);
                    }
                    if (dt) (*g_jvm)->DetachCurrentThread(g_jvm);
                }
                usleep(PAIR_RETRY_SLEEP_MS * 1000);
                continue;
            }

            case LOCKDOWN_E_PASSWORD_PROTECTED:
                emit_log("[pair] \u274c iPhone đang khoá — mở khoá trước khi ghép nối");
                return JNI_FALSE;

            case LOCKDOWN_E_INVALID_HOST_ID:
                emit_log("[pair] \u274c HostID không hợp lệ — thử pair lại");
                return JNI_FALSE;

            default: {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "[pair] \u274c lockdownd_pair() err=%d", (int)err);
                emit_log(msg);
                return JNI_FALSE;
            }
        }
    }

    emit_log("[pair] \u274c Hết thời gian chờ Trust — vui lòng thử lại");
    return JNI_FALSE;
}

/* ── nativeSideload ─────────────────────────────────────────────────────── */
JNIEXPORT jboolean JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeSideload(
        JNIEnv *env, jobject obj, jstring jipaPath) {
    (void)obj;
    const char *ipa_path = (*env)->GetStringUTFChars(env, jipaPath, NULL);
    char msg[512];
    snprintf(msg, sizeof(msg), "[sideload] IPA: %s", ipa_path);
    emit_log(msg);

    if (!g_device || !g_lockdown) {
        emit_log("[sideload] \u274c Chưa kết nối — gọi nativeConnect() và nativePair() trước");
        (*env)->ReleaseStringUTFChars(env, jipaPath, ipa_path);
        return JNI_FALSE;
    }

    bool success = false;
    lockdownd_service_descriptor_t svc = NULL;

    /* ── Bước 1: AFC service ─────────────────────────────────────────────── */
    emit_log("[afc] Mở kết nối AFC...");
    if (lockdownd_start_service(g_lockdown, "com.apple.afc", &svc)
        != LOCKDOWN_E_SUCCESS || !svc) {
        emit_log("[afc] \u274c lockdownd_start_service(afc) thất bại");
        goto done;
    }

    afc_client_t afc = NULL;
    if (afc_client_new(g_device, svc, &afc) != AFC_E_SUCCESS) {
        emit_log("[afc] \u274c afc_client_new() thất bại");
        lockdownd_service_descriptor_free(svc); svc = NULL;
        goto done;
    }
    lockdownd_service_descriptor_free(svc); svc = NULL;
    emit_log("[afc] \u2705 AFC session OK");

    /* ── Bước 2: Tạo thư mục staging ────────────────────────────────────── */
    afc_make_directory(afc, "/PublicStaging");

    /* ── Bước 3: Copy IPA qua AFC ──────────────────────────────────────── */
    {
        const char *fname = strrchr(ipa_path, '/');
        fname = fname ? fname + 1 : ipa_path;
        char remote_path[512];
        snprintf(remote_path, sizeof(remote_path), "/PublicStaging/%s", fname);

        snprintf(msg, sizeof(msg), "[afc] Ghi IPA → %s ...", remote_path);
        emit_log(msg);

        FILE *fp = fopen(ipa_path, "rb");
        if (!fp) {
            emit_log("[afc] \u274c Không mở được IPA — kiểm tra đường dẫn file");
            afc_client_free(afc);
            goto done;
        }

        uint64_t afc_fd_handle = 0;
        if (afc_file_open(afc, remote_path, AFC_FOPEN_WRONLY, &afc_fd_handle)
            != AFC_E_SUCCESS) {
            emit_log("[afc] \u274c afc_file_open() thất bại");
            fclose(fp);
            afc_client_free(afc);
            goto done;
        }

        static char chunk[AFC_CHUNK_SIZE];
        size_t total_written = 0;
        size_t n;
        bool write_ok = true;
        while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
            uint32_t written = 0;
            if (afc_file_write(afc, afc_fd_handle, chunk, (uint32_t)n, &written)
                != AFC_E_SUCCESS) {
                emit_log("[afc] \u274c afc_file_write() thất bại");
                write_ok = false;
                break;
            }
            total_written += written;
            /* Log tiến trình mỗi 1MB */
            if (total_written % (1024*1024) < (size_t)AFC_CHUNK_SIZE) {
                snprintf(msg, sizeof(msg), "[afc] Đang ghi... %zu bytes", total_written);
                emit_log(msg);
            }
        }
        fclose(fp);
        afc_file_close(afc, afc_fd_handle);
        afc_client_free(afc);

        if (!write_ok) goto done;

        snprintf(msg, sizeof(msg), "[afc] \u2705 %zu bytes → %s", total_written, remote_path);
        emit_log(msg);

        /* ── Bước 4: installation_proxy ───────────────────────────────── */
        emit_log("[instproxy] Bắt đầu cài đặt...");
        instproxy_client_t ipc = NULL;
        if (instproxy_client_start_service(g_device, &ipc, "sideloadtool")
            != INSTPROXY_E_SUCCESS) {
            emit_log("[instproxy] \u274c instproxy_client_start_service() thất bại");
            goto done;
        }

        plist_t client_opts = instproxy_client_options_new();
        instproxy_client_options_add(client_opts, "PackageType", "Developer", NULL);

        instproxy_error_t ie = instproxy_install(ipc, remote_path, client_opts,
                                                  NULL, NULL);
        plist_free(client_opts);
        instproxy_client_free(ipc);

        if (ie != INSTPROXY_E_SUCCESS) {
            snprintf(msg, sizeof(msg),
                     "[instproxy] \u274c instproxy_install() err=%d", (int)ie);
            emit_log(msg);
            goto done;
        }

        emit_log("[instproxy] \u2705 Cài đặt thành công!");
        success = true;
    }

done:
    if (svc) lockdownd_service_descriptor_free(svc);
    (*env)->ReleaseStringUTFChars(env, jipaPath, ipa_path);
    return success ? JNI_TRUE : JNI_FALSE;
}

/* ── Các JNI getters / helpers ───────────────────────────────────────────── */

JNIEXPORT jstring JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeGetUdid(
        JNIEnv *env, jobject obj) {
    (void)obj;
    return g_udid[0] ? (*env)->NewStringUTF(env, g_udid) : NULL;
}

JNIEXPORT jboolean JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeIsPaired(
        JNIEnv *env, jobject obj) {
    (void)env; (void)obj;
    return g_paired ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeIsConnected(
        JNIEnv *env, jobject obj) {
    (void)env; (void)obj;
    return (g_device && g_lockdown) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeGetConnectionState(
        JNIEnv *env, jobject obj) {
    (void)env; (void)obj;
    if (!g_device)   return 0;
    if (!g_lockdown) return 1;
    if (!g_paired)   return 2;
    return 3;
}

JNIEXPORT jstring JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeGetPairingPlist(
        JNIEnv *env, jobject obj) {
    (void)obj;
    if (!g_lockdown) return NULL;
    plist_t record = NULL;
    if (lockdownd_get_value(g_lockdown, NULL, "PairRecord", &record)
        != LOCKDOWN_E_SUCCESS || !record)
        return NULL;
    char *xml = NULL; uint32_t len = 0;
    plist_to_xml(record, &xml, &len);
    plist_free(record);
    jstring result = (xml && len > 0) ? (*env)->NewStringUTF(env, xml) : NULL;
    free(xml);
    return result;
}

JNIEXPORT void JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeReset(
        JNIEnv *env, jobject obj) {
    (void)env; (void)obj;
    if (g_lockdown) { lockdownd_client_free(g_lockdown); g_lockdown = NULL; }
    if (g_device)   { idevice_free(g_device);             g_device   = NULL; }
    usbmuxd_server_stop();
    usb_bridge_close();
    g_udid[0]    = '\0';
    g_paired     = false;
    g_product_id = 0;
    emit_log("[jni] Reset hoàn tất");
}

/* ════════════════════════════════════════════════════════════════════════
 * nativeListInstalledApps — trả về plist XML danh sách ứng dụng User
 * đang cài trên iPhone (qua com.apple.mobile.installation_proxy).
 *
 * Trả về chuỗi plist XML (cần parse ở Kotlin) hoặc null nếu thất bại.
 * Dùng bởi device_link.py → list_installed_apps() để kiểm tra xem app
 * đã cài trước đó chưa (logic re-use App ID trong sideload_core.py).
 * ════════════════════════════════════════════════════════════════════════ */
JNIEXPORT jstring JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeListInstalledApps(
        JNIEnv *env, jobject obj) {
    (void)obj;
    if (!g_device || !g_lockdown) {
        emit_log("[jni] nativeListInstalledApps: thiết bị chưa kết nối");
        return NULL;
    }

    lockdownd_service_descriptor_t svc = NULL;
    if (lockdownd_start_service(g_lockdown,
                                "com.apple.mobile.installation_proxy",
                                &svc) != LOCKDOWN_E_SUCCESS || !svc) {
        emit_log("[jni] nativeListInstalledApps: start_service thất bại");
        return NULL;
    }

    instproxy_client_t ip = NULL;
    if (instproxy_client_new(g_device, svc, &ip) != INSTPROXY_E_SUCCESS) {
        emit_log("[jni] nativeListInstalledApps: instproxy_client_new thất bại");
        lockdownd_service_descriptor_free(svc);
        return NULL;
    }
    lockdownd_service_descriptor_free(svc);

    /* Chỉ lấy User apps, trả về BundleID + DisplayName */
    plist_t opts = instproxy_client_options_new();
    instproxy_client_options_add(opts, "ApplicationType", "User", NULL);
    instproxy_client_options_set_return_attributes(
        opts, "CFBundleIdentifier", "CFBundleDisplayName", NULL);

    plist_t apps = NULL;
    instproxy_error_t ie = instproxy_browse(ip, opts, &apps);
    instproxy_client_options_free(opts);
    instproxy_client_free(ip);

    if (ie != INSTPROXY_E_SUCCESS || !apps) {
        emit_log("[jni] nativeListInstalledApps: instproxy_browse thất bại");
        return NULL;
    }

    char *xml = NULL; uint32_t xml_len = 0;
    plist_to_xml(apps, &xml, &xml_len);
    plist_free(apps);

    jstring result = (xml && xml_len > 0) ? (*env)->NewStringUTF(env, xml) : NULL;
    free(xml);
    return result;
}

/* ════════════════════════════════════════════════════════════════════════
 * nativeDiagnostics — trả về chuỗi chẩn đoán trạng thái kết nối.
 * Học từ lệnh "termux-usbmuxd doctor" — báo cáo mọi thành phần.
 * ════════════════════════════════════════════════════════════════════════ */
JNIEXPORT jstring JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeDiagnostics(
        JNIEnv *env, jobject obj) {
    (void)obj;
    char buf[1024];
    const char *socket_addr = getenv("USBMUXD_SOCKET_ADDRESS");
    snprintf(buf, sizeof(buf),
        "=== Native Diagnostics (học từ termux-usbmuxd doctor) ===\n"
        "device: %s\n"
        "lockdown: %s\n"
        "paired: %s\n"
        "udid: %s\n"
        "product_id: 0x%04x\n"
        "USBMUXD_SOCKET_ADDRESS: %s\n"
        "usb_bridge ep_in=0x%02x ep_out=0x%02x\n",
        g_device   ? "OK"    : "NULL",
        g_lockdown ? "OK"    : "NULL",
        g_paired   ? "true"  : "false",
        g_udid[0]  ? g_udid  : "(unknown)",
        g_product_id,
        socket_addr ? socket_addr : "(not set)",
        usb_bridge_ep_in(), usb_bridge_ep_out()
    );
    return (*env)->NewStringUTF(env, buf);
}
