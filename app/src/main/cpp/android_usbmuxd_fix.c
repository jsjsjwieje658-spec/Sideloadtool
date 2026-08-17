/* android_usbmuxd_fix.c
 *
 * Android non-root link-time override for libusbmuxd.
 *
 * Implemented via the linker's --wrap mechanism (see CMakeLists.txt:
 * "-Wl,--wrap=usbmuxd_get_device" / "-Wl,--wrap=usbmuxd_connect"), so the
 * functions below are named __wrap_usbmuxd_get_device() /
 * __wrap_usbmuxd_connect() rather than usbmuxd_get_device()/
 * usbmuxd_connect() directly. The linker transparently redirects every
 * call to the real symbol name (including calls made from inside the
 * prebuilt libimobiledevice-1.0.a) to these __wrap_ versions instead —
 * without ever defining a second symbol with the same name as the one
 * already in libusbmuxd-2.0.a (which would be a fatal "duplicate symbol"
 * link error under NDK r25's lld).
 *
 * CRITICAL FIXES:
 * 1. usbmuxd_get_device() now returns a placeholder device even when UDID
 *    is not yet known. This breaks the deadlock:
 *    idevice_new_with_options() -> usbmuxd_get_device() -> -ENOENT
 *
 * 2. usbmuxd_connect() now reads USBMUXD_SOCKET_ADDRESS from environment
 *    to connect to the Unix domain socket created by usbmuxd_server.c,
 *    instead of hardcoding TCP 127.0.0.1:27015.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <android/log.h>

#include <usbmuxd.h>
#include <plist/plist.h>

#include "android_usbmuxd_fix.h"

/* libusbmuxd plist socket framing is a packed 16-byte little-endian header.
 * The in-process server uses the same layout; do not send a standalone
 * 4-byte length followed by XML because the server will then consume the
 * first 12 XML bytes as version/type/tag and compute a bogus body length. */
#pragma pack(push, 1)
typedef struct {
    uint32_t length;
    uint32_t version;
    uint32_t type;
    uint32_t tag;
} android_usbmux_plist_hdr_t;
#pragma pack(pop)

#define ANDROID_USBMUX_PROTO_PLIST 1u
#define ANDROID_USBMUX_TYPE_PLIST  8u

/* ── Logging helper ── */
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static android_usbmuxd_log_callback_t g_log_callback = NULL;

void android_usbmuxd_fix_set_log_callback(android_usbmuxd_log_callback_t callback)
{
    pthread_mutex_lock(&g_log_mutex);
    g_log_callback = callback;
    pthread_mutex_unlock(&g_log_mutex);
}

void android_usbmuxd_fix_log(const char *msg)
{
    if (!msg) return;
    __android_log_print(ANDROID_LOG_INFO, "sideloadnative", "%s", msg);

    pthread_mutex_lock(&g_log_mutex);
    android_usbmuxd_log_callback_t callback = g_log_callback;
    pthread_mutex_unlock(&g_log_mutex);
    if (callback) callback(msg);
}

void android_usbmuxd_fix_logf(const char *fmt, ...)
{
    if (!fmt) return;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    android_usbmuxd_fix_log(msg);
}

/* Socket reads/writes may be partial even for a local stream socket. */
static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = send(fd, p + done, len - done, MSG_NOSIGNAL);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
    char *p = (char *)buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = recv(fd, p + done, len - done, 0);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

/* ── Cache ── */
static pthread_mutex_t g_fix_mutex = PTHREAD_MUTEX_INITIALIZER;
static char  g_fix_udid[44] = {0};
static int   g_fix_product_id = 0x12a8;
static int   g_fix_has_device = 0;

void android_fix_set_device(const char *udid, int product_id)
{
    pthread_mutex_lock(&g_fix_mutex);
    if (udid && udid[0]) {
        strncpy(g_fix_udid, udid, sizeof(g_fix_udid) - 1);
        g_fix_udid[sizeof(g_fix_udid) - 1] = '\0';
        g_fix_has_device = 1;
    }
    g_fix_product_id = product_id;
    pthread_mutex_unlock(&g_fix_mutex);
}

/* Alias with namespaced name — called from jni_bridge_imd.c */
void android_usbmuxd_fix_set_device(const char *udid, int product_id)
{
    android_fix_set_device(udid, product_id);
}

/* ═══════════════════════════════════════════════════════════════════════
 * OVERRIDE (via -Wl,--wrap=usbmuxd_get_device): __wrap_usbmuxd_get_device()
 *
 * CRITICAL FIX: Return a transport-only device with handle=1 even when
 * the real UDID is not yet known. The mux handle can open lockdown first;
 * the actual UniqueDeviceID is queried from lockdown after that connection.
 *
 * Without this fix, idevice_new_with_options() fails with
 * IDEVICE_E_NO_DEVICE (err=-3) forever because usbmuxd_get_device()
 * returns -ENOENT when no UDID is cached.
 * ═══════════════════════════════════════════════════════════════════════ */
__attribute__((visibility("default")))
int __wrap_usbmuxd_get_device(const char *udid, usbmuxd_device_info_t *device,
                       enum usbmux_lookup_options options)
{
    (void)options;

    pthread_mutex_lock(&g_fix_mutex);
    int has = g_fix_has_device;
    char cached[44];
    strncpy(cached, g_fix_udid, sizeof(cached));
    cached[sizeof(cached) - 1] = '\0';
    int pid = g_fix_product_id;
    pthread_mutex_unlock(&g_fix_mutex);

