/*
 * jni_bridge_imd.c  v35 (NO-ROOT)
 *
 * FIX: Bỏ hoàn toàn lockdownd_client_new_with_handshake().
 * Dùng thẳng lockdownd_client_new() + lockdownd_pair() để giao tiếp.
 * Sideload không cần TLS — pair trực tiếp là đủ.
 */

#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>

#include "usbmuxd_server.h"
#include "android_usbmuxd_fix.h"

#define SOCKET_READY_RETRIES 30
#define SOCKET_READY_SLEEP_MS 100
#define PAIR_RETRY_MAX 20
#define PAIR_RETRY_SLEEP_MS 3000

static JavaVM *g_jvm = NULL;
static idevice_t g_device = NULL;
static lockdownd_client_t g_lockdown = NULL;
static char g_udid[64] = "";

static void emit_log(const char *msg) {
    if (!g_jvm) return;
    JNIEnv *env = NULL;
    int dt = 0;
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        (*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL);
        dt = 1;
    }
    jclass cls = (*env)->FindClass(env, "com/superalpha/sideload/bridge/NativeBridge");
    if (cls) {
        jmethodID mid = (*env)->GetStaticMethodID(env, cls, "onLog", "(Ljava/lang/String;)V");
        if (mid) {
            jstring jmsg = (*env)->NewStringUTF(env, msg);
            (*env)->CallStaticVoidMethod(env, cls, mid, jmsg);
            (*env)->DeleteLocalRef(env, jmsg);
        }
        (*env)->DeleteLocalRef(env, cls);
    }
    if (dt) (*g_jvm)->DetachCurrentThread(g_jvm);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

/* ════════════════════════════════════════════════════════════════════════
 * nativeConnect - v35: Dùng thẳng pair mới, KHÔNG qua handshake
 * 
 * Flow mới (no-root):
 *   1. idevice_new_with_options() — kết nối usbmuxd
 *   2. lockdownd_client_new() — kết nối KHÔNG TLS
 *   3. lockdownd_pair() — pair với iPhone (hiện Trust popup)
 *   4. Done! Không cần handshake — sideload dùng no-TLS session
 * ════════════════════════════════════════════════════════════════════════ */
