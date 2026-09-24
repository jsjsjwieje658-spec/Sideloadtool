/*
 * test_main.c — kiểm thử tích hợp usb_fd_bridge.c + usbmuxd_server.c trên host
 * với libusbmuxd 2.0.2 và libimobiledevice 1.3.0 THẬT (cùng phiên bản CI dùng),
 * nói chuyện với iPhone giả lập (fake_iphone.py) qua libusb giả (mock_libusb.c).
 */
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <usbmuxd.h>
#include <plist/plist.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <openssl/sha.h>

#include "usb_fd_bridge.h"
#include "usbmuxd_server.h"

extern int mock_current_config, mock_set_config_calls, mock_claimed_iface, mock_no_discovery, mock_zlp_sent;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { g_pass++; printf("  ✅ " __VA_ARGS__); printf("\n"); } \
    else { g_fail++; printf("  ❌ " __VA_ARGS__); printf("\n"); } } while (0)

static const char *EXPECT_UDID = "00008030-001A35E80C41802E";

static int send_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) { ssize_t w = send(fd, c, n, MSG_NOSIGNAL); if (w <= 0) return -1; c += w; n -= (size_t)w; }
    return 0;
}
static int recv_all(int fd, void *p, size_t n, int timeout_s) {
    struct timeval tv = { timeout_s, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char *c = p;
    while (n) { ssize_t r = recv(fd, c, n, 0); if (r <= 0) return -1; c += r; n -= (size_t)r; }
    return 0;
}

/* libimobiledevice-glue để socket ở O_NONBLOCK (libimobiledevice tự select()
 * trước mỗi send/recv). Test dùng socket thô nên chuyển về blocking. */
static int mux_connect(int port) {
    int fd = usbmuxd_connect(1, (unsigned short)port);
    if (fd >= 0) {
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    }
    return fd;
}

struct echo_job { int port; size_t size; int ok; unsigned seed; };

static void *echo_worker(void *arg) {
    struct echo_job *j = arg;
    int fd = mux_connect(j->port);
    if (fd < 0) { j->ok = 0; return NULL; }
    unsigned char *out = malloc(j->size), *in = malloc(j->size);
    for (size_t i = 0; i < j->size; i++) out[i] = (unsigned char)(rand_r(&j->seed) & 0xff);
    /* gửi và nhận xen kẽ để không kẹt cửa sổ */
    size_t sent = 0, got = 0;
    j->ok = 1;
    while (got < j->size) {
        if (sent < j->size) {
            size_t n = j->size - sent < 30000 ? j->size - sent : 30000;
            if (send_all(fd, out + sent, n) != 0) { j->ok = 0; break; }
            sent += n;
        }
        struct timeval tv = { 10, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssize_t r = recv(fd, in + got, j->size - got, sent < j->size ? MSG_DONTWAIT : 0);
        if (r > 0) got += (size_t)r;
        else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { j->ok = 0; break; }
    }
    if (j->ok) j->ok = memcmp(in, out, j->size) == 0;
    free(out); free(in);
    close(fd);
    return NULL;
}

int main(void) {
    printf("=== 1. USB: chọn configuration + claim (usb_fd_bridge.c) ===\n");
    bool ok = usb_bridge_init_from_fd2(42, 0x05ac, 0x12a8, 0x85, 0x04, 1);
    CHECK(ok, "usb_bridge_init_from_fd2 thành công");
    CHECK(mock_no_discovery, "libusb khởi tạo với NO_DEVICE_DISCOVERY (Android không cho quét /dev/bus/usb)");
    CHECK(mock_current_config == 4 && mock_set_config_calls == 1,
          "đổi configuration 1 → 4 (config cao nhất có usbmux) — hiện tại %d, số lần set %d",
          mock_current_config, mock_set_config_calls);
    CHECK(usb_bridge_iface_claimed() && mock_claimed_iface == 1, "claim được interface usbmux #1 (bản cũ: NOT_FOUND)");
    CHECK(usb_bridge_ep_in() == 0x85 && usb_bridge_ep_out() == 0x04 && usb_bridge_max_packet_size() == 512,
          "endpoint 0x85/0x04, wMaxPacketSize 512");
    CHECK(usb_bridge_serial() && strcmp(usb_bridge_serial(), EXPECT_UDID) == 0,
          "UDID từ iSerialNumber đã chèn '-': %s", usb_bridge_serial() ? usb_bridge_serial() : "(null)");

    printf("=== 2. usbmuxd nội bộ: bắt tay VERSION/SETUP ===\n");
    char dir[] = "/tmp/muxtestXXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    ok = usbmuxd_server_start(dir, usb_bridge_serial(), 0x12a8);
    CHECK(ok, "usbmuxd_server_start");
    setenv("USBMUXD_SOCKET_ADDRESS", usbmuxd_server_socket_address(), 1);
    CHECK(strncmp(usbmuxd_server_socket_address(), "UNIX:", 5) == 0, "địa chỉ socket dạng UNIX:… (%s)",
          usbmuxd_server_socket_address());
    CHECK(usbmuxd_server_device_ready(8000), "iPhone giả trả lời VERSION → thiết bị ACTIVE");

    printf("=== 3. libusbmuxd 2.0.2 thật: ListDevices / BUID / pair record ===\n");
    usbmuxd_device_info_t *list = NULL;
    int cnt = usbmuxd_get_device_list(&list);
    CHECK(cnt == 1 && list && strcmp(list[0].udid, EXPECT_UDID) == 0 && list[0].conn_type == CONNECTION_TYPE_USB,
          "usbmuxd_get_device_list → %d thiết bị, UDID %s", cnt, cnt > 0 ? list[0].udid : "-");
    if (list) usbmuxd_device_list_free(&list);
    char *buid1 = NULL, *buid2 = NULL;
    int r1 = usbmuxd_read_buid(&buid1), r2 = usbmuxd_read_buid(&buid2);
    CHECK(r1 == 0 && buid1 && strlen(buid1) == 36 && strcmp(buid1, "00000000-0000-0000-0000-000000000000") != 0,
          "ReadBUID trả UUID thật (bản cũ: toàn số 0 và bị libusbmuxd bỏ): %s", buid1 ? buid1 : "(null)");
    CHECK(r2 == 0 && buid2 && buid1 && strcmp(buid1, buid2) == 0, "SystemBUID được lưu bền (đọc lại giống hệt)");
    free(buid1); free(buid2);

    char *rec = NULL; uint32_t rsz = 0;
    int rr = usbmuxd_read_pair_record(EXPECT_UDID, &rec, &rsz);
    CHECK(rr == -ENOENT, "ReadPairRecord khi chưa pair → -ENOENT (libimobiledevice hiểu là 'chưa ghép nối'), nhận %d", rr);
    free(rec); rec = NULL;
    plist_t pr = plist_new_dict();
    plist_dict_set_item(pr, "HostID", plist_new_string("11111111-2222-3333-4444-555555555555"));
    plist_dict_set_item(pr, "SystemBUID", plist_new_string("AAAA"));
    plist_dict_set_item(pr, "HostCertificate", plist_new_data("CERTDATA", 8));
    char *xml = NULL; uint32_t xlen = 0;
    plist_to_xml(pr, &xml, &xlen);
    int sr = usbmuxd_save_pair_record_with_device_id(EXPECT_UDID, 1, xml, xlen);
    CHECK(sr == 0, "SavePairRecord → 0 (bản cũ: trả OK nhưng KHÔNG lưu gì)");
    rr = usbmuxd_read_pair_record(EXPECT_UDID, &rec, &rsz);
    plist_t back = NULL;
    if (rr == 0) plist_from_memory(rec, rsz, &back, NULL);
    char *hid = NULL;
    if (back) { plist_t n = plist_dict_get_item(back, "HostID"); if (n) plist_get_string_val(n, &hid); }
    CHECK(rr == 0 && hid && strcmp(hid, "11111111-2222-3333-4444-555555555555") == 0,
          "ReadPairRecord đọc lại đúng pair record đã lưu");
    free(hid); free(rec); rec = NULL; free(xml);
    if (back) plist_free(back);
    plist_free(pr);
    CHECK(usbmuxd_delete_pair_record(EXPECT_UDID) == 0 && usbmuxd_read_pair_record(EXPECT_UDID, &rec, &rsz) == -ENOENT,
          "DeletePairRecord xoá thật");
    free(rec); rec = NULL;

    printf("=== 4. libimobiledevice 1.3.0 thật: lockdownd qua mux ===\n");
    idevice_t dev = NULL;
    idevice_error_t ie = idevice_new_with_options(&dev, EXPECT_UDID, IDEVICE_LOOKUP_USBMUX);
    CHECK(ie == IDEVICE_E_SUCCESS, "idevice_new_with_options(UDID thật) = %d", ie);
    lockdownd_client_t ld = NULL;
    lockdownd_error_t le = lockdownd_client_new(dev, &ld, "sideloadtool");
    CHECK(le == LOCKDOWN_E_SUCCESS, "lockdownd_client_new = %d", le);
    char *type = NULL;
    le = lockdownd_query_type(ld, &type);
    CHECK(le == LOCKDOWN_E_SUCCESS && type && strcmp(type, "com.apple.mobile.lockdown") == 0,
          "QueryType → %s", type ? type : "(null)");
    free(type);
    plist_t val = NULL; char *udid = NULL;
    le = lockdownd_get_value(ld, NULL, "UniqueDeviceID", &val);
    if (val) plist_get_string_val(val, &udid);
    CHECK(le == LOCKDOWN_E_SUCCESS && udid && strcmp(udid, EXPECT_UDID) == 0, "GetValue UniqueDeviceID → %s", udid ? udid : "-");
    free(udid); if (val) plist_free(val);

    printf("=== 5. Nhiều kết nối ĐỒNG THỜI (lockdownd vẫn mở) + dữ liệu lớn ===\n");
    struct echo_job jobs[3] = { { 5555, 2 * 1024 * 1024, 0, 1 }, { 5555, 1536 * 1024, 0, 2 }, { 5555, 777777, 0, 3 } };
    pthread_t th[3];
    for (int i = 0; i < 3; i++) pthread_create(&th[i], NULL, echo_worker, &jobs[i]);
    for (int i = 0; i < 3; i++) pthread_join(th[i], NULL);
    CHECK(jobs[0].ok && jobs[1].ok && jobs[2].ok, "3 luồng echo song song (2 MiB + 1.5 MiB + 760 KiB) toàn vẹn dữ liệu");
    val = NULL; udid = NULL;
    le = lockdownd_get_value(ld, NULL, "ProductVersion", &val);
    if (val) plist_get_string_val(val, &udid);
    CHECK(le == LOCKDOWN_E_SUCCESS && udid && strcmp(udid, "17.5.1") == 0,
          "kết nối lockdownd mở từ trước vẫn dùng được sau đó (bản cũ: gói bị luồng khác 'ăn')");
    free(udid); if (val) plist_free(val);

    /* sink 6 MiB: host → iPhone, iPhone trả sha256 */
    {
        size_t n = 6 * 1024 * 1024 + 123;
        unsigned char *buf = malloc(n);
        for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)((i * 131) ^ (i >> 7));
        unsigned char want[32], got[32];
        SHA256(buf, n, want);
        int fd = mux_connect(5557);
        unsigned char hdr[8];
        for (int i = 0; i < 8; i++) hdr[i] = (unsigned char)(((uint64_t)n) >> (56 - 8 * i));
        struct timeval t0, t1;
        gettimeofday(&t0, NULL);
        int okk = fd >= 0 && send_all(fd, hdr, 8) == 0 && send_all(fd, buf, n) == 0 && recv_all(fd, got, 32, 30) == 0;
        gettimeofday(&t1, NULL);
        double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
        CHECK(okk && memcmp(want, got, 32) == 0, "đẩy 6 MiB host→iPhone (như chép IPA qua AFC), sha256 khớp (%.2f s)", secs);
        if (fd >= 0) close(fd);
        free(buf);
    }
    /* source 3 MiB: iPhone → host, gói > 16 KiB phải gom qua nhiều transfer */
    {
        size_t n = 3 * 1024 * 1024;
        unsigned char *buf = malloc(n);
        int fd = mux_connect(5558);
        int okk = fd >= 0 && recv_all(fd, buf, n, 30) == 0;
        for (size_t i = 0; okk && i < n; i++) if (buf[i] != (unsigned char)((i * 7 + 3) & 0xff)) okk = 0;
        CHECK(okk, "nhận 3 MiB iPhone→host (gói 40 KB tách qua nhiều transfer 16 KiB) đúng từng byte");
        if (fd >= 0) close(fd);
        free(buf);
    }

    printf("=== 6. Từ chối / reset ===\n");
    {
        struct timeval t0, t1;
        gettimeofday(&t0, NULL);
        int fd = mux_connect(9999);
        gettimeofday(&t1, NULL);
        double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
        CHECK(fd < 0 && secs < 3.0, "cổng không có dịch vụ → Connect bị từ chối ngay (%.2f s)", secs);
        if (fd >= 0) close(fd);
    }
    {
        int fd = mux_connect(5556);
        char junk[1500];
        memset(junk, 'x', sizeof(junk));
        int okk = fd >= 0 && send_all(fd, junk, sizeof(junk)) == 0;
        char c;
        struct timeval tv = { 5, 0 };
        if (fd >= 0) setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssize_t r = fd >= 0 ? recv(fd, &c, 1, 0) : -1;
        CHECK(okk && r == 0, "iPhone RST giữa chừng → socket client nhận EOF (không treo)");
        if (fd >= 0) close(fd);
    }
    lockdownd_client_free(ld);
    idevice_free(dev);

    printf("=== 7. Báo cáo vi phạm giao thức từ iPhone giả ===\n");
    {
        usleep(200 * 1000);
        int fd = mux_connect(5599);
        char rep[65536];
        size_t got = 0;
        struct timeval tv = { 5, 0 };
        if (fd >= 0) setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        while (fd >= 0 && got < sizeof(rep) - 1) {
            ssize_t r = recv(fd, rep + got, sizeof(rep) - 1 - got, 0);
            if (r <= 0) break;
            got += (size_t)r;
            if (memchr(rep, '}', got) && rep[got - 1] == '}') break;
        }
        rep[got] = 0;
        printf("  %s\n", rep);
        CHECK(got > 0 && strstr(rep, "\"violations\": []") != NULL, "0 vi phạm so với giao thức usbmuxd upstream");
        CHECK(mock_zlp_sent > 0, "ZLP đã được gửi cho gói có độ dài chia hết 512 (%d lần)", mock_zlp_sent);
        if (fd >= 0) close(fd);
    }

    usbmuxd_server_stop();
    usb_bridge_close();
    printf("\n%d đạt, %d lỗi\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
