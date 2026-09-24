/*
 * usb_fd_bridge.c — fd USB của Android (UsbDeviceConnection) → libusb
 *
 * ════════════════════════════════════════════════════════════════════════
 * VIẾT LẠI (v49) theo đúng cách termux-usb + usbmuxd upstream làm việc:
 *
 *   termux-api UsbAPI.java:  usbManager.openDevice(dev).getFileDescriptor()
 *   termux-usb -e usbmuxd:   fd → libusb_wrap_sys_device()
 *   usbmuxd src/usb.c:       set_valid_configuration() → claim → bulk I/O
 *
 * NGUYÊN NHÂN GỐC của log "libusb_claim_interface NOT_FOUND" ở bản cũ:
 * iPhone có 4–6 USB configuration; Android để nó ở configuration 1 (chỉ có
 * PTP/ảnh). Interface usbmux (class 0xFF / sub 0xFE / proto 2) chỉ có ở
 * config 3/4 (hoặc 5/6 trên máy mới). Kotlin liệt kê interface của MỌI config
 * nên tìm thấy iface=1, nhưng claim trên config 1 → ENOENT = NOT_FOUND. Bản cũ
 * không bao giờ đổi configuration (upstream thì có: libusb_set_configuration),
 * rồi chồng thêm "Android JNI mode", clear_halt, flush… mà không chạm tới gốc.
 *
 * Các điểm khác so với bản cũ:
 *   - libusb_init_context(NO_DEVICE_DISCOVERY): app Android không được liệt
 *     kê /dev/bus/usb; chỉ dùng fd được cấp (đúng khuyến nghị của libusb).
 *   - Đọc bulk-IN nguyên một transfer vào bộ đệm lớn (16 KiB như upstream);
 *     bản cũ đọc 8/16 byte header trước → LIBUSB_ERROR_OVERFLOW, mất dữ liệu.
 *   - Gửi ZLP khi độ dài gói chia hết wMaxPacketSize (upstream usb_send()) —
 *     thiếu ZLP thì iPhone chờ mãi phần còn lại của gói.
 *   - Không clear_halt/flush vô cớ trước mỗi lần gửi (reset data toggle).
 *   - Luồng Java được attach MỘT lần cho mỗi luồng native (chế độ dự phòng
 *     Android), không attach/detach mỗi transfer.
 * ════════════════════════════════════════════════════════════════════════
 */
#include "usb_fd_bridge.h"
#include "android_usbmuxd_fix.h"

#include <libusb.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#define ALOG(prio, ...) __android_log_print(prio, "usb_fd_bridge", __VA_ARGS__)
#define PRIO_I ANDROID_LOG_INFO
#define PRIO_E ANDROID_LOG_ERROR
#else
#define ALOG(prio, ...) do { fprintf(stderr, "[usb_fd_bridge] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define PRIO_I 4
#define PRIO_E 6
#endif

#define LOGI(...) do { ALOG(PRIO_I, __VA_ARGS__); android_usbmuxd_fix_logf(__VA_ARGS__); } while (0)
#define LOGE(...) do { ALOG(PRIO_E, __VA_ARGS__); android_usbmuxd_fix_logf(__VA_ARGS__); } while (0)

#define APPLE_IF_CLASS     0xFF
#define APPLE_IF_SUBCLASS  0xFE
#define APPLE_IF_PROTO     0x02

static libusb_context       *g_ctx     = NULL;
static libusb_device_handle *g_handle  = NULL;
static uint8_t  g_ep_in  = 0;
static uint8_t  g_ep_out = 0;
static int      g_iface_num = -1;
static int      g_iface_claimed = 0;
static int      g_initialized = 0;
static int      g_maxpkt = 512;
static int      g_config = -1;
static char     g_serial[128];

/* Chế độ dự phòng: bulk I/O đi qua UsbDeviceConnection.bulkTransfer() (Java) */
static int       g_use_android = 0;
static JavaVM   *g_jvm = NULL;
static jobject   g_bridge_ref = NULL;
static jclass    g_nb_class = NULL;
static jmethodID g_mid_bulk_write = NULL;
static jmethodID g_mid_bulk_read = NULL;
static pthread_mutex_t g_jni_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t   g_env_key;
static pthread_once_t  g_env_once = PTHREAD_ONCE_INIT;

static int g_read_err_logged = 0;

