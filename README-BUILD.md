# Build libimobiledevice cho Android (Non-Root)

## Chuẩn bị

1. Cài Android NDK (r25c hoặc mới hơn)
2. Clone 3 repo:

```bash
git clone https://github.com/libimobiledevice/libplist.git
git clone https://github.com/libimobiledevice/libusbmuxd.git
git clone https://github.com/libimobiledevice/libimobiledevice.git
```

3. Copy các patch vào cùng thư mục:

```bash
cp libusbmuxd-android.patch libimobiledevice-android.patch build-android.sh ./
```

## Build

```bash
chmod +x build-android.sh
export ANDROID_NDK=/path/to/android-ndk-r25c
./build-android.sh
```

## Copy vào Sideloadtool

```bash
cp install/arm64-v8a/lib/*.so    Sideloadtool/app/src/main/cpp/prebuilt/arm64-v8a/lib/
```

## Build APK

```bash
cd Sideloadtool
./gradlew assembleDebug
```

## Các patch

| Patch | File | Mô tả |
|-------|------|-------|
| `libusbmuxd-android.patch` | `src/libusbmuxd.c` | Xử lý Unix socket path không có `UNIX:` prefix |
| `libimobiledevice-android.patch` | `src/idevice.c` | Tạo device trực tiếp từ UDID khi discover fail |
