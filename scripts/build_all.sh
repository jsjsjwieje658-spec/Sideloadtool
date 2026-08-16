#!/usr/bin/env bash
# ============================================================================
#  build_all.sh — Cross-compile libimobiledevice stack cho Android ARM64/x86_64
#
#  CI builds the upstream sources for each ABI into a disposable, gitignored
#  directory. The resulting static archives and headers are consumed by
#  CMake and linked into libsideloadnative.so; no opaque native archive is
#  committed to the application source tree.
#
#  Kết quả: source-built output đặt vào:
#    .native-deps/arm64-v8a/lib/ và .native-deps/arm64-v8a/include/
#    .native-deps/x86_64/lib/   và .native-deps/x86_64/include/
#
#  Thứ tự build: libusb → libplist → libusbmuxd → libimobiledevice-glue
#                → openssl → libimobiledevice
#
#  Yêu cầu:
#    - Android NDK 25 (ANDROID_NDK_HOME set)
#    - autotools, libtool, pkg-config, git, cmake
#    - Linux host (hoặc macOS với GNU coreutils)
# ============================================================================
set -euo pipefail

# ── Cấu hình ──────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
NATIVE_DEPS_BASE="${NATIVE_DEPS_BASE:-${REPO_ROOT}/.native-deps}"
BUILD_TMP="${TMPDIR:-/tmp}/sideload_native_build"

NDK_VERSION="${NDK_VERSION:-25.2.9519653}"
# The app minSdk is 26; getifaddrs is exposed by Android from API 24.
ANDROID_API="${ANDROID_API:-26}"
ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-${HOME}/Android/Sdk/ndk/${NDK_VERSION}}"

read -r -a TARGETS <<< "${TARGET_ABIS:-arm64-v8a x86_64}"

# Phiên bản thư viện
LIBUSB_VERSION="1.0.27"
LIBPLIST_VERSION="2.6.0"
LIBUSBMUXD_VERSION="2.0.2"
LIBIMGLU_VERSION="1.3.2"   # libimobiledevice-glue upstream tag
LIBIMD_VERSION="1.3.0"     # libimobiledevice
OPENSSL_VERSION="3.2.1"

NCPU=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Màu log
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[+] $*${NC}"; }
warn()  { echo -e "${YELLOW}[!] $*${NC}"; }
error() { echo -e "${RED}[x] $*${NC}" >&2; exit 1; }

# ── Kiểm tra NDK ────────────────────────────────────────────────────────────
check_ndk() {
    if [[ ! -d "${ANDROID_NDK_HOME}" ]]; then
        error "ANDROID_NDK_HOME không tồn tại: ${ANDROID_NDK_HOME}\n" \
              "  Cài đặt: sdkmanager --install 'ndk;${NDK_VERSION}'\n" \
              "  Hoặc set: export ANDROID_NDK_HOME=/path/to/ndk"
    fi
    info "NDK: ${ANDROID_NDK_HOME}"
}

# ── Toolchain setup ──────────────────────────────────────────────────────────
setup_toolchain() {
    local abi="$1"
    case "${abi}" in
        arm64-v8a)
            TRIPLE="aarch64-linux-android"
            ARCH_FLAGS="-march=armv8-a"
            ;;
        x86_64)
            TRIPLE="x86_64-linux-android"
            ARCH_FLAGS="-m64"
            ;;
        *) error "ABI không hỗ trợ: ${abi}" ;;
    esac

    HOST_TRIPLE="${TRIPLE}${ANDROID_API}"
    TOOLCHAIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64"
    if [[ ! -d "${TOOLCHAIN}" ]]; then
        TOOLCHAIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/darwin-x86_64"
    fi
    [[ -d "${TOOLCHAIN}" ]] || error "Không tìm thấy LLVM toolchain"

    # OpenSSL's Android Configure detects modern NDKs through clang on PATH.
    # NDK r25 no longer ships aarch64-linux-android-gcc wrappers.
    export PATH="${TOOLCHAIN}/bin:${PATH}"
    export ANDROID_NDK_ROOT="${ANDROID_NDK_HOME}"
    export ANDROID_NDK="${ANDROID_NDK_HOME}"
    export CC="${TOOLCHAIN}/bin/${HOST_TRIPLE}-clang"
    export CXX="${TOOLCHAIN}/bin/${HOST_TRIPLE}-clang++"
    export AR="${TOOLCHAIN}/bin/llvm-ar"
    export RANLIB="${TOOLCHAIN}/bin/llvm-ranlib"
    export STRIP="${TOOLCHAIN}/bin/llvm-strip"
    export NM="${TOOLCHAIN}/bin/llvm-nm"
    export LD="${TOOLCHAIN}/bin/ld.lld"

    SYSROOT="${TOOLCHAIN}/sysroot"
    export CFLAGS="${ARCH_FLAGS} -O2 -fPIC -ffunction-sections -fdata-sections"
    export CFLAGS="${CFLAGS} --sysroot=${SYSROOT} -DANDROID"
    export LDFLAGS="-Wl,--gc-sections --sysroot=${SYSROOT}"

    PREFIX="${NATIVE_DEPS_BASE}/${abi}"
    export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig"
    export PKG_CONFIG_LIBDIR="${PREFIX}/lib/pkgconfig"

    mkdir -p "${PREFIX}/lib" "${PREFIX}/include" "${PREFIX}/lib/pkgconfig"
}