/* ── JNI: attach một lần cho mỗi luồng, detach khi luồng kết thúc ─────────── */
static void env_detach(void *unused) {
    (void)unused;
    if (g_jvm) (*g_jvm)->DetachCurrentThread(g_jvm);
}
static void env_key_create(void) { pthread_key_create(&g_env_key, env_detach); }

static JNIEnv *get_env(void) {
    if (!g_jvm) return NULL;
    JNIEnv *env = NULL;
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) == JNI_OK) return env;
    pthread_once(&g_env_once, env_key_create);
    /* NDK khai báo JNIEnv**, JDK khai báo void** — void* hợp lệ cho cả hai */
    if ((*g_jvm)->AttachCurrentThread(g_jvm, (void *)&env, NULL) != JNI_OK) return NULL;
    pthread_setspecific(g_env_key, (void *)1);
    return env;
}

void usb_bridge_set_jvm(void *vm) { g_jvm = (JavaVM *)vm; }

void usb_bridge_set_bridge_ref(void *bridge_obj) {
    pthread_mutex_lock(&g_jni_mutex);
    JNIEnv *env = get_env();
    if (!env) { pthread_mutex_unlock(&g_jni_mutex); return; }
    if (g_bridge_ref) { (*env)->DeleteGlobalRef(env, g_bridge_ref); g_bridge_ref = NULL; }
    if (g_nb_class)   { (*env)->DeleteGlobalRef(env, g_nb_class);   g_nb_class = NULL; }
    g_mid_bulk_write = g_mid_bulk_read = NULL;
    jobject bridge = (jobject)bridge_obj;
    if (bridge) {
        g_bridge_ref = (*env)->NewGlobalRef(env, bridge);
        jclass cls = (*env)->GetObjectClass(env, bridge);
        if (cls) {
            g_nb_class = (jclass)(*env)->NewGlobalRef(env, cls);
            (*env)->DeleteLocalRef(env, cls);
            g_mid_bulk_write = (*env)->GetStaticMethodID(env, g_nb_class, "onNativeBulkWrite", "([BI)I");
            g_mid_bulk_read  = (*env)->GetStaticMethodID(env, g_nb_class, "onNativeBulkRead", "([BI)I");
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        }
    }
    pthread_mutex_unlock(&g_jni_mutex);
}

bool usb_bridge_set_android_mode(void) {
    if (!g_jvm || !g_mid_bulk_write || !g_mid_bulk_read) {
        LOGE("[usb] Không bật được chế độ bulkTransfer Android (JNI chưa sẵn sàng)");
        return false;
    }
    g_use_android = 1;
    LOGI("[usb] Dùng UsbDeviceConnection.bulkTransfer() (libusb không claim được interface)");
    return true;
}
bool usb_bridge_using_android_mode(void) { return g_use_android != 0; }
bool usb_bridge_iface_claimed(void)      { return g_iface_claimed != 0; }
uint8_t usb_bridge_ep_in(void)  { return g_ep_in; }
uint8_t usb_bridge_ep_out(void) { return g_ep_out; }
int usb_bridge_max_packet_size(void) { return g_maxpkt; }
int usb_bridge_active_config(void)   { return g_config; }
int usb_bridge_interface(void)       { return g_iface_num; }
const char *usb_bridge_serial(void)  { return g_serial[0] ? g_serial : NULL; }

static int call_android_bulk_write(const void *buf, int len, unsigned int timeout_ms) {
    JNIEnv *env = get_env();
    if (!env || !g_mid_bulk_write || !g_nb_class) return -1;
    jbyteArray arr = (*env)->NewByteArray(env, len);
    if (!arr) { if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env); return -1; }
    (*env)->SetByteArrayRegion(env, arr, 0, len, (const jbyte *)buf);
    jint r = (*env)->CallStaticIntMethod(env, g_nb_class, g_mid_bulk_write, arr, (jint)timeout_ms);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = -1; }
    (*env)->DeleteLocalRef(env, arr);
    return (int)r;
}

static int call_android_bulk_read(void *buf, int len, unsigned int timeout_ms) {
    JNIEnv *env = get_env();
    if (!env || !g_mid_bulk_read || !g_nb_class) return -1;
    jbyteArray arr = (*env)->NewByteArray(env, len);
    if (!arr) { if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env); return -1; }
    jint r = (*env)->CallStaticIntMethod(env, g_nb_class, g_mid_bulk_read, arr, (jint)timeout_ms);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = -1; }
    else if (r > 0) (*env)->GetByteArrayRegion(env, arr, 0, r, (jbyte *)buf);
    (*env)->DeleteLocalRef(env, arr);
    return (int)r;
}

