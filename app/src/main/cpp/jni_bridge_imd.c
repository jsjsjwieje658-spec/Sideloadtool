/**
 * jni_bridge_imd.c — JNI bridge dùng libimobiledevice API thật (Mode 1)
 *
 * ═══════════════════════════════════════════════════════════════════
 *  v48 — Comprehensive protocol fix:
 * ═══════════════════════════════════════════════════════════════════
 *  1. XÓA eager version exchange trong nativeSetUsbFd — gây race condition
 *     với usbmuxd server start. Version exchange CHỈ xảy ra trong
 *     do_usb_v1_connect() khi client Connect lần đầu.
 *
 *  2. FIX lockdown retry: Tăng ld_attempts 3 → 5, delay 300ms → 500ms,
 *     thêm clear_halt trước mỗi retry.
 *
 *  3. FIX nativePair re-establish: Reset version state + clear_halt
 *     trước khi re-create lockdown client. Nếu re-establish fail,
 *     KHÔNG return false ngay — tiếp tục retry trong loop.
 *
 *  4. Tương thích iOS 17+: iPhone 17+ cần thêm thời gian để dọn socket
 *     entry, tăng delay cho mọi retry.
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
    /*
     * FIX v38: Truyền JavaVM pointer xuống usb_fd_bridge.c để nó có thể
     * AttachCurrentThread và gọi NativeBridge.onNativeBulkWrite/Read
     * từ worker threads (khi Android JNI transport mode được enable).
     */
    usb_bridge_set_jvm((void *)vm);
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
     * FIX v38: Truyền NativeBridge instance xuống usb_fd_bridge.c để nó
     * có thể gọi onNativeBulkWrite/Read qua JNI khi Android transport mode.
     */
    usb_bridge_set_bridge_ref((void *)g_bridge_obj);
    emit_log("[jni] ═══════════════════════════════════════════════════════");
    emit_log("[jni]   SideloadTool native v48 (2026-08-21)");
    emit_log("[jni]   Fixes: v33-v47 + v48 comprehensive protocol fix");
    emit_log("[jni]   v48: removed eager version exchange race, fixed");
    emit_log("[jni]   thread safety, TIME_WAIT avoidance, retry improvements");
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
 *
 * FIX v37: Nhận thêm epIn, epOut, ifaceNum từ Kotlin để bypass
 * discover_apple_endpoints() — function này thường fail với Android fd.
 */
