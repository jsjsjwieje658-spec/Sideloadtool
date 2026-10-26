#!/usr/bin/env bash
# v73 — Compile-check 4 file C thật sự được CMakeLists.txt biên dịch.
#
# Dùng header libplist/libimobiledevice THẬT:
#   - CI: $HOSTDEPS (do tests/native-host/build_host_deps.sh build, libplist 2.6.0)
#   - Local: pkg-config (apt install libplist-dev libimobiledevice-dev libusbmuxd-dev
#            libusb-1.0-0-dev default-jdk-headless)
#
# Nếu thiếu header thì IN RA SKIP va thoát 0 (không làm đỏ CI oan), nhưng
# khi có đủ header moi sai sót C deu lam job FAIL.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CPP_DIR="$ROOT/app/src/main/cpp"
HOSTDEPS="${HOSTDEPS:-${RUNNER_TEMP:-}/hostdeps}"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/stub/android"

cat > "$TMP/stub/android/log.h" <<'EOF'
#ifndef ANDROID_LOG_H_STUB
#define ANDROID_LOG_H_STUB
#include <stdarg.h>
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_ERROR 6
static inline int __android_log_print(int p, const char* t, const char* f, ...) {
    (void)p; (void)t; (void)f; return 0;
}
#endif
EOF

# jni.h: lấy từ JDK nếu có, không thì tạo stub tối thiểu.
JNI_INC=""
JNI_MD_INC=""
JNI_H="$(find /usr/lib/jvm /usr/include -name jni.h -print -quit 2>/dev/null || true)"
if [[ -n "$JNI_H" ]]; then
  JNI_INC="$(dirname "$JNI_H")"
  JNI_MD_INC="$JNI_INC/linux"
else
  cat > "$TMP/stub/jni.h" <<'EOF'
#ifndef JNI_H_STUB
#define JNI_H_STUB
#include <stdint.h>
typedef int jint; typedef unsigned char jboolean;
typedef void* jobject; typedef void* jclass; typedef void* jstring; typedef void* jmethodID;
typedef struct { const void* functions; } JNIEnv;
typedef struct JavaVM_ JavaVM;
#define JNI_TRUE 1
#define JNI_FALSE 0
#define JNI_VERSION_1_6 0x00010006
#define JNIEXPORT
#define JNICALL
#endif
EOF
  JNI_INC="$TMP/stub"
fi

INCS=(-I"$TMP/stub" -I"$CPP_DIR" -I"$JNI_INC" -I/usr/include/libusb-1.0)
[[ -d "$JNI_MD_INC" ]] && INCS+=(-I"$JNI_MD_INC")

if [[ -d "$HOSTDEPS/include" ]]; then
  export PKG_CONFIG_PATH="$HOSTDEPS/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
  INCS+=(-I"$HOSTDEPS/include" -I"$HOSTDEPS/include/libusb-1.0"
         -I"$HOSTDEPS/include/libimobiledevice"
         -I"$HOSTDEPS/include/libimobiledevice-glue")
fi
if pkg-config --exists libplist-2.0 2>/dev/null; then
  read -r -a PKG_CFLAGS <<< "$(pkg-config --cflags libplist-2.0 libusbmuxd-2.0 2>/dev/null || true)"
  INCS+=("${PKG_CFLAGS[@]}")
fi

if ! printf '#include <plist/plist.h>\n#include <libimobiledevice/libimobiledevice.h>\n#include <usbmuxd.h>\nint main(void){return 0;}\n' \
     | gcc -x c -fsyntax-only - "${INCS[@]}" >/dev/null 2>&1; then
  echo "⚠️  SKIP: header libplist/libimobiledevice chưa có trong môi trường này."
  echo "    (CI chạy script này SAU build_host_deps.sh nên sẽ kiểm tra đầy đủ.)"
  exit 0
fi

FILES=(jni_bridge_imd.c usb_fd_bridge.c android_usbmuxd_fix.c usbmuxd_server.c)
FAILED=0
for f in "${FILES[@]}"; do
  if gcc -fsyntax-only -std=gnu11 -D_GNU_SOURCE -Wall -Wextra \
        -Wno-unused-parameter "${INCS[@]}" "$CPP_DIR/$f" 2>"$TMP/err"; then
    echo "  ✅ $f"
  else
    echo "  ❌ $f"
    head -40 "$TMP/err"
    FAILED=1
  fi
done

if [[ "$FAILED" -ne 0 ]]; then
  echo "❌ Native syntax check FAILED"
  exit 1
fi
echo "✅ Native syntax check OK (4/4 file C)"