/* ── Chọn configuration + interface usbmux (usb.c::set_valid_configuration) ── */
struct mux_iface {
    int cfg_value;
    int iface;
    uint8_t ep_in, ep_out;
    int maxpkt;
};

static int find_mux_iface_in_config(const struct libusb_config_descriptor *cfg, struct mux_iface *out) {
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        if (cfg->interface[i].num_altsetting < 1) continue;
        const struct libusb_interface_descriptor *alt = &cfg->interface[i].altsetting[0];
        if (alt->bInterfaceClass != APPLE_IF_CLASS || alt->bInterfaceSubClass != APPLE_IF_SUBCLASS ||
            alt->bInterfaceProtocol != APPLE_IF_PROTO)
            continue;
        uint8_t in = 0, outp = 0;
        int mp = 0;
        for (int e = 0; e < alt->bNumEndpoints; e++) {
            const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
            if ((ep->bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_BULK) continue;
            if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) { if (!in) in = ep->bEndpointAddress; }
            else if (!outp) { outp = ep->bEndpointAddress; mp = ep->wMaxPacketSize & 0x7ff; }
        }
        if (in && outp) {
            out->cfg_value = cfg->bConfigurationValue;
            out->iface = alt->bInterfaceNumber;
            out->ep_in = in;
            out->ep_out = outp;
            out->maxpkt = mp > 0 ? mp : 512;
            return 1;
        }
    }
    return 0;
}

static void detach_drivers_of_config(libusb_device *dev, int cfg_value) {
    struct libusb_config_descriptor *cfg = NULL;
    if (cfg_value <= 0 || libusb_get_config_descriptor_by_value(dev, (uint8_t)cfg_value, &cfg) != 0) return;
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        if (cfg->interface[i].num_altsetting < 1) continue;
        int n = cfg->interface[i].altsetting[0].bInterfaceNumber;
        if (libusb_kernel_driver_active(g_handle, n) == 1) {
            int r = libusb_detach_kernel_driver(g_handle, n);
            LOGI("[usb] Gỡ kernel driver khỏi interface %d → %s", n, libusb_error_name(r));
        }
    }
    libusb_free_config_descriptor(cfg);
}

static void normalize_serial(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
    if (n == 24 && strchr(s, '-') == NULL) {        /* UDID kiểu mới: 8-16 */
        memmove(s + 9, s + 8, 17);
        s[8] = '-';
    }
}

static void read_serial(libusb_device *dev) {
    struct libusb_device_descriptor dd;
    g_serial[0] = 0;
    if (libusb_get_device_descriptor(dev, &dd) != 0 || !dd.iSerialNumber) return;
    unsigned char buf[128];
    int r = libusb_get_string_descriptor_ascii(g_handle, dd.iSerialNumber, buf, sizeof(buf) - 1);
    if (r > 0) {
        buf[r] = 0;
        snprintf(g_serial, sizeof(g_serial), "%s", (const char *)buf);
        normalize_serial(g_serial);
    }
}