# ── Download helper ──────────────────────────────────────────────────────────
dl() {
    local url="$1" dst="$2"
    if [[ ! -f "${dst}" ]]; then
        info "Download: $(basename ${dst})"
        curl -fsSL --retry 3 -o "${dst}" "${url}" || \
        wget -q --tries=3 -O "${dst}" "${url}" || \
        error "Không download được: ${url}"
    fi
}

clone_or_update() {
    local url="$1" dir="$2" tag="${3:-}"
    if [[ ! -d "${dir}/.git" ]]; then
        if [[ -n "${tag}" ]]; then
            git clone --depth=1 --branch "${tag}" "${url}" "${dir}"
        else
            git clone --depth=1 "${url}" "${dir}"
        fi
    else
        warn "$(basename ${dir}) đã clone — bỏ qua"
    fi
}

# ── Build libusb ─────────────────────────────────────────────────────────────
build_libusb() {
    local abi="$1" prefix="$2"
    local src="${BUILD_TMP}/libusb-${LIBUSB_VERSION}"
    local tarball="${BUILD_TMP}/libusb-${LIBUSB_VERSION}.tar.bz2"

    info "[${abi}] Build libusb-${LIBUSB_VERSION}"
    dl "https://github.com/libusb/libusb/releases/download/v${LIBUSB_VERSION}/libusb-${LIBUSB_VERSION}.tar.bz2" \
       "${tarball}"
    [[ -d "${src}" ]] || tar -xjf "${tarball}" -C "${BUILD_TMP}"

    pushd "${src}" >/dev/null
    autoreconf -fi 2>/dev/null || true
    ./configure \
        --host="${TRIPLE}" \
        --prefix="${prefix}" \
        --disable-shared \
        --enable-static \
        --disable-udev \
        --disable-examples-build \
        --disable-tests-build \
        ac_cv_func_timerfd_create=no \
        CFLAGS="${CFLAGS} -Wno-deprecated-declarations"
    make -j"${NCPU}" install
    popd >/dev/null
}

# ── Build OpenSSL ────────────────────────────────────────────────────────────
build_openssl() {
    local abi="$1" prefix="$2"
    local src="${BUILD_TMP}/openssl-${OPENSSL_VERSION}"
    local tarball="${BUILD_TMP}/openssl-${OPENSSL_VERSION}.tar.gz"

    info "[${abi}] Build OpenSSL-${OPENSSL_VERSION}"
    dl "https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz" \
       "${tarball}"
    [[ -d "${src}" ]] || tar -xzf "${tarball}" -C "${BUILD_TMP}"

    local openssl_target
    case "${abi}" in
        arm64-v8a) openssl_target="android-arm64" ;;
        x86_64)    openssl_target="android-x86_64" ;;
    esac

    pushd "${src}" >/dev/null
    CC="${CC}" CXX="${CXX}" AR="${AR}" RANLIB="${RANLIB}" \
    ./Configure \
        "${openssl_target}" \
        -D__ANDROID_API__=${ANDROID_API} \
        --prefix="${prefix}" \
        --openssldir="${prefix}/ssl" \
        no-shared no-tests no-docs \
        -fPIC

    # OpenSSL uses the NDK Clang toolchain exported above.
    ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT}" ANDROID_NDK="${ANDROID_NDK}" \
      PATH="${TOOLCHAIN}/bin:${PATH}" make -j"${NCPU}" build_libs
    make install_sw
    popd >/dev/null
}

# ── Build libplist ───────────────────────────────────────────────────────────
build_libplist() {
    local abi="$1" prefix="$2"
    local src="${BUILD_TMP}/libplist-${LIBPLIST_VERSION}"

    info "[${abi}] Build libplist-${LIBPLIST_VERSION}"
    clone_or_update \
        "https://github.com/libimobiledevice/libplist.git" \
        "${src}" \
        "${LIBPLIST_VERSION}"

    pushd "${src}" >/dev/null
    ./autogen.sh --prefix="${prefix}" \
        --host="${TRIPLE}" \
        --without-cython \
        --disable-shared \
        --enable-static \
        CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}"
    make -j"${NCPU}" install
    popd >/dev/null
}

# ── Build libimobiledevice-glue ──────────────────────────────────────────────
build_libimglu() {
    local abi="$1" prefix="$2"
    local src="${BUILD_TMP}/libimobiledevice-glue-${LIBIMGLU_VERSION}"

    info "[${abi}] Build libimobiledevice-glue-${LIBIMGLU_VERSION}"
    clone_or_update \
        "https://github.com/libimobiledevice/libimobiledevice-glue.git" \
        "${src}" \
        "${LIBIMGLU_VERSION}"

    pushd "${src}" >/dev/null
    ./autogen.sh --prefix="${prefix}" \
        --host="${TRIPLE}" \
        --disable-shared \
        --enable-static \
        CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}"
    make -j"${NCPU}" install
    popd >/dev/null
}