JNIEXPORT jboolean JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeConnect(
        JNIEnv *env, jobject obj) {
    (void)env; (void)obj;

    /* Cleanup cũ */
    if (g_lockdown) { lockdownd_client_free(g_lockdown); g_lockdown = NULL; }
    if (g_device)   { idevice_free(g_device);              g_device   = NULL; }

    /* Kiểm tra USB bridge */
    extern int usb_bridge_ep_in(void);
    extern int usb_bridge_ep_out(void);
    if (!usb_bridge_ep_in() || !usb_bridge_ep_out()) {
        emit_log("[imd] ❌ USB bridge chưa khởi tạo — gọi nativeSetUsbFd() trước");
        return JNI_FALSE;
    }

    /* Kiểm tra usbmuxd server */
    const char *sock_path = usbmuxd_server_socket_path();
    if (!sock_path) {
        emit_log("[imd] ❌ usbmuxd server chưa chạy");
        return JNI_FALSE;
    }

    /* Chờ socket ready */
    {
        int ready = 0;
        for (int i = 0; i < SOCKET_READY_RETRIES && !ready; i++) {
            int test_fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (test_fd >= 0) {
                struct sockaddr_un sa;
                memset(&sa, 0, sizeof(sa));
                sa.sun_family = AF_UNIX;
                strncpy(sa.sun_path, sock_path, sizeof(sa.sun_path) - 1);
                if (connect(test_fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) ready = 1;
                close(test_fd);
            }
            if (!ready) usleep(SOCKET_READY_SLEEP_MS * 1000);
        }
        if (!ready) {
            emit_log("[imd] ❌ usbmuxd socket không ready");
            return JNI_FALSE;
        }
    }

    /* idevice_new_with_options */
    emit_log("[imd] idevice_new_with_options(USBMUX)...");
    idevice_error_t err = IDEVICE_E_UNKNOWN_ERROR;
    for (int attempt = 0; attempt < 30; attempt++) {
        err = idevice_new_with_options(&g_device, NULL, IDEVICE_LOOKUP_USBMUX);
        if (err == IDEVICE_E_SUCCESS) break;
        {
            char msg[200];
            snprintf(msg, sizeof(msg), "[imd] idevice_new err=%d (lần %d/30)", (int)err, attempt + 1);
            emit_log(msg);
        }
        if (attempt < 29) usleep(500000);
    }

    if (err != IDEVICE_E_SUCCESS) {
        char msg[200];
        snprintf(msg, sizeof(msg), "[imd] ❌ idevice_new err=%d", (int)err);
        emit_log(msg);
        return JNI_FALSE;
    }

    emit_log("[imd] ✅ idevice OK");

    /* Lấy UDID */
    char *udid = NULL;
    idevice_get_udid(g_device, &udid);
    if (udid) {
        strncpy(g_udid, udid, sizeof(g_udid) - 1);
        free(udid);
        usbmuxd_server_update_udid(g_udid);
        android_usbmuxd_fix_set_device(g_udid, 0x12a8);
        usbmuxd_server_broadcast_attached();

        char msg[128];
        snprintf(msg, sizeof(msg), "[imd] ✅ iPhone UDID: %s", g_udid);
        emit_log(msg);
    }

    /*
     * ════════════════════════════════════════════════════════════════════════
     * v35 (NO-ROOT): Dùng thẳng lockdownd_client_new() + pair()
     * 
     * KHÔNG dùng lockdownd_client_new_with_handshake() nữa vì:
     *   - Nó yêu cầu TLS certificate (cần root để đọc pairing keys)
     *   - Sideload chỉ cần kết nối no-TLS để cài app
     *   - Pair trực tiếp qua lockdownd_pair() là đủ
     * ════════════════════════════════════════════════════════════════════════
     */
    emit_log("[lockdown] Kết nối lockdownd (no-TLS)...");

    lockdownd_error_t ld_err = lockdownd_client_new(g_device, &g_lockdown, "sideloadtool");
    if (ld_err != LOCKDOWN_E_SUCCESS) {
        char msg[256];
        snprintf(msg, sizeof(msg), "[lockdown] ❌ lockdownd_client_new err=%d", (int)ld_err);
        emit_log(msg);
        idevice_free(g_device); g_device = NULL;
        return JNI_FALSE;
    }
    emit_log("[lockdown] ✅ lockdownd_client_new OK (no-TLS)");

    /* Pair với iPhone */
    emit_log("[lockdown] Bắt đầu pair...");

    int pair_attempts = 0;
    while (pair_attempts < PAIR_RETRY_MAX) {
        ld_err = lockdownd_pair(g_lockdown, NULL);

        if (ld_err == LOCKDOWN_E_SUCCESS) {
            emit_log("[lockdown] ✅ Pair thành công!");
            break;
        }

        if (ld_err == LOCKDOWN_E_PAIRING_DIALOG_RESPONSE_PENDING) {
            char msg[256];
            snprintf(msg, sizeof(msg), "[lockdown] ⏳ Chờ bấm 'Tin cậy' trên iPhone... (%d/%d)",
                     pair_attempts + 1, PAIR_RETRY_MAX);
            emit_log(msg);

            /* Hiện notification */
            if (g_jvm) {
                JNIEnv *e = NULL; int dt = 0;
                if ((*g_jvm)->GetEnv(g_jvm, (void **)&e, JNI_VERSION_1_6) != JNI_OK) {
                    (*g_jvm)->AttachCurrentThread(g_jvm, &e, NULL); dt = 1;
                }
                jclass cls = (*e)->FindClass(e, "com/superalpha/sideload/bridge/NativeBridge");
                if (cls) {
                    jmethodID mid = (*e)->GetStaticMethodID(e, cls, "onTrustRequired", "()V");
                    if (mid) (*e)->CallStaticVoidMethod(e, cls, mid);
                    (*e)->DeleteLocalRef(e, cls);
                }
                if (dt) (*g_jvm)->DetachCurrentThread(g_jvm);
            }

            usleep(PAIR_RETRY_SLEEP_MS * 1000);
            pair_attempts++;
            continue;
        }

        if (ld_err == LOCKDOWN_E_PASSWORD_PROTECTED) {
            emit_log("[lockdown] ❌ iPhone đang khoá — mở khoá trước");
            lockdownd_client_free(g_lockdown); g_lockdown = NULL;
            idevice_free(g_device); g_device = NULL;
            return JNI_FALSE;
        }

        /* Lỗi khác */
        char msg[256];
        snprintf(msg, sizeof(msg), "[lockdown] ❌ lockdownd_pair err=%d", (int)ld_err);
        emit_log(msg);
        lockdownd_client_free(g_lockdown); g_lockdown = NULL;
        idevice_free(g_device); g_device = NULL;
        return JNI_FALSE;
    }

    if (pair_attempts >= PAIR_RETRY_MAX) {
        emit_log("[lockdown] ❌ Hết thờigian chờ Trust");
        lockdownd_client_free(g_lockdown); g_lockdown = NULL;
        idevice_free(g_device); g_device = NULL;
        return JNI_FALSE;
    }

    emit_log("[lockdown] ✅ Kết nối OK (no-TLS, đã pair)");
    return JNI_TRUE;
}

/* ════════════════════════════════════════════════════════════════════════
 * nativePair - giữ nguyên
 * ════════════════════════════════════════════════════════════════════════ */
JNIEXPORT jboolean JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativePair(
        JNIEnv *env, jobject obj) {
    (void)env; (void)obj;

    if (!g_device || !g_lockdown) {
        emit_log("[imd] ❌ Chưa connect");
        return JNI_FALSE;
    }

    emit_log("[imd] Pair...");
    lockdownd_error_t err = lockdownd_pair(g_lockdown, NULL);
    if (err == LOCKDOWN_E_SUCCESS) {
        emit_log("[imd] ✅ Pair OK");
        return JNI_TRUE;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "[imd] ❌ lockdownd_pair err=%d", (int)err);
    emit_log(msg);
    return JNI_FALSE;
}

/* ════════════════════════════════════════════════════════════════════════
 * nativeGetUdid
 * ════════════════════════════════════════════════════════════════════════ */
JNIEXPORT jstring JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeGetUdid(
        JNIEnv *env, jobject obj) {
    (void)obj;
    if (g_udid[0]) {
        return (*env)->NewStringUTF(env, g_udid);
    }
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════
 * nativeDisconnect
 * ════════════════════════════════════════════════════════════════════════ */
JNIEXPORT void JNICALL
Java_com_superalpha_sideload_bridge_NativeBridge_nativeDisconnect(
        JNIEnv *env, jobject obj) {
    (void)env; (void)obj;
    if (g_lockdown) { lockdownd_client_free(g_lockdown); g_lockdown = NULL; }
    if (g_device)   { idevice_free(g_device);              g_device   = NULL; }
    g_udid[0] = '\0';
    emit_log("[imd] Đã disconnect");
}
