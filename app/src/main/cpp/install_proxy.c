/*
 * install_proxy.c — com.apple.mobile.installation_proxy protocol.
 * Tham chiếu: libimobiledevice/src/installation_proxy.c
 * Giao thức: plist qua TCP-mux, prefix 4-byte BE length.
 * Gửi Install command → poll response đến khi Complete hoặc Error.
 */
#include "install_proxy.h"
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <android/log.h>

#define TAG "install_proxy"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static int ip_send(install_proxy_t *ip, const char *xml) {
    uint32_t len = (uint32_t)strlen(xml);
    uint32_t len_be = htonl(len);
    if (mux_send(ip->mux, &len_be, 4) < 0) return -1;
    if (mux_send(ip->mux, xml, len)   < 0) return -1;
    return 0;
}

static char *ip_recv(install_proxy_t *ip) {
    uint32_t len_be = 0;
    if (mux_recv_exact(ip->mux, &len_be, 4) < 0) return NULL;
    uint32_t len = ntohl(len_be);
    if (len == 0 || len > 8 * 1024 * 1024) {
        LOGE("ip_recv: length không hợp lệ %u", len);
        return NULL;
    }
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    if (mux_recv_exact(ip->mux, buf, (int)len) < 0) {
        free(buf); return NULL;
    }
    buf[len] = '\0';
    return buf;
}

int install_proxy_open(install_proxy_t *ip, mux_conn_t *mux) {
    memset(ip, 0, sizeof(*ip));
    ip->mux = mux;
    return 0;
}

int install_proxy_install(install_proxy_t *ip, const char *pkg_path,
                           void (*progress_cb)(const char *status, int pct)) {
    /* Gửi Install command */
    char *req = plist_build_install_request(pkg_path);
    if (!req) return -1;
    LOGI("[install_proxy] Gửi Install command cho %s", pkg_path);
    if (ip_send(ip, req) < 0) {
        free(req); LOGE("[install_proxy] Gửi Install command thất bại"); return -1;
    }
    free(req);

    /* Poll responses đến khi Complete hoặc Error */
    int max_polls = 600;    /* 600 * 1s = 10 phút tối đa */
    while (max_polls-- > 0) {
        char *resp_xml = ip_recv(ip);
        if (!resp_xml) {
            LOGE("[install_proxy] Nhận response thất bại");
            return -1;
        }
        plist_dict_t *d = plist_parse(resp_xml);
        free(resp_xml);
        if (!d) continue;

        const char *status  = plist_get_str(d, "Status");
        const char *err     = plist_get_str(d, "Error");
        long long   pct     = plist_get_int(d, "PercentComplete");

        if (err && strlen(err) > 0) {
            LOGE("[install_proxy] ❌ Lỗi cài đặt: %s", err);
            plist_free(d);
            return -1;
        }

        if (status) {
            LOGI("[install_proxy] %s (%lld%%)", status, pct);
            if (progress_cb) progress_cb(status, (int)pct);

            if (strcmp(status, "Complete") == 0) {
                LOGI("[install_proxy] ✅ Cài đặt hoàn tất!");
                plist_free(d);
                return 0;
            }
        }
        plist_free(d);
    }
    LOGE("[install_proxy] Hết thời gian chờ install");
    return -1;
}

void install_proxy_close(install_proxy_t *ip) {
    memset(ip, 0, sizeof(*ip));
}
