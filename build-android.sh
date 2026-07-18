#!/bin/bash
# build-android-libimobiledevice.sh
# Cross-compile libimobiledevice stack for Android (non-root)
# Requires: Android NDK r25c or later

set -e

# ═══════════════════════════════════════════════════════════════════════
# Configuration
# ═══════════════════════════════════════════════════════════════════════
export NDK="${ANDROID_NDK:-/opt/android-ndk}"
export API=28
export TARGET=aarch64-linux-android
export ARCH=arm64-v8a

export TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
export CC="$TOOLCHAIN/bin/$TARGET$API-clang"
export CXX="$TOOLCHAIN/bin/$TARGET$API-clang++"
export AR="$TOOLCHAIN/bin/llvm-ar"
export STRIP="$TOOLCHAIN/bin/llvm-strip"
export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
export LD="$TOOLCHAIN/bin/ld"

export PREFIX="$(pwd)/install/$ARCH"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export CFLAGS="-O2 -fPIC -DANDROID -I$PREFIX/include"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="-L$PREFIX/lib"

mkdir -p "$PREFIX"
mkdir -p "$PREFIX/lib"
mkdir -p "$PREFIX/include"

echo "========================================"
echo "Building for: $TARGET (API $API)"
echo "CC: $CC"
echo "PREFIX: $PREFIX"
echo "========================================"

# ═══════════════════════════════════════════════════════════════════════
# 1. Build libplist
# ═══════════════════════════════════════════════════════════════════════
if [ -d "libplist" ]; then
    echo ""
    echo "[1/3] Building libplist..."
    cd libplist
    ./autogen.sh --host="$TARGET" --prefix="$PREFIX" --without-cython
    make -j$(nproc)
    make install
    cd ..
    echo "✅ libplist built"
else
    echo "⚠️  libplist/ not found — skipping"
fi

# ═══════════════════════════════════════════════════════════════════════
# 2. Build libusbmuxd (with Android patch)
# ═══════════════════════════════════════════════════════════════════════
if [ -d "libusbmuxd" ]; then
    echo ""
    echo "[2/3] Building libusbmuxd..."
    cd libusbmuxd

    # Apply Android patch if not already applied
    if ! grep -q "Android non-root" src/libusbmuxd.c 2>/dev/null; then
        echo "Applying Android patch..."
        patch -p1 < ../libusbmuxd-android.patch || true
    fi

    ./autogen.sh --host="$TARGET" --prefix="$PREFIX" --without-preflight
    make -j$(nproc)
    make install
    cd ..
    echo "✅ libusbmuxd built"
else
    echo "⚠️  libusbmuxd/ not found — skipping"
fi

# ═══════════════════════════════════════════════════════════════════════
# 3. Build libimobiledevice (with Android patches)
# ═══════════════════════════════════════════════════════════════════════
if [ -d "libimobiledevice" ]; then
    echo ""
    echo "[3/3] Building libimobiledevice..."
    cd libimobiledevice

    # Apply Android patches if not already applied
    if ! grep -q "Android non-root bypass" src/idevice.c 2>/dev/null; then
        echo "Applying Android patches..."
        patch -p1 < ../libimobiledevice-android.patch || true
    fi

    ./autogen.sh --host="$TARGET" --prefix="$PREFIX" --without-cython
    make -j$(nproc)
    make install
    cd ..
    echo "✅ libimobiledevice built"
else
    echo "⚠️  libimobiledevice/ not found — skipping"
fi

# ═══════════════════════════════════════════════════════════════════════
# 4. Strip and copy to Sideloadtool
# ═══════════════════════════════════════════════════════════════════════
echo ""
echo "[4/4] Stripping and copying libraries..."

for lib in libplist-2.0 libusbmuxd-2.0 libimobiledevice-1.0; do
    if [ -f "$PREFIX/lib/${lib}.so" ]; then
        "$STRIP" "$PREFIX/lib/${lib}.so"
        echo "  ✅ ${lib}.so"
    fi
done

echo ""
echo "========================================"
echo "Build complete!"
echo "Libraries in: $PREFIX/lib/"
echo ""
echo "Copy to Sideloadtool:"
echo "  cp $PREFIX/lib/*.so \"
echo "     /path/to/Sideloadtool/app/src/main/cpp/prebuilt/$ARCH/lib/"
echo "========================================"
