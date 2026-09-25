#!/usr/bin/env bash
# Chạy test tích hợp tầng USB/usbmuxd trên Linux (không cần iPhone thật).
#
# Cần: gcc, python3, libssl-dev, và libplist 2.6.0 + libimobiledevice-glue 1.3.2
# + libusbmuxd 2.0.2 + libimobiledevice 1.3.0 build cho host (cùng phiên bản
# scripts/build_all.sh dùng cho Android), cài vào $HOSTDEPS (mặc định /opt/hostdeps),
# header libusb 1.0.27 ($LIBUSB_INC) và jni.h ($JNI_INC — từ NDK hoặc JDK).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CPP="$HERE/../../app/src/main/cpp"
HOSTDEPS="${HOSTDEPS:-/opt/hostdeps}"
LIBUSB_INC="${LIBUSB_INC:-$HOSTDEPS/include/libusb-1.0}"
OUT="${OUT:-${TMPDIR:-/tmp}/sideload-native-host-test}"
mkdir -p "$OUT/include"

# jni.h: ưu tiên NDK (tự chứa), không có thì dùng JDK (+ include/linux)
JNI_FLAGS=""
if [[ -n "${JNI_INC:-}" ]]; then
  JNI_FLAGS="-I$JNI_INC"
else
  NDK_JNI="${ANDROID_NDK_HOME:-/opt/android/android-ndk-r25c}/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/jni.h"
  if [[ -f "$NDK_JNI" ]]; then
    cp "$NDK_JNI" "$OUT/include/jni.h"
    JNI_FLAGS="-I$OUT/include"
  else
    for j in "${JAVA_HOME:-/nonexistent}" /usr/lib/jvm/*; do
      if [[ -f "$j/include/jni.h" ]]; then JNI_FLAGS="-I$j/include -I$j/include/linux"; break; fi
    done
  fi
fi
[[ -n "$JNI_FLAGS" ]] || { echo "Không tìm thấy jni.h (cài JDK hoặc đặt JNI_INC)"; exit 2; }

gcc -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -pthread \
  -I"$CPP" $JNI_FLAGS -I"$LIBUSB_INC" \
  -I"$HOSTDEPS/include" \
  "$HERE/test_main.c" "$HERE/mock_libusb.c" \
  "$CPP/usbmuxd_server.c" "$CPP/usb_fd_bridge.c" "$CPP/android_usbmuxd_fix.c" \
  "$HOSTDEPS/lib/libimobiledevice-1.0.a" "$HOSTDEPS/lib/libusbmuxd-2.0.a" \
  "$HOSTDEPS/lib/libimobiledevice-glue-1.0.a" "$HOSTDEPS/lib/libplist-2.0.a" \
  -lssl -lcrypto -lm -o "$OUT/test_native"

SOCK="$OUT/fake_iphone.sock"
python3 "$HERE/fake_iphone.py" "$SOCK" > "$OUT/fake.log" 2>&1 &
FAKE=$!
trap 'kill $FAKE 2>/dev/null || true' EXIT
for _ in $(seq 50); do grep -q READY "$OUT/fake.log" 2>/dev/null && break; sleep 0.1; done
FAKE_IPHONE_SOCK="$SOCK" "$OUT/test_native"
