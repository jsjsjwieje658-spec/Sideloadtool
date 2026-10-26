#!/usr/bin/env python3
"""v73 — Functional test cho ĐÚNG code C production xử lý pair record.

Thay vì viết lại logic trong test (dễ "xanh giả"), script này TRÍCH các hàm
thuần plist trong app/src/main/cpp/jni_bridge_imd.c rồi biên dịch cùng
libplist thật và chạy trên host. Nhờ vậy nếu ai đó sửa sai
plist_normalise_data_keys() / plist_item_usable() / pairing_has_rp() thì test
này đỏ ngay.

Yêu cầu: gcc + libplist-2.0 (CI: $HOSTDEPS từ build_host_deps.sh).
Chạy: python3 tests/host/test_pairing_helpers.py
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SRC = os.path.join(ROOT, "app", "src", "main", "cpp", "jni_bridge_imd.c")
HOSTDEPS = os.environ.get("HOSTDEPS", "")


def slice_between(text, start, end):
    i = text.index(start)
    j = text.index(end, i)
    return text[i:j]


def main():
    src = open(SRC, encoding="utf-8").read()

    # Các hàm thuần plist (không đụng JNI/lockdown/AFC).
    pure = slice_between(src,
                         "static const char *PAIRING_DATA_KEYS[]",
                         "/* Đọc WiFi MAC trực tiếp từ lockdownd")
    has_rp_fn = slice_between(src,
                              "static bool pairing_has_rp(plist_t pl) {",
                              "/* Trả XML plist đầy đủ")

    harness = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <plist/plist.h>

/* stub 2 hàm log của JNI layer */
static void emit_log(const char *msg) { (void)msg; }
static void emitf(const char *fmt, ...) { (void)fmt; }

__PURE_CODE__

int main(void) {
    int failed = 0;
#define CHECK(c, m) do { if (!(c)) { printf("  ❌ %s\n", m); failed++; } \
                         else printf("  ✅ %s\n", m); } while (0)

    plist_t pl = plist_new_dict();
    /* 1. cert/key lưu dạng <string> (một số pair record) phải thành <data>. */
    plist_dict_set_item(pl, "DeviceCertificate", plist_new_string("LS0tLS1CRUdJTg=="));
    plist_normalise_data_keys(pl);
    plist_t dc = plist_dict_get_item(pl, "DeviceCertificate");
    CHECK(dc && plist_get_node_type(dc) == PLIST_DATA,
          "DeviceCertificate <string> được chuẩn hoá sang <data>");

    /* 2. chuỗi rỗng / key thiếu phải bị coi là không dùng được. */
    plist_dict_set_item(pl, "HostID", plist_new_string(""));
    CHECK(!plist_item_usable(pl, "HostID"), "HostID rỗng bị phát hiện (sẽ bị từ chối)");
    CHECK(!plist_item_usable(pl, "RootPrivateKey"), "key hoàn toàn thiếu bị phát hiện");
    plist_dict_set_item(pl, "RootPrivateKey", plist_new_data("KEY", 3));
    CHECK(plist_item_usable(pl, "RootPrivateKey"), "key <data> hợp lệ được chấp nhận");

    /* 3. nhận diện RPPairing (iOS 17.4+ / SideStore nightly). */
    CHECK(!pairing_has_rp(pl), "chưa có alt_irk → không ghi file RemoteRP");
    plist_dict_set_item(pl, "alt_irk", plist_new_data("irk", 3));
    CHECK(pairing_has_rp(pl), "có alt_irk → nhận diện RPPairing");

    /* 4. danh sách key bắt buộc phải đúng bộ SideStore/minimuxer cần. */
    const char *needed[] = {"DeviceCertificate", "HostCertificate", "HostPrivateKey",
                            "RootCertificate", "RootPrivateKey", "HostID",
                            "SystemBUID", "UDID", NULL};
    int found = 0;
    for (int i = 0; needed[i]; i++)
        for (int k = 0; PAIRING_REQUIRED_KEYS[k]; k++)
            if (strcmp(needed[i], PAIRING_REQUIRED_KEYS[k]) == 0) found++;
    CHECK(found == 8, "PAIRING_REQUIRED_KEYS chứa đủ 8 key bắt buộc");

    plist_free(pl);
    if (failed) { printf("❌ %d kiểm tra C thất bại\n", failed); return 1; }
    printf("✅ PAIRING C HELPERS OK\n");
    return 0;
}
'''.replace("__PURE_CODE__", pure + "\n" + has_rp_fn)

    with tempfile.TemporaryDirectory(prefix="pairing_c_test_") as tmp:
        cfile = os.path.join(tmp, "test_pairing_helpers.c")
        exe = os.path.join(tmp, "test_pairing_helpers")
        open(cfile, "w", encoding="utf-8").write(harness)

        incs = []
        cflags, libs = [], ["-lplist-2.0"]
        if HOSTDEPS and os.path.isdir(os.path.join(HOSTDEPS, "include")):
            incs += ["-I" + os.path.join(HOSTDEPS, "include")]
            env = dict(os.environ, PKG_CONFIG_PATH=os.path.join(HOSTDEPS, "lib", "pkgconfig"))
        else:
            env = dict(os.environ)
        try:
            pc = subprocess.run(["pkg-config", "--cflags", "libplist-2.0"],
                                capture_output=True, text=True, env=env)
            if pc.returncode == 0:
                cflags += pc.stdout.split()
            pcl = subprocess.run(["pkg-config", "--libs", "libplist-2.0"],
                                 capture_output=True, text=True, env=env)
            if pcl.returncode == 0:
                libs = pcl.stdout.split()
        except FileNotFoundError:
            pass

        # The CI host dependencies build libplist as a static archive. Its
        # implementation calls fmin()/pow(), which need libm on the final
        # executable link (pkg-config --libs does not expose this dependency).
        # Keep -lm after the archive flags so GNU ld resolves those symbols.
        libs.append("-lm")
        cmd = ["gcc", "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Wextra",
               *cflags, *incs, cfile, "-o", exe, *libs]
        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            # Không có libplist trên máy → bỏ qua có chủ đích (CI có đủ).
            if "plist/plist.h" in build.stderr or "cannot find -lplist" in build.stderr:
                print("⚠️  SKIP: libplist-2.0 chưa có; CI (sau build_host_deps.sh) sẽ chạy đầy đủ.")
                return 0
            print(build.stderr[:4000])
            print("❌ Biên dịch C test thất bại")
            return 1
        run = subprocess.run([exe], capture_output=True, text=True)
        print(run.stdout, end="")
        if run.stderr:
            print(run.stderr[:2000])
        return run.returncode


if __name__ == "__main__":
    sys.exit(main())
