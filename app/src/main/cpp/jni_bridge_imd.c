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
 * FIX v23 Bug B: nativeSetUsbFd() giờ gọi usbmux_version_exchange() một
 * lần duy nhất, ngay sau libusb init và TRƯỚC KHI usbmuxd_server_start()
 * — thay vì để mỗi TCP "Connect" tự lặp lại version exchange (khiến
 * iPhone reject connection từ lần thứ 2 trở đi).
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
#include <fcntl.h>
#include <errno.h>

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
#define SOCKET_READY_RETRIES  30
#define SOCKET_READY_SLEEP_MS 100

/* ── Global state ────────────────────────────────────────────────────────── */
static idevice_t          g_device   = NULL;
static lockdownd_client_t g_lockdown = NULL;
static char               g_udid[64] = {0};
static char               g_files_dir[512] = {0};
static bool               g_paired   = false;
static int                g_product_id = 0;

/* ── JNI helpers ─────────────────────────────────────────────────────────── */
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
        (*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL);
        detach = true;
    }
    jclass cls = (*env)->FindClass(env,
        "com/superalpha/sideload/bridge/NativeBridge");
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
    emit_log("[jni] Mode 1: libimobiledevice thật + usbmuxd server nội bộ");
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
        JNIEnv *env, jobject obj, jint fd, jint vendorId, jint productId) {
    (void)env; (void)obj;

    g_product_id = (int)productId;

    /* FIX: Dup fd để tránh invalid khi Java GC thu hồi UsbDeviceConnection */
    int fd_copy = dup((int)fd);
    if (fd_copy < 0) {
        emit_log("[usb] ❌ dup(fd) thất bại — fd không hợp lệ");
        return JNI_FALSE;
    }
    int flags = fcntl(fd_copy, F_GETFL, 0);
    if (flags >= 0) fcntl(fd_copy, F_SETFL, flags | O_NONBLOCK);

    char buf[256];
    snprintf(buf, sizeof(buf),
             "[usb] libusb_wrap_sys_device(fd=%d[dup từ %d], vid=0x%04x, pid=0x%04x)",
             fd_copy, (int)fd, (int)vendorId, (int)productId);
    emit_log(buf);

    /* Bước 1: Khởi tạo libusb với fd copy */
    bool ok = usb_bridge_init_from_fd(fd_copy, (int)vendorId, (int)productId);
    if (!ok) {
        emit_log("[usb] \u274c libusb_wrap_sys_device() thất bại — kiểm tra quyền USB Host");
        return JNI_FALSE;
    }

    snprintf(buf, sizeof(buf),
             "[usb] \u2705 libusb ready: ep_in=0x%02x ep_out=0x%02x",
             usb_bridge_ep_in(), usb_bridge_ep_out());
    emit_log(buf);

    /*
     * Bước 1.5 (FIX Bug B): Version exchange v1 — ĐÚNG MỘT LẦN cho toàn bộ
     * USB session. Phải gọi ngay sau libusb init và TRƯỚC KHI
     * usbmuxd_server_start() bắt đầu chấp nhận "Connect" request từ
     * libusbmuxd. Nếu không, mỗi Connect (lockdownd/AFC/instproxy...) sẽ
     * lặp lại version exchange bên trong do_usb_v1_connect() và khiến
     * iPhone nhận packet version không mong đợi trong lúc chờ SYN → reject
     * connection từ lần thứ 2 trở đi.
     *
     * Reset flag idempotent trước: nativeSetUsbFd() đại diện cho một
     * session USB mới (fd mới), nên version exchange PHẢI được phép chạy
     * lại ở đây kể cả khi đã chạy cho session trước (VD: rút/cắm lại cáp
     * mà không restart app process).
     *
     * FIX v24 Bug I: Chờ 300ms sau libusb init trước khi bắt đầu version
     * exchange. Sau khi libusb_wrap_sys_device(), USB endpoint cần thời
     * gian để sẵn sàng (đặc biệt trên Android). Không có delay này, lần
     * bulk_write đầu tiên bị LIBUSB_ERROR_PIPE và version exchange thất bại.
     */
    /*
     * FIX v25: Tăng delay từ 300ms → 800ms và thêm USB IN flush.
     *
     * Lý do cần 800ms:
     *   Sau libusb_wrap_sys_device(), iPhone gửi notification packet (attach
     *   event, MFI auth init...) trong vài trăm ms. Nếu bắt đầu version
     *   exchange quá sớm, usb_send_version() bị LIBUSB_ERROR_PIPE vì endpoint
     *   OUT chưa sẵn sàng; hoặc usb_recv_version() đọc stale notification
     *   data thay vì version response.
     *   800ms cho đủ thời gian để:
     *     (a) iPhone finalize USB endpoint setup
     *     (b) Endpoint stall (PIPE) tự clear sau khi hardware khởi tạo xong
     *     (c) Notification packets từ iPhone kịp đến buffer (để flush)
     *
     * Flush USB IN endpoint:
     *   Drain tất cả notification/stale packets trước khi gửi version packet.
     *   Mỗi lần đọc timeout 150ms (nhỏ để flush nhanh).
     */
    /*
     * FIX v27 (học từ termux-usbmuxd + UsbAPI.java):
     *
     * Tăng delay từ 800ms → 2000ms và flush packets từ 8 → 20.
     *
     * Lý do:
     *   termux-usbmuxd dùng fd SẠCH (UsbAPI.java không claim interface) nên
     *   endpoint sẵn sàng sớm hơn. Sideloadtool v27 cũng đã bỏ interface
     *   claim từ UsbTransport.open(), nhưng để backward compat với thiết bị
     *   khó chịu, giữ delay 2000ms.
     *
     *   20 packet × 200ms timeout = tối đa 4 giây flush. Đủ để drain
     *   tất cả notification packets từ iPhone (attach event, MFI auth init,
     *   v.v.) trước khi bắt đầu version exchange.
     *
     *   Gọi usb_bridge_clear_endpoints_halt() TRƯỚC flush để endpoint
     *   không bị STALL khi đang drain. Giống cách termux-usbmuxd cleanup
     *   và restart processes trước khi bắt đầu session mới.
     */
    // FIX: Tăng thời gian chờ ban đầu và số lần flush để đảm bảo endpoint ổn định hơn.
    // Thêm log chi tiết hơn để chẩn đoán.
    /*
     * FIX v28: Thời gian chờ ban đầu giảm từ 3000ms → 500ms.
     *
     * LƯU Ý (sửa comment sai — không có thay đổi hành vi): usb_fd_bridge.c
     * KHÔNG gọi libusb_reset_device() (đã bị gỡ bỏ hoàn toàn — xem "FIX
     * Bug 3 — CRITICAL" trong usb_bridge_init_from_fd(), vì reset_device()
     * làm fd của Android bị invalid). Endpoint sạch nhờ clear_halt() +
     * flush ở usb_bridge_init_from_fd(), không phải nhờ reset_device().
     * Chờ lâu hơn trong outer retry loop bên dưới nếu cần.
     */
    emit_log("[usb] Clear endpoint halts trước khi settle...");
    usb_bridge_clear_endpoints_halt();
    /* Không có libusb_reset_device() nào chạy ở đây — usb_bridge_init_from_fd()
     * cố tình KHÔNG reset device (xem "FIX Bug 3 — CRITICAL" trong
     * usb_fd_bridge.c). Log cũ ghi "sau reset" là sai/gây hiểu lầm; endpoint
     * sạch nhờ clear_halt() + flush ở trên, không phải nhờ reset. */
    emit_log("[usb] Chờ USB endpoint ổn định (500ms)...");
    usleep(500 * 1000);

    emit_log("[usb] Flush stale USB data trước version exchange (15 packets, 100ms/packet)...");
    usb_bridge_flush_in(15, 100);

    usbmuxd_server_reset_version_state();

    /*
     * FIX v28 (CRITICAL): Outer retry làm FULL USB re-init giữa các lần thử.
     *
     * VẤN ĐỀ CŨ (v27): Outer retry chỉ gọi clear_halt + flush nhưng KHÔNG
     * reinit libusb. Nếu version exchange thất bại do libusb state bị corrupt
     * (PIPE lỗi trên endpoint), flush và clear_halt không đủ để phục hồi.
     * libusb handle vẫn giữ state hỏng → lần retry tiếp vẫn thất bại.
     *
     * FIX: Mỗi lần thất bại:
     *   1. usb_bridge_close() — giải phóng toàn bộ libusb state
     *   2. usb_bridge_init_from_fd(fd, ...) — init lại từ đầu với cùng fd
     *      (fd vẫn còn valid vì UsbDeviceConnection trong Kotlin vẫn giữ)
     *   3. usbmuxd_server_reset_version_state() — cho phép exchange chạy lại
     *   4. usbmux_version_exchange() — thử lại với state hoàn toàn mới
     *
     * Delay giữa outer retry: 3 giây — đủ cho iPhone reset MUX endpoint.
     * Max outer retry: 5 lần × (5 inner attempts × 1.5s) = tối đa ~50 giây.
     * Trong thực tế, lần đầu (với libusb reset) thường thành công.
     */
    int version_exchange_attempts = 0;
    bool version_exchange_ok = false;
    const int MAX_OUTER_RETRY = 5;

    while (version_exchange_attempts < MAX_OUTER_RETRY) {
        version_exchange_attempts++;
        char log_buf[256];
        snprintf(log_buf, sizeof(log_buf),
                 "[usbmux] Thử version exchange lần %d/%d...",
                 version_exchange_attempts, MAX_OUTER_RETRY);
        emit_log(log_buf);

        if (usbmux_version_exchange()) {
            version_exchange_ok = true;
            break;
        }

        if (version_exchange_attempts >= MAX_OUTER_RETRY) break;

        snprintf(log_buf, sizeof(log_buf),
                 "[usbmux] ⚠️ version exchange (%d retry) thất bại - thử lại jni-level sau 3s...",
                 version_exchange_attempts);
        emit_log(log_buf);
        emit_log("[usbmux] ⚠️ iPhone đã unlock chưa? Bấm 'Trust' nếu có popup trên iPhone");
        emit_log("[usbmux] ⚠️ Kiểm tra: Cáp USB có hỗ trợ data không? (không phải cáp sạc)");

        /*
         * FIX v28: Full USB re-init — khác với v27 chỉ clear_halt + flush.
         *
         * usb_bridge_close() giải phóng toàn bộ libusb state (context, handle).
         * usb_bridge_init_from_fd() khởi tạo lại từ đầu với cùng fd.
         *
         * SỬA COMMENT SAI (không đổi hành vi): bản trước ghi nhầm là bước này
         * "bao gồm libusb_reset_device()". Điều đó KHÔNG ĐÚNG và đã bị xoá bỏ có
         * chủ đích (xem "FIX Bug 3 — CRITICAL" trong usb_fd_bridge.c) vì reset
         * làm fd do Android cấp bị invalid. Re-init ở đây chỉ là: đóng handle
         * libusb cũ → mở lại libusb_wrap_sys_device() với CÙNG fd (fd vẫn sống
         * vì Kotlin giữ UsbDeviceConnection) → discover + clear_halt lại từ đầu.
         * Không có USB bus reset nào xảy ra trong toàn bộ vòng lặp này.
         */
        usb_bridge_close();
        snprintf(log_buf, sizeof(log_buf),
                 "[bridge] ⚠️ nativeSetUsbfd lần %d thất bại - thử lại sau 3s...",
                 version_exchange_attempts);
        emit_log(log_buf);
        emit_log("[bridge] 💡 Giữ cáp USB, không rút ra. iPhone đã unlock + bấm Trust chưa?");
        usleep(3000 * 1000); /* 3 giây cho iPhone reset MUX endpoint */

        /* Re-init libusb từ cùng fd (fd vẫn valid do Kotlin giữ UsbDeviceConnection) */
        bool reinit_ok = usb_bridge_init_from_fd((int)fd, (int)vendorId, (int)productId);
        if (!reinit_ok) {
            emit_log("[bridge] ❌ Không thể reinit libusb — fd có thể không còn valid");
            emit_log("[bridge] 💡 Thử: Rút cáp USB 5s rồi cắm lại và nhấn 'Kết nối'");
            return JNI_FALSE;
        }
        snprintf(log_buf, sizeof(log_buf),
                 "[usb] ✅ libusb ready: ep_in=0x%02x ep_out=0x%02x",
                 usb_bridge_ep_in(), usb_bridge_ep_out());
        emit_log(log_buf);

        usleep(500 * 1000); /* 500ms ổn định sau reinit */
        usb_bridge_flush_in(15, 100); /* drain stale data */
        usbmuxd_server_reset_version_state(); /* cho phép exchange chạy lại */
    }

    if (!version_exchange_ok) {
        char log_buf[256];
        snprintf(log_buf, sizeof(log_buf),
                 "[usbmux] ❌ version exchange thất bại hoàn toàn - iPhone không phản hồi v1 protocol");
        emit_log(log_buf);
        emit_log("[usbmux] 💡 Gợi ý: rút cáp USB 5 giây rồi cắm lại, sau đó nhấn 'Kết nối'");
        emit_log("[usbmux] ⚠️ Kiểm tra: Cáp USB có hỗ trợ data không? (không phải cáp sạc)");
        usb_bridge_close();
        return JNI_FALSE;
    }
    emit_log("[usbmux] ✅ version exchange OK (1 lần/session)");

    /*
     * Bước 2: Khởi động usbmuxd server nội bộ.
     *
     * FIX: Dùng UDID placeholder khi chưa biết UDID thật.
     * Sau khi idevice_get_udid() trả về, gọi usbmuxd_server_update_udid()
     * KHÔNG restart server (tránh race condition).
     */
    bool srv = usbmuxd_server_start(
        g_files_dir,
        g_udid[0] ? g_udid : "00000000-0000-0000-0000-000000000000",
        (int)productId
    );

    if (srv) {
        snprintf(buf, sizeof(buf),
                 "[usbmuxd_srv] \u2705 Server listening: %s (UDID: %s)",
                 usbmuxd_server_socket_path(), g_udid[0] ? g_udid : "(chưa có)");
        emit_log(buf);
        /* Set env var ngay lập tức */
        setenv("USBMUXD_SOCKET_ADDRESS", usbmuxd_server_socket_path(), 1);

        /* FIX: Inject UDID into shim so usbmuxd_get_device() can bypass discover */
        android_fix_set_device(g_udid[0] ? g_udid : "00000000-0000000000000000", (int)productId);

        /* FIX CRITICAL: Fetch UDID early before nativeConnect() runs. */
        setenv("USBMUXD_SOCKET_PATH", usbmuxd_server_socket_path(), 1);
        usleep(300000); /* 300ms let server bind */

        idevice_t temp_dev = NULL;
        idevice_error_t early_err = idevice_new_with_options(&temp_dev, NULL, IDEVICE_LOOKUP_USBMUX);
        if (early_err == IDEVICE_E_SUCCESS && temp_dev) {
            char *early_udid = NULL;
            if (idevice_get_udid(temp_dev, &early_udid) == IDEVICE_E_SUCCESS && early_udid) {
                snprintf(buf, sizeof(buf),
                         "[bridge] ✅ Early UDID obtained: %s", early_udid);
                emit_log(buf);
                strncpy(g_udid, early_udid, sizeof(g_udid) - 1);
                g_udid[sizeof(g_udid) - 1] = '\0';
                usbmuxd_server_update_udid(g_udid);
                android_fix_set_device(g_udid, (int)productId);
                free(early_udid);
            } else {
                emit_log("[bridge] ⚠️ Early device found but no UDID yet");
            }
            idevice_free(temp_dev);
        } else {
            snprintf(buf, sizeof(buf),
                     "[bridge] ⚠️ Early idevice_new err=%d — will retry in nativeConnect()",
                     (int)early_err);
            emit_log(buf);
        }
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
    for (int attempt = 0; attempt < 30; attempt++) {
        enum idevice_options opts = IDEVICE_LOOKUP_USBMUX;
        err = idevice_new_with_options(&g_device, NULL, opts);
        if (err == IDEVICE_E_SUCCESS) break;
        {
            char msg[200];
            snprintf(msg, sizeof(msg),
                     "[imd] idevice_new_with_options() err=%d (lần %d/30) — đợi UDID sẵn sàng...",
                     (int)err, attempt + 1);
            emit_log(msg);
        }
        if (attempt < 29) usleep(500000);  /* chờ 500ms */
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

    /* Lấy UDID và cập nhật server (KHÔNG restart) */
    char *udid = NULL;
    idevice_get_udid(g_device, &udid);
    if (udid) {
        strncpy(g_udid, udid, sizeof(g_udid) - 1);
        free(udid);
        /* FIX: dùng update_udid thay vì restart (tránh race condition) */
        usbmuxd_server_update_udid(g_udid);
        android_fix_set_device(g_udid, 0x12a8);

        /*
         * FIX (báo cáo lỗi #7): usbmuxd_server_update_udid() giờ đã tự
         * broadcast "Attached" (UDID mới) cho mọi client đang Listen bên
         * trong nó, nhưng gọi tường minh usbmuxd_server_broadcast_attached()
         * ở đây — ngay sau khi UDID thật được xác nhận qua
         * idevice_get_udid() — như một safety-net rõ ràng ở tầng JNI, đảm
         * bảo libusbmuxd (và bất kỳ client nào khác đang mở kết nối tới
         * server nội bộ) luôn được thông báo lại ngay khi UDID thật sẵn
         * sàng, thay vì phụ thuộc hoàn toàn vào side-effect bên trong
         * update_udid().
         */
        usbmuxd_server_broadcast_attached();

        char msg[128];
        snprintf(msg, sizeof(msg), "[imd] \u2705 iPhone UDID: %s", g_udid);
        emit_log(msg);
    }

    /* Mở lockdownd session với TLS handshake */
    emit_log("[lockdown] Mở lockdownd session...");
    lockdownd_error_t ld_err = lockdownd_client_new_with_handshake(
            g_device, &g_lockdown, "sideloadtool");
    if (ld_err != LOCKDOWN_E_SUCCESS) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "[lockdown] \u274c lockdownd_client_new_with_handshake() err=%d",
                 (int)ld_err);
        emit_log(msg);
        idevice_free(g_device);
        g_device = NULL;
        return JNI_FALSE;
    }

    emit_log("[lockdown] \u2705 lockdownd session OK");
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
                /* Re-init TLS session sau khi pair */
                lockdownd_client_free(g_lockdown);
                g_lockdown = NULL;
                {
                    lockdownd_error_t rr = lockdownd_client_new_with_handshake(
                            g_device, &g_lockdown, "sideloadtool");
                    if (rr != LOCKDOWN_E_SUCCESS) {
                        char msg[128];
                        snprintf(msg, sizeof(msg),
                                 "[pair] \u26a0\ufe0f Re-init lockdownd err=%d (không critical)", (int)rr);
                        emit_log(msg);
                        /* Không fatal — session chính đã pair thành công */
                    }
                }
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
                        (*g_jvm)->AttachCurrentThread(g_jvm, &e, NULL); dt = true;
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