bool usb_bridge_init_from_fd2(int fd, int vendor_id, int product_id, int ep_in, int ep_out, int iface_num) {
    (void)vendor_id;
    if (g_initialized) usb_bridge_close();
    g_use_android = 0;
    g_iface_claimed = 0;
    g_iface_num = -1;
    g_config = -1;
    g_maxpkt = 512;
    g_read_err_logged = 0;

    int r;
#if defined(LIBUSB_API_VERSION) && (LIBUSB_API_VERSION >= 0x0100010A)
    struct libusb_init_option opts[1];
    memset(opts, 0, sizeof(opts));
    opts[0].option = LIBUSB_OPTION_NO_DEVICE_DISCOVERY;
    r = libusb_init_context(&g_ctx, opts, 1);
#else
    r = libusb_init(&g_ctx);
#endif
    if (r != 0) {
        LOGE("[usb] libusb_init lỗi %s", libusb_error_name(r));
        g_ctx = NULL;
        return false;
    }
    libusb_set_option(g_ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);

    r = libusb_wrap_sys_device(g_ctx, (intptr_t)fd, &g_handle);
    if (r != 0) {
        LOGE("[usb] libusb_wrap_sys_device(fd=%d) lỗi %s — fd không hợp lệ hoặc thiếu quyền USB",
             fd, libusb_error_name(r));
        libusb_exit(g_ctx);
        g_ctx = NULL;
        g_handle = NULL;
        return false;
    }
    libusb_device *dev = libusb_get_device(g_handle);
    struct libusb_device_descriptor dd;
    memset(&dd, 0, sizeof(dd));
    libusb_get_device_descriptor(dev, &dd);

    int cur = -1;
    if (libusb_get_configuration(g_handle, &cur) != 0) cur = -1;

    /* Như upstream: duyệt từ configuration CAO nhất xuống, lấy cái đầu tiên có
     * interface usbmux. */
    struct mux_iface mi;
    memset(&mi, 0, sizeof(mi));
    int found = 0;
    for (int idx = (int)dd.bNumConfigurations - 1; idx >= 0 && !found; idx--) {
        struct libusb_config_descriptor *cfg = NULL;
        if (libusb_get_config_descriptor(dev, (uint8_t)idx, &cfg) != 0 || !cfg) continue;
        found = find_mux_iface_in_config(cfg, &mi);
        libusb_free_config_descriptor(cfg);
    }
    LOGI("[usb] iPhone pid=0x%04x: %d configuration, đang dùng config %d%s",
         product_id, dd.bNumConfigurations, cur,
         found ? "" : " — KHÔNG thấy interface usbmux trong descriptor");

    if (!found) {
        if (ep_in && ep_out && iface_num >= 0) {
            mi.cfg_value = cur;
            mi.iface = iface_num;
            mi.ep_in = (uint8_t)ep_in;
            mi.ep_out = (uint8_t)ep_out;
            mi.maxpkt = 512;
            found = 1;
            LOGI("[usb] Dùng endpoint do Kotlin cung cấp: iface=%d in=0x%02x out=0x%02x", iface_num, ep_in, ep_out);
        } else {
            LOGE("[usb] Không tìm được interface usbmux (class 0xFF/0xFE/2). Thiết bị không phải iPhone/iPad?");
            libusb_close(g_handle); g_handle = NULL;
            libusb_exit(g_ctx); g_ctx = NULL;
            return false;
        }
    }

    if (mi.cfg_value > 0 && cur != mi.cfg_value) {
        detach_drivers_of_config(dev, cur);
        detach_drivers_of_config(dev, mi.cfg_value);
        r = libusb_set_configuration(g_handle, mi.cfg_value);
        if (r == 0) {
            LOGI("[usb] ✅ Đổi USB configuration %d → %d (interface usbmux nằm ở config %d)",
                 cur, mi.cfg_value, mi.cfg_value);
            cur = mi.cfg_value;
        } else {
            LOGE("[usb] libusb_set_configuration(%d) lỗi %s — sẽ thử claim trực tiếp / dự phòng Android",
                 mi.cfg_value, libusb_error_name(r));
            if (r == LIBUSB_ERROR_BUSY)
                LOGE("[usb] 💡 Một app khác đang giữ iPhone (Files/Ảnh/MTP). Đóng app đó, rút cáp cắm lại.");
            int c2 = -1;
            if (libusb_get_configuration(g_handle, &c2) == 0) cur = c2;
        }
    }
    g_config = cur;
    g_iface_num = mi.iface;
    g_ep_in = mi.ep_in;
    g_ep_out = mi.ep_out;
    g_maxpkt = mi.maxpkt > 0 ? mi.maxpkt : 512;

    r = libusb_claim_interface(g_handle, g_iface_num);
    if (r == LIBUSB_ERROR_BUSY) {
        int d = libusb_detach_kernel_driver(g_handle, g_iface_num);
        LOGI("[usb] Interface %d đang bận → detach kernel driver: %s", g_iface_num, libusb_error_name(d));
        r = libusb_claim_interface(g_handle, g_iface_num);
    }
    if (r == 0) {
        g_iface_claimed = 1;
        LOGI("[usb] ✅ Claim interface %d (config %d) — ep_in=0x%02x ep_out=0x%02x maxpkt=%d",
             g_iface_num, g_config, g_ep_in, g_ep_out, g_maxpkt);
    } else {
        LOGE("[usb] libusb_claim_interface(%d) lỗi %s (config hiện tại %d) — chuyển sang "
             "UsbDeviceConnection.bulkTransfer()", g_iface_num, libusb_error_name(r), g_config);
    }
    read_serial(dev);
    if (g_serial[0]) LOGI("[usb] Serial USB (UDID): %s", g_serial);
    g_initialized = 1;
    return true;
}

bool usb_bridge_init_from_fd(int fd, int vendor_id, int product_id) {
    return usb_bridge_init_from_fd2(fd, vendor_id, product_id, 0, 0, -1);
}