JNIEXPORT jboolean JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeSetUsbFd(
        JNIEnv *env, jobject obj, jint fd, jint vendorId, jint productId,
        jstring udidHint, jint epIn, jint epOut, jint ifaceNum) {
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
             "[usb] libusb_wrap_sys_device(fd=%d, vid=0x%04x, pid=0x%04x, "
             "ep_in=0x%02x, ep_out=0x%02x, iface=%d)",
             (int)fd, (int)vendorId, (int)productId,
             (int)epIn, (int)epOut, (int)ifaceNum);
    emit_log(buf);

    /* Bước 1: Khởi tạo libusb với Android USB fd + endpoints từ Kotlin */
    bool ok = usb_bridge_init_from_fd2((int)fd, (int)vendorId, (int)productId,
                                       (int)epIn, (int)epOut, (int)ifaceNum);
    if (!ok) {
        emit_log("[usb] \u274c libusb_wrap_sys_device() thất bại — kiểm tra quyền USB Host");
        return JNI_FALSE;
    }

    snprintf(buf, sizeof(buf),
             "[usb] \u2705 libusb ready: ep_in=0x%02x ep_out=0x%02x",
             usb_bridge_ep_in(), usb_bridge_ep_out());
    emit_log(buf);

    /*
     * FIX v39: CHỈ switch sang Android JNI transport mode nếu libusb_claim_interface()
     * đã thất bại (g_iface_claimed == 0). Nếu libusb_claim_interface() thành công
     * (như trong log v38 của user), dùng libusb path bình thường — nó ổn định hơn.
     *
     * Lý do phải thay đổi từ v38:
     *   v38 luôn gọi set_android_mode() → Kotlin's prepareForBulkTransfers() được
     *   gọi TRƯỚC nativeSetUsbFd() → claimInterface(iface, true) với force=true
     *   → Android steals interface từ libusb → libusb_claim_interface vẫn trả
     *   "claimed successfully" (nhưng thực ra là Android claim) → libusb_bulk_transfer
     *   gửi packet vào endpoint nhưng không có ai đọc (Android không chủ động poll) →
     *   iPhone nhận packet nhưng phản hồi của iPhone bị loopback vào buffer của
     *   chúng ta (đó là lý do "flush_in: drained 20 bytes" = chính VERSION packet
     *   mà chúng ta vừa gửi!).
     *
     *   Fix v39: KHÔNG gọi prepareForBulkTransfers() nếu libusb_claim_interface()
     *   thành công. Để libusb path hoạt động bình thường.
     */
    if (!usb_bridge_using_android_mode()) {
        /* Gọi accessor mới: usb_bridge_iface_claimed() return 1 nếu libusb claim OK */
        if (!usb_bridge_iface_claimed()) {
            emit_log("[usbmux] libusb_claim_interface fail — switch sang Android JNI transport mode");
            if (usb_bridge_set_android_mode()) {
                emit_log("[usbmux] ✅ Đã switch sang Android JNI bulk transport mode");
            } else {
                emit_log("[usbmux] ⚠️ Không switch được Android mode — JNI callbacks chưa sẵn sàng");
            }
        } else {
            emit_log("[usbmux] libusb_claim_interface đã OK — dùng libusb path (không cần Android mode)");
        }
    }

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
    emit_log("[usbmux] v48: KHÔNG eager version exchange — để do_usb_v1_connect() xử lý");

    /*
     * FIX v48 (CRITICAL): XÓA hoàn toàn eager version exchange.
     *
     * v33 đã thêm eager exchange "best-effort" ngay sau USB bridge init.
     * Tuy nhiên, eager exchange gây race condition nghiêm trọng:
     *
     *   1. Eager exchange gửi VERSION + SETUP
     *   2. iPhone nhận VERSION, reply, rồi chuyển sang "idle" (mux session
     *      đã được thiết lập nhưng chưa có client nào kết nối)
     *   3. usbmuxd_server_start() tạo threads — threads chưa dùng USB
     *   4. libimobiledevice Connect → do_usb_v1_connect() →
     *      usbmux_version_exchange() → returns true (g_version_done=1) →
     *      KHÔNG retry version exchange
     *   5. iPhone đã "forget" mux session từ step 1 → SYN bị reject
     *
     * Version exchange PHẢI xảy ra trong do_usb_v1_connect() — đúng thời
     * điểm khi client muốn mở TCP connection đến lockdown port 62078.
     * Tại thời điểm đó, iPhone mux daemon đang ở trạng thái "accepting"
     * và sẽ trả VERSION response đúng cách.
     *
     * Học từ upstream usbmuxd: VERSION được gửi trong device_receive_packet()
     * khi có client thực sự kết nối, KHÔNG phải lúc enumeration.
     */

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

    /*
     * FIX v47/v48 (CRITICAL — Trust popup không hiện):
     *
     * Retry loop cho lockdownd_client_new + lockdownd_get_value.
     *
     * Vấn đề: lockdownd_client_new() chỉ mở TCP connection (mux Connect),
     * chưa gửi data. Nó return SUCCESS ngay cả khi iPhone sắp RST trên
     * DATA packet đầu tiên (do mux_rx_seq semantics sai, hoặc iPhone
     * usbmuxd chưa kịp tạo socket entry). Khi lockdownd_get_value() gửi
     * DATA packet đầu tiên và bị RST, function return error nhưng
     * nativeConnect() cũ chỉ log "UDID chưa đọc được" và return JNI_TRUE.
     * Kết quả: nativePair() được gọi trên dead client → fail luôn.
     *
     * Fix: nếu lockdownd_get_value() fail với transport error (MUX_ERROR,
     * RECEIVE_TIMEOUT, UNKNOWN_ERROR), re-establish lockdown client (tạo
     * TCP connection MỚI với source port mới nhờ alloc_source_port()) và
     * retry.
     *
     * v48: Tăng ld_attempts từ 3 → 5, delay từ 300ms → 500ms.
     * iPhone iOS 17+ cần thêm thời gian để dọn socket entry.
     */
    int ld_attempts = 5;
    lockdownd_error_t ld_err = LOCKDOWN_E_UNKNOWN_ERROR;
    for (int ld_attempt = 0; ld_attempt < ld_attempts; ld_attempt++) {
        if (ld_attempt > 0) {
            emit_log("[lockdown] Retry mở lockdownd client (sau RST trước đó)...");
            /*
             * FIX v48: Tăng delay từ 300ms → 500ms.
             * iPhone usbmuxd cần thời gian để dọn TIME_WAIT state.
             * Thêm clear_halt để endpoint sạch cho phiên mới.
             */
            usleep(500 * 1000);
            usb_bridge_clear_endpoints_halt();
            usleep(100 * 1000);
        }
        /* Free old lockdown client nếu có (dead transport) */
        if (g_lockdown) {
            lockdownd_client_free(g_lockdown);
            g_lockdown = NULL;
        }
        ld_err = lockdownd_client_new(g_device, &g_lockdown, "sideloadtool");
        if (ld_err != LOCKDOWN_E_SUCCESS || !g_lockdown) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "[lockdown] ❌ lockdownd_client_new() err=%d (lần %d/%d)",
                     (int)ld_err, ld_attempt + 1, ld_attempts);
            emit_log(msg);
            g_lockdown = NULL;
            continue;
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

        if (value_err == LOCKDOWN_E_SUCCESS) {
            /* SUCCESS — UDID đọc được (hoặc empty), connection alive */
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
            break;  /* connection alive — exit retry loop */
        }

        /* Transport error — connection died (RST received) */
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "[lockdown] ⚠️ lockdownd_get_value() err=%d (lần %d/%d) — "
                 "transport died (RST?), sẽ re-establish lockdown client",
                 (int)value_err, ld_attempt + 1, ld_attempts);
        emit_log(msg);
        if (real_udid) free(real_udid);
        if (unique_value) plist_free(unique_value);
        /* Loop continues — will re-create lockdown client */
    }

    if (!g_lockdown) {
        emit_log("[lockdown] ❌ Không thể mở lockdownd client sau nhiều lần thử");
        idevice_free(g_device);
        g_device = NULL;
        return JNI_FALSE;
    }

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

            /*
             * FIX v45 (Critical — Trust popup không hiện):
             *
             * Transport errors (MUX_ERROR, RECEIVE_TIMEOUT, SSL_ERROR) xảy ra
             * khi underlying TCP connection bị iPhone reset (RST). Trước fix v45,
             * code rơi vào default case và return false ngay lập tức → user
             * phải bấm Pair lại thủ công.
             *
             * Fix: re-establish lockdown client (sẽ tạo TCP connection MỚI với
             * source port mới nhờ alloc_source_port() trong usbmuxd_server.c),
             * rồi retry lockdownd_pair. Tối đa 3 lần retry trên transport error.
             */
            case LOCKDOWN_E_MUX_ERROR:
            case LOCKDOWN_E_RECEIVE_TIMEOUT:
            case LOCKDOWN_E_SSL_ERROR:
            /*
             * FIX v46 (Critical — Trust popup không hiện trên iPhone):
             *
             * Trên iOS 16/17+, khi app gửi Pair request, lockdownd thường:
             *   1. Hiển thị "Trust This Computer" popup trên iPhone
             *   2. Đóng TCP connection (RST) — yêu cầu client retry
             *
             * libimobiledevice 1.3.0 (phiên bản đang dùng) không có
             * LOCKDOWN_E_PAIRING_DIALOG_RESPONSE_PENDING cho flow mới; thay vào
             * đó nó map response "PairingDialogResponsePending" hoặc RST giữa
             * chừng thành LOCKDOWN_E_UNKNOWN_ERROR (-256).
             *
             * Không xử lý -256 → code rơi vào default case → return false ngay
             * lập tức → user không có thời gian bấm "Trust" → popup trust
             * không bao giờ được xử lý đầy đủ.
             *
             * Fix: xem -256 (UNKNOWN_ERROR) như một "transport/pairing dialog
             * pending" — re-establish lockdown client, chờ user bấm Trust,
             * retry lockdownd_pair. Tối đa PAIR_RETRY_MAX (20) lần = 40 giây.
             */
            case LOCKDOWN_E_UNKNOWN_ERROR: {
                /*
                 * FIX v45/v46: Transport error (do iPhone RST hoặc PairingDialog
                 * pending). Re-establish lockdown client — sẽ tạo TCP connection
                 * MỚI với source port mới nhờ alloc_source_port() trong
                 * usbmuxd_server.c.
                 *
                 * Loop counter `i` đã tự động giới hạn tổng số retry
                 * (PAIR_RETRY_MAX = 20), không cần counter riêng.
                 */
                int is_unknown = (err == LOCKDOWN_E_UNKNOWN_ERROR);
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "[pair] \u26a0\ufe0f %s err=%d (RST hoặc Trust dialog pending?) — "
                         "re-establish lockdown client (loop %d/%d)",
                         is_unknown ? "UNKNOWN_ERROR" : "Transport",
                         (int)err, i + 1, PAIR_RETRY_MAX);
                emit_log(msg);

                /* FIX v46: Thông báo cho UI biết đang chờ user bấm Trust */
                if (is_unknown && g_jvm) {
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

                /* Free old lockdown client (dead transport) */
                if (g_lockdown) { lockdownd_client_free(g_lockdown); g_lockdown = NULL; }

                /*
                 * FIX v48: Khi re-establish lockdown client, mux session có thể
                 * đã bị reset (iPhone RST → mux state lost). Cần:
                 *   1. Reset version state để version exchange được thực hiện lại
                 *   2. Clear USB endpoint halt
                 *   3. Delay cho iPhone usbmuxd dọn TIME_WAIT
                 *   4. Tạo lockdown client mới (trigger Connect → version exchange
                 *      → SYN handshake trong do_usb_v1_connect)
                 */
                usbmuxd_server_reset_version_state();
                usb_bridge_clear_endpoints_halt();

                /* Delay cho iPhone usbmuxd dọn TIME_WAIT state cũ */
                usleep(500 * 1000);
                /* Extra delay cho UNKNOWN_ERROR để user có thời gian bấm Trust */
                if (is_unknown) {
                    usleep(500 * 1000);  /* +500ms = 1000ms total */
                }

                /* Re-create lockdown client → triggers new TCP connection via
                 * our usbmuxd server, with NEW source port (alloc_source_port).
                 * Nếu transport dead, idevice_new_with_options sẽ fail →
                 * return false và để outer retry loop xử lý. */
                lockdownd_error_t ld_err = lockdownd_client_new(
                        g_device, &g_lockdown, "sideloadtool");
                if (ld_err != LOCKDOWN_E_SUCCESS || !g_lockdown) {
                    char msg2[160];
                    snprintf(msg2, sizeof(msg2),
                             "[pair] ❌ Re-establish lockdown client err=%d (attempt %d/%d)",
                             (int)ld_err, i + 1, PAIR_RETRY_MAX);
                    emit_log(msg2);
                    g_lockdown = NULL;
                    /* KHÔNG return false ngay — retry trong loop */
                    usleep(800 * 1000);  /* 800ms trước khi retry */
                    continue;
                }
                emit_log("[pair] ✅ Re-established lockdown client — retry pair");
                continue;  /* retry lockdownd_pair */
            }

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
