#!/usr/bin/env bash
# Build cho MÁY HOST (Linux x86_64) đúng các phiên bản mà scripts/build_all.sh
# build cho Android, kèm đúng các sed/patch của build_all.sh (kể cả bản vá
# SSL_OP_LEGACY_SERVER_CONNECT cho OpenSSL 3), để tests/native-host/run.sh
# chạy libusbmuxd + libimobiledevice thật với usbmuxd_server.c.
set -euo pipefail
HOSTDEPS="${HOSTDEPS:-/opt/hostdeps}"
SRC="${SRC:-${TMPDIR:-/tmp}/sideload-host-src}"
JOBS="$(nproc 2>/dev/null || echo 2)"
mkdir -p "$HOSTDEPS/include/libusb-1.0" "$SRC"
export PKG_CONFIG_PATH="$HOSTDEPS/lib/pkgconfig"
export CFLAGS="-O1 -g -fPIC"
cd "$SRC"

clone() { [[ -d "$2/.git" ]] || git clone -q --depth 1 --branch "$3" "$1" "$2"; }

clone https://github.com/libimobiledevice/libplist.git libplist 2.6.0
clone https://github.com/libimobiledevice/libimobiledevice-glue.git glue 1.3.2
clone https://github.com/libimobiledevice/libusbmuxd.git libusbmuxd 2.0.2
clone https://github.com/libimobiledevice/libimobiledevice.git libimobiledevice 1.3.0
[[ -f libusb-1.0.27.tar.bz2 ]] || curl -fsSL -o libusb-1.0.27.tar.bz2 \
  https://github.com/libusb/libusb/releases/download/v1.0.27/libusb-1.0.27.tar.bz2
[[ -d libusb-1.0.27 ]] || tar xjf libusb-1.0.27.tar.bz2
cp libusb-1.0.27/libusb/libusb.h "$HOSTDEPS/include/libusb-1.0/libusb.h"

(cd libplist && ./autogen.sh --prefix="$HOSTDEPS" --without-cython --disable-shared --enable-static >/dev/null && make -j"$JOBS" install >/dev/null)
(cd glue && ./autogen.sh --prefix="$HOSTDEPS" --disable-shared --enable-static >/dev/null && make -j"$JOBS" install >/dev/null)
(cd libusbmuxd && ./autogen.sh --prefix="$HOSTDEPS" --disable-shared --enable-static >/dev/null && make -j"$JOBS" install >/dev/null)
(
  cd libimobiledevice
  sed -i '/^enum plist_format_t {/,/^};$/d' common/utils.h
  sed -i 's/enum plist_format_t format/plist_format_t format/g' common/utils.h common/utils.c
  sed -i 's/^SUBDIRS = .*/SUBDIRS = common src include/' Makefile.am
  if ! grep -q 'SSL_OP_LEGACY_SERVER_CONNECT' src/idevice.c; then
    awk '{print} /SSL_CTX_set_security_level\(ssl_ctx, 0\);/ && !done {getline; print; print "#if defined(SSL_OP_LEGACY_SERVER_CONNECT)"; print "\tSSL_CTX_set_options(ssl_ctx, SSL_OP_LEGACY_SERVER_CONNECT);"; print "#endif"; print "#if defined(SSL_OP_IGNORE_UNEXPECTED_EOF)"; print "\tSSL_CTX_set_options(ssl_ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);"; print "#endif"; done=1}' src/idevice.c > src/idevice.c.new
    mv src/idevice.c.new src/idevice.c
  fi
  grep -q 'SSL_OP_LEGACY_SERVER_CONNECT' src/idevice.c
  ./autogen.sh --prefix="$HOSTDEPS" --disable-shared --enable-static --without-cython --enable-openssl \
    CFLAGS="$CFLAGS -Wno-error=deprecated -Wno-deprecated-declarations" >/dev/null
  make -j"$JOBS" install >/dev/null
)
ls -1 "$HOSTDEPS"/lib/*.a
echo "HOST DEPS OK → $HOSTDEPS"