# ── Build libusbmuxd ─────────────────────────────────────────────────────────
build_libusbmuxd() {
    local abi="$1" prefix="$2"
    local src="${BUILD_TMP}/libusbmuxd-${LIBUSBMUXD_VERSION}"

    info "[${abi}] Build libusbmuxd-${LIBUSBMUXD_VERSION}"
    clone_or_update \
        "https://github.com/libimobiledevice/libusbmuxd.git" \
        "${src}" \
        "${LIBUSBMUXD_VERSION}"

    pushd "${src}" >/dev/null
    ./autogen.sh --prefix="${prefix}" \
        --host="${TRIPLE}" \
        --disable-shared \
        --enable-static \
        CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}"
    make -j"${NCPU}" install
    popd >/dev/null
}

# ── Build libimobiledevice ────────────────────────────────────────────────────
build_libimobiledevice() {
    local abi="$1" prefix="$2"
    local src="${BUILD_TMP}/libimobiledevice-${LIBIMD_VERSION}"

    info "[${abi}] Build libimobiledevice-${LIBIMD_VERSION}"
    clone_or_update \
        "https://github.com/libimobiledevice/libimobiledevice.git" \
        "${src}" \
        "${LIBIMD_VERSION}"

    pushd "${src}" >/dev/null
    # Android exposes pthread_once from libc; there is no standalone
    # libpthread to probe/link. Skip libimobiledevice's Unix-only check.
    sed -i '/AC_CHECK_LIB(pthread, \[pthread_once\]/d' configure.ac
    # libplist >= 2.6 already owns plist_format_t and PLIST_FORMAT_*.
    # libimobiledevice 1.3.0 still redeclares the old enum in common/utils.h.
    sed -i '/^enum plist_format_t {/,/^};$/d' common/utils.h
    sed -i 's/enum plist_format_t format/plist_format_t format/g' common/utils.h common/utils.c
    # Patch: disable các binary tool (chỉ cần lib)
    # Keep common: libimobiledevice links its internal common utility library.
    sed -i 's/^SUBDIRS = .*/SUBDIRS = common src/' Makefile.am 2>/dev/null || true

    ./autogen.sh --prefix="${prefix}" \
        --host="${TRIPLE}" \
        --disable-shared \
        --enable-static \
        --without-cython \
        --enable-openssl \
        CFLAGS="${CFLAGS} -Wno-error=deprecated" \
        LDFLAGS="${LDFLAGS} -L${prefix}/lib" \
        PKG_CONFIG_PATH="${PKG_CONFIG_PATH}"
    make -j"${NCPU}" install
    popd >/dev/null
}

# ── Build một ABI ────────────────────────────────────────────────────────────
build_abi() {
    local abi="$1"
    info "════════════════════════════════════"
    info " Build ABI: ${abi}"
    info "════════════════════════════════════"

    setup_toolchain "${abi}"
    local prefix="${NATIVE_DEPS_BASE}/${abi}"

    # Kiểm tra đã build chưa
    if [[ -f "${prefix}/lib/libimobiledevice-1.0.a" ]]; then
        warn "[${abi}] libimobiledevice đã build — bỏ qua (xóa ${prefix}/lib/ để rebuild)"
        return 0
    fi

    build_libusb          "${abi}" "${prefix}"
    build_openssl         "${abi}" "${prefix}"
    build_libplist        "${abi}" "${prefix}"
    build_libimglu        "${abi}" "${prefix}"
    build_libusbmuxd      "${abi}" "${prefix}"
    build_libimobiledevice "${abi}" "${prefix}"

    # Verify
    local ok=1
    for lib in libimobiledevice-1.0.a libusbmuxd-2.0.a libplist-2.0.a \
               libimobiledevice-glue-1.0.a libusb-1.0.a libssl.a libcrypto.a; do
        if [[ ! -f "${prefix}/lib/${lib}" ]]; then
            error "[${abi}] THIẾU: ${prefix}/lib/${lib}"
            ok=0
        fi
    done
    [[ "${ok}" -eq 1 ]] && info "[${abi}] ✅ Tất cả libs OK"
}

# ── Main ─────────────────────────────────────────────────────────────────────
main() {
    info "SideloadTool — build_all.sh bắt đầu"
    info "Repo root: ${REPO_ROOT}"
    info "Native deps: ${NATIVE_DEPS_BASE}"
    info "NDK:       ${ANDROID_NDK_HOME}"
    info "API:       ${ANDROID_API}"
    info "CPU:       ${NCPU}"

    check_ndk
    mkdir -p "${BUILD_TMP}"

    # Build tất cả ABI
    for abi in "${TARGETS[@]}"; do
        build_abi "${abi}"
    done

    info ""
    info "════════════════════════════════════"
    info " Build source stack hoàn tất; CMake sẽ link các archive vào APK."
    info " Chạy: ./gradlew assembleDebug"
    info "════════════════════════════════════"
    ls -lh "${NATIVE_DEPS_BASE}/arm64-v8a/lib/"*.a 2>/dev/null || true
}

main "$@"