    /* A lookup for a specific UDID cannot be satisfied before lockdown has
       revealed the real identity. The NULL lookup used by nativeConnect()
       is intentionally allowed to proceed with the transport label below. */
    if (udid && udid[0] && (!has || strcmp(udid, cached) != 0)) {
        return -ENOENT;
    }

    memset(device, 0, sizeof(*device));
    device->handle = 1;
    device->product_id = pid;

    if (has) {
        strncpy(device->udid, cached, sizeof(device->udid) - 1);
        device->udid[sizeof(device->udid) - 1] = '\0';
    } else {
        /* This is deliberately not formatted as an UDID. It is only a
         * transport label and is never exposed as the phone's identity. */
        strcpy(device->udid, "pending-device");
    }

    device->conn_type = CONNECTION_TYPE_USB;
    device->conn_data[0] = '\0';

    return 1; /* 1 device found */
}

/* ═══════════════════════════════════════════════════════════════════════
 * OVERRIDE (via -Wl,--wrap=usbmuxd_connect): __wrap_usbmuxd_connect()
 *
 * CRITICAL FIX: Read USBMUXD_SOCKET_ADDRESS from environment.
 * usbmuxd_server.c creates a Unix domain socket and jni_bridge_imd.c
 * sets USBMUXD_SOCKET_ADDRESS=unix:/path/to/socket.
 *
 * Old code hardcoded 127.0.0.1:27015 which never reached the internal
 * usbmuxd server → iPhone never paired.
 * ═══════════════════════════════════════════════════════════════════════ */
__attribute__((visibility("default")))
int __wrap_usbmuxd_connect(const uint32_t handle, const unsigned short port)
{
    const char *addr_env = getenv("USBMUXD_SOCKET_ADDRESS");
    android_usbmuxd_fix_logf("[usbmux] connect handle=%u port=%u wire_port=%u addr=%s",
                             handle, (unsigned)port, (unsigned)htons(port),
                             addr_env ? addr_env : "(default)");
    int sfd = -1;

    if (addr_env && (strncmp(addr_env, "unix:", 5) == 0 || addr_env[0] == '/')) {
        /* ── Unix domain socket (correct path) ── */
        const char *path = addr_env;
        if (strncmp(addr_env, "unix:", 5) == 0) path = addr_env + 5;
        struct sockaddr_un sun;
        memset(&sun, 0, sizeof(sun));
        sun.sun_family = AF_UNIX;
        strncpy(sun.sun_path, path, sizeof(sun.sun_path) - 1);

        sfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sfd < 0) return -1;

        if (connect(sfd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
            close(sfd);
            return -1;
        }
    } else {
        /* ── TCP fallback ── */
        const char *ip = "127.0.0.1";
        unsigned short srv_port = 27015;

        if (addr_env && strstr(addr_env, ":")) {
            char buf[256];
            strncpy(buf, addr_env, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            char *colon = strrchr(buf, ':');
            if (colon) {
                *colon = '\0';
                ip = buf;
                srv_port = (unsigned short)atoi(colon + 1);
            }
        }

        sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd < 0) return -1;

        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = htons(srv_port);
        inet_pton(AF_INET, ip, &sin.sin_addr);

        if (connect(sfd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
            close(sfd);
            return -1;
        }
    }

    /* Send Connect plist to usbmuxd server */
    plist_t req = plist_new_dict();
    plist_dict_set_item(req, "MessageType", plist_new_string("Connect"));
    plist_dict_set_item(req, "DeviceID", plist_new_uint(1));
    /* libusbmuxd v1 encodes PortNumber as network-order uint16 in the plist.
     * The in-process server decodes it with ntohs(), matching upstream
     * send_connect_packet(). Sending host-order here turns lockdown 62078
     * into 32498 on little-endian Android and causes LOCKDOWN_E_MUX_ERROR. */
    plist_dict_set_item(req, "PortNumber", plist_new_uint(htons(port)));

    char *xml = NULL;
    uint32_t xml_len = 0;
    plist_to_xml(req, &xml, &xml_len);
    plist_free(req);

    if (!xml) {
        close(sfd);
        return -1;
    }

    android_usbmux_plist_hdr_t req_hdr;
    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.length  = (uint32_t)(sizeof(req_hdr) + xml_len);
    req_hdr.version = ANDROID_USBMUX_PROTO_PLIST;
    req_hdr.type    = ANDROID_USBMUX_TYPE_PLIST;
    req_hdr.tag     = 1;
    int ok = 0;

    if (send_all(sfd, &req_hdr, sizeof(req_hdr)) == 0 &&
        send_all(sfd, xml, xml_len) == 0) {
        android_usbmux_plist_hdr_t resp_hdr;
        memset(&resp_hdr, 0, sizeof(resp_hdr));
        if (recv_all(sfd, &resp_hdr, sizeof(resp_hdr)) == 0 &&
            resp_hdr.length >= sizeof(resp_hdr) &&
            resp_hdr.length <= 65536 &&
            resp_hdr.version == ANDROID_USBMUX_PROTO_PLIST &&
            resp_hdr.type == ANDROID_USBMUX_TYPE_PLIST) {
            uint32_t resp_len = resp_hdr.length - (uint32_t)sizeof(resp_hdr);
            char *resp_xml = (char *)malloc(resp_len + 1);
            if (resp_xml && recv_all(sfd, resp_xml, resp_len) == 0) {
                resp_xml[resp_len] = '\0';
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

    free(xml);

    android_usbmuxd_fix_logf("[usbmux] Connect Result=%d socket_fd=%d", ok ? 0 : -1, sfd);
    if (!ok) {
        close(sfd);
        return -1;
    }

    return sfd;
}