bool usb_bridge_clear_endpoints_halt(void) {
    if (g_use_android) return true;
    if (!g_handle) return false;
    if (g_ep_out) libusb_clear_halt(g_handle, g_ep_out);
    if (g_ep_in) libusb_clear_halt(g_handle, g_ep_in);
    return true;
}

/* Ghi trọn một gói mux; tự gửi ZLP khi len % wMaxPacketSize == 0 (usb_send của
 * upstream). Trả về len khi thành công, <0 khi lỗi. */
int usb_bridge_bulk_write(const void *buf, int len, unsigned int timeout) {
    if (len <= 0) return 0;
    if (g_use_android) return call_android_bulk_write(buf, len, timeout ? timeout : 5000);
    if (!g_handle || !g_ep_out) return -1;
    int off = 0, pipe_retry = 0;
    while (off < len) {
        int t = 0;
        int r = libusb_bulk_transfer(g_handle, g_ep_out, (unsigned char *)buf + off, len - off, &t,
                                     timeout ? timeout : 5000);
        if (t > 0) off += t;
        if (r == 0) {
            if (off < len && t == 0) return -1;
            continue;
        }
        if (r == LIBUSB_ERROR_PIPE && pipe_retry++ < 2) { libusb_clear_halt(g_handle, g_ep_out); continue; }
        if (r == LIBUSB_ERROR_TIMEOUT && t > 0) continue;
        LOGE("[usb] bulk OUT 0x%02x lỗi %s (%d/%d byte)", g_ep_out, libusb_error_name(r), off, len);
        return -1;
    }
    if (g_maxpkt > 0 && (len % g_maxpkt) == 0) {
        int t = 0;
        int r = libusb_bulk_transfer(g_handle, g_ep_out, (unsigned char *)buf, 0, &t, 1000);
        if (r != 0) LOGE("[usb] Gửi ZLP lỗi %s", libusb_error_name(r));
    }
    return len;
}

/* Đọc MỘT transfer (≤ len). 0 = timeout/ZLP. *completed = 0 nếu transfer bị
 * cắt do timeout (còn phần sau). <0 = lỗi. */
int usb_bridge_bulk_read_ex(void *buf, int len, unsigned int timeout, int *completed) {
    if (completed) *completed = 1;
    if (g_use_android) {
        int n = call_android_bulk_read(buf, len, timeout ? timeout : 1000);
        return n;
    }
    if (!g_handle || !g_ep_in) return -1;
    int t = 0;
    int r = libusb_bulk_transfer(g_handle, g_ep_in, (unsigned char *)buf, len, &t, timeout ? timeout : 1000);
    if (r == 0) return t;
    if (r == LIBUSB_ERROR_TIMEOUT) {
        if (t > 0) { if (completed) *completed = 0; return t; }
        return 0;
    }
    if (r == LIBUSB_ERROR_INTERRUPTED) return t > 0 ? t : 0;
    if (r == LIBUSB_ERROR_OVERFLOW && t > 0) return t;
    if (r == LIBUSB_ERROR_PIPE) libusb_clear_halt(g_handle, g_ep_in);
    if (g_read_err_logged++ < 5)
        LOGE("[usb] bulk IN 0x%02x lỗi %s", g_ep_in, libusb_error_name(r));
    return -1;
}

int usb_bridge_bulk_read(void *buf, int len, unsigned int timeout) {
    return usb_bridge_bulk_read_ex(buf, len, timeout, NULL);
}

void usb_bridge_flush_in(int max_packets, int timeout_ms) {
    uint8_t *buf = malloc(16384);
    if (!buf) return;
    for (int i = 0; i < max_packets; i++) {
        int n = usb_bridge_bulk_read(buf, 16384, (unsigned)timeout_ms);
        if (n <= 0) break;
    }
    free(buf);
}

void usb_bridge_close(void) {
    if (g_handle) {
        if (g_iface_claimed && g_iface_num >= 0) libusb_release_interface(g_handle, g_iface_num);
        libusb_close(g_handle);      /* fd do Android sở hữu: libusb không đóng fd đã wrap */
        g_handle = NULL;
    }
    if (g_ctx) { libusb_exit(g_ctx); g_ctx = NULL; }
    g_iface_claimed = 0;
    g_iface_num = -1;
    g_ep_in = g_ep_out = 0;
    g_initialized = 0;
    g_use_android = 0;
    g_config = -1;
}
