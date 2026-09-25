/*
 * lockdown.c — Lockdown protocol: plist qua TCP-mux, prefix 4-byte BE length.
 * Tham chiếu: libimobiledevice/src/lockdown.c
 */
#include "lockdown.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <android/log.h>

#ifndef _GNU_SOURCE
/* asprintf() là phần mở rộng GNU/BSD, không nằm trong ISO C — bionic (NDK)
 * expose nó không cần _GNU_SOURCE, nhưng khai báo tường minh ở đây để tránh
 * "implicit declaration" trên các toolchain glibc nghiêm ngặt hơn. */
extern int asprintf(char **strp, const char *fmt, ...);
#endif

#define TAG "lockdown"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

int lockdown_open(lockdown_t *ld, mux_conn_t *mux) {
    memset(ld, 0, sizeof(*ld));
    ld->mux = mux;
    /* Kết nối mux vào lockdownd port 62078 */
    int r = mux_connect(mux, LOCKDOWN_PORT);
    if (r < 0) { LOGE("mux_connect(62078) thất bại"); return -1; }
    LOGI("✅ lockdown_open: kết nối port 62078 thành công");
    return 0;
}

/* Gửi plist XML có prefix 4-byte big-endian length */
static int ld_send(lockdown_t *ld, const char *xml) {
    uint32_t len = (uint32_t)strlen(xml);
    uint32_t len_be = htonl(len);
    if (mux_send(ld->mux, &len_be, 4) < 0) return -1;
    if (mux_send(ld->mux, xml, len)   < 0) return -1;
    return 0;
}

/* Nhận plist XML có prefix 4-byte big-endian length */
static char *ld_recv(lockdown_t *ld) {
    uint32_t len_be = 0;
    if (mux_recv_exact(ld->mux, &len_be, 4) < 0) return NULL;
    uint32_t len = ntohl(len_be);
    if (len == 0 || len > 4 * 1024 * 1024) {
        LOGE("lockdown recv: length không hợp lệ %u", len);
        return NULL;
    }
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    if (mux_recv_exact(ld->mux, buf, (int)len) < 0) { free(buf); return NULL; }
    buf[len] = '\0';
    return buf;
}

int lockdown_exchange(lockdown_t *ld, const char *req_xml, plist_dict_t **resp_out) {
    if (ld_send(ld, req_xml) < 0) {
        LOGE("lockdown_exchange: gửi thất bại");
        return -1;
    }
    char *resp_xml = ld_recv(ld);
    if (!resp_xml) {
        LOGE("lockdown_exchange: nhận thất bại");
        return -1;
    }
    if (resp_out) {
        *resp_out = plist_parse(resp_xml);
    }
    free(resp_xml);
    return 0;
}

int lockdown_get_value(lockdown_t *ld, const char *domain,
                        const char *key, char **val_out) {
    /* Build GetValue request */
    char *req = NULL;
    if (domain) {
        asprintf(&req,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\""
            " \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
            "<plist version=\"1.0\"><dict>"
            "<key>Domain</key><string>%s</string>"
            "<key>Key</key><string>%s</string>"
            "<key>Label</key><string>SideloadAndroid</string>"
            "<key>Request</key><string>GetValue</string>"
            "</dict></plist>", domain, key ? key : "");
    } else {
        asprintf(&req,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\""
            " \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
            "<plist version=\"1.0\"><dict>"
            "<key>Key</key><string>%s</string>"
            "<key>Label</key><string>SideloadAndroid</string>"
            "<key>Request</key><string>GetValue</string>"
            "</dict></plist>", key ? key : "");
    }
    plist_dict_t *resp = NULL;
    int r = lockdown_exchange(ld, req, &resp);
    free(req);
    if (r < 0 || !resp) return -1;

    /* BUGFIX: kiểm tra Error trước khi đọc Value */
    const char *err = plist_get_str(resp, "Error");
    if (err) {
        LOGE("lockdown_get_value(%s): lockdownd trả lỗi: %s", key ? key : "all", err);
        plist_free(resp);
        return -1;
    }
    /*
     * FIX (Bug 6 — CRITICAL): plist_get_str() chỉ match PTYPE_STR.
     * Nhiều giá trị quan trọng từ lockdownd dùng <data> (PTYPE_DATA):
     *   - DevicePublicKey → cần để build pair record
     *   - RootCertificate, HostCertificate, DeviceCertificate
     * Nếu plist_get_str() trả NULL, thử plist_get_data() trước khi fail.
     * Nếu không có cả hai → pairing_do() thất bại ngay bước GetValue(DevicePublicKey)
     * → Pair request không bao giờ được gửi → Trust popup không xuất hiện.
     */
    const char *val = plist_get_str(resp, "Value");
    if (!val) val = plist_get_data(resp, "Value");  /* FIX: also check <data> type */
    if (val && val_out) *val_out = strdup(val);
    plist_free(resp);
    if (!val) LOGE("lockdown_get_value(%s): không tìm thấy trường Value trong response (STR hoặc DATA)", key ? key : "all");
    return val ? 0 : -1;
}

int lockdown_start_service(lockdown_t *ld, const char *service,
                             int *port_out, int *ssl_out) {
    char *req = plist_build_start_service(service);
    if (!req) return -1;
    plist_dict_t *resp = NULL;
    int r = lockdown_exchange(ld, req, &resp);
    free(req);
    if (r < 0 || !resp) return -1;

    const char *err = plist_get_str(resp, "Error");
    if (err) {
        LOGE("StartService(%s) lỗi: %s", service, err);
        plist_free(resp);
        return -1;
    }
    long long port = plist_get_int(resp, "Port");
    /*
     * BUGFIX: EnableServiceSSL là <true/> boolean trong plist của Apple — KHÔNG
     * phải <string>. plist_get_str() luôn trả NULL cho nó → use_ssl luôn = 0.
     * Dùng plist_get_bool() thay thế.
     */
    int use_ssl = plist_get_bool(resp, "EnableServiceSSL");
    plist_free(resp);

    if (port_out) *port_out = (int)port;
    if (ssl_out)  *ssl_out  = use_ssl;
    LOGI("StartService(%s): port=%lld ssl=%d", service, port, use_ssl);
    return 0;
}

/* lockdown_start_tls: Được implement bởi TlsHelper.kt qua JNI.
 * Hàm này chỉ là placeholder — gọi thực tế ở jni_bridge.c nơi có JNIEnv. */
int lockdown_start_tls(lockdown_t *ld) {
    (void)ld;
    /* TLS được khởi tạo ở jni_bridge.c: nativeConnect() gọi TlsHelper.handshake() */
    return 0;
}

void lockdown_close(lockdown_t *ld) {
    if (!ld) return;
    mux_disconnect(ld->mux);
    free(ld->host_cert_pem);
    free(ld->host_key_pem);
    ld->host_cert_pem = NULL;
    ld->host_key_pem  = NULL;
    ld->tls_active    = 0;
    LOGI("lockdown_close: đã đóng.");
}
