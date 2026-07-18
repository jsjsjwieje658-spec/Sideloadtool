/* android_usbmuxd_fix.c
 *
 * Android non-root link-time override for libusbmuxd.
 *
 * This file is compiled into libsideloadnative.so BEFORE libusbmuxd-2.0.a
 * is linked. Because object files are searched before static archives,
 * the linker resolves usbmuxd_get_device() and usbmuxd_connect() from
 * this file instead of libusbmuxd-2.0.a.
 *
 * Fixes: idevice_new_with_options() err=-3 on Android non-root.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#include <usbmuxd.h>
#include <plist/plist.h>

/* ── Cache ── */
static pthread_mutex_t g_fix_mutex = PTHREAD_MUTEX_INITIALIZER;
static char  g_fix_udid[44] = {0};
static int   g_fix_product_id = 0x12a8;
static int   g_fix_has_device = 0;

void android_fix_set_device(const char *udid, int product_id)
{
    pthread_mutex_lock(&g_fix_mutex);
    if (udid) {
        strncpy(g_fix_udid, udid, sizeof(g_fix_udid) - 1);
        g_fix_udid[sizeof(g_fix_udid) - 1] = '\0';
        g_fix_has_device = 1;
    }
    g_fix_product_id = product_id;
    pthread_mutex_unlock(&g_fix_mutex);
}

/* ═══════════════════════════════════════════════════════════════════════
 * OVERRIDE: usbmuxd_get_device()
 * Return cached device info directly — bypass usbmuxd socket query.
 * ═══════════════════════════════════════════════════════════════════════ */
int usbmuxd_get_device(const char *udid, usbmuxd_device_info_t *device,
                       enum usbmux_lookup_options options)
{
    (void)options;

    pthread_mutex_lock(&g_fix_mutex);
    int has = g_fix_has_device;
    char cached[44];
    strncpy(cached, g_fix_udid, sizeof(cached));
    int pid = g_fix_product_id;
    pthread_mutex_unlock(&g_fix_mutex);

    if (!has) {
        return -ENOENT;
    }

    if (udid && udid[0] && strcmp(udid, cached) != 0) {
        return -ENOENT;
    }

    memset(device, 0, sizeof(*device));
    device->handle = 1;
    device->product_id = pid;
    strncpy(device->udid, cached, sizeof(device->udid) - 1);
    device->conn_type = CONNECTION_TYPE_USB;
    device->conn_data[0] = '\0';

    return 1; /* 1 device found */
}

/* ═══════════════════════════════════════════════════════════════════════
 * OVERRIDE: usbmuxd_connect()
 * Connect directly to 127.0.0.1:27015 (TCP) and send Connect plist.
 * ═══════════════════════════════════════════════════════════════════════ */
int usbmuxd_connect(const uint32_t handle, const unsigned short port)
{
    (void)handle;

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) return -1;

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(27015);
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);

    if (connect(sfd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        close(sfd);
        return -1;
    }

    /* Send Connect plist */
    plist_t req = plist_new_dict();
    plist_dict_set_item(req, "MessageType", plist_new_string("Connect"));
    plist_dict_set_item(req, "DeviceID", plist_new_uint(1));
    plist_dict_set_item(req, "PortNumber", plist_new_uint(port));

    char *xml = NULL;
    uint32_t xml_len = 0;
    plist_to_xml(req, &xml, &xml_len);
    plist_free(req);

    if (!xml) {
        close(sfd);
        return -1;
    }

    uint32_t len_be = __builtin_bswap32(xml_len);
    int ok = 0;

    if (send(sfd, &len_be, 4, 0) == 4 &&
        send(sfd, xml, xml_len, 0) == (ssize_t)xml_len) {
        uint32_t resp_len_be = 0;
        if (recv(sfd, &resp_len_be, 4, 0) == 4) {
            uint32_t resp_len = __builtin_bswap32(resp_len_be);
            if (resp_len > 0 && resp_len < 65536) {
                char *resp_xml = (char *)malloc(resp_len);
                if (resp_xml) {
                    int total = 0;
                    while (total < (int)resp_len) {
                        int n = recv(sfd, resp_xml + total, resp_len - total, 0);
                        if (n <= 0) break;
                        total += n;
                    }
                    if (total == (int)resp_len) {
                        plist_t resp = NULL;
                        plist_from_xml(resp_xml, resp_len, &resp);
                        if (resp) {
                            plist_t num = plist_dict_get_item(resp, "Number");
                            uint64_t result = 0;
                            if (num) plist_get_uint_val(num, &result);
                            plist_free(resp);
                            if (result == 0) ok = 1;
                        }
                    }
                    free(resp_xml);
                }
            }
        }
    }

    free(xml);

    if (!ok) {
        close(sfd);
        return -1;
    }

    return sfd;
}
